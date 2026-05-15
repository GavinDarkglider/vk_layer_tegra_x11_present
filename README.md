# vk_layer_tegra_x11_present

An implicit Vulkan layer for NVIDIA Tegra L4T r32.x (Tegra X1, Switch, Jetson
Nano family) that fixes tearing in Vulkan-on-X11 applications by routing
presentation through GL/GLX.

## What it does

On Tegra L4T r32.x, the NVIDIA Vulkan ICD's X11 WSI implementation does not
produce vsync-locked presentation. Frames tear. This is a driver bug that
NVIDIA will not fix; the r32.x BSP is EOL'd and the device is no longer
supported.

The same driver, however, does fully support:

- Vulkan external memory export with OPAQUE_FD handle type (OPTIMAL tiling,
  BGRA8/RGBA8 UNORM and SRGB)
- Vulkan external semaphore export with OPAQUE_FD handle type
- GL import of both via `GL_EXT_memory_object_fd` and `GL_EXT_semaphore_fd`
- Vsync-locked GLX presentation via `glXSwapIntervalEXT(1)` + `glXSwapBuffers`

This layer plumbs the two stacks together. The application uses Vulkan
normally; the layer replaces the X11 swapchain with one whose images are
OPAQUE_FD-exported Vulkan images. At present time, the layer imports each
image into a GLX context as a GL texture, has GL draw it to the GLX window's
back buffer, and calls `glXSwapBuffers`. The application is unaware that GL
is involved.

The result is vsync-locked, tear-free presentation on a driver that
otherwise can't do it.

## What it does not do

- **EGL.** Not used. The driver advertises `EGL_EXT_image_dma_buf_import`
  but the Vulkan ICD refuses to export dmabufs, so no dmabuf path works
  through EGL. The layer goes around EGL entirely.
- **DRM/KMS.** Not used. `tegra-udrm` is a stub on this BSP and is not loaded
  by default on Ubuntu noble builds. The layer never opens `/dev/dri/card*`.
- **libgbm.** Not used. The cross-allocate-then-import workaround was
  considered and dropped once direct Vulkan↔GL interop was confirmed working.
- **Wayland.** Not in scope. This layer hooks the X11 surface extensions
  (`VK_KHR_xlib_surface`, `VK_KHR_xcb_surface`) only. Wayland surfaces are
  passed through to the underlying WSI unmodified.
- **MAILBOX present mode.** Not yet implemented. The layer reports support
  for `VK_PRESENT_MODE_FIFO_KHR`, `VK_PRESENT_MODE_FIFO_RELAXED_KHR`, and
  `VK_PRESENT_MODE_IMMEDIATE_KHR`. Apps requesting MAILBOX get FIFO.

## How it works

For each swapchain image (N images per swapchain, configurable 2–8):

1. **Vulkan allocation.** The layer creates a `VkImage` with
   `VK_IMAGE_TILING_OPTIMAL` and `VkExternalMemoryImageCreateInfo` requesting
   `VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT`. The backing
   `VkDeviceMemory` is allocated with `VkExportMemoryAllocateInfo` for the
   same handle type. An fd is exported via `vkGetMemoryFdKHR`.

2. **GL import.** The layer creates a GLX 3.3-core context on the X window
   bound to the surface, makes it current, and:
   - Calls `glCreateMemoryObjectsEXT` and `glImportMemoryFdEXT` with a `dup()`
     of the Vulkan fd. GL takes ownership of its copy.
   - Creates a `GL_TEXTURE_2D` and binds it to the imported memory with
     `glTexStorageMem2DEXT` (`GL_RGBA8` or `GL_SRGB8_ALPHA8` as appropriate).

3. **Semaphore pairs.** Each image gets two `VkSemaphore` objects, both
   created with `VkExportSemaphoreCreateInfo` for `OPAQUE_FD`:
   - `vk_render_done[i]` — Vulkan signals after the application's render
     completes, GL waits before sampling. Bridged in `vkQueuePresentKHR`.
   - `gl_sample_done[i]` — GL signals after sampling completes, Vulkan
     waits before the application re-acquires the image. Bridged in
     `vkAcquireNextImageKHR`.
   - The fds are exported via `vkGetSemaphoreFdKHR` and imported into GL
     via `glGenSemaphoresEXT` + `glImportSemaphoreFdEXT`.

4. **Acquire** (`vkAcquireNextImageKHR`):
   - Picks the next free image index (round-robin).
   - Issues a bridge Vulkan submit: wait on `gl_sample_done[i]`, signal the
     application's requested acquire semaphore, signal our internal per-image
     fence.
   - CPU-blocks on the internal fence with `vkWaitForFences`. This is the
     backpressure point — the call returns only when GL has actually
     finished sampling the image, which paces the application to the real
     presentation rate.
   - Returns the index.

