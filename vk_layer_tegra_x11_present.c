/*
 * vk_layer_tegra_x11_present.c
 *
 * Vulkan implicit layer for NVIDIA Tegra L4T r32.x that fixes the broken
 * Vulkan-on-X11 present path by routing presentation through GL/GLX
 * instead of Vulkan WSI.
 *
 * BACKGROUND
 *
 * On Tegra L4T r32.x the Nvidia Vulkan ICD's WSI implementation for X11
 * does not produce vsync-locked presentation. Frames tear. This is a known
 * driver-side issue with no available fix from Nvidia (the BSP is EOL'd).
 *
 * However, the same driver does support:
 *   - Vulkan external memory export via VK_KHR_external_memory_fd
 *     (OPAQUE_FD handle type, OPTIMAL tiling, BGRA8/RGBA8 UNORM/SRGB)
 *   - Vulkan external semaphore export via VK_KHR_external_semaphore_fd
 *     (OPAQUE_FD handle type)
 *   - GL import of both via GL_EXT_memory_object_fd and GL_EXT_semaphore_fd
 *   - Working vsync-locked GL presentation through GLX (glXSwapIntervalEXT(1)
 *     + glXSwapBuffers).
 *
 * This layer plumbs the two together. The application's Vulkan rendering is
 * untouched; we replace the WSI surface and swapchain with our own
 * implementation that:
 *
 *   1. Allocates the swapchain images as OPAQUE_FD-exportable Vulkan images
 *      (the application renders into them as if they were normal swapchain
 *      images).
 *   2. Imports each image into our GL/GLX context as a GL texture.
 *   3. At vkQueuePresentKHR time: bridges the application's render-done
 *      semaphore into a GL semaphore, has GL sample the texture into the
 *      GLX-owned default framebuffer, and calls glXSwapBuffers.
 *   4. Bridges GL's sample-done back into a Vulkan semaphore so that the
 *      next vkAcquireNextImageKHR correctly gates the application's
 *      re-use of the image.
 *
 * No EGL, no dmabuf, no DRM. Only Nvidia's own Vulkan↔GL interop primitives,
 * which are supported on this driver.
 *
 * THREADING
 *
 * GLX contexts are not safe to use from multiple threads without explicit
 * make-current handoffs. We make the context current on whichever thread
 * calls vkQueuePresentKHR, do the GL work, and un-make-current. The cost
 * is a few hundred microseconds per present; negligible at 60Hz.
 *
 * Applications that call vkQueuePresentKHR from different threads on the
 * same swapchain are unusual; we log a warning if we detect it but still
 * function correctly because make-current serializes us.
 *
 * BYPASS
 *
 * Setting the environment variable VK_TEGRA_X11_PRESENT_DISABLE=1 turns
 * the layer into a transparent passthrough. Useful for debugging or for
 * comparing tearing behavior with/without the layer.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>

#define VK_USE_PLATFORM_XLIB_KHR 1
#define VK_USE_PLATFORM_XCB_KHR  1
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include <vulkan/vk_icd.h>

#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/glx.h>
#include <GL/glxext.h>
#include <X11/Xlib.h>
#include <X11/Xlib-xcb.h>
#include <X11/Xatom.h>
#include <xcb/xcb.h>

/* The vendored vk_layer.h used to define VK_LAYER_EXPORT for us, but the
   modern Vulkan-Loader removed it in favor of the layer's own visibility
   control. Define it portably here. With -fvisibility=hidden in the
   Makefile, this attribute is what makes vkGetInstanceProcAddr,
   vkGetDeviceProcAddr, and vkNegotiateLoaderLayerInterfaceVersion the
   only externally visible symbols. */
#if defined(__GNUC__) && (__GNUC__ >= 4)
#  define VK_LAYER_EXPORT __attribute__((visibility("default")))
#else
#  define VK_LAYER_EXPORT
#endif

/* ----------------------------------------------------------------------- */
/* Configuration                                                           */
/* ----------------------------------------------------------------------- */

#define LAYER_NAME            "VK_LAYER_TEGRA_x11_present"
#define LAYER_VERSION         2
#define LAYER_DESC            "Tegra L4T r32.x Vulkan→GL X11 present relay"

/* Clamp image counts to a sane range. */
#define MIN_IMAGES   2
#define MAX_IMAGES   8

/* Default image count if the app requests something outside the range. */
#define DEFAULT_IMAGES 3

/* ----------------------------------------------------------------------- */
/* Logging                                                                 */
/* ----------------------------------------------------------------------- */

static int g_log_level = 1;   /* 0=silent, 1=warn/err, 2=info, 3=debug */
static FILE *g_log_fp = NULL;

static void layer_log_init(void) {
    const char *lvl = getenv("VK_TEGRA_X11_PRESENT_LOG");
    if (lvl) g_log_level = atoi(lvl);
    const char *path = getenv("VK_TEGRA_X11_PRESENT_LOG_FILE");
    if (path) {
        g_log_fp = fopen(path, "a");
        if (g_log_fp) setvbuf(g_log_fp, NULL, _IONBF, 0);
    }
    /* Make stderr unbuffered so log lines survive abnormal exit (assert/abort
       in the application before we'd otherwise flush). */
    setvbuf(stderr, NULL, _IONBF, 0);
}

static void layer_logv(int lvl, const char *prefix, const char *fmt, va_list ap) {
    if (lvl > g_log_level) return;
    char buf[2048];
    int n = snprintf(buf, sizeof(buf), "[" LAYER_NAME " %s] ", prefix);
    if (n > 0 && n < (int)sizeof(buf))
        vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
    fputs(buf, stderr); fputc('\n', stderr);
    if (g_log_fp) { fputs(buf, g_log_fp); fputc('\n', g_log_fp); }
}

#define LOG_ERR(fmt, ...)   do { va_list ap_; (void)ap_; layer_log(1, "ERR",  fmt, ##__VA_ARGS__); } while (0)
#define LOG_WARN(fmt, ...)  do { va_list ap_; (void)ap_; layer_log(1, "WARN", fmt, ##__VA_ARGS__); } while (0)
#define LOG_INFO(fmt, ...)  do { va_list ap_; (void)ap_; layer_log(2, "info", fmt, ##__VA_ARGS__); } while (0)
#define LOG_DBG(fmt, ...)   do { va_list ap_; (void)ap_; layer_log(3, "dbg ", fmt, ##__VA_ARGS__); } while (0)

static void layer_log(int lvl, const char *prefix, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); layer_logv(lvl, prefix, fmt, ap); va_end(ap);
}

/* Log every non-success Vulkan result from a call our layer makes. */
#define VK_CHECK(call) ({ \
    VkResult _r = (call); \
    if (_r != VK_SUCCESS) layer_log(1, "VK", "%s:%d %s -> %d", __func__, __LINE__, #call, _r); \
    _r; \
})

/* ----------------------------------------------------------------------- */
/* Layer enable / disable                                                  */
/* ----------------------------------------------------------------------- */

static bool g_layer_disabled = false;

static void layer_check_disabled(void) {
    const char *d = getenv("VK_TEGRA_X11_PRESENT_DISABLE");
    if (d && d[0] == '1') {
        g_layer_disabled = true;
        LOG_INFO("layer disabled via VK_TEGRA_X11_PRESENT_DISABLE=1 (passthrough)");
    }
}

/* ----------------------------------------------------------------------- */
/* Dispatch tables                                                         */
/* ----------------------------------------------------------------------- */

typedef struct {
    /* From next layer / loader */
    PFN_vkGetInstanceProcAddr               GetInstanceProcAddr;
    PFN_vkDestroyInstance                   DestroyInstance;
    PFN_vkEnumeratePhysicalDevices          EnumeratePhysicalDevices;
    PFN_vkGetPhysicalDeviceProperties       GetPhysicalDeviceProperties;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties;

    /* WSI bits we override or pass through */
    PFN_vkCreateXlibSurfaceKHR              CreateXlibSurfaceKHR;
    PFN_vkCreateXcbSurfaceKHR               CreateXcbSurfaceKHR;
    PFN_vkDestroySurfaceKHR                 DestroySurfaceKHR;
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR        GetPhysicalDeviceSurfaceSupportKHR;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR   GetPhysicalDeviceSurfaceCapabilitiesKHR;
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR        GetPhysicalDeviceSurfaceFormatsKHR;
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR   GetPhysicalDeviceSurfacePresentModesKHR;

    PFN_vkGetPhysicalDeviceImageFormatProperties2 GetPhysicalDeviceImageFormatProperties2;
    PFN_vkGetPhysicalDeviceExternalSemaphoreProperties GetPhysicalDeviceExternalSemaphoreProperties;
} InstanceDispatch;

typedef struct {
    PFN_vkGetDeviceProcAddr      GetDeviceProcAddr;
    PFN_vkDestroyDevice          DestroyDevice;
    PFN_vkDeviceWaitIdle         DeviceWaitIdle;
    PFN_vkGetDeviceQueue         GetDeviceQueue;
    PFN_vkQueueSubmit            QueueSubmit;
    PFN_vkQueueWaitIdle          QueueWaitIdle;

    PFN_vkCreateImage            CreateImage;
    PFN_vkDestroyImage           DestroyImage;
    PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements;
    PFN_vkAllocateMemory         AllocateMemory;
    PFN_vkFreeMemory             FreeMemory;
    PFN_vkBindImageMemory        BindImageMemory;
    PFN_vkCreateSemaphore        CreateSemaphore;
    PFN_vkDestroySemaphore       DestroySemaphore;
    PFN_vkCreateFence            CreateFence;
    PFN_vkDestroyFence           DestroyFence;
    PFN_vkResetFences            ResetFences;
    PFN_vkWaitForFences          WaitForFences;
    PFN_vkGetFenceStatus         GetFenceStatus;

    PFN_vkGetMemoryFdKHR         GetMemoryFdKHR;
    PFN_vkGetSemaphoreFdKHR      GetSemaphoreFdKHR;

    /* The real WSI calls (we delegate format/presentmode queries to them
       sometimes, but otherwise we replace these completely). */
    PFN_vkCreateSwapchainKHR     CreateSwapchainKHR;
    PFN_vkDestroySwapchainKHR    DestroySwapchainKHR;
    PFN_vkGetSwapchainImagesKHR  GetSwapchainImagesKHR;
    PFN_vkAcquireNextImageKHR    AcquireNextImageKHR;
    PFN_vkQueuePresentKHR        QueuePresentKHR;
} DeviceDispatch;

/* ----------------------------------------------------------------------- */
/* Per-instance and per-device state, looked up by dispatch key            */
/* ----------------------------------------------------------------------- */

#define HASH_BUCKETS 64

typedef struct InstNode {
    void *key;
    VkInstance instance;
    InstanceDispatch d;
    bool external_mem_caps;
    bool external_sem_caps;
    struct InstNode *next;
} InstNode;
static InstNode *g_inst_table[HASH_BUCKETS];
static pthread_mutex_t g_inst_lock = PTHREAD_MUTEX_INITIALIZER;

