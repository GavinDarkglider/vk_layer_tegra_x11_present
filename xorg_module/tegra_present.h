/*
 * tegra_present.h -- shared types for the Xorg extension module.
 */
#ifndef TEGRA_PRESENT_H
#define TEGRA_PRESENT_H

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include "xorg-server.h"
#include "scrnintstr.h"
#include "screenint.h"
#include "pixmapstr.h"

struct tegra_nvdc;
struct tegra_gbm;
struct gbm_bo;
struct gbm_device;

/* Per-screen state, keyed off the ScreenPtr's devPrivates. */
typedef struct tegra_screen {
    ScreenPtr          screen;

    struct tegra_nvdc *nvdc;
    struct tegra_gbm  *gbm;

    void              *nvdc_dev;   /* opaque libnvdc device handle */
    struct gbm_device *gbm_dev;    /* libnvgbm device */
    int                drm_fd;     /* /dev/dri/card0 */

    /* Filled in by piece 2/3: */
    int                claimed_head;     /* CRTC index, -1 if unclaimed */
    int                claimed_window;   /* plane index on that head, -1 */
    int                vblank_fd;        /* poll fd for vblank events, -1 */

    bool               dri3_registered;
    bool               present_registered;
} tegra_screen_t;

/* Per-pixmap state for DRI3-imported buffers, keyed off PixmapPtr. */
typedef struct tegra_pixmap {
    struct gbm_bo *bo;
    int            dmabuf_fd;
    uint32_t       width, height, stride;
    uint32_t       format;
    uint64_t       modifier;
} tegra_pixmap_t;

/* Lookup helpers. */
tegra_screen_t *tegra_screen_get(ScreenPtr screen);
tegra_pixmap_t *tegra_pixmap_get(PixmapPtr pixmap);

/* Library lifecycle. Defined in tegra_nvdc.c and tegra_gbm.c. */
struct tegra_nvdc *tegra_nvdc_open(void);
void               tegra_nvdc_close(struct tegra_nvdc *);

struct tegra_gbm  *tegra_gbm_open(void);
void               tegra_gbm_close(struct tegra_gbm *);

/* Accessors that hide the internal struct layout from callers.
 * These let tegra_present.c invoke library functions without seeing the
 * function-pointer struct definitions in tegra_nvdc.c / tegra_gbm.c. */
void *tegra_nvdc_dev_open (struct tegra_nvdc *, const char *path);
void  tegra_nvdc_dev_close(struct tegra_nvdc *, void *dev);

struct gbm_device *tegra_gbm_dev_create (struct tegra_gbm *, int drm_fd);
void               tegra_gbm_dev_destroy(struct tegra_gbm *,
                                         struct gbm_device *dev);

/* Logging. */
void tegra_log(int verb, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#define TLOG(...)   tegra_log(1, __VA_ARGS__)
#define TLOG_V(...) tegra_log(3, __VA_ARGS__)

#endif /* TEGRA_PRESENT_H */