5. **Present** (`vkQueuePresentKHR`):
   - Issues a Vulkan bridge submit: wait on the application's present-time
     wait semaphores, signal `vk_render_done[i]`.
   - Posts the image index to the swapchain's worker thread via a single-slot
     queue. If the worker is still processing the previous frame, the post
     blocks; otherwise it returns immediately.
   - Returns to the application.

6. **Worker thread** (one per swapchain, lifetime-pinned to swapchain):
   - Owns the GLX context for the swapchain's entire lifetime. The context
     is made current at thread start and never released until the worker
     exits. This eliminates per-frame `glXMakeCurrent` overhead and isolates
     `glXSwapBuffers`'s vsync wait from the application's render thread.
   - Opens its own X `Display*` connection. Xlib is not thread-safe to
     share even with `XInitThreads`; the main thread and the worker need
     separate connections.
   - Loop: wait for a posted image index, refresh window geometry if the
     parent window resized, `glWaitSemaphoreEXT(vk_render_done[i])`,
     blit textured quad, `glSignalSemaphoreEXT(gl_sample_done[i])`,
     `glXSwapBuffers`. Then mark the slot free.

The cost per present is two Vulkan submits and a single textured-quad
draw. The application's render thread is paced by `vkAcquireNextImageKHR`'s
fence wait; the worker thread is paced by `glXSwapBuffers`'s vsync wait.
Measured pacing on Tegra X1: clean 60 Hz lock.

## NVIDIA driver environment

On Tegra L4T r32.x, NVIDIA's GLX implementation does `glXSwapBuffers`'s
vsync wait as a `sched_yield()` loop instead of a kernel sleep. The thread
running `glXSwapBuffers` therefore shows up at ~100% CPU in tools like
`top` even when the actual work is trivial.

The fix is to set `__GL_YIELD=USLEEP`, which tells NVIDIA's driver to use
`usleep(1)` between vblank checks. CPU drops to single digits, vsync
accuracy is preserved.