typedef struct DevNode {
    void *key;
    VkDevice device;
    VkPhysicalDevice phys;
    VkInstance inst;
    DeviceDispatch d;
    InstanceDispatch *idisp;     /* points into the InstNode for this device */
    VkPhysicalDeviceMemoryProperties memp;
    uint32_t graphics_qfi;
    VkQueue graphics_queue;      /* lazy-resolved */
    struct DevNode *next;
} DevNode;
static DevNode *g_dev_table[HASH_BUCKETS];
static pthread_mutex_t g_dev_lock = PTHREAD_MUTEX_INITIALIZER;

/* The Vulkan loader's "dispatch key" is the first sizeof(void*) bytes of
   the object — both VkInstance and VkDevice are dispatchable handles. */
/* The Vulkan loader's "dispatch key" is the first sizeof(void*) bytes of
   the dispatchable object — it points to the loader's per-{instance,device}
   dispatch table. ALL dispatchable handles derived from the same
   device share that pointer (the device, every VkQueue, every VkCommandBuffer
   from that device). We use it as a hash key so any of those handles can
   resolve back to our DevNode.

   Important: pass the handle itself, NOT &handle. The argument is a
   dispatchable handle (a pointer-typed value). We dereference it to read
   the first 8 bytes. */
static void *dispatch_key(const void *handle) {
    return *(void * const *)handle;
}

static unsigned bucket(void *k) {
    uintptr_t x = (uintptr_t)k;
    x ^= x >> 16; x *= 0x9E3779B1u; x ^= x >> 16;
    return (unsigned)(x & (HASH_BUCKETS - 1));
}

static InstNode *inst_lookup(void *key) {
    pthread_mutex_lock(&g_inst_lock);
    InstNode *n = g_inst_table[bucket(key)];
    while (n && n->key != key) n = n->next;
    pthread_mutex_unlock(&g_inst_lock);
    return n;
}
static void inst_insert(InstNode *n) {
    pthread_mutex_lock(&g_inst_lock);
    unsigned b = bucket(n->key); n->next = g_inst_table[b]; g_inst_table[b] = n;
    pthread_mutex_unlock(&g_inst_lock);
}
static void inst_remove(void *key) {
    pthread_mutex_lock(&g_inst_lock);
    unsigned b = bucket(key);
    InstNode **p = &g_inst_table[b];
    while (*p && (*p)->key != key) p = &(*p)->next;
    if (*p) { InstNode *n = *p; *p = n->next; free(n); }
    pthread_mutex_unlock(&g_inst_lock);
}

static DevNode *dev_lookup(void *key) {
    pthread_mutex_lock(&g_dev_lock);
    DevNode *n = g_dev_table[bucket(key)];
    while (n && n->key != key) n = n->next;
    pthread_mutex_unlock(&g_dev_lock);
    return n;
}
static void dev_insert(DevNode *n) {
    pthread_mutex_lock(&g_dev_lock);
    unsigned b = bucket(n->key); n->next = g_dev_table[b]; g_dev_table[b] = n;
    pthread_mutex_unlock(&g_dev_lock);
}
static void dev_remove(void *key) {
    pthread_mutex_lock(&g_dev_lock);
    unsigned b = bucket(key);
    DevNode **p = &g_dev_table[b];
    while (*p && (*p)->key != key) p = &(*p)->next;
    if (*p) { DevNode *n = *p; *p = n->next; free(n); }
    pthread_mutex_unlock(&g_dev_lock);
}

/* For queue->device lookup. Vulkan queues are dispatchable; their key is the
   parent device's. */

/* ----------------------------------------------------------------------- */
/* Surfaces                                                                */
/* ----------------------------------------------------------------------- */

typedef enum {
    SURF_XLIB,
    SURF_XCB,
} SurfaceKind;

typedef struct Surface {
    /* Magic value so we can recognize our own VkSurfaceKHR-shaped handles. */
    uint64_t magic;
    SurfaceKind kind;
    Display *dpy;            /* Xlib handle. For XCB-only apps, we open one ourselves. */
    bool owns_dpy;
    Window window;
    /* The GLX context lives in the SwapchainData, not here, because it
       must match the GLXFBConfig of the rendering format and the app
       chooses format at swapchain creation time. */
} Surface;

#define SURFACE_MAGIC 0x53524654594c5253ULL  /* "SRFTYLSR" backwards-ish */

/* Cast a VkSurfaceKHR (non-dispatchable, 64-bit) into our Surface*. */
static Surface *as_surface(VkSurfaceKHR s) {
    Surface *p = (Surface *)(uintptr_t)s;
    if (!p || p->magic != SURFACE_MAGIC) return NULL;
    return p;
}

/* ----------------------------------------------------------------------- */
/* Swapchains                                                              */
/* ----------------------------------------------------------------------- */

typedef struct PerImage {
    VkImage          image;        /* Vulkan image, OPAQUE_FD exportable */
    VkDeviceMemory   memory;
    VkDeviceSize     size;
    int              mem_fd;       /* fd from vkGetMemoryFdKHR (kept open) */

    VkSemaphore      vk_render_done;  /* Vulkan signals, GL waits.
                                         Exported as OPAQUE_FD. */
    VkSemaphore      gl_sample_done;  /* GL signals, Vulkan waits at next acquire.
                                         Exported as OPAQUE_FD. */
    int              vk_render_done_fd;
    int              gl_sample_done_fd;

    GLuint           gl_memobj;       /* glImportMemoryFdEXT */
    GLuint           gl_texture;      /* bound to gl_memobj */
    GLuint           gl_render_sem;   /* glImportSemaphoreFdEXT of vk_render_done */
    GLuint           gl_sample_sem;   /* glImportSemaphoreFdEXT of gl_sample_done */

    /* Tracking the acquire state.

       Vulkan's swapchain model says an acquired image is either in the
       app's possession or in the presentation engine. We treat it as:
         - "acquired by app" between Acquire and Present
         - "in flight in GL" between Present and the moment glXSwapBuffers
           returns
         - "free" after glXSwapBuffers returns

       We use gl_sample_done as the gate: when the app calls Acquire and
       requests image idx, we make the app's acquire semaphore wait on
       gl_sample_done[idx] via a bridge submit. */
    bool             acquired;        /* currently held by app */
    bool             in_flight;       /* GL has work pending on it */
} PerImage;

typedef struct Swapchain {
    uint64_t magic;
    DevNode *dev;
    Surface *surf;

    /* Properties */
    uint32_t      image_count;
    VkExtent2D    extent;
    VkFormat      format;
    VkColorSpaceKHR color_space;
    VkPresentModeKHR present_mode;

    PerImage      images[MAX_IMAGES];

    /* X / GLX resources owned by this swapchain.

       GLX requires the drawable's visual to match the GLX context's
       FBConfig visual. The application's surface window was created
       without GLX in mind and almost certainly has a visual that no GLX
       FBConfig matches — glXMakeCurrent on it returns BadMatch. We work
       around this by creating a child X window inside the app's window
       with our own chosen visual, sized to match the parent, and using
       that as the GLX drawable. The app never sees it; the X server
       composites it into the parent's area automatically. */
    GLXFBConfig   fbcfg;
    XVisualInfo  *visinfo;
    Colormap      child_colormap;
    Window        child_window;     /* the actual GLX drawable */
    GLXContext    glctx;
    bool          glctx_owned;
    /* The parent X window's actual size, refreshed each present from
       XGetGeometry. We resize the child window to match when it changes. */
    int           win_w, win_h;

    /* GL program state for the textured-quad blit */
    GLuint        prog;
    GLuint        vao;
    GLuint        vbo;
    GLint         u_tex;

    /* Cached extension entrypoints, resolved once per swapchain when its
       GLX context is first made current. */
    PFNGLXSWAPINTERVALEXTPROC        glXSwapIntervalEXT;
    PFNGLCREATEMEMORYOBJECTSEXTPROC  glCreateMemoryObjectsEXT;
    PFNGLIMPORTMEMORYFDEXTPROC       glImportMemoryFdEXT;
    PFNGLTEXSTORAGEMEM2DEXTPROC      glTexStorageMem2DEXT;
    PFNGLDELETEMEMORYOBJECTSEXTPROC  glDeleteMemoryObjectsEXT;
    PFNGLGENSEMAPHORESEXTPROC        glGenSemaphoresEXT;
    PFNGLIMPORTSEMAPHOREFDEXTPROC    glImportSemaphoreFdEXT;
    PFNGLWAITSEMAPHOREEXTPROC        glWaitSemaphoreEXT;
    PFNGLSIGNALSEMAPHOREEXTPROC      glSignalSemaphoreEXT;
    PFNGLDELETESEMAPHORESEXTPROC     glDeleteSemaphoresEXT;

    /* Acquire ring */
    uint32_t      next_acquire;       /* round-robin starting point */

    /* Per-swapchain command pool for our bridge submits. */
    VkCommandPool  cpool;

    /* Thread that owns the current GLX make-current. */
    pthread_t      glx_owner_thread;
    bool           glx_made_current;

    pthread_mutex_t lock;
} Swapchain;

#define SWAPCHAIN_MAGIC 0x5357415043484149ULL    /* "SWAPCHAI" — 8 bytes, fits uint64_t */

static Swapchain *as_swapchain(VkSwapchainKHR s) {
    Swapchain *p = (Swapchain *)(uintptr_t)s;
    if (!p || p->magic != SWAPCHAIN_MAGIC) return NULL;
    return p;
}

static void track_swapchain(Swapchain *sc);
static void untrack_swapchain(Swapchain *sc);

/* ----------------------------------------------------------------------- */
/* GLX helpers                                                             */
/* ----------------------------------------------------------------------- */

/* Choose an FBConfig matching the requested swapchain format. We always
   double-buffer; depth is irrelevant since GL renders only a textured quad.
   We need an FBConfig whose visual matches the X window's visual, but
   that's a problem the app already solved at window creation time — the
   window has SOME visual, and we ask GLX to find a doublebuffered RGBA
   FBConfig of equivalent depth. */
