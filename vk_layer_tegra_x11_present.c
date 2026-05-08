/*
 * VK_LAYER_TEGRA_x11_present
 *
 * Vulkan implicit layer that bypasses the broken WSI in libGLX_nvidia.so.0
 * (Tegra L4T r32.x) by routing presents through the X11 Present extension
 * via DRI3 pixmaps.
 *
 * Architecture:
 *   - Intercepts vkCreate*SurfaceKHR, vkCreateSwapchainKHR,
 *     vkAcquireNextImageKHR, vkQueuePresentKHR.
 *   - At swapchain creation, allocates VkImages with external-memory export
 *     (VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT), exports each as a
 *     dmabuf fd via vkGetMemoryFdKHR, imports each fd as an X11 Pixmap via
 *     xcb_dri3_pixmap_from_buffer.
 *   - At present time, signals an exportable VkSemaphore on the rendered
 *     image, exports as a sync_file fd, and uses xcb_present_pixmap to
 *     schedule the flip at the next vblank.
 *
 * The layer enables itself only on Nvidia Tegra GPUs.  On other vendors
 * it is a transparent pass-through.
 *
 * License: GPL-2.0-or-later
 */

#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

/* These platform defines must precede vulkan.h so the XCB / Xlib surface
 * structures and PFN_ typedefs are exposed.  Without them the ICD-side
 * surface entry points and types are invisible. */
#define VK_USE_PLATFORM_XCB_KHR
#define VK_USE_PLATFORM_XLIB_KHR

#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include <xcb/xcb.h>
#include <xcb/dri3.h>
#include <xcb/present.h>
#include <X11/Xlib-xcb.h>

#define LAYER_NAME        "VK_LAYER_TEGRA_x11_present"
#define LAYER_DESCRIPTION "X11 Present extension WSI shim for Tegra L4T"

/* Logging — concise, prefixed, and only when env var enables it. */
static int g_log_enabled = -1;
static void layer_log(const char *fmt, ...)
{
   if (g_log_enabled == -1)
      g_log_enabled = getenv("TEGRA_X11_PRESENT_DEBUG") ? 1 : 0;
   if (!g_log_enabled)
      return;
   va_list ap;
   va_start(ap, fmt);
   fprintf(stderr, "[%s] ", LAYER_NAME);
   vfprintf(stderr, fmt, ap);
   va_end(ap);
   fputc('\n', stderr);
}

/* ============================================================ *
 *  Dispatch tables — we keep our own keyed by VkInstance/Device *
 * ============================================================ */

#define MAX_SWAPCHAIN_IMAGES 8

typedef struct {
   VkInstance       instance;
   /* Instance-level dispatch */
   PFN_vkGetInstanceProcAddr               GetInstanceProcAddr;
   PFN_vkDestroyInstance                   DestroyInstance;
   PFN_vkEnumeratePhysicalDevices          EnumeratePhysicalDevices;
   PFN_vkGetPhysicalDeviceProperties       GetPhysicalDeviceProperties;
   PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties;
   PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
   PFN_vkCreateDevice                      CreateDevice;
   PFN_vkDestroySurfaceKHR                 DestroySurfaceKHR;
   PFN_vkGetPhysicalDeviceSurfaceSupportKHR    GetPhysicalDeviceSurfaceSupportKHR;
   PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR GetPhysicalDeviceSurfaceCapabilitiesKHR;
   PFN_vkGetPhysicalDeviceSurfaceFormatsKHR    GetPhysicalDeviceSurfaceFormatsKHR;
   PFN_vkGetPhysicalDeviceSurfacePresentModesKHR GetPhysicalDeviceSurfacePresentModesKHR;
   /* Real ICD WSI surface creators (kept for fall-through if not Tegra) */
   PFN_vkCreateXcbSurfaceKHR               CreateXcbSurfaceKHR;
   PFN_vkCreateXlibSurfaceKHR              CreateXlibSurfaceKHR;
} instance_data_t;

typedef struct {
   VkDevice         device;
   VkPhysicalDevice physical_device;
   instance_data_t *instance;
   uint32_t         graphics_queue_family;
   /* Device-level dispatch (subset we use) */
   PFN_vkGetDeviceProcAddr                 GetDeviceProcAddr;
   PFN_vkDestroyDevice                     DestroyDevice;
   PFN_vkGetDeviceQueue                    GetDeviceQueue;
   PFN_vkQueueSubmit                       QueueSubmit;
   PFN_vkDeviceWaitIdle                    DeviceWaitIdle;
   PFN_vkQueueWaitIdle                     QueueWaitIdle;
   PFN_vkCreateImage                       CreateImage;
   PFN_vkDestroyImage                      DestroyImage;
   PFN_vkGetImageMemoryRequirements        GetImageMemoryRequirements;
   PFN_vkAllocateMemory                    AllocateMemory;
   PFN_vkFreeMemory                        FreeMemory;
   PFN_vkBindImageMemory                   BindImageMemory;
   PFN_vkCreateSemaphore                   CreateSemaphore;
   PFN_vkDestroySemaphore                  DestroySemaphore;
   PFN_vkCreateFence                       CreateFence;
   PFN_vkDestroyFence                      DestroyFence;
   PFN_vkWaitForFences                     WaitForFences;
   PFN_vkResetFences                       ResetFences;
   PFN_vkGetFenceStatus                    GetFenceStatus;
   /* External-memory entry points */
   PFN_vkGetMemoryFdKHR                    GetMemoryFdKHR;
   PFN_vkGetSemaphoreFdKHR                 GetSemaphoreFdKHR;
   PFN_vkImportSemaphoreFdKHR              ImportSemaphoreFdKHR;
   /* Real ICD swapchain entry points (used in pass-through mode) */
   PFN_vkCreateSwapchainKHR                RealCreateSwapchainKHR;
   PFN_vkDestroySwapchainKHR               RealDestroySwapchainKHR;
   PFN_vkGetSwapchainImagesKHR             RealGetSwapchainImagesKHR;
   PFN_vkAcquireNextImageKHR               RealAcquireNextImageKHR;
   PFN_vkQueuePresentKHR                   RealQueuePresentKHR;
} device_data_t;

