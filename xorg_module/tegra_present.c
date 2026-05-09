/*
 * tegra_present.c -- Xorg extension module entry.
 *
 * Loaded via "Module" section in xorg.conf. Walks the screen list at
 * extension-init time, attaches per-screen state to nvidia-driven
 * screens, opens libnvdc and libnvgbm.
 *
 * Piece 1: just the skeleton. DRI3 and Present hooks are stubbed.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "xorg-server.h"
#include "xf86.h"
#include "xf86Module.h"
#include "scrnintstr.h"

#include "tegra_present.h"

/* --- Logging ------------------------------------------------------------ */

void tegra_log(int verb, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    xf86MsgVerb(X_INFO, verb, "tegra-present: %s\n", buf);
}

/* --- Screen / pixmap private keys -------------------------------------- */

static DevPrivateKeyRec tegra_screen_key_rec;
#define tegra_screen_key (&tegra_screen_key_rec)

static DevPrivateKeyRec tegra_pixmap_key_rec;
#define tegra_pixmap_key (&tegra_pixmap_key_rec)

tegra_screen_t *tegra_screen_get(ScreenPtr screen)
{
    return dixLookupPrivate(&screen->devPrivates, tegra_screen_key);
}

static void tegra_screen_set(ScreenPtr screen, tegra_screen_t *ts)
{
    dixSetPrivate(&screen->devPrivates, tegra_screen_key, ts);
}

tegra_pixmap_t *tegra_pixmap_get(PixmapPtr pixmap)
{
    return dixLookupPrivate(&pixmap->devPrivates, tegra_pixmap_key);
}

/* --- CloseScreen hook (teardown) -------------------------------------- */

static CloseScreenProcPtr saved_CloseScreen;

static Bool tegra_close_screen(ScreenPtr screen)
{
    tegra_screen_t *ts = tegra_screen_get(screen);

    if (ts) {
        TLOG("closing screen %d", screen->myNum);
        if (ts->nvdc_dev) tegra_nvdc_dev_close(ts->nvdc, ts->nvdc_dev);
        if (ts->gbm_dev)  tegra_gbm_dev_destroy(ts->gbm, ts->gbm_dev);
        if (ts->drm_fd >= 0) close(ts->drm_fd);
        tegra_nvdc_close(ts->nvdc);
        tegra_gbm_close(ts->gbm);
        free(ts);
        tegra_screen_set(screen, NULL);
    }

    screen->CloseScreen = saved_CloseScreen;
    return screen->CloseScreen(screen);
}

/* --- Per-screen attach ------------------------------------------------- */

/* Try a few likely device paths for libnvgbm. /dev/dri/card0 is the
 * obvious one but tegra-udrm is a stub; libnvgbm may want a different
 * node or may accept any of these. */
static const char *gbm_device_candidates[] = {
    "/dev/dri/card0",
    "/dev/dri/renderD128",
    "/dev/nvmap",
    NULL,
};

static struct gbm_device *
try_gbm_create(struct tegra_gbm *gbm, int *out_fd, const char **out_path)
{
    for (int i = 0; gbm_device_candidates[i]; ++i) {
        int fd = open(gbm_device_candidates[i], O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            TLOG_V("gbm: open(%s) failed: errno=%d (%s)",
                   gbm_device_candidates[i], errno, strerror(errno));
            continue;
        }
        struct gbm_device *dev = tegra_gbm_dev_create(gbm, fd);
        if (dev) {
            *out_fd   = fd;
            *out_path = gbm_device_candidates[i];
            return dev;
        }
        TLOG_V("gbm: gbm_create_device on %s failed",
               gbm_device_candidates[i]);
        close(fd);
    }
    return NULL;
}