static GLXFBConfig pick_fbconfig(Display *dpy, int screen, VkFormat fmt, XVisualInfo **out_vi) {
    int red=8, green=8, blue=8, alpha=8;
    /* SRGB needs GLX_FRAMEBUFFER_SRGB_CAPABLE_ARB. */
    bool want_srgb = (fmt == VK_FORMAT_R8G8B8A8_SRGB || fmt == VK_FORMAT_B8G8R8A8_SRGB);

    int attrs[32]; int n = 0;
    attrs[n++] = GLX_X_RENDERABLE;     attrs[n++] = True;
    attrs[n++] = GLX_DRAWABLE_TYPE;    attrs[n++] = GLX_WINDOW_BIT;
    attrs[n++] = GLX_RENDER_TYPE;      attrs[n++] = GLX_RGBA_BIT;
    attrs[n++] = GLX_RED_SIZE;         attrs[n++] = red;
    attrs[n++] = GLX_GREEN_SIZE;       attrs[n++] = green;
    attrs[n++] = GLX_BLUE_SIZE;        attrs[n++] = blue;
    attrs[n++] = GLX_ALPHA_SIZE;       attrs[n++] = alpha;
    attrs[n++] = GLX_DEPTH_SIZE;       attrs[n++] = 0;
    attrs[n++] = GLX_DOUBLEBUFFER;     attrs[n++] = True;
    if (want_srgb) { attrs[n++] = GLX_FRAMEBUFFER_SRGB_CAPABLE_ARB; attrs[n++] = True; }
    attrs[n++] = None;

    int nfb = 0;
    GLXFBConfig *fbs = glXChooseFBConfig(dpy, screen, attrs, &nfb);
    if (!fbs || nfb == 0) {
        /* Retry without SRGB if that was the only constraint failing. */
        if (want_srgb) {
            int j = 0;
            while (attrs[j] != GLX_FRAMEBUFFER_SRGB_CAPABLE_ARB && attrs[j] != None) j++;
            if (attrs[j] != None) { attrs[j] = None; }
            fbs = glXChooseFBConfig(dpy, screen, attrs, &nfb);
        }
    }
    if (!fbs || nfb == 0) return NULL;
    GLXFBConfig pick = fbs[0];
    *out_vi = glXGetVisualFromFBConfig(dpy, pick);
    XFree(fbs);
    return pick;
}

static GLXContext create_glx_context(Display *dpy, GLXFBConfig fbc) {
    PFNGLXCREATECONTEXTATTRIBSARBPROC f =
        (PFNGLXCREATECONTEXTATTRIBSARBPROC)glXGetProcAddressARB((const GLubyte*)"glXCreateContextAttribsARB");
    if (f) {
        int a[] = {
            GLX_CONTEXT_MAJOR_VERSION_ARB, 3,
            GLX_CONTEXT_MINOR_VERSION_ARB, 3,
            GLX_CONTEXT_PROFILE_MASK_ARB, GLX_CONTEXT_CORE_PROFILE_BIT_ARB,
            None
        };
        GLXContext c = f(dpy, fbc, NULL, True, a);
        if (c) return c;
    }
    return glXCreateNewContext(dpy, fbc, GLX_RGBA_TYPE, NULL, True);
}

static bool make_current_locked(Swapchain *sc) {
    pthread_t self = pthread_self();
    if (sc->glx_made_current && pthread_equal(sc->glx_owner_thread, self)) return true;
    if (sc->glx_made_current && !pthread_equal(sc->glx_owner_thread, self)) {
        LOG_WARN("present called from multiple threads on swapchain %p (current=%p new=%p)",
                 sc, (void*)sc->glx_owner_thread, (void*)self);
        /* The other thread must un-make-current first. We can't do it from
           here. Best effort: try anyway; glXMakeCurrent will fail and the
           caller will get an INITIALIZATION_FAILED. */
    }
    if (!glXMakeCurrent(sc->surf->dpy, sc->child_window, sc->glctx)) {
        LOG_ERR("glXMakeCurrent failed");
        return false;
    }
    sc->glx_owner_thread = self;
    sc->glx_made_current = true;
    return true;
}

static void unmake_current_locked(Swapchain *sc) {
    if (!sc->glx_made_current) return;
    glXMakeCurrent(sc->surf->dpy, None, NULL);
    sc->glx_made_current = false;
}

/* Resolve GL extension entrypoints. Must be called with the GLX context current. */
static bool resolve_gl_funcs(Swapchain *sc) {
#define GLX(t, name) sc->name = (t)glXGetProcAddressARB((const GLubyte*)#name)
    GLX(PFNGLXSWAPINTERVALEXTPROC,        glXSwapIntervalEXT);
    GLX(PFNGLCREATEMEMORYOBJECTSEXTPROC,  glCreateMemoryObjectsEXT);
    GLX(PFNGLIMPORTMEMORYFDEXTPROC,       glImportMemoryFdEXT);
    GLX(PFNGLTEXSTORAGEMEM2DEXTPROC,      glTexStorageMem2DEXT);
    GLX(PFNGLDELETEMEMORYOBJECTSEXTPROC,  glDeleteMemoryObjectsEXT);
    GLX(PFNGLGENSEMAPHORESEXTPROC,        glGenSemaphoresEXT);
    GLX(PFNGLIMPORTSEMAPHOREFDEXTPROC,    glImportSemaphoreFdEXT);
    GLX(PFNGLWAITSEMAPHOREEXTPROC,        glWaitSemaphoreEXT);
    GLX(PFNGLSIGNALSEMAPHOREEXTPROC,      glSignalSemaphoreEXT);
    GLX(PFNGLDELETESEMAPHORESEXTPROC,     glDeleteSemaphoresEXT);
#undef GLX
    if (!sc->glCreateMemoryObjectsEXT || !sc->glImportMemoryFdEXT || !sc->glTexStorageMem2DEXT
     || !sc->glGenSemaphoresEXT || !sc->glImportSemaphoreFdEXT
     || !sc->glWaitSemaphoreEXT || !sc->glSignalSemaphoreEXT) {
        LOG_ERR("missing GL_EXT_memory_object_fd or GL_EXT_semaphore_fd entrypoints");
        return false;
    }
    return true;
}

/* Compile the textured-quad blit program once per swapchain. */
static GLuint compile_shader(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char buf[1024]; glGetShaderInfoLog(s, sizeof(buf), NULL, buf);
        LOG_ERR("shader compile: %s", buf);
        glDeleteShader(s); return 0;
    }
    return s;
}

static bool build_blit_program(Swapchain *sc) {
    const char *vs =
        "#version 330 core\n"
        "layout(location=0) in vec2 pos;\n"
        "layout(location=1) in vec2 uv;\n"
        "out vec2 v_uv;\n"
        "void main(){ v_uv = uv; gl_Position = vec4(pos, 0.0, 1.0); }\n";
    const char *fs =
        "#version 330 core\n"
        "in vec2 v_uv;\n"
        "out vec4 frag;\n"
        "uniform sampler2D tex;\n"
        "void main(){ frag = texture(tex, v_uv); }\n";
    GLuint v = compile_shader(GL_VERTEX_SHADER, vs);
    GLuint f = compile_shader(GL_FRAGMENT_SHADER, fs);
    if (!v || !f) return false;
    sc->prog = glCreateProgram();
    glAttachShader(sc->prog, v); glAttachShader(sc->prog, f);
    glLinkProgram(sc->prog);
    GLint ok = 0; glGetProgramiv(sc->prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char buf[1024]; glGetProgramInfoLog(sc->prog, sizeof(buf), NULL, buf);
        LOG_ERR("program link: %s", buf);
        return false;
    }
    glDeleteShader(v); glDeleteShader(f);
    sc->u_tex = glGetUniformLocation(sc->prog, "tex");

    glGenVertexArrays(1, &sc->vao); glBindVertexArray(sc->vao);
    glGenBuffers(1, &sc->vbo); glBindBuffer(GL_ARRAY_BUFFER, sc->vbo);
    /* Quad in clip space, UVs flipped vertically because Vulkan's coordinate
       system and GL's differ in Y, and we want the on-screen image to match
       what the app rendered (top-of-image -> top-of-window). */
    float quad[] = {
        -1.0f, -1.0f,  0.0f, 1.0f,
         1.0f, -1.0f,  1.0f, 1.0f,
        -1.0f,  1.0f,  0.0f, 0.0f,
         1.0f, -1.0f,  1.0f, 1.0f,
         1.0f,  1.0f,  1.0f, 0.0f,
        -1.0f,  1.0f,  0.0f, 0.0f,
    };
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, (void*)0);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, (void*)8);
    return true;
}

/* ----------------------------------------------------------------------- */
/* Per-image setup / teardown                                              */
/* ----------------------------------------------------------------------- */

static int find_memtype(const VkPhysicalDeviceMemoryProperties *p, uint32_t bits,
                        VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < p->memoryTypeCount; i++)
        if ((bits & (1u << i)) && (p->memoryTypes[i].propertyFlags & want) == want)
            return (int)i;
    return -1;
}

static VkResult create_exportable_image(DevNode *dev, Swapchain *sc, PerImage *pi) {
    DeviceDispatch *d = &dev->d;

    VkExternalMemoryImageCreateInfo emi = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
    emi.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.pNext = &emi;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = sc->format;
    ici.extent.width = sc->extent.width;
    ici.extent.height = sc->extent.height;
    ici.extent.depth = 1;
    ici.mipLevels = 1; ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
              | VK_IMAGE_USAGE_SAMPLED_BIT
              | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
              | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkResult r = d->CreateImage(dev->device, &ici, NULL, &pi->image);
    if (r != VK_SUCCESS) return r;

    VkMemoryRequirements mreq;
    d->GetImageMemoryRequirements(dev->device, pi->image, &mreq);
    pi->size = mreq.size;

    int mt = find_memtype(&dev->memp, mreq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt < 0) mt = find_memtype(&dev->memp, mreq.memoryTypeBits, 0);
    if (mt < 0) { d->DestroyImage(dev->device, pi->image, NULL); pi->image = VK_NULL_HANDLE; return VK_ERROR_OUT_OF_DEVICE_MEMORY; }

    VkExportMemoryAllocateInfo eai = { VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO };
    eai.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkMemoryDedicatedAllocateInfo dai = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
    dai.image = pi->image;
    eai.pNext = &dai;
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.pNext = &eai; mai.allocationSize = mreq.size; mai.memoryTypeIndex = (uint32_t)mt;

    r = d->AllocateMemory(dev->device, &mai, NULL, &pi->memory);
    if (r != VK_SUCCESS) goto fail_img;
    r = d->BindImageMemory(dev->device, pi->image, pi->memory, 0);
    if (r != VK_SUCCESS) goto fail_mem;

    VkMemoryGetFdInfoKHR gfi = { VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR };
    gfi.memory = pi->memory; gfi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    pi->mem_fd = -1;
    r = d->GetMemoryFdKHR(dev->device, &gfi, &pi->mem_fd);
    if (r != VK_SUCCESS || pi->mem_fd < 0) goto fail_mem;
    return VK_SUCCESS;

fail_mem:
    d->FreeMemory(dev->device, pi->memory, NULL); pi->memory = VK_NULL_HANDLE;
fail_img:
    d->DestroyImage(dev->device, pi->image, NULL); pi->image = VK_NULL_HANDLE;
    return r;
}

