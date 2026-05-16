# vk_layer_tegra_x11_present

A Vulkan implicit layer that fixes broken Vulkan→X11 presentation on
NVIDIA Tegra L4T r32.x by routing presentation through GL/GLX instead of
the native Vulkan WSI path.

Built originally for Nintendo Switch running Lakka and L4T r32.x; should
work unchanged on any Tegra device on the r32.x BSP whose Vulkan driver
has the same WSI tearing problem.

## What it does

The NVIDIA Vulkan ICD shipped with L4T r32.x has a broken X11 WSI
implementation: presented frames tear regardless of present mode. The BSP
is end-of-life and there is no driver fix. The same driver, however,
provides:

- Vulkan external memory and semaphore export via
  `VK_KHR_external_memory_fd` and `VK_KHR_external_semaphore_fd`
  (OPAQUE_FD handle type)
- GL import of both via `GL_EXT_memory_object_fd` and `GL_EXT_semaphore_fd`
- Working vsync-locked GL presentation via `GLX_SGI_video_sync`

This layer plumbs the three together. The application's Vulkan rendering
is untouched. We replace the WSI surface and swapchain with our own
implementation that:

1. Allocates swapchain images as OPAQUE_FD-exportable Vulkan images.
2. Imports each image into a GL/GLX context as a GL texture.
3. At `vkQueuePresentKHR` time: bridges the application's render-done
   semaphore into a GL semaphore and hands the image to a per-swapchain
   worker thread.
4. The worker samples the image into the GLX backbuffer, calls
   `glXSwapBuffers`, then explicitly blocks on the hardware vblank via
   `glXWaitVideoSyncSGI`. This is the same vsync mechanism KWin uses for
   NVIDIA on X11.
5. Bridges GL's sample-done back into a Vulkan semaphore so that the
   next `vkAcquireNextImageKHR` correctly gates the application's re-use
   of the image.

No EGL, no dmabuf, no DRM, no custom kernel modules. Only the Vulkan↔GL
interop primitives that the stock driver provides.

## Status

Working on Lakka L4T r32.x Switch builds.

Tested:

- vkcube — clean 60Hz vsync
- RetroArch with various Vulkan-capable cores (Dolphin etc.) — clean,
  no tearing, expected idle CPU
- Sascha Willems Vulkan samples (triangle, bloom, and the rest of the
  suite) — work normally; the symbol-export tightening in v2 fixed an
  earlier loader-deadlock with these
- PPSSPP libretro core — works for simple titles; some titles fault, see
  KNOWN ISSUES

## Architecture

```
Application's render thread                       Worker thread (per swapchain)
─────────────────────────                         ──────────────────────────
vkAcquireNextImageKHR
  ├── bridge submit: wait gl_sample[i], signal acquire_sem,
  │   signal acquire_fence
  └── WaitForFences(acquire_fence)   ← backpressure                    ←┐
                                                                        │
[app renders into image i]                                              │
                                                                        │
vkQueuePresentKHR                                                       │
  ├── bridge submit: wait app_render_done, signal vk_render_done[i]     │
  └── post {idx=i} to worker mailbox                  → take work       │
                                                       glWaitSemaphoreEXT(vk_render_done)
                                                       draw textured quad
                                                       glSignalSemaphoreEXT(gl_sample[i])  ─┘
                                                       glXSwapBuffers (interval 0)
                                                       glXWaitVideoSyncSGI  ← kernel vsync wait
                                                       mark mailbox free
```

The worker thread owns the GLX context for the swapchain's entire
lifetime and opens its own Xlib `Display*` (Xlib is not thread-safe to
share even with `XInitThreads`).

The backpressure point is the `WaitForFences` in
`vkAcquireNextImageKHR`: the fence signals only after the worker has
actually consumed the image through GL. The app's render loop is paced
to real presentation rate through this fence.

## Why glXWaitVideoSyncSGI and not just glXSwapBuffers

NVIDIA's `glXSwapBuffers` on Tegra L4T r32.x implements its built-in
vsync wait as a `sched_yield()` busy loop, not a kernel sleep. A thread
parked in that wait shows up as ~100% CPU in `top` even though it's
doing nothing useful. On a Switch, that one core is hot, drawing power,
and triggering DVFS upclocks.

Setting swap interval to 0 stops `glXSwapBuffers` from doing that wait
at all. Instead, the worker explicitly waits for the next vblank using
`glXWaitVideoSyncSGI`, which goes through the kernel and actually
sleeps. The CPU stays cold during vsync.