static Bool tegra_setup_screen(ScreenPtr screen)
{
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
    tegra_screen_t *ts;

    /* Already attached? */
    if (tegra_screen_get(screen)) {
        TLOG_V("screen %d already attached, skipping", screen->myNum);
        return TRUE;
    }

    if (!scrn || !scrn->driverName ||
        strcmp(scrn->driverName, "nvidia") != 0) {
        TLOG_V("skipping screen %d (driver=%s)",
               screen->myNum,
               scrn ? (scrn->driverName ? scrn->driverName : "(null)")
                    : "(no scrn)");
        return TRUE;
    }

    TLOG("attaching to screen %d (nvidia)", screen->myNum);

    ts = calloc(1, sizeof(*ts));
    if (!ts) return FALSE;

    ts->screen         = screen;
    ts->drm_fd         = -1;
    ts->claimed_head   = -1;
    ts->claimed_window = -1;
    ts->vblank_fd      = -1;

    ts->nvdc = tegra_nvdc_open();
    ts->gbm  = tegra_gbm_open();
    if (!ts->nvdc || !ts->gbm) {
        TLOG("library open failed (nvdc=%p gbm=%p) -- module disabled",
             (void*)ts->nvdc, (void*)ts->gbm);
        goto fail;
    }

    /* Try several device paths for the GBM device. If all fail, log it
     * but continue -- piece 1 doesn't actually need GBM yet, and we
     * want the rest of the bring-up to proceed so we can see other
     * errors. */
    {
        const char *gbm_path = NULL;
        ts->gbm_dev = try_gbm_create(ts->gbm, &ts->drm_fd, &gbm_path);
        if (ts->gbm_dev) {
            TLOG("gbm_create_device succeeded with %s (fd=%d)",
                 gbm_path, ts->drm_fd);
        } else {
            TLOG("gbm_create_device failed on all candidate paths "
                 "(piece 2 will need this fixed)");
        }
    }

    /* Open libnvdc. The path argument is a guess. */
    ts->nvdc_dev = tegra_nvdc_dev_open(ts->nvdc, "/dev/tegra_dc.0");
    if (!ts->nvdc_dev)
        ts->nvdc_dev = tegra_nvdc_dev_open(ts->nvdc, NULL);
    if (!ts->nvdc_dev) {
        TLOG("nvdcOpen failed (tried /dev/tegra_dc.0 and NULL)");
        goto fail;
    }

    tegra_screen_set(screen, ts);

    saved_CloseScreen   = screen->CloseScreen;
    screen->CloseScreen = tegra_close_screen;

    TLOG("screen %d ready (DRI3/Present registration pending in piece 2)",
         screen->myNum);
    return TRUE;

fail:
    if (ts->nvdc_dev) tegra_nvdc_dev_close(ts->nvdc, ts->nvdc_dev);
    if (ts->gbm_dev)  tegra_gbm_dev_destroy(ts->gbm, ts->gbm_dev);
    if (ts->drm_fd >= 0) close(ts->drm_fd);
    tegra_nvdc_close(ts->nvdc);
    tegra_gbm_close(ts->gbm);
    free(ts);
    return FALSE;
}

/* --- Module init ------------------------------------------------------- */

/* Walk all screens, attaching to nvidia ones. Called from the deferred
 * hook below, after Xorg has finished ScreenInit on every screen. */
static void tegra_attach_all_screens(void)
{
    int n = 0;
    for (int i = 0; xf86Screens && xf86Screens[i]; ++i) {
        ScrnInfoPtr scrn = xf86Screens[i];
        if (scrn && scrn->pScreen) {
            tegra_setup_screen(scrn->pScreen);
            ++n;
        }
    }
    TLOG("attach pass: walked %d screen(s)", n);
}

static Bool tegra_attached = FALSE;

static void tegra_attach_once(const char *trigger)
{
    if (tegra_attached) return;
    tegra_attached = TRUE;
    TLOG("attach trigger: %s", trigger);
    tegra_attach_all_screens();
}

/* ClientStateCallback fires on every client state transition. The very
 * first transition (initial connection of the privileged serverClient,
 * if nothing else) happens after all screens are ready. */
static void tegra_client_state_cb(CallbackListPtr *list, void *closure,
                                  void *data)
{
    if (!tegra_attached && xf86Screens && xf86Screens[0])
        tegra_attach_once("ClientStateCallback");
}

/* ServerGrabCallback as a backup, in case some session does grab. */
static void tegra_server_grab_cb(CallbackListPtr *list, void *closure,
                                 void *data)
{
    if (!tegra_attached && xf86Screens && xf86Screens[0])
        tegra_attach_once("ServerGrabCallback");
}

static void tegra_extension_init(void)
{
    if (!dixRegisterPrivateKey(tegra_screen_key, PRIVATE_SCREEN, 0) ||
        !dixRegisterPrivateKey(tegra_pixmap_key, PRIVATE_PIXMAP, 0)) {
        TLOG("dixRegisterPrivateKey failed");
        return;
    }

    TLOG("extension init (deferring screen attach until first client connects)");

    AddCallback(&ClientStateCallback, tegra_client_state_cb, NULL);
    AddCallback(&ServerGrabCallback,  tegra_server_grab_cb,  NULL);
}

/* --- Boilerplate ------------------------------------------------------- */

static XF86ModuleVersionInfo tegrapresent_vers = {
    "tegrapresent", MODULEVENDORSTRING, MODINFOSTRING1, MODINFOSTRING2,
    XORG_VERSION_CURRENT, 1, 0, 0,
    ABI_CLASS_EXTENSION, ABI_EXTENSION_VERSION,
    MOD_CLASS_EXTENSION, { 0, 0, 0, 0 }
};

static pointer tegrapresent_setup(pointer module, pointer opts,
                                  int *errmaj, int *errmin)
{
    static Bool done = FALSE;
    if (done) {
        if (errmaj) *errmaj = LDR_ONCEONLY;
        return NULL;
    }
    done = TRUE;
    tegra_extension_init();
    return (pointer)1;
}

_X_EXPORT XF86ModuleData tegrapresentModuleData = {
    &tegrapresent_vers, tegrapresent_setup, NULL
};