typedef struct {
   VkSurfaceKHR     handle;
   xcb_connection_t *xcb;
   xcb_window_t     window;
   bool             dri3_supported;
   bool             present_supported;
} surface_t;

typedef struct {
   VkImage           image;
   VkDeviceMemory    memory;
   int               dmabuf_fd;     /* exported dmabuf */
   xcb_pixmap_t      pixmap;
   bool              busy;          /* true if currently submitted to Present */
   uint32_t          serial;        /* Present serial for completion tracking */
   VkSemaphore       acquire_done;  /* signalled when this image is free again */
   VkFence           idle_fence;    /* CPU-signalled when idle */
} swap_image_t;

typedef struct {
   device_data_t   *device;
   surface_t       *surface;
   uint32_t         image_count;
   uint32_t         width;
   uint32_t         height;
   VkFormat         format;
   uint32_t         depth;
   uint32_t         bpp;
   uint32_t         stride;
   swap_image_t     images[MAX_SWAPCHAIN_IMAGES];
   uint32_t         next_serial;
   uint32_t         last_completed_serial;
   uint64_t         target_msc;
   xcb_special_event_t *special_event;
   uint32_t         eid;
   pthread_mutex_t  lock;
   /* Configuration */
   bool             vsync_enabled;   /* swap_interval > 0 */
} swapchain_t;

/* ============================================================ *
 *  Hash maps — keyed by Vulkan handle for dispatch lookup       *
 * ============================================================ */

#define HASH_BUCKETS 128
typedef struct hash_entry {
   void               *key;
   void               *value;
   struct hash_entry  *next;
} hash_entry_t;

static hash_entry_t   *g_instance_map[HASH_BUCKETS];
static hash_entry_t   *g_device_map[HASH_BUCKETS];
static hash_entry_t   *g_surface_map[HASH_BUCKETS];
static hash_entry_t   *g_swapchain_map[HASH_BUCKETS];
static pthread_mutex_t g_hash_lock = PTHREAD_MUTEX_INITIALIZER;

static unsigned hash_ptr(void *p)
{
   uintptr_t v = (uintptr_t)p;
   v = (v ^ (v >> 16)) * 0x45d9f3bU;
   v = (v ^ (v >> 16)) * 0x45d9f3bU;
   return (v ^ (v >> 16)) % HASH_BUCKETS;
}

static void *hash_get(hash_entry_t **map, void *key)
{
   pthread_mutex_lock(&g_hash_lock);
   hash_entry_t *e = map[hash_ptr(key)];
   for (; e; e = e->next)
      if (e->key == key) {
         void *v = e->value;
         pthread_mutex_unlock(&g_hash_lock);
         return v;
      }
   pthread_mutex_unlock(&g_hash_lock);
   return NULL;
}

static void hash_put(hash_entry_t **map, void *key, void *value)
{
   hash_entry_t *e = calloc(1, sizeof(*e));
   if (!e) return;
   e->key = key;
   e->value = value;
   pthread_mutex_lock(&g_hash_lock);
   unsigned b = hash_ptr(key);
   e->next = map[b];
   map[b] = e;
   pthread_mutex_unlock(&g_hash_lock);
}

static void *hash_remove(hash_entry_t **map, void *key)
{
   pthread_mutex_lock(&g_hash_lock);
   hash_entry_t **slot = &map[hash_ptr(key)];
   while (*slot) {
      if ((*slot)->key == key) {
         hash_entry_t *e = *slot;
         void *v = e->value;
         *slot = e->next;
         free(e);
         pthread_mutex_unlock(&g_hash_lock);
         return v;
      }
      slot = &(*slot)->next;
   }
   pthread_mutex_unlock(&g_hash_lock);
   return NULL;
}

/* ============================================================ *
 *  Tegra detection                                              *
 * ============================================================ */

static bool is_tegra_gpu(instance_data_t *id, VkPhysicalDevice pd)
{
   VkPhysicalDeviceProperties props;
   id->GetPhysicalDeviceProperties(pd, &props);

   /* Force-enable / force-disable via env */
   const char *force = getenv("TEGRA_X11_PRESENT_FORCE");
   if (force && force[0] == '1') return true;
   if (force && force[0] == '0') return false;

   if (props.vendorID != 0x10DE)
      return false;
   if (strstr(props.deviceName, "Tegra") || strstr(props.deviceName, "tegra"))
      return true;
   /* Tegra integrated GPUs report INTEGRATED_GPU type with vendor 0x10DE */
   if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)
      return true;
   return false;
}

/* ============================================================ *
 *  X11 / DRI3 / Present helpers                                 *
 * ============================================================ */

static bool surface_init_x11(surface_t *s, xcb_connection_t *xcb,
      xcb_window_t window)
{
   s->xcb = xcb;
   s->window = window;

   /* Probe DRI3 */
   const xcb_query_extension_reply_t *dri3_ext =
      xcb_get_extension_data(xcb, &xcb_dri3_id);
   if (!dri3_ext || !dri3_ext->present) {
      layer_log("DRI3 extension not present on X server");
      return false;
   }
   s->dri3_supported = true;

   /* Probe Present */
   const xcb_query_extension_reply_t *present_ext =
      xcb_get_extension_data(xcb, &xcb_present_id);
   if (!present_ext || !present_ext->present) {
      layer_log("Present extension not present on X server");
      return false;
   }
   s->present_supported = true;

   layer_log("X11 surface ready: DRI3=yes Present=yes window=0x%x", window);
   return true;
}

/* Choose a depth/bpp pair matching a Vulkan format that we can import
 * via DRI3.  We support the two formats RetroArch / DXVK actually use. */