This is the same workaround KWin uses for NVIDIA on X11
([reference](https://github.com/KDE/kwin/blob/master/src/plugins/platforms/x11/standalone/glxbackend.cpp)
— search for `SGIVideoSyncVsyncMonitor`).

If `GLX_SGI_video_sync` is not available at runtime (it should always
be on NVIDIA proprietary drivers), the layer falls back to swap
interval 1 + `glXSwapBuffers` for vsync. A line in the log on swapchain
creation tells you which path is active.

## Runtime library loading

The `.so` does NOT link libGL, libGLX, or libX11. It depends only on
libc, libdl, and libpthread.

This is deliberate. The Vulkan loader dlopens implicit layers while
holding an internal mutex during `vkCreateInstance`. If our DT_NEEDED
list included libGL, ld.so would run libGL's constructor as part of the
load, with the loader's mutex held, and on some loader/driver
combinations that constructor calls back into the Vulkan loader (e.g.,
glvnd vendor registration) — which then tries to acquire the
already-held mutex, and the whole vkCreateInstance hangs.

The fix: `dlopen()` libGL / libGLX / libX11 ourselves at swapchain
creation time, well past the loader-mutex window, and resolve every
function via `dlsym` into a small table. The call sites use `#define`
macros to redirect transparently — `glXSwapBuffers(...)` expands to
`g_libs.glXSwapBuffers(...)`.

## Symbol export

Only `vkNegotiateLoaderLayerInterfaceVersion` is exported from the
`.so`. `vkGetInstanceProcAddr` and `vkGetDeviceProcAddr` are NOT
exported.

Why: under Vulkan layer interface v2, the loader stores the layer's
GIPA and GDPA function pointers from the `VkNegotiateLayerInterface`
struct filled in by negotiate, and calls them directly. The loader does
not need those symbols exported by name.

But if we DO export them, the loader's ICD-load path can do
`dlsym(RTLD_DEFAULT, "vkGetInstanceProcAddr")` and find OUR symbol
instead of the loader's own. The loader then re-enters our GIPA looking
for `vkCreateInstance`, gets our `layer_CreateInstance`, calls it —
while the outer `vkCreateInstance` is still on the stack with the
loader mutex held. Recursive deadlock.

The fix is one line in the version script: only export
`vkNegotiateLoaderLayerInterfaceVersion`. Confirmed with
`nm -D --defined-only libVkLayer_tegra_x11_present.so`.

## Vulkan queue serialization

The application's `vkQueueSubmit` calls and the layer's internal bridge
submits are serialized through a per-device mutex. This is required by
the Vulkan spec (external synchronization on the queue parameter) and
matters when the application uses a separate render thread (PPSSPP,
Citra, etc.). The cost is negligible — a mutex acquire per submit. Apps
that submit from a single thread pay nothing.

## Compositor coexistence

If a compositor is running and is the only thing rendering to the
screen, we still produce vsync-locked output to our window; the
compositor then syncs that to its own present cadence. Some compositors
(KWin, Mutter) may detect that we're drawing as fast as we can and
throttle accordingly.

For Lakka (no compositor running), we're directly painting to the X
window on the root and vsync timing is end-to-end accurate.

## Layout transition handling

The Vulkan WSI defines `VK_IMAGE_LAYOUT_PRESENT_SRC_KHR` for swapchain
images. Real WSI implementations track that as a hardware-meaningful
state. We don't have a real swapchain — our images are normal Vulkan
images with no presentation layout — so any pipeline barrier or render
pass that transitions our images to/from `PRESENT_SRC_KHR` would fault
the GPU on this driver.

The layer intercepts `vkCmdPipelineBarrier`, `vkCreateRenderPass`, and
`vkCreateRenderPass2KHR` and rewrites any `PRESENT_SRC_KHR` layout on a
managed image to `SHADER_READ_ONLY_OPTIMAL`. Application code is
unchanged; the rewrite is invisible.

## Environment variables

| Variable | Effect |
|---|---|
| `VK_TEGRA_X11_PRESENT_DISABLE=1` | Passthrough mode — the layer loads but forwards every call unmodified. Useful for A/B testing against the native (broken) WSI. Honored by the Vulkan loader via `disable_environment` in the manifest. |
| `VK_TEGRA_X11_PRESENT_LOG=N` | Log verbosity: 0=silent, 1=warn/err (default), 2=info, 3=debug. |
| `VK_TEGRA_X11_PRESENT_LOG_FILE=PATH` | Append logs to `PATH` in addition to stderr. |
| `VK_TEGRA_X11_PRESENT_DIAG=1` | Diagnostic mode. After every `vkQueueSubmit`, the layer calls `vkDeviceWaitIdle` and reports if the GPU faulted on that submit. Catastrophically slow; one-shot use only for fault localization. |

## Build and install

```sh
make
sudo make install
```

This installs the `.so` to `/usr/lib/aarch64-linux-gnu/` and the layer
JSON to `/usr/share/vulkan/implicit_layer.d/`. The Vulkan loader picks
the layer up automatically — no per-app environment variable required.

For non-multiarch layouts (Lakka, similar embedded distros), override
the library install path:

```sh
make install LAYERLIBDIR=/usr/lib
```

The Lakka build recipe does this automatically.

### Build dependencies

- A C compiler (gcc or clang)
- Vulkan headers (`vulkan/vulkan.h`, `vulkan/vk_layer.h`, `vulkan/vk_icd.h`)
- GL/GLX/X11 headers (for the type declarations only — these libraries
  are not linked at build time)

### Runtime dependencies

- libc, libdl, libpthread (DT_NEEDED)
- libGL / libGLX / libX11 (dlopen'd at swapchain creation; must be
  present at runtime)
- Vulkan loader 1.1+ with layer interface v2 support
- An NVIDIA Tegra Vulkan ICD (or any ICD that supports the OPAQUE_FD
  external memory/semaphore extensions and `GLX_SGI_video_sync`)

## Known issues

- **PPSSPP libretro core (some titles).** Some titles trigger a GPU
  fault in PPSSPP's render path that returns `VK_ERROR_DEVICE_LOST` from
  a later `vkQueueSubmit`. Without validation layers on Lakka we can't
  pinpoint the offending command. Simple titles work fine; complex
  titles with heavy post-processing don't. Use
  `VK_TEGRA_X11_PRESENT_DIAG=1` to narrow down which submit faults.

## Files

```
vk_layer_tegra_x11_present.c     main source
vk_layer_tegra_x11_present.json  Vulkan loader manifest
vk_layer_tegra_x11_present.map   linker version script (export control)
Makefile                         build and install
```