static VkResult create_exportable_semaphores(DevNode *dev, PerImage *pi) {
    DeviceDispatch *d = &dev->d;
    VkExportSemaphoreCreateInfo esci = { VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO };
    esci.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    sci.pNext = &esci;
    VkResult r;
    r = d->CreateSemaphore(dev->device, &sci, NULL, &pi->vk_render_done);
    if (r != VK_SUCCESS) return r;
    r = d->CreateSemaphore(dev->device, &sci, NULL, &pi->gl_sample_done);
    if (r != VK_SUCCESS) goto fail_a;

    VkSemaphoreGetFdInfoKHR gfi = { VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR };
    gfi.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
    gfi.semaphore = pi->vk_render_done;
    pi->vk_render_done_fd = -1;
    r = d->GetSemaphoreFdKHR(dev->device, &gfi, &pi->vk_render_done_fd);
    if (r != VK_SUCCESS || pi->vk_render_done_fd < 0) goto fail_b;
    gfi.semaphore = pi->gl_sample_done;
    pi->gl_sample_done_fd = -1;
    r = d->GetSemaphoreFdKHR(dev->device, &gfi, &pi->gl_sample_done_fd);
    if (r != VK_SUCCESS || pi->gl_sample_done_fd < 0) goto fail_b;
    return VK_SUCCESS;

fail_b:
    d->DestroySemaphore(dev->device, pi->gl_sample_done, NULL); pi->gl_sample_done = VK_NULL_HANDLE;
fail_a:
    d->DestroySemaphore(dev->device, pi->vk_render_done, NULL); pi->vk_render_done = VK_NULL_HANDLE;
    return r;
}

static bool gl_import_image(Swapchain *sc, PerImage *pi) {
    sc->glCreateMemoryObjectsEXT(1, &pi->gl_memobj);
    if (glGetError() != GL_NO_ERROR) return false;
    int dup_fd = dup(pi->mem_fd);
    if (dup_fd < 0) return false;
    sc->glImportMemoryFdEXT(pi->gl_memobj, pi->size, GL_HANDLE_TYPE_OPAQUE_FD_EXT, dup_fd);
    if (glGetError() != GL_NO_ERROR) return false;

    glGenTextures(1, &pi->gl_texture);
    glBindTexture(GL_TEXTURE_2D, pi->gl_texture);
    GLenum internal = GL_RGBA8;
    if (sc->format == VK_FORMAT_R8G8B8A8_SRGB || sc->format == VK_FORMAT_B8G8R8A8_SRGB)
        internal = GL_SRGB8_ALPHA8;
    sc->glTexStorageMem2DEXT(GL_TEXTURE_2D, 1, internal,
                             sc->extent.width, sc->extent.height, pi->gl_memobj, 0);
    if (glGetError() != GL_NO_ERROR) return false;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return true;
}

static bool gl_import_semaphores(Swapchain *sc, PerImage *pi) {
    sc->glGenSemaphoresEXT(1, &pi->gl_render_sem);
    sc->glGenSemaphoresEXT(1, &pi->gl_sample_sem);
    int fd1 = dup(pi->vk_render_done_fd);
    int fd2 = dup(pi->gl_sample_done_fd);
    if (fd1 < 0 || fd2 < 0) return false;
    sc->glImportSemaphoreFdEXT(pi->gl_render_sem, GL_HANDLE_TYPE_OPAQUE_FD_EXT, fd1);
    if (glGetError() != GL_NO_ERROR) return false;
    sc->glImportSemaphoreFdEXT(pi->gl_sample_sem, GL_HANDLE_TYPE_OPAQUE_FD_EXT, fd2);
    if (glGetError() != GL_NO_ERROR) return false;
    return true;
}

static void destroy_perimage(DevNode *dev, Swapchain *sc, PerImage *pi) {
    DeviceDispatch *d = &dev->d;
    if (pi->gl_texture) glDeleteTextures(1, &pi->gl_texture);
    if (pi->gl_memobj && sc->glDeleteMemoryObjectsEXT)
        sc->glDeleteMemoryObjectsEXT(1, &pi->gl_memobj);
    if (pi->gl_render_sem && sc->glDeleteSemaphoresEXT)
        sc->glDeleteSemaphoresEXT(1, &pi->gl_render_sem);
    if (pi->gl_sample_sem && sc->glDeleteSemaphoresEXT)
        sc->glDeleteSemaphoresEXT(1, &pi->gl_sample_sem);

    if (pi->vk_render_done) d->DestroySemaphore(dev->device, pi->vk_render_done, NULL);
    if (pi->gl_sample_done) d->DestroySemaphore(dev->device, pi->gl_sample_done, NULL);
    if (pi->image)          d->DestroyImage    (dev->device, pi->image, NULL);
    if (pi->memory)         d->FreeMemory      (dev->device, pi->memory, NULL);

    /* Close the fds we still own. Note: the fds we duped into GL are owned
       by GL now; the ones we duped into Vulkan-internal (none in our flow)
       would be owned by Vulkan. The "originals" from vkGetMemoryFdKHR /
       vkGetSemaphoreFdKHR are still ours. */
    if (pi->mem_fd            >= 0) close(pi->mem_fd);
    if (pi->vk_render_done_fd >= 0) close(pi->vk_render_done_fd);
    if (pi->gl_sample_done_fd >= 0) close(pi->gl_sample_done_fd);

    memset(pi, 0, sizeof(*pi));
    pi->mem_fd = pi->vk_render_done_fd = pi->gl_sample_done_fd = -1;
}

/* ----------------------------------------------------------------------- */
/* Surface bypass-compositor hint                                          */
/* ----------------------------------------------------------------------- */

static void set_bypass_compositor(Display *dpy, Window win) {
    Atom prop = XInternAtom(dpy, "_NET_WM_BYPASS_COMPOSITOR", False);
    long val = 1;
    XChangeProperty(dpy, win, prop, XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char*)&val, 1);
}

/* ----------------------------------------------------------------------- */
/* Surface hooks                                                           */
/* ----------------------------------------------------------------------- */

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
layer_CreateXlibSurfaceKHR(VkInstance instance,
                            const VkXlibSurfaceCreateInfoKHR *pCreateInfo,
                            const VkAllocationCallbacks *pAllocator,
                            VkSurfaceKHR *pSurface) {
    if (g_layer_disabled) {
        InstNode *in = inst_lookup(dispatch_key(instance));
        return in->d.CreateXlibSurfaceKHR(instance, pCreateInfo, pAllocator, pSurface);
    }
    Surface *s = calloc(1, sizeof(*s));
    if (!s) return VK_ERROR_OUT_OF_HOST_MEMORY;
    s->magic = SURFACE_MAGIC;
    s->kind = SURF_XLIB;
    s->dpy = pCreateInfo->dpy;
    s->window = pCreateInfo->window;
    s->owns_dpy = false;
    set_bypass_compositor(s->dpy, s->window);
    *pSurface = (VkSurfaceKHR)(uintptr_t)s;
    LOG_INFO("CreateXlibSurfaceKHR -> surface=%p dpy=%p win=0x%lx", s, s->dpy, s->window);
    return VK_SUCCESS;
}

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
layer_CreateXcbSurfaceKHR(VkInstance instance,
                           const VkXcbSurfaceCreateInfoKHR *pCreateInfo,
                           const VkAllocationCallbacks *pAllocator,
                           VkSurfaceKHR *pSurface) {
    if (g_layer_disabled) {
        InstNode *in = inst_lookup(dispatch_key(instance));
        return in->d.CreateXcbSurfaceKHR(instance, pCreateInfo, pAllocator, pSurface);
    }
    /* XCB-only apps don't give us an Xlib Display*. We need one for GLX
       (GLX is Xlib-bound). Open our own Display* over the same X server. */
    Surface *s = calloc(1, sizeof(*s));
    if (!s) return VK_ERROR_OUT_OF_HOST_MEMORY;
    s->magic = SURFACE_MAGIC;
    s->kind = SURF_XCB;
    s->dpy = XOpenDisplay(NULL);
    if (!s->dpy) { free(s); return VK_ERROR_INITIALIZATION_FAILED; }
    s->owns_dpy = true;
    s->window = pCreateInfo->window;
    set_bypass_compositor(s->dpy, s->window);
    *pSurface = (VkSurfaceKHR)(uintptr_t)s;
    LOG_INFO("CreateXcbSurfaceKHR -> surface=%p dpy=%p (opened) win=0x%x",
             s, s->dpy, pCreateInfo->window);
    return VK_SUCCESS;
}

VK_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL
layer_DestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface,
                         const VkAllocationCallbacks *pAllocator) {
    if (!surface) return;
    Surface *s = as_surface(surface);
    InstNode *in = inst_lookup(dispatch_key(instance));
    if (!s) {  /* not ours — was created as passthrough */
        in->d.DestroySurfaceKHR(instance, surface, pAllocator);
        return;
    }
    if (s->owns_dpy && s->dpy) XCloseDisplay(s->dpy);
    free(s);
}

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
layer_GetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice physicalDevice,
                                          uint32_t queueFamilyIndex,
                                          VkSurfaceKHR surface,
                                          VkBool32 *pSupported) {
    Surface *s = as_surface(surface);
    if (!s) {
        InstNode *in = inst_lookup(dispatch_key(physicalDevice));
        return in->d.GetPhysicalDeviceSurfaceSupportKHR(physicalDevice, queueFamilyIndex, surface, pSupported);
    }
    /* Any graphics queue family supports our surface; we don't depend on
       Vulkan WSI presentation queues. */
    *pSupported = VK_TRUE;
    return VK_SUCCESS;
}