static bool format_to_x_depth(VkFormat fmt, uint32_t *depth, uint32_t *bpp)
{
   switch (fmt) {
      case VK_FORMAT_B8G8R8A8_UNORM:
      case VK_FORMAT_B8G8R8A8_SRGB:
      case VK_FORMAT_R8G8B8A8_UNORM:
      case VK_FORMAT_R8G8B8A8_SRGB:
         *depth = 24; *bpp = 32;
         return true;
      case VK_FORMAT_R5G6B5_UNORM_PACK16:
         *depth = 16; *bpp = 16;
         return true;
      default:
         return false;
   }
   return false;
}

/* ============================================================ *
 *  Swapchain implementation                                     *
 * ============================================================ */

static int find_memory_type(instance_data_t *id, VkPhysicalDevice pd,
      uint32_t type_bits, VkMemoryPropertyFlags props)
{
   VkPhysicalDeviceMemoryProperties mem;
   id->GetPhysicalDeviceMemoryProperties(pd, &mem);
   for (uint32_t i = 0; i < mem.memoryTypeCount; i++) {
      if ((type_bits & (1u << i)) &&
          (mem.memoryTypes[i].propertyFlags & props) == props)
         return (int)i;
   }
   return -1;
}

static VkResult create_swap_image(swapchain_t *sc, swap_image_t *img,
      const VkSwapchainCreateInfoKHR *ci)
{
   device_data_t *dd = sc->device;

   /* 1. Create VkImage with external-memory export bit */
   VkExternalMemoryImageCreateInfo ext_ci = {
      .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
   };
   VkImageCreateInfo img_ci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .pNext = &ext_ci,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = ci->imageFormat,
      .extent = { ci->imageExtent.width, ci->imageExtent.height, 1 },
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = ci->imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   VkResult r = dd->CreateImage(dd->device, &img_ci, NULL, &img->image);
   if (r != VK_SUCCESS) {
      layer_log("CreateImage failed: %d", r);
      return r;
   }

   /* 2. Allocate exportable memory */
   VkMemoryRequirements req;
   dd->GetImageMemoryRequirements(dd->device, img->image, &req);

   int mt = find_memory_type(dd->instance, dd->physical_device,
         req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
   if (mt < 0) {
      layer_log("No suitable memory type for swap image");
      dd->DestroyImage(dd->device, img->image, NULL);
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   }

   VkExportMemoryAllocateInfo export_ai = {
      .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
   };
   VkMemoryAllocateInfo ai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = &export_ai,
      .allocationSize = req.size,
      .memoryTypeIndex = (uint32_t)mt,
   };
   r = dd->AllocateMemory(dd->device, &ai, NULL, &img->memory);
   if (r != VK_SUCCESS) {
      layer_log("AllocateMemory failed: %d", r);
      dd->DestroyImage(dd->device, img->image, NULL);
      return r;
   }

   r = dd->BindImageMemory(dd->device, img->image, img->memory, 0);
   if (r != VK_SUCCESS) {
      layer_log("BindImageMemory failed: %d", r);
      dd->FreeMemory(dd->device, img->memory, NULL);
      dd->DestroyImage(dd->device, img->image, NULL);
      return r;
   }

   /* 3. Export memory as dmabuf fd */
   VkMemoryGetFdInfoKHR fd_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
      .memory = img->memory,
      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
   };
   r = dd->GetMemoryFdKHR(dd->device, &fd_info, &img->dmabuf_fd);
   if (r != VK_SUCCESS) {
      layer_log("GetMemoryFdKHR failed: %d", r);
      dd->FreeMemory(dd->device, img->memory, NULL);
      dd->DestroyImage(dd->device, img->image, NULL);
      return r;
   }

   /* 4. Import dmabuf as X11 Pixmap via DRI3.
    *    The fd is consumed by the X server; we duplicate it first so we
    *    keep ownership of our copy for any later operation. */
   int fd_to_x = fcntl(img->dmabuf_fd, F_DUPFD_CLOEXEC, 3);
   if (fd_to_x < 0) {
      layer_log("dup of dmabuf fd failed: %s", strerror(errno));
      close(img->dmabuf_fd);
      dd->FreeMemory(dd->device, img->memory, NULL);
      dd->DestroyImage(dd->device, img->image, NULL);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   img->pixmap = xcb_generate_id(sc->surface->xcb);
   xcb_void_cookie_t cookie = xcb_dri3_pixmap_from_buffer_checked(
         sc->surface->xcb,
         img->pixmap,
         sc->surface->window,
         req.size,
         sc->width,
         sc->height,
         sc->stride,
         sc->depth,
         sc->bpp,
         fd_to_x);

   xcb_generic_error_t *err = xcb_request_check(sc->surface->xcb, cookie);
   if (err) {
      layer_log("dri3_pixmap_from_buffer failed: error_code=%d", err->error_code);
      free(err);
      close(img->dmabuf_fd);
      dd->FreeMemory(dd->device, img->memory, NULL);
      dd->DestroyImage(dd->device, img->image, NULL);
      return VK_ERROR_NATIVE_WINDOW_IN_USE_KHR;
   }

   /* 5. Per-image idle fence — initially signalled (image starts idle) */
   VkFenceCreateInfo fci = {
      .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
      .flags = VK_FENCE_CREATE_SIGNALED_BIT,
   };
   r = dd->CreateFence(dd->device, &fci, NULL, &img->idle_fence);
   if (r != VK_SUCCESS) {
      layer_log("CreateFence failed: %d", r);
      /* Still usable, just less efficient acquire */
      img->idle_fence = VK_NULL_HANDLE;
   }

   img->busy = false;
   img->serial = 0;
   return VK_SUCCESS;
}