The catch: `libnvidia-glcore` reads `__GL_YIELD` at libGL initialization
and caches the value. By the time the Vulkan loader has loaded this
layer, libGL has already been loaded and the cache is set. Setting the
variable from a `.so` constructor or from `vkNegotiateLoaderLayer` is
too late. The KWin team hit the same wall and worked around it
differently (see
[their commit](https://github.com/KDE/kwin/commit/3ce5af5c21fd80e3da231b50c39c3ae357e9f15c)),
but on Tegra L4T r32.x specifically their `__GL_MaxFramesAllowed=1`
workaround does not help — only `__GL_YIELD=USLEEP` does, and it must be
set from the shell environment before the application launches.

To handle this transparently, the install step drops a shell fragment
at `/etc/profile.d/99-tegra-x11-present.sh` that exports
`__GL_YIELD=USLEEP`. Any login shell, systemd service inheriting from a
profile-shell parent, or interactive session picks it up automatically.
The fragment is short and self-documenting; users who want to disable
the override can simply delete or edit the file.

For non-shell launch paths (e.g. a systemd unit that starts before
profile.d runs), add the export explicitly:

```ini
[Service]
Environment=__GL_YIELD=USLEEP
```

Or in a launch script:

```sh
__GL_YIELD=USLEEP retroarch
```

## Compositor coexistence

The layer sets `_NET_WM_BYPASS_COMPOSITOR=1` on the surface window before
swapchain creation. On EWMH-compliant compositors (Mutter, KWin, Compiz)
this causes the compositor to stop redirecting the window, so
`glXSwapBuffers` reaches the display directly. On Lakka and other
compositor-less environments the hint is harmless. Even with the hint
ignored, presentation pacing remained vsync-locked in testing — the layer
does not require the absence of a compositor to function.

## Building

```sh
make
```

For cross-compilation:

```sh
make CROSS_COMPILE=aarch64-linux-gnu- SYSROOT=/path/to/sysroot
```

Override install paths if your distro doesn't use multiarch:

```sh
make install LAYERLIBDIR=/usr/lib LAYERJSONDIR=/etc/vulkan/implicit_layer.d
```

Ubuntu noble (default targets):

```sh
sudo make install
```

This installs three files:

- the `.so` to `/usr/lib/aarch64-linux-gnu/` (or wherever `LAYERLIBDIR` points)
- the implicit-layer JSON to `/usr/share/vulkan/implicit_layer.d/`
- a shell fragment to `/etc/profile.d/99-tegra-x11-present.sh` that exports
  `__GL_YIELD=USLEEP` for any login shell

The Vulkan loader picks the layer up automatically once the .so and JSON
are in place — no per-app environment variable required for the layer
itself. The profile.d fragment is what keeps the worker thread off the
CPU during vsync waits; see "NVIDIA driver environment" above for the
underlying reasoning.

The Lakka build recipe applies a patch that sets `LAYERLIBDIR=/usr/lib`
to match Lakka's flat library layout. No source changes are required.

### Build dependencies

- A C compiler (gcc or clang)
- Vulkan loader and headers (`libvulkan-dev`)
- OpenGL and GLX headers (`libgl1-mesa-dev` — installs glvnd loaders and
  headers; the actual runtime calls go through NVIDIA's driver)
- Xlib + XCB headers (`libx11-dev`, `libxcb1-dev`, `libx11-xcb-dev`)
- pthread

On Ubuntu noble:

```sh
sudo apt install build-essential libvulkan-dev libgl1-mesa-dev \
                 libx11-dev libxcb1-dev libx11-xcb-dev
```

## Verifying

After installation, run any Vulkan-on-X11 application. The layer should be
loaded automatically. Confirm with:

```sh
VK_TEGRA_X11_PRESENT_LOG=2 your_vulkan_app 2>&1 | grep VK_LAYER_TEGRA
```

You should see `CreateInstance`, `CreateXlibSurfaceKHR` (or
`CreateXcbSurfaceKHR`), and `CreateSwapchainKHR` log lines.

Visually: the application should render without tearing. Frame pacing
should be locked to the display refresh rate.

## Environment variables

| Variable | Effect |
|---|---|
| `VK_TEGRA_X11_PRESENT_DISABLE=1` | Passthrough mode. The layer loads but forwards every call unmodified to the next layer / ICD. Useful for A/B comparison against the broken native WSI. |
| `VK_TEGRA_X11_PRESENT_LOG=N` | Log verbosity: 0=silent, 1=warn/err (default), 2=info, 3=debug. |
| `VK_TEGRA_X11_PRESENT_LOG_FILE=PATH` | Append logs to `PATH` in addition to stderr. |
| `VK_TEGRA_X11_PRESENT_DIAG=1` | Diagnostic mode. After every `vkQueueSubmit`, the layer calls `vkDeviceWaitIdle` and reports if the GPU faulted on that submit. Catastrophically slow (every submit becomes synchronous), useful only for one-shot fault localization. |
| `__GL_YIELD=USLEEP` | NVIDIA driver env var. Installed automatically via `/etc/profile.d/99-tegra-x11-present.sh`. Must be set in the shell environment before the application loads libGL. See "NVIDIA driver environment" above. |

## Limitations and known issues

- **Vulkan queue serialization.** The application's `vkQueueSubmit` and
  the layer's internal bridge submits are serialized through a per-device
  mutex. This is required by the Vulkan spec (external synchronization on
  the queue) and matters when the application uses a separate render
  thread (PPSSPP, Citra, etc.). The cost is negligible: a mutex acquire
  per submit. Apps that only submit from one thread pay nothing.

- **Image format restrictions.** The application's swapchain format must
  be one of `VK_FORMAT_B8G8R8A8_UNORM`, `VK_FORMAT_R8G8B8A8_UNORM`,
  `VK_FORMAT_B8G8R8A8_SRGB`, or `VK_FORMAT_R8G8B8A8_SRGB`. Other formats
  fall through to the broken native WSI (with a warning).

- **No surface support for non-X11 platforms.** The layer hooks
  `VkSurfaceKHR` only for Xlib and XCB surfaces. Wayland, Win32, Android,
  and other surface types are passed through unchanged.

- **Soft-fail on setup failure.** If the layer cannot create its GLX
  context or allocate OPAQUE_FD-exportable images, `vkCreateSwapchainKHR`
  falls through to the underlying WSI rather than failing the app. The
  application runs with the original tearing behavior. A `WARN` log
  message identifies what failed.

## Why a layer and not a wrapper / patch / Mesa fix

- **A wrapper around the application** would have to be RetroArch-specific
  and rebuilt every time RetroArch's WSI usage changed. The layer applies
  to every Vulkan-on-X11 app on the system unchanged.

- **A patch to the application** has the same problem and requires the
  application to be open source and rebuildable.

- **A Mesa fix** doesn't apply — the broken WSI is NVIDIA's, not Mesa's.
  Mesa isn't involved in the Vulkan path on this driver.

- **An NVIDIA fix** isn't coming. The r32.x BSP is EOL.

An implicit Vulkan layer is the right shape: it transparently intercepts
the broken behavior at the lowest possible level, requires no application
changes, and ships as one .so + one JSON.

## License

Same as the parent project. (Replace this line with the actual SPDX
identifier on commit.)

## History

Version 1 of this layer attempted to use `EGL_EXT_image_dma_buf_import` for
the present path. That approach does not work on Tegra L4T r32.x because
the NVIDIA Vulkan ICD does not export dmabufs — every combination of format
and tiling returns `VK_ERROR_FORMAT_NOT_SUPPORTED` at capability query
time. The layer was rewritten to use `GL_EXT_memory_object_fd` instead,
which the driver does support. The Xorg extension shim that was developed
to work around EGL dmabuf failures is no longer needed and is not part of
this branch.