static void window_size(Surface *s, uint32_t *w, uint32_t *h) {
    Window root; int x, y; unsigned int ww, hh, b, dep;
    if (XGetGeometry(s->dpy, s->window, &root, &x, &y, &ww, &hh, &b, &dep)) {
        *w = ww; *h = hh;
    } else {
        *w = *h = 0;
    }
}

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
layer_GetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice physicalDevice,
                                               VkSurfaceKHR surface,
                                               VkSurfaceCapabilitiesKHR *pCaps) {
    Surface *s = as_surface(surface);
    if (!s) {
        InstNode *in = inst_lookup(dispatch_key(physicalDevice));
        return in->d.GetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface, pCaps);
    }
    uint32_t w = 0, h = 0; window_size(s, &w, &h);
    pCaps->minImageCount = MIN_IMAGES;
    pCaps->maxImageCount = MAX_IMAGES;
    pCaps->currentExtent.width  = w ? w : 1;
    pCaps->currentExtent.height = h ? h : 1;
    pCaps->minImageExtent.width  = 1;
    pCaps->minImageExtent.height = 1;
    pCaps->maxImageExtent.width  = 16384;
    pCaps->maxImageExtent.height = 16384;
    pCaps->maxImageArrayLayers = 1;
    pCaps->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    pCaps->currentTransform    = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    pCaps->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    pCaps->supportedUsageFlags =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
      | VK_IMAGE_USAGE_SAMPLED_BIT
      | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
      | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    return VK_SUCCESS;
}

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
layer_GetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice physicalDevice,
                                          VkSurfaceKHR surface,
                                          uint32_t *pCount,
                                          VkSurfaceFormatKHR *pFormats) {
    Surface *s = as_surface(surface);
    if (!s) {
        InstNode *in = inst_lookup(dispatch_key(physicalDevice));
        return in->d.GetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, pCount, pFormats);
    }
    /* We support BGRA8 and RGBA8 UNORM and SRGB variants in OPTIMAL tiling
       with OPAQUE_FD export. These are the four formats we expose. */
    static const VkSurfaceFormatKHR formats[] = {
        { VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
        { VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
        { VK_FORMAT_B8G8R8A8_SRGB,  VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
        { VK_FORMAT_R8G8B8A8_SRGB,  VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
    };
    uint32_t avail = sizeof(formats) / sizeof(formats[0]);
    if (!pFormats) { *pCount = avail; return VK_SUCCESS; }
    uint32_t n = *pCount < avail ? *pCount : avail;
    memcpy(pFormats, formats, n * sizeof(VkSurfaceFormatKHR));
    *pCount = n;
    return n == avail ? VK_SUCCESS : VK_INCOMPLETE;
}

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
layer_GetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice physicalDevice,
                                               VkSurfaceKHR surface,
                                               uint32_t *pCount,
                                               VkPresentModeKHR *pModes) {
    Surface *s = as_surface(surface);
    if (!s) {
        InstNode *in = inst_lookup(dispatch_key(physicalDevice));
        return in->d.GetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface, pCount, pModes);
    }
    static const VkPresentModeKHR modes[] = {
        VK_PRESENT_MODE_FIFO_KHR,
        VK_PRESENT_MODE_FIFO_RELAXED_KHR,
        VK_PRESENT_MODE_IMMEDIATE_KHR,
    };
    uint32_t avail = sizeof(modes) / sizeof(modes[0]);
    if (!pModes) { *pCount = avail; return VK_SUCCESS; }
    uint32_t n = *pCount < avail ? *pCount : avail;
    memcpy(pModes, modes, n * sizeof(VkPresentModeKHR));
    *pCount = n;
    return n == avail ? VK_SUCCESS : VK_INCOMPLETE;
}

/* ----------------------------------------------------------------------- */
/* Swapchain creation                                                      */
/* ----------------------------------------------------------------------- */

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
layer_CreateSwapchainKHR(VkDevice device,
                          const VkSwapchainCreateInfoKHR *ci,
                          const VkAllocationCallbacks *pAlloc,
                          VkSwapchainKHR *pOut) {
    DevNode *dev = dev_lookup(dispatch_key(device));
    if (!dev) return VK_ERROR_INITIALIZATION_FAILED;

    Surface *surf = as_surface(ci->surface);
    if (g_layer_disabled || !surf)
        return dev->d.CreateSwapchainKHR(device, ci, pAlloc, pOut);

    /* Clamp image count to our range. */
    uint32_t want = ci->minImageCount;
    if (want < MIN_IMAGES) want = MIN_IMAGES;
    if (want > MAX_IMAGES) want = MAX_IMAGES;

    /* Reject formats we don't support. */
    switch (ci->imageFormat) {
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_R8G8B8A8_SRGB:
        break;
    default:
        LOG_WARN("CreateSwapchainKHR: unsupported format %d, falling through to passthrough", ci->imageFormat);
        return dev->d.CreateSwapchainKHR(device, ci, pAlloc, pOut);
    }

    Swapchain *sc = calloc(1, sizeof(*sc));
    if (!sc) return VK_ERROR_OUT_OF_HOST_MEMORY;
    sc->magic = SWAPCHAIN_MAGIC;
    sc->dev   = dev;
    sc->surf  = surf;
    sc->image_count = want;
    sc->extent  = ci->imageExtent;
    sc->format  = ci->imageFormat;
    sc->color_space = ci->imageColorSpace;
    sc->present_mode = ci->presentMode;
    pthread_mutex_init(&sc->lock, NULL);
    for (uint32_t i = 0; i < MAX_IMAGES; i++) {
        sc->images[i].mem_fd = sc->images[i].vk_render_done_fd = sc->images[i].gl_sample_done_fd = -1;
    }

    int screen = DefaultScreen(surf->dpy);
    sc->fbcfg = pick_fbconfig(surf->dpy, screen, sc->format, &sc->visinfo);
    if (!sc->fbcfg || !sc->visinfo) {
        LOG_ERR("pick_fbconfig: no suitable FBConfig for format %d", sc->format);
        free(sc);
        return dev->d.CreateSwapchainKHR(device, ci, pAlloc, pOut);
    }

    /* Create the GLX child window inside the app's surface window. We use
       our own visual (from the FBConfig) so GLX is happy. The app's window
       still has whatever visual the app chose; we never touch it directly. */
    {
        uint32_t pw = 0, ph = 0; window_size(surf, &pw, &ph);
        if (pw == 0 || ph == 0) { pw = sc->extent.width; ph = sc->extent.height; }
        sc->win_w = (int)pw; sc->win_h = (int)ph;

        sc->child_colormap = XCreateColormap(surf->dpy, surf->window,
                                              sc->visinfo->visual, AllocNone);
        XSetWindowAttributes swa = {0};
        swa.colormap = sc->child_colormap;
        swa.background_pixel = 0;
        swa.border_pixel = 0;
        /* No event mask — we don't want to receive events on the child,
           and even if we did, they'd go to whoever owns the X event queue. */
        sc->child_window = XCreateWindow(surf->dpy, surf->window,
                                         0, 0, pw, ph, 0,
                                         sc->visinfo->depth, InputOutput,
                                         sc->visinfo->visual,
                                         CWColormap | CWBackPixel | CWBorderPixel,
                                         &swa);
        if (!sc->child_window) {
            LOG_ERR("XCreateWindow(child) failed");
            if (sc->child_colormap) XFreeColormap(surf->dpy, sc->child_colormap);
            if (sc->visinfo) XFree(sc->visinfo);
            free(sc);
            return dev->d.CreateSwapchainKHR(device, ci, pAlloc, pOut);
        }
        XMapWindow(surf->dpy, sc->child_window);
        XFlush(surf->dpy);
    }

    sc->glctx = create_glx_context(surf->dpy, sc->fbcfg);
    if (!sc->glctx) {
        LOG_ERR("create_glx_context failed");
        XDestroyWindow(surf->dpy, sc->child_window);
        XFreeColormap(surf->dpy, sc->child_colormap);
        if (sc->visinfo) XFree(sc->visinfo);
        free(sc);
        return dev->d.CreateSwapchainKHR(device, ci, pAlloc, pOut);
    }
    sc->glctx_owned = true;

    if (!glXMakeCurrent(surf->dpy, sc->child_window, sc->glctx)) {
        LOG_ERR("initial glXMakeCurrent failed");
        glXDestroyContext(surf->dpy, sc->glctx);
        XDestroyWindow(surf->dpy, sc->child_window);
        XFreeColormap(surf->dpy, sc->child_colormap);
        if (sc->visinfo) XFree(sc->visinfo);
        free(sc);
        return dev->d.CreateSwapchainKHR(device, ci, pAlloc, pOut);
    }
    sc->glx_owner_thread = pthread_self();
    sc->glx_made_current = true;

    if (!resolve_gl_funcs(sc)) goto fail_gl_setup;
    if (sc->glXSwapIntervalEXT) {
        int interval = 1;
        if (sc->present_mode == VK_PRESENT_MODE_IMMEDIATE_KHR) interval = 0;
        else if (sc->present_mode == VK_PRESENT_MODE_FIFO_RELAXED_KHR) interval = -1;
        sc->glXSwapIntervalEXT(surf->dpy, sc->child_window, interval);
    }
    if (!build_blit_program(sc)) goto fail_gl_setup;

    /* Vulkan side: command pool for our bridge submits. */
    VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    cpi.queueFamilyIndex = dev->graphics_qfi;
    cpi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    /* We don't actually use command buffers for the bridges (they're signal/wait only),
       but a CommandPool is required by some drivers for queue submission scratch. We
       create it for parity with other paths but won't allocate from it currently. */
    (void)cpi;

    /* Allocate per-image Vulkan resources and import them into GL. */
    for (uint32_t i = 0; i < sc->image_count; i++) {
        VkResult r = create_exportable_image(dev, sc, &sc->images[i]);
        if (r != VK_SUCCESS) { LOG_ERR("create_exportable_image[%u]: %d", i, r); goto fail_perimg; }
        r = create_exportable_semaphores(dev, &sc->images[i]);
        if (r != VK_SUCCESS) { LOG_ERR("create_exportable_semaphores[%u]: %d", i, r); goto fail_perimg; }
        if (!gl_import_image(sc, &sc->images[i]))       { LOG_ERR("gl_import_image[%u]",      i); goto fail_perimg; }
        if (!gl_import_semaphores(sc, &sc->images[i]))  { LOG_ERR("gl_import_semaphores[%u]", i); goto fail_perimg; }

        /* Pre-signal gl_sample_done so the first Acquire doesn't block. */
        VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &sc->images[i].gl_sample_done;
        VkResult psr = dev->d.QueueSubmit(dev->graphics_queue, 1, &si, VK_NULL_HANDLE);
        if (psr != VK_SUCCESS) {
            LOG_ERR("pre-signal QueueSubmit for image %u failed: %d", i, psr);
            goto fail_perimg;
        }
        LOG_INFO("pre-signaled gl_sample_done[%u]=%p on queue=%p",
                 i, (void*)sc->images[i].gl_sample_done, (void*)dev->graphics_queue);
    }

    glXMakeCurrent(surf->dpy, None, NULL);
    sc->glx_made_current = false;

    track_swapchain(sc);

    *pOut = (VkSwapchainKHR)(uintptr_t)sc;
    LOG_INFO("CreateSwapchainKHR -> sc=%p %ux%u fmt=%d images=%u present_mode=%d",
             sc, sc->extent.width, sc->extent.height, sc->format,
             sc->image_count, sc->present_mode);
    return VK_SUCCESS;

fail_perimg:
    for (uint32_t i = 0; i < sc->image_count; i++) destroy_perimage(dev, sc, &sc->images[i]);
fail_gl_setup:
    if (sc->vbo) glDeleteBuffers(1, &sc->vbo);
    if (sc->vao) glDeleteVertexArrays(1, &sc->vao);
    if (sc->prog) glDeleteProgram(sc->prog);
    glXMakeCurrent(surf->dpy, None, NULL);
    if (sc->glctx_owned && sc->glctx) glXDestroyContext(surf->dpy, sc->glctx);
    if (sc->child_window) XDestroyWindow(surf->dpy, sc->child_window);
    if (sc->child_colormap) XFreeColormap(surf->dpy, sc->child_colormap);
    if (sc->visinfo) XFree(sc->visinfo);
    free(sc);
    /* Soft fail: fall through to the real Vulkan WSI so the app still runs (with tearing). */
    return dev->d.CreateSwapchainKHR(device, ci, pAlloc, pOut);
}

VK_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL
layer_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                           const VkAllocationCallbacks *pAlloc) {
    if (!swapchain) return;
    Swapchain *sc = as_swapchain(swapchain);
    DevNode *dev = dev_lookup(dispatch_key(device));
    if (!sc) { if (dev) dev->d.DestroySwapchainKHR(device, swapchain, pAlloc); return; }

    /* Drain any in-flight work. */
    dev->d.DeviceWaitIdle(dev->device);

    untrack_swapchain(sc);

    pthread_mutex_lock(&sc->lock);
    if (sc->glctx) {
        glXMakeCurrent(sc->surf->dpy, sc->child_window, sc->glctx);
        for (uint32_t i = 0; i < sc->image_count; i++) destroy_perimage(dev, sc, &sc->images[i]);
        if (sc->vbo)  glDeleteBuffers(1, &sc->vbo);
        if (sc->vao)  glDeleteVertexArrays(1, &sc->vao);
        if (sc->prog) glDeleteProgram(sc->prog);
        glXMakeCurrent(sc->surf->dpy, None, NULL);
        if (sc->glctx_owned) glXDestroyContext(sc->surf->dpy, sc->glctx);
    }
    if (sc->child_window)   XDestroyWindow(sc->surf->dpy, sc->child_window);
    if (sc->child_colormap) XFreeColormap (sc->surf->dpy, sc->child_colormap);
    if (sc->visinfo) XFree(sc->visinfo);
    pthread_mutex_unlock(&sc->lock);
    pthread_mutex_destroy(&sc->lock);
    memset(sc, 0, sizeof(*sc));
    free(sc);
}

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
layer_GetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain,
                             uint32_t *pCount, VkImage *pImages) {
    Swapchain *sc = as_swapchain(swapchain);
    if (!sc) {
        DevNode *dev = dev_lookup(dispatch_key(device));
        return dev->d.GetSwapchainImagesKHR(device, swapchain, pCount, pImages);
    }
    if (!pImages) { *pCount = sc->image_count; return VK_SUCCESS; }
    uint32_t n = *pCount < sc->image_count ? *pCount : sc->image_count;
    for (uint32_t i = 0; i < n; i++) pImages[i] = sc->images[i].image;
    *pCount = n;
    return n == sc->image_count ? VK_SUCCESS : VK_INCOMPLETE;
}