static void destroy_swap_image(swapchain_t *sc, swap_image_t *img)
{
   device_data_t *dd = sc->device;
   if (img->idle_fence)
      dd->DestroyFence(dd->device, img->idle_fence, NULL);
   if (img->pixmap)
      xcb_free_pixmap(sc->surface->xcb, img->pixmap);
   if (img->dmabuf_fd >= 0)
      close(img->dmabuf_fd);
   if (img->memory)
      dd->FreeMemory(dd->device, img->memory, NULL);
   if (img->image)
      dd->DestroyImage(dd->device, img->image, NULL);
   memset(img, 0, sizeof(*img));
   img->dmabuf_fd = -1;
}

/* ============================================================ *
 *  Intercepted entry points                                     *
 * ============================================================ */

static VKAPI_ATTR VkResult VKAPI_CALL
layer_CreateSwapchainKHR(VkDevice device,
      const VkSwapchainCreateInfoKHR *pCreateInfo,
      const VkAllocationCallbacks *pAllocator,
      VkSwapchainKHR *pSwapchain)
{
   device_data_t *dd = hash_get(g_device_map, device);
   if (!dd)
      return VK_ERROR_INITIALIZATION_FAILED;

   surface_t *surf = hash_get(g_surface_map, (void*)(uintptr_t)pCreateInfo->surface);
   if (!surf || !surf->dri3_supported || !surf->present_supported) {
      /* Surface was created by another path, fall through to ICD */
      return dd->RealCreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);
   }

   swapchain_t *sc = calloc(1, sizeof(*sc));
   if (!sc)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   sc->device  = dd;
   sc->surface = surf;
   sc->width   = pCreateInfo->imageExtent.width;
   sc->height  = pCreateInfo->imageExtent.height;
   sc->format  = pCreateInfo->imageFormat;
   sc->vsync_enabled =
      (pCreateInfo->presentMode == VK_PRESENT_MODE_FIFO_KHR ||
       pCreateInfo->presentMode == VK_PRESENT_MODE_FIFO_RELAXED_KHR ||
       pCreateInfo->presentMode == VK_PRESENT_MODE_MAILBOX_KHR);

   if (!format_to_x_depth(sc->format, &sc->depth, &sc->bpp)) {
      layer_log("Unsupported format %d, falling through", sc->format);
      free(sc);
      return dd->RealCreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);
   }
   sc->stride = sc->width * (sc->bpp / 8);

   /* Image count: respect minImageCount, clamp to MAX_SWAPCHAIN_IMAGES */
   uint32_t requested = pCreateInfo->minImageCount;
   if (requested < 2) requested = 2;
   if (requested > MAX_SWAPCHAIN_IMAGES) requested = MAX_SWAPCHAIN_IMAGES;
   sc->image_count = requested;

   pthread_mutex_init(&sc->lock, NULL);

   /* Register Present complete event subscription */
   sc->eid = xcb_generate_id(surf->xcb);
   xcb_present_select_input(surf->xcb, sc->eid, surf->window,
         XCB_PRESENT_EVENT_MASK_COMPLETE_NOTIFY |
         XCB_PRESENT_EVENT_MASK_IDLE_NOTIFY);
   sc->special_event = xcb_register_for_special_xge(
         surf->xcb, &xcb_present_id, sc->eid, NULL);

   /* Create images */
   for (uint32_t i = 0; i < sc->image_count; i++) {
      sc->images[i].dmabuf_fd = -1;
      VkResult r = create_swap_image(sc, &sc->images[i], pCreateInfo);
      if (r != VK_SUCCESS) {
         /* Cleanup partial */
         for (uint32_t j = 0; j < i; j++)
            destroy_swap_image(sc, &sc->images[j]);
         if (sc->special_event)
            xcb_unregister_for_special_event(surf->xcb, sc->special_event);
         pthread_mutex_destroy(&sc->lock);
         free(sc);
         return r;
      }
   }

   /* Encode the swapchain pointer in a fake VkSwapchainKHR handle.
    * The handle must be unique per-instance but is opaque to the app. */
   *pSwapchain = (VkSwapchainKHR)(uintptr_t)sc;
   hash_put(g_swapchain_map, *pSwapchain, sc);

   layer_log("Created shim swapchain %p: %dx%d %d images vsync=%d",
         (void*)sc, sc->width, sc->height, sc->image_count, sc->vsync_enabled);
   return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL
layer_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
      const VkAllocationCallbacks *pAllocator)
{
   swapchain_t *sc = hash_remove(g_swapchain_map, swapchain);
   if (!sc) {
      device_data_t *dd = hash_get(g_device_map, device);
      if (dd) dd->RealDestroySwapchainKHR(device, swapchain, pAllocator);
      return;
   }
   sc->device->DeviceWaitIdle(device);
   for (uint32_t i = 0; i < sc->image_count; i++)
      destroy_swap_image(sc, &sc->images[i]);
   if (sc->special_event)
      xcb_unregister_for_special_event(sc->surface->xcb, sc->special_event);
   pthread_mutex_destroy(&sc->lock);
   free(sc);
}

static VKAPI_ATTR VkResult VKAPI_CALL
layer_GetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain,
      uint32_t *pCount, VkImage *pImages)
{
   swapchain_t *sc = hash_get(g_swapchain_map, swapchain);
   if (!sc) {
      device_data_t *dd = hash_get(g_device_map, device);
      if (dd) return dd->RealGetSwapchainImagesKHR(device, swapchain, pCount, pImages);
      return VK_ERROR_INITIALIZATION_FAILED;
   }
   if (!pImages) {
      *pCount = sc->image_count;
      return VK_SUCCESS;
   }
   uint32_t n = (*pCount < sc->image_count) ? *pCount : sc->image_count;
   for (uint32_t i = 0; i < n; i++)
      pImages[i] = sc->images[i].image;
   *pCount = n;
   return (n < sc->image_count) ? VK_INCOMPLETE : VK_SUCCESS;
}

