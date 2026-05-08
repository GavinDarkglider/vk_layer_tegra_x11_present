/*
 * tegra_gbm.c -- dlopen wrapper for libnvgbm.so (or libgbm.so.1 fallback).
 *
 * libnvgbm exports the standard public GBM API, so signatures are stable
 * and unguessed. The wrapper exists so the xorg module has no link-time
 * dependency on libgbm, in case the build host doesn't ship gbm headers.
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "tegra_present.h"

/* Forward decls -- no gbm.h include, to avoid build-host header dependency. */
struct gbm_device;
struct gbm_bo;

typedef struct gbm_device *(*pfn_gbm_create_device)(int fd);
typedef void               (*pfn_gbm_device_destroy)(struct gbm_device *);
typedef struct gbm_bo     *(*pfn_gbm_bo_create)(struct gbm_device *,
                                                uint32_t w, uint32_t h,
                                                uint32_t format,
                                                uint32_t flags);
typedef struct gbm_bo     *(*pfn_gbm_bo_import)(struct gbm_device *,
                                                uint32_t type, void *buffer,
                                                uint32_t flags);
typedef void               (*pfn_gbm_bo_destroy)(struct gbm_bo *);
typedef int                (*pfn_gbm_bo_get_fd)(struct gbm_bo *);
typedef uint32_t           (*pfn_gbm_bo_get_stride)(struct gbm_bo *);
typedef uint32_t           (*pfn_gbm_bo_get_width)(struct gbm_bo *);
typedef uint32_t           (*pfn_gbm_bo_get_height)(struct gbm_bo *);
typedef uint32_t           (*pfn_gbm_bo_get_format)(struct gbm_bo *);
typedef uint64_t           (*pfn_gbm_bo_get_modifier)(struct gbm_bo *);

struct tegra_gbm {
    void *dl;
    pfn_gbm_create_device    create_device;
    pfn_gbm_device_destroy   device_destroy;
    pfn_gbm_bo_create        bo_create;
    pfn_gbm_bo_import        bo_import;
    pfn_gbm_bo_destroy       bo_destroy;
    pfn_gbm_bo_get_fd        bo_get_fd;
    pfn_gbm_bo_get_stride    bo_get_stride;
    pfn_gbm_bo_get_width     bo_get_width;
    pfn_gbm_bo_get_height    bo_get_height;
    pfn_gbm_bo_get_format    bo_get_format;
    pfn_gbm_bo_get_modifier  bo_get_modifier;
};

#define G_TRY(target, name) do {                                \
    *(void **)&(target) = dlsym(g->dl, name);                   \
    if (!(target)) {                                            \
        TLOG("libnvgbm: missing symbol %s", name);              \
        goto fail;                                              \
    }                                                           \
} while (0)

struct tegra_gbm *tegra_gbm_open(void)
{
    struct tegra_gbm *g = calloc(1, sizeof(*g));
    if (!g) return NULL;

    /* Prefer libnvgbm directly; fall back to libgbm.so.1 in case the
     * system has the OE4T shim wired in differently. */
    g->dl = dlopen("libnvgbm.so", RTLD_NOW | RTLD_LOCAL);
    if (!g->dl) g->dl = dlopen("/usr/lib/libnvgbm.so", RTLD_NOW | RTLD_LOCAL);
    if (!g->dl) g->dl = dlopen("libgbm.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!g->dl) {
        TLOG("gbm: dlopen failed: %s", dlerror());
        goto fail;
    }

    G_TRY(g->create_device,    "gbm_create_device");
    G_TRY(g->device_destroy,   "gbm_device_destroy");
    G_TRY(g->bo_create,        "gbm_bo_create");
    G_TRY(g->bo_import,        "gbm_bo_import");
    G_TRY(g->bo_destroy,       "gbm_bo_destroy");
    G_TRY(g->bo_get_fd,        "gbm_bo_get_fd");
    G_TRY(g->bo_get_stride,    "gbm_bo_get_stride");
    G_TRY(g->bo_get_width,     "gbm_bo_get_width");
    G_TRY(g->bo_get_height,    "gbm_bo_get_height");
    G_TRY(g->bo_get_format,    "gbm_bo_get_format");
    G_TRY(g->bo_get_modifier,  "gbm_bo_get_modifier");

    TLOG("gbm: loaded successfully");
    return g;

fail:
    if (g->dl) dlclose(g->dl);
    free(g);
    return NULL;
}

void tegra_gbm_close(struct tegra_gbm *g)
{
    if (!g) return;
    if (g->dl) dlclose(g->dl);
    free(g);
}

/* --- Accessors used by tegra_present.c --------------------------------- */

struct gbm_device *tegra_gbm_dev_create(struct tegra_gbm *g, int drm_fd)
{
    if (!g || !g->create_device) return NULL;
    return g->create_device(drm_fd);
}

void tegra_gbm_dev_destroy(struct tegra_gbm *g, struct gbm_device *dev)
{
    if (!g || !g->device_destroy || !dev) return;
    g->device_destroy(dev);
}