/* ----------------------------------------------------------------------- */
/* Acquire / Present                                                       */
/* ----------------------------------------------------------------------- */

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
layer_AcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain,
                           uint64_t timeout, VkSemaphore semaphore, VkFence fence,
                           uint32_t *pIndex) {
    Swapchain *sc = as_swapchain(swapchain);
    DevNode *dev = dev_lookup(dispatch_key(device));
    if (!sc) return dev->d.AcquireNextImageKHR(device, swapchain, timeout, semaphore, fence, pIndex);

    pthread_mutex_lock(&sc->lock);
    /* Find the next free image. Round-robin among in_flight==false images. */
    uint32_t idx = sc->next_acquire;
    uint32_t tried = 0;
    while (tried < sc->image_count && sc->images[idx].acquired) {
        idx = (idx + 1) % sc->image_count;
        tried++;
    }
    if (tried == sc->image_count) {
        pthread_mutex_unlock(&sc->lock);
        return VK_NOT_READY;
    }
    sc->images[idx].acquired = true;
    sc->next_acquire = (idx + 1) % sc->image_count;

    /* Bridge submit: wait on the per-image gl_sample_done semaphore, signal
       the app's requested acquire semaphore and/or fence. The wait ensures
       the app doesn't get to render into images[idx] before GL has finished
       sampling it. */
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.waitSemaphoreCount   = 1;
    si.pWaitSemaphores      = &sc->images[idx].gl_sample_done;
    si.pWaitDstStageMask    = &stage;
    if (semaphore) { si.signalSemaphoreCount = 1; si.pSignalSemaphores = &semaphore; }
    VkResult r = dev->d.QueueSubmit(dev->graphics_queue, 1, &si, fence);
    pthread_mutex_unlock(&sc->lock);
    if (r != VK_SUCCESS) {
        LOG_ERR("AcquireNextImageKHR: bridge QueueSubmit failed: %d (queue=%p sem=%p fence=%p signal=%p)",
                r, (void*)dev->graphics_queue, (void*)semaphore, (void*)fence, (void*)sc->images[idx].gl_sample_done);
        return r;
    }
    LOG_INFO("Acquire -> idx=%u (bridge submit ok, signal app_sem=%p fence=%p)",
             idx, (void*)semaphore, (void*)fence);

    *pIndex = idx;
    return VK_SUCCESS;
}

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
layer_EndCommandBuffer(VkCommandBuffer cb) {
    DevNode *dev = dev_lookup(dispatch_key(cb));
    if (!dev) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkEndCommandBuffer next = (PFN_vkEndCommandBuffer)
        dev->d.GetDeviceProcAddr(dev->device, "vkEndCommandBuffer");
    if (!next) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = next(cb);
    if (r != VK_SUCCESS) {
        LOG_ERR("vkEndCommandBuffer FAILED: ret=%d cb=%p", r, (void*)cb);
    }
    return r;
}

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
layer_QueueWaitIdle(VkQueue queue) {
    DevNode *dev = dev_lookup(dispatch_key(queue));
    if (!dev) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = dev->d.QueueWaitIdle(queue);
    if (r != VK_SUCCESS) {
        LOG_ERR("vkQueueWaitIdle FAILED: ret=%d queue=%p", r, (void*)queue);
    }
    return r;
}

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
layer_DeviceWaitIdle(VkDevice device) {
    DevNode *dev = dev_lookup(dispatch_key(device));
    if (!dev) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = dev->d.DeviceWaitIdle(device);
    if (r != VK_SUCCESS) {
        LOG_ERR("vkDeviceWaitIdle FAILED: ret=%d", r);
    }
    return r;
}

/* Track swapchains in a global list so we can answer "is this image ours" without
   knowing which swapchain it belongs to up front. */
#define MAX_TRACKED_SWAPCHAINS 16
static Swapchain *g_swapchains[MAX_TRACKED_SWAPCHAINS];
static pthread_mutex_t g_sc_lock = PTHREAD_MUTEX_INITIALIZER;

static void track_swapchain(Swapchain *sc) {
    pthread_mutex_lock(&g_sc_lock);
    for (int i = 0; i < MAX_TRACKED_SWAPCHAINS; i++) {
        if (!g_swapchains[i]) { g_swapchains[i] = sc; break; }
    }
    pthread_mutex_unlock(&g_sc_lock);
}
static void untrack_swapchain(Swapchain *sc) {
    pthread_mutex_lock(&g_sc_lock);
    for (int i = 0; i < MAX_TRACKED_SWAPCHAINS; i++) {
        if (g_swapchains[i] == sc) { g_swapchains[i] = NULL; break; }
    }
    pthread_mutex_unlock(&g_sc_lock);
}
static bool image_is_managed(VkImage img) {
    if (!img) return false;
    pthread_mutex_lock(&g_sc_lock);
    bool found = false;
    for (int i = 0; i < MAX_TRACKED_SWAPCHAINS && !found; i++) {
        Swapchain *sc = g_swapchains[i];
        if (!sc) continue;
        for (uint32_t j = 0; j < sc->image_count; j++) {
            if (sc->images[j].image == img) { found = true; break; }
        }
    }
    pthread_mutex_unlock(&g_sc_lock);
    return found;
}

/* Rewrite VK_IMAGE_LAYOUT_PRESENT_SRC_KHR (and SHARED_PRESENT_KHR) on barriers
   targeting our managed images. The driver can't transition a non-swapchain
   image to PRESENT_SRC — it tries to invoke present-engine metadata that doesn't
   exist for our externally-allocated images, and faults the GPU. We rewrite
   to SHADER_READ_ONLY_OPTIMAL, which is what we actually want anyway since GL
   will sample the image after the app is done with it. */