/* Drain Present events to update which images are idle */
static void drain_present_events(swapchain_t *sc)
{
   if (!sc->special_event) return;
   xcb_generic_event_t *ev;
   while ((ev = xcb_poll_for_special_event(sc->surface->xcb, sc->special_event))) {
      xcb_present_generic_event_t *ge = (xcb_present_generic_event_t*)ev;
      switch (ge->evtype) {
         case XCB_PRESENT_EVENT_IDLE_NOTIFY: {
            xcb_present_idle_notify_event_t *ie = (xcb_present_idle_notify_event_t*)ev;
            for (uint32_t i = 0; i < sc->image_count; i++) {
               if (sc->images[i].pixmap == ie->pixmap) {
                  sc->images[i].busy = false;
                  break;
               }
            }
            break;
         }
         case XCB_PRESENT_EVENT_COMPLETE_NOTIFY: {
            xcb_present_complete_notify_event_t *ce = (xcb_present_complete_notify_event_t*)ev;
            sc->last_completed_serial = ce->serial;
            sc->target_msc = ce->msc;
            break;
         }
         default: break;
      }
      free(ev);
   }
}

static VKAPI_ATTR VkResult VKAPI_CALL
layer_AcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain,
      uint64_t timeout, VkSemaphore semaphore, VkFence fence,
      uint32_t *pImageIndex)
{
   swapchain_t *sc = hash_get(g_swapchain_map, swapchain);
   if (!sc) {
      device_data_t *dd = hash_get(g_device_map, device);
      if (dd)
         return dd->RealAcquireNextImageKHR(device, swapchain, timeout,
               semaphore, fence, pImageIndex);
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   pthread_mutex_lock(&sc->lock);

   /* Drain any pending IDLE events so we know which images are free */
   drain_present_events(sc);

   uint32_t found = UINT32_MAX;
   for (uint32_t tries = 0; tries < 4 && found == UINT32_MAX; tries++) {
      for (uint32_t i = 0; i < sc->image_count; i++) {
         if (!sc->images[i].busy) {
            found = i;
            break;
         }
      }
      if (found == UINT32_MAX) {
         /* Wait briefly for an IDLE event then retry */
         xcb_flush(sc->surface->xcb);
         xcb_generic_event_t *ev = xcb_wait_for_special_event(
               sc->surface->xcb, sc->special_event);
         if (!ev) break;
         /* Process inline */
         xcb_present_generic_event_t *ge = (xcb_present_generic_event_t*)ev;
         if (ge->evtype == XCB_PRESENT_EVENT_IDLE_NOTIFY) {
            xcb_present_idle_notify_event_t *ie = (xcb_present_idle_notify_event_t*)ev;
            for (uint32_t i = 0; i < sc->image_count; i++) {
               if (sc->images[i].pixmap == ie->pixmap) {
                  sc->images[i].busy = false;
                  break;
               }
            }
         } else if (ge->evtype == XCB_PRESENT_EVENT_COMPLETE_NOTIFY) {
            xcb_present_complete_notify_event_t *ce = (xcb_present_complete_notify_event_t*)ev;
            sc->last_completed_serial = ce->serial;
            sc->target_msc = ce->msc;
         }
         free(ev);
      }
   }

   if (found == UINT32_MAX) {
      pthread_mutex_unlock(&sc->lock);
      return VK_TIMEOUT;
   }

   *pImageIndex = found;
   sc->images[found].busy = true;
   pthread_mutex_unlock(&sc->lock);

   /* Signal the caller's semaphore/fence immediately:
    * the image is genuinely free at this point (X server has released it),
    * so there is no GPU work to wait on.  We use a queue submit with no
    * commands to signal the semaphore. */
   if (semaphore != VK_NULL_HANDLE || fence != VK_NULL_HANDLE) {
      VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO };
      if (semaphore != VK_NULL_HANDLE) {
         si.signalSemaphoreCount = 1;
         si.pSignalSemaphores = &semaphore;
      }
      VkQueue q;
      sc->device->GetDeviceQueue(device, sc->device->graphics_queue_family, 0, &q);
      sc->device->QueueSubmit(q, 1, &si, fence);
   }
   return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL
layer_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pPresentInfo)
{
   VkResult overall = VK_SUCCESS;

   for (uint32_t i = 0; i < pPresentInfo->swapchainCount; i++) {
      swapchain_t *sc = hash_get(g_swapchain_map, pPresentInfo->pSwapchains[i]);
      if (!sc) {
         /* Fall through to ICD per-swapchain — find device */
         /* Construct a single-swapchain present info */
         VkPresentInfoKHR one = *pPresentInfo;
         one.swapchainCount = 1;
         one.pSwapchains = &pPresentInfo->pSwapchains[i];
         one.pImageIndices = &pPresentInfo->pImageIndices[i];
         one.waitSemaphoreCount = (i == 0) ? pPresentInfo->waitSemaphoreCount : 0;
         /* Find any device — only one in the typical case */
         for (uint32_t b = 0; b < HASH_BUCKETS; b++) {
            for (hash_entry_t *e = g_device_map[b]; e; e = e->next) {
               device_data_t *dd = e->value;
               if (dd && dd->RealQueuePresentKHR) {
                  dd->RealQueuePresentKHR(queue, &one);
                  goto next_sc;
               }
            }
         }
next_sc:
         continue;
      }

      uint32_t idx = pPresentInfo->pImageIndices[i];
      if (idx >= sc->image_count) {
         overall = VK_ERROR_OUT_OF_DATE_KHR;
         continue;
      }

      pthread_mutex_lock(&sc->lock);

      /* Wait for caller's wait-semaphores by submitting a no-op signal
       * onto the queue.  We do not consume the semaphores ourselves since
       * we are not reading the image on the GPU side — but we must wait
       * for the GPU to finish writing to it before X reads the dmabuf.
       * The simplest correct behaviour is QueueWaitIdle on the present
       * queue, which is what most apps already expect from FIFO present. */
      if (pPresentInfo->waitSemaphoreCount > 0 && i == 0) {
         VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO };
         VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
         /* Use the wait-stages parameter only on the first iteration */
         VkPipelineStageFlags *stages = malloc(sizeof(*stages) *
               pPresentInfo->waitSemaphoreCount);
         for (uint32_t s = 0; s < pPresentInfo->waitSemaphoreCount; s++)
            stages[s] = stage;
         si.waitSemaphoreCount = pPresentInfo->waitSemaphoreCount;
         si.pWaitSemaphores = pPresentInfo->pWaitSemaphores;
         si.pWaitDstStageMask = stages;
         sc->device->QueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
         free(stages);
      }
      /* Ensure GPU work for this image is complete before X reads dmabuf */
      sc->device->QueueWaitIdle(queue);

      sc->next_serial++;
      sc->images[idx].serial = sc->next_serial;
      sc->images[idx].busy = true;

      uint32_t options = 0;
      if (!sc->vsync_enabled)
         options |= XCB_PRESENT_OPTION_ASYNC;

      /* target_msc = 0 means "next vblank" when vsync is on.
       * For mailbox/immediate, ASYNC option lets the server present
       * immediately without waiting for vblank. */
      xcb_void_cookie_t cookie = xcb_present_pixmap_checked(
            sc->surface->xcb,
            sc->surface->window,
            sc->images[idx].pixmap,
            sc->next_serial,
            0,                       /* valid-area: 0 = whole pixmap */
            0,                       /* update-area: 0 = whole pixmap */
            0, 0,                    /* x_off, y_off */
            XCB_NONE,                /* target_crtc: 0 = window's CRTC */
            XCB_NONE,                /* wait fence: none */
            XCB_NONE,                /* idle fence: none */
            options,
            0,                       /* target_msc: 0 = next vblank */
            0,                       /* divisor */
            0,                       /* remainder */
            0, NULL);                /* notifies */
      xcb_generic_error_t *err = xcb_request_check(sc->surface->xcb, cookie);
      if (err) {
         layer_log("present_pixmap failed: error_code=%d", err->error_code);
         free(err);
         sc->images[idx].busy = false;
         overall = VK_ERROR_OUT_OF_DATE_KHR;
      }
      xcb_flush(sc->surface->xcb);

      pthread_mutex_unlock(&sc->lock);
   }

   return overall;
}

