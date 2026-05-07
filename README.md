# VK_LAYER_TEGRA_x11_present

A Vulkan implicit layer that fixes screen tearing on Nvidia Tegra X11
when using the proprietary `libGLX_nvidia.so.0` Vulkan ICD shipped with
L4T r32.x.

## Why this exists

The Vulkan ICD in L4T r32.x (used on Jetson Nano, TX1, and Nintendo
Switch L4T builds) implements `VK_KHR_swapchain` for X11 surfaces using
the legacy DRI2 + XPutImage path. It does not support DRI3 or the X11
Present extension. As a result, every `vkQueuePresentKHR` ends up doing
a CPU/EGL CopyArea blit into the framebuffer with no vblank
synchronisation. This produces tearing on every frame regardless of
the present mode the application requests.

This layer bypasses that broken WSI by:

1. Allocating swapchain images with `VK_KHR_external_memory_fd` so they
   can be exported as dmabufs.
2. Importing each dmabuf as an X11 Pixmap via `xcb_dri3_pixmap_from_buffer`.
3. Replacing `vkQueuePresentKHR` with `xcb_present_pixmap`, which the
   X server schedules at the next vblank using glamor's hardware flip
   path — fully tear-free.

The X server does the heavy lifting; we just hand it the right buffer
at the right time.

## Requirements

- Nvidia Tegra GPU (vendor 0x10DE, integrated GPU). On non-Tegra GPUs
  the layer is a transparent pass-through.
- `libGLX_nvidia.so.0` providing `VK_KHR_external_memory_fd`,
  `VK_KHR_external_semaphore_fd`, `VK_KHR_swapchain`, and one of
  `VK_KHR_xcb_surface` / `VK_KHR_xlib_surface`. (All confirmed present
  on L4T r32.3.1 via vulkaninfo.)
- X server with DRI3 + Present extensions loaded. For Lakka, this
  requires the `xorg-server` package to be built with `--enable-dri3
  --enable-present` and `--enable-glamor`. The L4T project package.mk
  must be updated to enable glamor (it is currently disabled).
- xcb client libraries: `libxcb-dri3`, `libxcb-present`, `libX11-xcb`.

## Build

```
make
```

Optional cross-compile from a build host:

```
CC=aarch64-linux-gnu-gcc make
```

## Install

```
sudo make install
```

This places:
- `/usr/lib/aarch64-linux-gnu/libVkLayer_tegra_x11_present.so`
- `/etc/vulkan/implicit_layer.d/VkLayer_tegra_x11_present.json`

The Vulkan loader picks up implicit layers from
`/etc/vulkan/implicit_layer.d/` and `/usr/share/vulkan/implicit_layer.d/`
automatically — no application change required.

## Verifying it loaded

```
TEGRA_X11_PRESENT_DEBUG=1 vulkaninfo 2>&1 | grep TEGRA_x11_present
```

You should see lines like:
```
[VK_LAYER_TEGRA_x11_present] Instance created
[VK_LAYER_TEGRA_x11_present] Device created on Tegra GPU, shim active
```

When running RetroArch with the Vulkan video driver:
```
TEGRA_X11_PRESENT_DEBUG=1 retroarch ... 2>&1 | head -20
```

You should see `Created shim swapchain` log lines.

## Disabling

To bypass the layer at runtime without uninstalling:
```
DISABLE_TEGRA_X11_PRESENT=1 retroarch ...
```

Useful for A/B testing whether the tearing disappears with the layer
active.

## Troubleshooting

**`DRI3 extension not present on X server`**

Your xorg-server build does not include DRI3 support. Rebuild
`xorg-server` with `--enable-dri3` and ensure the Nvidia driver loads
glamor (`--enable-glamor`).

**`dri3_pixmap_from_buffer failed: error_code=N`**

The X server rejected our dmabuf import. Most common causes:
- The Vulkan image format doesn't have a matching X11 visual.
  Currently supported: BGRA8 / RGBA8 (depth 24, bpp 32) and
  RGB565 (depth 16, bpp 16).
- The dmabuf stride doesn't match what X expects. The layer assumes
  stride = width * (bpp/8) which is correct for linear-tiled images.
  If the Tegra driver allocates with implicit-modifier tiling, this
  will fail and the layer will report VK_ERROR_NATIVE_WINDOW_IN_USE_KHR
  back to the application.

**Tearing still present**

Confirm the layer is active:
```
TEGRA_X11_PRESENT_DEBUG=1 retroarch 2>&1 | grep -i 'shim active'
```

If you see `Non-Tegra GPU, layer is pass-through`, the GPU detection
failed. Force-enable with `TEGRA_X11_PRESENT_FORCE=1`.

If the layer is active and tearing persists, the X server's Present
extension is not using a flip-capable backend — confirm with:
```
grep -E 'Initializing extension Present|glamor' /var/log/Xorg.0.log
```

## License

GPL-2.0-or-later