static VkImageLayout fix_layout(VkImageLayout l) {
    if (l == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (l == VK_IMAGE_LAYOUT_SHARED_PRESENT_KHR) return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return l;
}

VK_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL
layer_CmdPipelineBarrier(VkCommandBuffer cb,
                          VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage,
                          VkDependencyFlags depFlags,
                          uint32_t memBarrierCount, const VkMemoryBarrier *memBarriers,
                          uint32_t bufBarrierCount, const VkBufferMemoryBarrier *bufBarriers,
                          uint32_t imgBarrierCount, const VkImageMemoryBarrier *imgBarriers) {
    static bool first_call = true;
    if (first_call) {
        first_call = false;
        LOG_INFO("CmdPipelineBarrier intercept active (first call): imgBarriers=%u", imgBarrierCount);
    }
    /* Locate the device for this command buffer. Command-buffer dispatch key
       is the parent device's. */
    DevNode *dev = dev_lookup(dispatch_key(cb));
    if (!dev) {
        LOG_ERR("CmdPipelineBarrier: no device for cb=%p key=%p", (void*)cb, dispatch_key(cb));
        return;
    }
    PFN_vkCmdPipelineBarrier next = (PFN_vkCmdPipelineBarrier)
        dev->d.GetDeviceProcAddr(dev->device, "vkCmdPipelineBarrier");
    if (!next) return;

    /* Copy and rewrite layouts on barriers that hit our managed images. */
    VkImageMemoryBarrier *fixed = NULL;
    if (imgBarrierCount > 0) {
        bool any = false;
        for (uint32_t i = 0; i < imgBarrierCount; i++) {
            if (image_is_managed(imgBarriers[i].image)) { any = true; break; }
        }
        if (any) {
            fixed = malloc(imgBarrierCount * sizeof(*fixed));
            if (fixed) {
                memcpy(fixed, imgBarriers, imgBarrierCount * sizeof(*fixed));
                for (uint32_t i = 0; i < imgBarrierCount; i++) {
                    if (image_is_managed(fixed[i].image)) {
                        VkImageLayout o = fixed[i].oldLayout, n = fixed[i].newLayout;
                        fixed[i].oldLayout = fix_layout(o);
                        fixed[i].newLayout = fix_layout(n);
                        if (o != fixed[i].oldLayout || n != fixed[i].newLayout) {
                            LOG_DBG("rewrote barrier on managed image %p: old %d->%d new %d->%d",
                                    (void*)fixed[i].image, o, fixed[i].oldLayout, n, fixed[i].newLayout);
                        }
                    }
                }
                imgBarriers = fixed;
            }
        }
    }
    next(cb, srcStage, dstStage, depFlags,
         memBarrierCount, memBarriers,
         bufBarrierCount, bufBarriers,
         imgBarrierCount, imgBarriers);
    if (fixed) free(fixed);
}

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
layer_QueueSubmit(VkQueue queue, uint32_t submitCount, const VkSubmitInfo *pSubmits, VkFence fence) {
    DevNode *dev = dev_lookup(dispatch_key(queue));
    if (!dev) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = dev->d.QueueSubmit(queue, submitCount, pSubmits, fence);
    if (r != VK_SUCCESS) {
        LOG_ERR("vkQueueSubmit FAILED: ret=%d queue=%p submitCount=%u fence=%p",
                r, (void*)queue, submitCount, (void*)fence);
        for (uint32_t i = 0; i < submitCount; i++) {
            LOG_ERR("  pSubmits[%u]: waitSem=%u cmdBuf=%u sigSem=%u",
                    i, pSubmits[i].waitSemaphoreCount,
                    pSubmits[i].commandBufferCount, pSubmits[i].signalSemaphoreCount);
        }
    }
    return r;
}

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
layer_WaitForFences(VkDevice device, uint32_t fenceCount, const VkFence *pFences,
                     VkBool32 waitAll, uint64_t timeout) {
    DevNode *dev = dev_lookup(dispatch_key(device));
    if (!dev) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = dev->d.WaitForFences(device, fenceCount, pFences, waitAll, timeout);
    if (r != VK_SUCCESS) {
        LOG_ERR("vkWaitForFences FAILED: ret=%d fenceCount=%u timeout=%" PRIu64,
                r, fenceCount, timeout);
    }
    return r;
}

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
layer_ResetFences(VkDevice device, uint32_t fenceCount, const VkFence *pFences) {
    DevNode *dev = dev_lookup(dispatch_key(device));
    if (!dev) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = dev->d.ResetFences(device, fenceCount, pFences);
    if (r != VK_SUCCESS) LOG_ERR("vkResetFences FAILED: ret=%d", r);
    return r;
}

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
layer_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pInfo) {
    /* Queue's dispatch key is the parent device's. */
    DevNode *dev = dev_lookup(dispatch_key(queue));
    if (!dev) return VK_ERROR_INITIALIZATION_FAILED;
    if (g_layer_disabled) return dev->d.QueuePresentKHR(queue, pInfo);

    LOG_INFO("Present entry: queue=%p (graphics_queue=%p) swapchains=%u waitSems=%u",
             (void*)queue, (void*)dev->graphics_queue,
             pInfo->swapchainCount, pInfo->waitSemaphoreCount);

    VkResult overall = VK_SUCCESS;

    for (uint32_t s = 0; s < pInfo->swapchainCount; s++) {
        Swapchain *sc = as_swapchain(pInfo->pSwapchains[s]);
        if (!sc) {
            /* Mixed batch — submit just this one through real WSI. */
            VkPresentInfoKHR sub = *pInfo;
            sub.swapchainCount = 1;
            sub.pSwapchains    = &pInfo->pSwapchains[s];
            sub.pImageIndices  = &pInfo->pImageIndices[s];
            sub.pResults       = pInfo->pResults ? &pInfo->pResults[s] : NULL;
            sub.waitSemaphoreCount = (s == 0) ? pInfo->waitSemaphoreCount : 0;
            sub.pWaitSemaphores    = (s == 0) ? pInfo->pWaitSemaphores    : NULL;
            VkResult r = dev->d.QueuePresentKHR(queue, &sub);
            if (pInfo->pResults) pInfo->pResults[s] = r;
            if (r != VK_SUCCESS && overall == VK_SUCCESS) overall = r;
            continue;
        }
        uint32_t idx = pInfo->pImageIndices[s];
        if (idx >= sc->image_count) {
            VkResult r = VK_ERROR_OUT_OF_DATE_KHR;
            if (pInfo->pResults) pInfo->pResults[s] = r;
            if (overall == VK_SUCCESS) overall = r;
            continue;
        }

        pthread_mutex_lock(&sc->lock);

        /* Bridge: wait on the app's render-done semaphore(s), signal our
           per-image vk_render_done. Only do this for the first swapchain in
           the batch; subsequent ones don't get the app's waitSemaphores
           applied per WSI semantics. */
        VkPipelineStageFlags stages[16];
        for (int i = 0; i < 16; i++) stages[i] = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
        VkSubmitInfo bridge = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        if (s == 0 && pInfo->waitSemaphoreCount > 0) {
            bridge.waitSemaphoreCount = pInfo->waitSemaphoreCount;
            bridge.pWaitSemaphores    = pInfo->pWaitSemaphores;
            bridge.pWaitDstStageMask  = stages;
        }
        bridge.signalSemaphoreCount = 1;
        bridge.pSignalSemaphores = &sc->images[idx].vk_render_done;
        VkResult rb = dev->d.QueueSubmit(queue, 1, &bridge, VK_NULL_HANDLE);
        if (rb != VK_SUCCESS) {
            pthread_mutex_unlock(&sc->lock);
            LOG_ERR("QueuePresentKHR: bridge QueueSubmit failed: %d (queue=%p idx=%u waitSems=%u signal=%p)",
                    rb, (void*)queue, idx, pInfo->waitSemaphoreCount,
                    (void*)sc->images[idx].vk_render_done);
            if (pInfo->pResults) pInfo->pResults[s] = rb;
            if (overall == VK_SUCCESS) overall = rb;
            continue;
        }
        LOG_DBG("Present idx=%u: bridge submit ok", idx);

        /* GL side. */
        if (!make_current_locked(sc)) {
            pthread_mutex_unlock(&sc->lock);
            if (pInfo->pResults) pInfo->pResults[s] = VK_ERROR_INITIALIZATION_FAILED;
            if (overall == VK_SUCCESS) overall = VK_ERROR_INITIALIZATION_FAILED;
            continue;
        }

        /* Refresh window size and resize the child window to track the parent.
           Also update GL viewport to match. */
        uint32_t ww = 0, wh = 0; window_size(sc->surf, &ww, &wh);
        if (ww > 0 && wh > 0) {
            if ((int)ww != sc->win_w || (int)wh != sc->win_h) {
                XResizeWindow(sc->surf->dpy, sc->child_window, ww, wh);
                XFlush(sc->surf->dpy);
                sc->win_w = (int)ww; sc->win_h = (int)wh;
            }
            glViewport(0, 0, (GLsizei)ww, (GLsizei)wh);
        }

        GLenum srcLayouts[1] = { GL_LAYOUT_SHADER_READ_ONLY_EXT };
        sc->glWaitSemaphoreEXT(sc->images[idx].gl_render_sem, 0, NULL,
                               1, &sc->images[idx].gl_texture, srcLayouts);
        { GLenum e = glGetError(); if (e) LOG_ERR("glWaitSemaphoreEXT error: 0x%x", e); }

        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, sc->images[idx].gl_texture);
        glBindVertexArray(sc->vao);
        glUseProgram(sc->prog);
        glUniform1i(sc->u_tex, 0);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        { GLenum e = glGetError(); if (e) LOG_ERR("post-draw GL error: 0x%x", e); }

        GLenum dstLayouts[1] = { GL_LAYOUT_SHADER_READ_ONLY_EXT };
        sc->glSignalSemaphoreEXT(sc->images[idx].gl_sample_sem, 0, NULL,
                                 1, &sc->images[idx].gl_texture, dstLayouts);
        { GLenum e = glGetError(); if (e) LOG_ERR("glSignalSemaphoreEXT error: 0x%x", e); }
        glFlush();
        glXSwapBuffers(sc->surf->dpy, sc->child_window);
        unmake_current_locked(sc);

        sc->images[idx].acquired = false;

        if (pInfo->pResults) pInfo->pResults[s] = VK_SUCCESS;
        pthread_mutex_unlock(&sc->lock);
    }
    LOG_INFO("Present exit: ret=%d", overall);
    return overall;
}

/* ----------------------------------------------------------------------- */
/* Device / instance lifecycle                                             */
/* ----------------------------------------------------------------------- */

/* Forward decls. */
static PFN_vkVoidFunction layer_intercept_instance(const char *name);
static PFN_vkVoidFunction layer_intercept_device  (const char *name);
VK_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
layer_GetDeviceProcAddr(VkDevice dev, const char *name);
VK_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
layer_GetInstanceProcAddr(VkInstance inst, const char *name);

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
layer_CreateInstance(const VkInstanceCreateInfo *ci,
                      const VkAllocationCallbacks *pAlloc,
                      VkInstance *pInst) {
    VkLayerInstanceCreateInfo *lci = (VkLayerInstanceCreateInfo *)ci->pNext;
    while (lci && !(lci->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
                    lci->function == VK_LAYER_LINK_INFO))
        lci = (VkLayerInstanceCreateInfo *)lci->pNext;
    if (!lci) return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr next_gipa = lci->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    lci->u.pLayerInfo = lci->u.pLayerInfo->pNext;

    /* Like with CreateDevice, the app may not request the instance-level
       external memory/semaphore capability extensions. They're core in
       Vulkan 1.1 but still need to be listed if the app requested 1.0.
       Add them if the app didn't, harmlessly redundant if it did. */
    static const char *required_inst[] = {
        "VK_KHR_external_memory_capabilities",
        "VK_KHR_external_semaphore_capabilities",
        "VK_KHR_get_physical_device_properties2",
    };
    static const uint32_t n_required_inst = 3;
    uint32_t to_add = 0;
    bool need[3] = { true, true, true };
    for (uint32_t i = 0; i < ci->enabledExtensionCount; i++) {
        for (uint32_t j = 0; j < n_required_inst; j++) {
            if (need[j] && !strcmp(ci->ppEnabledExtensionNames[i], required_inst[j])) need[j] = false;
        }
    }
    for (uint32_t j = 0; j < n_required_inst; j++) if (need[j]) to_add++;

    VkInstanceCreateInfo modci = *ci;
    const char **new_exts = NULL;
    if (to_add > 0) {
        uint32_t total = ci->enabledExtensionCount + to_add;
        new_exts = malloc(total * sizeof(const char *));
        if (!new_exts) return VK_ERROR_OUT_OF_HOST_MEMORY;
        memcpy(new_exts, ci->ppEnabledExtensionNames,
               ci->enabledExtensionCount * sizeof(const char *));
        uint32_t k = ci->enabledExtensionCount;
        for (uint32_t j = 0; j < n_required_inst; j++) if (need[j]) new_exts[k++] = required_inst[j];
        modci.enabledExtensionCount = total;
        modci.ppEnabledExtensionNames = new_exts;
    }

    PFN_vkCreateInstance next_create = (PFN_vkCreateInstance)next_gipa(NULL, "vkCreateInstance");
    VkResult r = next_create(&modci, pAlloc, pInst);
    if (new_exts) free(new_exts);
    if (r != VK_SUCCESS) return r;

    InstNode *node = calloc(1, sizeof(*node));
    if (!node) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
    node->instance = *pInst;
    node->key = dispatch_key(*pInst);
#define I(name) node->d.name = (PFN_vk##name)next_gipa(*pInst, "vk" #name)
    I(GetInstanceProcAddr);
    I(DestroyInstance);
    I(EnumeratePhysicalDevices);
    I(GetPhysicalDeviceProperties);
    I(GetPhysicalDeviceMemoryProperties);
    I(GetPhysicalDeviceQueueFamilyProperties);
    I(CreateXlibSurfaceKHR);
    I(CreateXcbSurfaceKHR);
    I(DestroySurfaceKHR);
    I(GetPhysicalDeviceSurfaceSupportKHR);
    I(GetPhysicalDeviceSurfaceCapabilitiesKHR);
    I(GetPhysicalDeviceSurfaceFormatsKHR);
    I(GetPhysicalDeviceSurfacePresentModesKHR);
    I(GetPhysicalDeviceImageFormatProperties2);
    I(GetPhysicalDeviceExternalSemaphoreProperties);
#undef I
    inst_insert(node);

    layer_check_disabled();
    LOG_INFO("CreateInstance: inst=%p%s", *pInst, g_layer_disabled ? " (disabled)" : "");
    return VK_SUCCESS;
}