/* ============================================================ *
 *  Surface creation interception                                *
 * ============================================================ */

static VKAPI_ATTR VkResult VKAPI_CALL
layer_CreateXcbSurfaceKHR(VkInstance instance,
      const VkXcbSurfaceCreateInfoKHR *pCreateInfo,
      const VkAllocationCallbacks *pAllocator,
      VkSurfaceKHR *pSurface)
{
   instance_data_t *id = hash_get(g_instance_map, instance);
   if (!id || !id->CreateXcbSurfaceKHR)
      return VK_ERROR_INITIALIZATION_FAILED;

   /* Always create the real surface so the ICD has a valid handle even
    * if we shim the swapchain. */
   VkResult r = id->CreateXcbSurfaceKHR(instance, pCreateInfo, pAllocator, pSurface);
   if (r != VK_SUCCESS)
      return r;

   surface_t *s = calloc(1, sizeof(*s));
   if (!s) return VK_SUCCESS;  /* leave the real surface usable, no shim */
   s->handle = *pSurface;
   if (surface_init_x11(s, pCreateInfo->connection, pCreateInfo->window))
      hash_put(g_surface_map, (void*)(uintptr_t)*pSurface, s);
   else
      free(s);

   return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL
layer_CreateXlibSurfaceKHR(VkInstance instance,
      const VkXlibSurfaceCreateInfoKHR *pCreateInfo,
      const VkAllocationCallbacks *pAllocator,
      VkSurfaceKHR *pSurface)
{
   instance_data_t *id = hash_get(g_instance_map, instance);
   if (!id || !id->CreateXlibSurfaceKHR)
      return VK_ERROR_INITIALIZATION_FAILED;

   VkResult r = id->CreateXlibSurfaceKHR(instance, pCreateInfo, pAllocator, pSurface);
   if (r != VK_SUCCESS)
      return r;

   surface_t *s = calloc(1, sizeof(*s));
   if (!s) return VK_SUCCESS;
   s->handle = *pSurface;
   /* Convert Xlib display to xcb connection */
   xcb_connection_t *xcb = XGetXCBConnection(pCreateInfo->dpy);
   if (xcb && surface_init_x11(s, xcb, (xcb_window_t)pCreateInfo->window))
      hash_put(g_surface_map, (void*)(uintptr_t)*pSurface, s);
   else
      free(s);

   return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL
layer_DestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface,
      const VkAllocationCallbacks *pAllocator)
{
   instance_data_t *id = hash_get(g_instance_map, instance);
   surface_t *s = hash_remove(g_surface_map, (void*)(uintptr_t)surface);
   if (s) free(s);
   if (id && id->DestroySurfaceKHR)
      id->DestroySurfaceKHR(instance, surface, pAllocator);
}

/* ============================================================ *
 *  Instance/Device creation — set up our dispatch tables        *
 * ============================================================ */

static VKAPI_ATTR VkResult VKAPI_CALL
layer_CreateInstance(const VkInstanceCreateInfo *pCreateInfo,
      const VkAllocationCallbacks *pAllocator, VkInstance *pInstance)
{
   VkLayerInstanceCreateInfo *lci = (VkLayerInstanceCreateInfo*)pCreateInfo->pNext;
   while (lci && (lci->sType != VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO ||
                  lci->function != VK_LAYER_LINK_INFO))
      lci = (VkLayerInstanceCreateInfo*)lci->pNext;
   if (!lci) return VK_ERROR_INITIALIZATION_FAILED;

   PFN_vkGetInstanceProcAddr next_gipa =
      lci->u.pLayerInfo->pfnNextGetInstanceProcAddr;
   lci->u.pLayerInfo = lci->u.pLayerInfo->pNext;

   PFN_vkCreateInstance next_create =
      (PFN_vkCreateInstance)next_gipa(NULL, "vkCreateInstance");
   if (!next_create) return VK_ERROR_INITIALIZATION_FAILED;

   VkResult r = next_create(pCreateInfo, pAllocator, pInstance);
   if (r != VK_SUCCESS) return r;

   instance_data_t *id = calloc(1, sizeof(*id));
   if (!id) return VK_ERROR_OUT_OF_HOST_MEMORY;
   id->instance = *pInstance;
   id->GetInstanceProcAddr = next_gipa;

   #define LOAD_I(name) id->name = (PFN_vk##name)next_gipa(*pInstance, "vk" #name)
   LOAD_I(DestroyInstance);
   LOAD_I(EnumeratePhysicalDevices);
   LOAD_I(GetPhysicalDeviceProperties);
   LOAD_I(GetPhysicalDeviceQueueFamilyProperties);
   LOAD_I(GetPhysicalDeviceMemoryProperties);
   LOAD_I(CreateDevice);
   LOAD_I(DestroySurfaceKHR);
   LOAD_I(GetPhysicalDeviceSurfaceSupportKHR);
   LOAD_I(GetPhysicalDeviceSurfaceCapabilitiesKHR);
   LOAD_I(GetPhysicalDeviceSurfaceFormatsKHR);
   LOAD_I(GetPhysicalDeviceSurfacePresentModesKHR);
   LOAD_I(CreateXcbSurfaceKHR);
   LOAD_I(CreateXlibSurfaceKHR);
   #undef LOAD_I

   hash_put(g_instance_map, *pInstance, id);
   layer_log("Instance created");
   return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL
layer_DestroyInstance(VkInstance instance, const VkAllocationCallbacks *pAllocator)
{
   instance_data_t *id = hash_remove(g_instance_map, instance);
   if (id) {
      id->DestroyInstance(instance, pAllocator);
      free(id);
   }
}

static VKAPI_ATTR VkResult VKAPI_CALL
layer_CreateDevice(VkPhysicalDevice physicalDevice,
      const VkDeviceCreateInfo *pCreateInfo,
      const VkAllocationCallbacks *pAllocator, VkDevice *pDevice)
{
   VkLayerDeviceCreateInfo *lci = (VkLayerDeviceCreateInfo*)pCreateInfo->pNext;
   while (lci && (lci->sType != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO ||
                  lci->function != VK_LAYER_LINK_INFO))
      lci = (VkLayerDeviceCreateInfo*)lci->pNext;
   if (!lci) return VK_ERROR_INITIALIZATION_FAILED;

   PFN_vkGetInstanceProcAddr next_gipa =
      lci->u.pLayerInfo->pfnNextGetInstanceProcAddr;
   PFN_vkGetDeviceProcAddr   next_gdpa =
      lci->u.pLayerInfo->pfnNextGetDeviceProcAddr;
   lci->u.pLayerInfo = lci->u.pLayerInfo->pNext;

   /* Find owning instance */
   instance_data_t *id = NULL;
   for (uint32_t b = 0; b < HASH_BUCKETS && !id; b++)
      for (hash_entry_t *e = g_instance_map[b]; e; e = e->next) {
         instance_data_t *cand = e->value;
         /* Just take the first instance — typical apps have one */
         id = cand; break;
      }

   /* If not Tegra, this layer is a pass-through. Still need to chain through
    * for correctness, but don't install our shim. */
   bool tegra = id ? is_tegra_gpu(id, physicalDevice) : false;

   /* Ensure the chain knows we want VK_KHR_external_memory_fd etc.
    * The app's pCreateInfo already lists VK_KHR_swapchain.  We need to
    * add our extensions if missing. */
   const char *needed[] = {
      "VK_KHR_external_memory",
      "VK_KHR_external_memory_fd",
      "VK_KHR_external_semaphore",
      "VK_KHR_external_semaphore_fd",
   };
   #define NEEDED_COUNT (sizeof(needed)/sizeof(needed[0]))

   const char **merged = malloc(sizeof(char*) *
         (pCreateInfo->enabledExtensionCount + NEEDED_COUNT));
   uint32_t merged_count = 0;
   for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; i++)
      merged[merged_count++] = pCreateInfo->ppEnabledExtensionNames[i];
   if (tegra) {
      for (uint32_t i = 0; i < NEEDED_COUNT; i++) {
         bool dup = false;
         for (uint32_t j = 0; j < pCreateInfo->enabledExtensionCount; j++)
            if (!strcmp(pCreateInfo->ppEnabledExtensionNames[j], needed[i])) {
               dup = true; break;
            }
         if (!dup) merged[merged_count++] = needed[i];
      }
   }

   VkDeviceCreateInfo modified = *pCreateInfo;
   modified.enabledExtensionCount   = merged_count;
   modified.ppEnabledExtensionNames = merged;

   PFN_vkCreateDevice next_create =
      (PFN_vkCreateDevice)next_gipa(id ? id->instance : NULL, "vkCreateDevice");
   VkResult r = next_create(physicalDevice, &modified, pAllocator, pDevice);
   free(merged);
   if (r != VK_SUCCESS) return r;

   if (!tegra) {
      layer_log("Non-Tegra GPU, layer is pass-through");
      return VK_SUCCESS;
   }

   device_data_t *dd = calloc(1, sizeof(*dd));
   if (!dd) { return VK_SUCCESS; }
   dd->device = *pDevice;
   dd->physical_device = physicalDevice;
   dd->instance = id;
   dd->GetDeviceProcAddr = next_gdpa;

   /* First graphics queue family — typical apps create only this one */
   uint32_t qf_count = 0;
   id->GetPhysicalDeviceQueueFamilyProperties(physicalDevice, &qf_count, NULL);
   VkQueueFamilyProperties *qfs = calloc(qf_count, sizeof(*qfs));
   id->GetPhysicalDeviceQueueFamilyProperties(physicalDevice, &qf_count, qfs);
   dd->graphics_queue_family = 0;
   for (uint32_t i = 0; i < qf_count; i++)
      if (qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
         dd->graphics_queue_family = i; break;
      }
   free(qfs);

   #define LOAD_D(name) dd->name = (PFN_vk##name)next_gdpa(*pDevice, "vk" #name)
   LOAD_D(DestroyDevice);
   LOAD_D(GetDeviceQueue);
   LOAD_D(QueueSubmit);
   LOAD_D(DeviceWaitIdle);
   LOAD_D(QueueWaitIdle);
   LOAD_D(CreateImage);
   LOAD_D(DestroyImage);
   LOAD_D(GetImageMemoryRequirements);
   LOAD_D(AllocateMemory);
   LOAD_D(FreeMemory);
   LOAD_D(BindImageMemory);
   LOAD_D(CreateSemaphore);
   LOAD_D(DestroySemaphore);
   LOAD_D(CreateFence);
   LOAD_D(DestroyFence);
   LOAD_D(WaitForFences);
   LOAD_D(ResetFences);
   LOAD_D(GetFenceStatus);
   LOAD_D(GetMemoryFdKHR);
   LOAD_D(GetSemaphoreFdKHR);
   LOAD_D(ImportSemaphoreFdKHR);
   #undef LOAD_D

   /* Save real swapchain entry points for fall-through and DestroySwapchain */
   dd->RealCreateSwapchainKHR = (PFN_vkCreateSwapchainKHR)
      next_gdpa(*pDevice, "vkCreateSwapchainKHR");
   dd->RealDestroySwapchainKHR = (PFN_vkDestroySwapchainKHR)
      next_gdpa(*pDevice, "vkDestroySwapchainKHR");
   dd->RealGetSwapchainImagesKHR = (PFN_vkGetSwapchainImagesKHR)
      next_gdpa(*pDevice, "vkGetSwapchainImagesKHR");
   dd->RealAcquireNextImageKHR = (PFN_vkAcquireNextImageKHR)
      next_gdpa(*pDevice, "vkAcquireNextImageKHR");
   dd->RealQueuePresentKHR = (PFN_vkQueuePresentKHR)
      next_gdpa(*pDevice, "vkQueuePresentKHR");

   if (!dd->GetMemoryFdKHR) {
      layer_log("ICD does not export VK_KHR_external_memory_fd, "
                "falling back to pass-through");
      free(dd);
      return VK_SUCCESS;
   }

   hash_put(g_device_map, *pDevice, dd);
   layer_log("Device created on Tegra GPU, shim active");
   return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL
layer_DestroyDevice(VkDevice device, const VkAllocationCallbacks *pAllocator)
{
   device_data_t *dd = hash_remove(g_device_map, device);
   if (dd) {
      dd->DestroyDevice(device, pAllocator);
      free(dd);
   }
}

/* ============================================================ *
 *  GetProcAddr — entry point dispatch for the loader            *
 * ============================================================ */

#define IF_HOOK(name) if (!strcmp(pName, "vk" #name)) return (PFN_vkVoidFunction)layer_##name

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
layer_GetDeviceProcAddr(VkDevice device, const char *pName)
{
   IF_HOOK(GetDeviceProcAddr);
   IF_HOOK(DestroyDevice);
   IF_HOOK(CreateSwapchainKHR);
   IF_HOOK(DestroySwapchainKHR);
   IF_HOOK(GetSwapchainImagesKHR);
   IF_HOOK(AcquireNextImageKHR);
   IF_HOOK(QueuePresentKHR);

   device_data_t *dd = hash_get(g_device_map, device);
   if (dd && dd->GetDeviceProcAddr)
      return dd->GetDeviceProcAddr(device, pName);
   return NULL;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
layer_GetInstanceProcAddr(VkInstance instance, const char *pName)
{
   IF_HOOK(GetInstanceProcAddr);
   IF_HOOK(CreateInstance);
   IF_HOOK(DestroyInstance);
   IF_HOOK(CreateDevice);
   IF_HOOK(GetDeviceProcAddr);
   IF_HOOK(CreateXcbSurfaceKHR);
   IF_HOOK(CreateXlibSurfaceKHR);
   IF_HOOK(DestroySurfaceKHR);
   /* Device-level hooks must be discoverable via GIPA too */
   IF_HOOK(CreateSwapchainKHR);
   IF_HOOK(DestroySwapchainKHR);
   IF_HOOK(GetSwapchainImagesKHR);
   IF_HOOK(AcquireNextImageKHR);
   IF_HOOK(QueuePresentKHR);

   instance_data_t *id = hash_get(g_instance_map, instance);
   if (id && id->GetInstanceProcAddr)
      return id->GetInstanceProcAddr(instance, pName);
   return NULL;
}

#undef IF_HOOK

/* ============================================================ *
 *  Loader negotiation interface                                 *
 * ============================================================ */

VK_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice device, const char *pName)
{
   return layer_GetDeviceProcAddr(device, pName);
}

VK_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char *pName)
{
   return layer_GetInstanceProcAddr(instance, pName);
}

/* Newer loader negotiation entry — optional */
VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *pVersionStruct)
{
   if (pVersionStruct->loaderLayerInterfaceVersion >= 2) {
      pVersionStruct->pfnGetInstanceProcAddr       = vkGetInstanceProcAddr;
      pVersionStruct->pfnGetDeviceProcAddr         = vkGetDeviceProcAddr;
      pVersionStruct->pfnGetPhysicalDeviceProcAddr = NULL;
   }
   if (pVersionStruct->loaderLayerInterfaceVersion > 2)
      pVersionStruct->loaderLayerInterfaceVersion = 2;
   return VK_SUCCESS;
}