VK_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL
layer_DestroyInstance(VkInstance inst, const VkAllocationCallbacks *pAlloc) {
    InstNode *node = inst_lookup(dispatch_key(inst));
    if (!node) return;
    node->d.DestroyInstance(inst, pAlloc);
    inst_remove(dispatch_key(inst));
}

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
layer_CreateDevice(VkPhysicalDevice phys, const VkDeviceCreateInfo *ci,
                    const VkAllocationCallbacks *pAlloc, VkDevice *pDev) {
    VkLayerDeviceCreateInfo *lci = (VkLayerDeviceCreateInfo *)ci->pNext;
    while (lci && !(lci->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
                    lci->function == VK_LAYER_LINK_INFO))
        lci = (VkLayerDeviceCreateInfo *)lci->pNext;
    if (!lci) return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr next_gipa = lci->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr   next_gdpa = lci->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    lci->u.pLayerInfo = lci->u.pLayerInfo->pNext;

    /* The application's vkCreateDevice doesn't enable the extensions we
       depend on (VK_KHR_external_memory_fd, VK_KHR_external_semaphore_fd,
       etc.). We need them for the present path. Build a modified
       VkDeviceCreateInfo with our extensions appended if not already
       present, then call through with that. */
    static const char *required[] = {
        "VK_KHR_external_memory",
        "VK_KHR_external_memory_fd",
        "VK_KHR_external_semaphore",
        "VK_KHR_external_semaphore_fd",
        "VK_KHR_dedicated_allocation",
        "VK_KHR_get_memory_requirements2",
    };
    static const uint32_t n_required = sizeof(required) / sizeof(required[0]);

    /* Count how many of our required extensions the app didn't already enable. */
    uint32_t to_add = 0;
    bool need[6] = { true, true, true, true, true, true };
    for (uint32_t i = 0; i < ci->enabledExtensionCount; i++) {
        for (uint32_t j = 0; j < n_required; j++) {
            if (need[j] && !strcmp(ci->ppEnabledExtensionNames[i], required[j])) {
                need[j] = false;
            }
        }
    }
    for (uint32_t j = 0; j < n_required; j++) if (need[j]) to_add++;

    VkDeviceCreateInfo modci = *ci;
    const char **new_exts = NULL;
    if (to_add > 0) {
        uint32_t total = ci->enabledExtensionCount + to_add;
        new_exts = malloc(total * sizeof(const char *));
        if (!new_exts) return VK_ERROR_OUT_OF_HOST_MEMORY;
        memcpy(new_exts, ci->ppEnabledExtensionNames,
               ci->enabledExtensionCount * sizeof(const char *));
        uint32_t k = ci->enabledExtensionCount;
        for (uint32_t j = 0; j < n_required; j++) if (need[j]) new_exts[k++] = required[j];
        modci.enabledExtensionCount = total;
        modci.ppEnabledExtensionNames = new_exts;
        LOG_INFO("CreateDevice: appending %u required extensions", to_add);
    }

    PFN_vkCreateDevice next_create = (PFN_vkCreateDevice)next_gipa(VK_NULL_HANDLE, "vkCreateDevice");
    VkResult r = next_create(phys, &modci, pAlloc, pDev);
    if (new_exts) free(new_exts);
    if (r != VK_SUCCESS) {
        LOG_ERR("next CreateDevice failed: %d (extension support missing?)", r);
        return r;
    }

    InstNode *in = NULL;
    /* Find the instance whose physical device list contains 'phys'. We don't
       track that explicitly; just take the only instance in our table.
       This is brittle for multi-instance apps but matches our actual use. */
    pthread_mutex_lock(&g_inst_lock);
    for (int b = 0; b < HASH_BUCKETS && !in; b++)
        for (InstNode *n = g_inst_table[b]; n; n = n->next) { in = n; break; }
    pthread_mutex_unlock(&g_inst_lock);

    DevNode *node = calloc(1, sizeof(*node));
    if (!node) return VK_ERROR_OUT_OF_HOST_MEMORY;
    node->device = *pDev;
    node->phys = phys;
    node->key = dispatch_key(*pDev);
    node->idisp = in ? &in->d : NULL;
    node->inst = in ? in->instance : VK_NULL_HANDLE;

#define D(name) node->d.name = (PFN_vk##name)next_gdpa(*pDev, "vk" #name)
    D(GetDeviceProcAddr);
    D(DestroyDevice);
    D(DeviceWaitIdle);
    D(GetDeviceQueue);
    D(QueueSubmit);
    D(QueueWaitIdle);
    D(CreateImage);
    D(DestroyImage);
    D(GetImageMemoryRequirements);
    D(AllocateMemory);
    D(FreeMemory);
    D(BindImageMemory);
    D(CreateSemaphore);
    D(DestroySemaphore);
    D(CreateFence);
    D(DestroyFence);
    D(ResetFences);
    D(WaitForFences);
    D(GetFenceStatus);
    D(GetMemoryFdKHR);
    D(GetSemaphoreFdKHR);
    D(CreateSwapchainKHR);
    D(DestroySwapchainKHR);
    D(GetSwapchainImagesKHR);
    D(AcquireNextImageKHR);
    D(QueuePresentKHR);
#undef D

    /* Sanity check: any of these being NULL means the next layer / ICD
       didn't expose them, which means our present path can't function.
       Don't crash — set the disabled flag so subsequent swapchain hooks
       fall through to passthrough. */
    if (!node->d.GetMemoryFdKHR || !node->d.GetSemaphoreFdKHR
     || !node->d.CreateSwapchainKHR) {
        LOG_WARN("CreateDevice: required entrypoints missing (GetMemoryFdKHR=%p GetSemaphoreFdKHR=%p CreateSwapchainKHR=%p); disabling layer for this device",
                 (void*)node->d.GetMemoryFdKHR,
                 (void*)node->d.GetSemaphoreFdKHR,
                 (void*)node->d.CreateSwapchainKHR);
        g_layer_disabled = true;
    }

    if (in) in->d.GetPhysicalDeviceMemoryProperties(phys, &node->memp);

    /* Find the first graphics-capable queue family and cache it. */
    uint32_t nqf = 0;
    in->d.GetPhysicalDeviceQueueFamilyProperties(phys, &nqf, NULL);
    VkQueueFamilyProperties *qfs = calloc(nqf, sizeof(*qfs));
    in->d.GetPhysicalDeviceQueueFamilyProperties(phys, &nqf, qfs);
    node->graphics_qfi = 0;
    for (uint32_t i = 0; i < nqf; i++)
        if (qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { node->graphics_qfi = i; break; }
    free(qfs);
    node->d.GetDeviceQueue(*pDev, node->graphics_qfi, 0, &node->graphics_queue);

    dev_insert(node);
    LOG_INFO("CreateDevice: dev=%p qfi=%u", *pDev, node->graphics_qfi);
    return VK_SUCCESS;
}

VK_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL
layer_DestroyDevice(VkDevice dev, const VkAllocationCallbacks *pAlloc) {
    DevNode *node = dev_lookup(dispatch_key(dev));
    if (!node) return;
    node->d.DestroyDevice(dev, pAlloc);
    dev_remove(dispatch_key(dev));
}

/* ----------------------------------------------------------------------- */
/* Entrypoint dispatch                                                     */
/* ----------------------------------------------------------------------- */

static PFN_vkVoidFunction layer_intercept_instance(const char *name) {
#define MATCH(n) if (!strcmp(name, "vk" #n)) return (PFN_vkVoidFunction)layer_##n
    MATCH(GetInstanceProcAddr);
    MATCH(CreateInstance);
    MATCH(DestroyInstance);
    MATCH(CreateDevice);
    MATCH(CreateXlibSurfaceKHR);
    MATCH(CreateXcbSurfaceKHR);
    MATCH(DestroySurfaceKHR);
    MATCH(GetPhysicalDeviceSurfaceSupportKHR);
    MATCH(GetPhysicalDeviceSurfaceCapabilitiesKHR);
    MATCH(GetPhysicalDeviceSurfaceFormatsKHR);
    MATCH(GetPhysicalDeviceSurfacePresentModesKHR);
#undef MATCH
    return NULL;
}

static PFN_vkVoidFunction layer_intercept_device(const char *name) {
#define MATCH(n) if (!strcmp(name, "vk" #n)) return (PFN_vkVoidFunction)layer_##n
    MATCH(GetDeviceProcAddr);
    MATCH(DestroyDevice);
    MATCH(CreateSwapchainKHR);
    MATCH(DestroySwapchainKHR);
    MATCH(GetSwapchainImagesKHR);
    MATCH(AcquireNextImageKHR);
    MATCH(QueuePresentKHR);
    MATCH(QueueSubmit);
    MATCH(QueueWaitIdle);
    MATCH(DeviceWaitIdle);
    MATCH(WaitForFences);
    MATCH(ResetFences);
    MATCH(CmdPipelineBarrier);
    MATCH(EndCommandBuffer);
#undef MATCH
    return NULL;
}

VK_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
layer_GetDeviceProcAddr(VkDevice dev, const char *name) {
    PFN_vkVoidFunction fn = layer_intercept_device(name);
    if (fn) return fn;
    DevNode *node = dev_lookup(dispatch_key(dev));
    if (!node) return NULL;
    return node->d.GetDeviceProcAddr(dev, name);
}

VK_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
layer_GetInstanceProcAddr(VkInstance inst, const char *name) {
    PFN_vkVoidFunction fn = layer_intercept_instance(name);
    if (fn) return fn;
    fn = layer_intercept_device(name);
    if (fn) return fn;
    if (!inst) return NULL;
    InstNode *node = inst_lookup(dispatch_key(inst));
    if (!node) return NULL;
    return node->d.GetInstanceProcAddr(inst, name);
}

/* The Vulkan loader looks up these names by exact match. */
VK_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance inst, const char *name) {
    return layer_GetInstanceProcAddr(inst, name);
}
VK_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice dev, const char *name) {
    return layer_GetDeviceProcAddr(dev, name);
}

/* ----------------------------------------------------------------------- */
/* Loader negotiation (Vulkan 1.1+ layer interface v2)                     */
/* ----------------------------------------------------------------------- */

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *pInterface) {
    if (pInterface->loaderLayerInterfaceVersion < 2) return VK_ERROR_INITIALIZATION_FAILED;
    pInterface->loaderLayerInterfaceVersion = 2;
    pInterface->pfnGetInstanceProcAddr      = vkGetInstanceProcAddr;
    pInterface->pfnGetDeviceProcAddr        = vkGetDeviceProcAddr;
    pInterface->pfnGetPhysicalDeviceProcAddr = NULL;
    layer_log_init();
    /* Loud one-time banner so a tester running an older cached binary can spot
       the version mismatch immediately. Bump when the layer's behaviour changes. */
    fprintf(stderr, "[" LAYER_NAME "] loaded, build %s %s (negotiated interface v2)\n",
            __DATE__, __TIME__);
    return VK_SUCCESS;
}
