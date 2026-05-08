/*
 * tegra_nvdc.c -- dlopen wrapper for libnvdc.so.
 *
 * Symbols are confirmed via `nm -D` against the L4T r32.x libnvdc.so.
 * Argument types are guessed (no public header for libnvdc on L4T) and
 * isolated to the typedef block below. If first-link reveals different
 * signatures, edit only the typedefs and the wrapper call sites in
 * piece 3 (the Present hooks).
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "tegra_present.h"

/* --- Function-pointer signature guesses ---------------------------------- */

typedef void * nvdc_handle_t;       /* device handle from nvdcOpen */
typedef void * nvdc_displays_t;     /* opaque list from nvdcQueryDisplays */
typedef void * nvdc_window_args_t;  /* opaque PutWindow input */
typedef void * nvdc_flip_args_t;    /* opaque Flip / Flip2 input */

/* Lifecycle. */
typedef nvdc_handle_t (*pfn_nvdcOpen)(const char *path);
typedef void          (*pfn_nvdcClose)(nvdc_handle_t);

/* Display + head enumeration. */
typedef int  (*pfn_nvdcQueryNumHeads)  (nvdc_handle_t dev, int *out_n);
typedef int  (*pfn_nvdcQueryDisplays)  (nvdc_handle_t dev,
                                        nvdc_displays_t *out_list,
                                        int *out_count);
typedef void (*pfn_nvdcFreeDisplays)   (nvdc_displays_t list);
typedef int  (*pfn_nvdcQueryHeadStatus)(nvdc_handle_t dev, int head,
                                        void *out_status);
typedef int  (*pfn_nvdcGetCapabilities)(nvdc_handle_t dev, int head,
                                        void *out_caps);

/* Window (plane) management. */
typedef int  (*pfn_nvdcGetWinmask)(nvdc_handle_t dev, int head,
                                   uint32_t *out_mask);
typedef int  (*pfn_nvdcSetWinmask)(nvdc_handle_t dev, int head,
                                   uint32_t mask);
typedef int  (*pfn_nvdcGetWindow) (nvdc_handle_t dev, int head, int win,
                                   nvdc_window_args_t out);
typedef int  (*pfn_nvdcPutWindow) (nvdc_handle_t dev, int head, int win,
                                   nvdc_window_args_t in);

/* Flip submission. */
typedef int  (*pfn_nvdcFlip)            (nvdc_handle_t dev, int head,
                                         nvdc_flip_args_t args);
typedef int  (*pfn_nvdcFlipSyncFd)      (nvdc_handle_t dev, int head,
                                         nvdc_flip_args_t args,
                                         int wait_sync_fd,
                                         int *out_present_sync_fd);
typedef int  (*pfn_nvdcFlipSyncFd2)     (nvdc_handle_t dev, int head,
                                         nvdc_flip_args_t args,
                                         int wait_sync_fd,
                                         int *out_present_sync_fd);
typedef int  (*pfn_nvdcFlipProposeSafe) (nvdc_handle_t dev, int head,
                                         nvdc_flip_args_t args);

/* Vblank. */
typedef int  (*pfn_nvdcEnableVblank)     (nvdc_handle_t dev, int head,
                                          int enable);
typedef int  (*pfn_nvdcQueryVblankSyncpt)(nvdc_handle_t dev, int head,
                                          uint32_t *out_id,
                                          uint32_t *out_threshold);

/* Event delivery (fd-based, preferred over async callback path). */
typedef int  (*pfn_nvdcEventFds)    (nvdc_handle_t dev, int **out_fds,
                                     int *out_count);
typedef void (*pfn_nvdcFreeEventFds)(int *fds);
typedef int  (*pfn_nvdcEventData)   (nvdc_handle_t dev, int fd,
                                     int *out_event_type,
                                     void *out_event_payload,
                                     size_t payload_size);

/* --- Wrapper struct ------------------------------------------------------ */

struct tegra_nvdc {
    void *dl;

    pfn_nvdcOpen              Open;
    pfn_nvdcClose             Close;

    pfn_nvdcQueryNumHeads     QueryNumHeads;
    pfn_nvdcQueryDisplays     QueryDisplays;
    pfn_nvdcFreeDisplays      FreeDisplays;
    pfn_nvdcQueryHeadStatus   QueryHeadStatus;
    pfn_nvdcGetCapabilities   GetCapabilities;

    pfn_nvdcGetWinmask        GetWinmask;
    pfn_nvdcSetWinmask        SetWinmask;
    pfn_nvdcGetWindow         GetWindow;
    pfn_nvdcPutWindow         PutWindow;

    pfn_nvdcFlip              Flip;
    pfn_nvdcFlipSyncFd        FlipSyncFd;
    pfn_nvdcFlipSyncFd2       FlipSyncFd2;
    pfn_nvdcFlipProposeSafe   FlipProposeSafe;

    pfn_nvdcEnableVblank      EnableVblank;
    pfn_nvdcQueryVblankSyncpt QueryVblankSyncpt;

    pfn_nvdcEventFds          EventFds;
    pfn_nvdcFreeEventFds      FreeEventFds;
    pfn_nvdcEventData         EventData;
};

#define TRY_SYM(target, name) do {                              \
    *(void **)&(target) = dlsym(n->dl, name);                   \
    if (!(target)) {                                            \
        TLOG("libnvdc: missing symbol %s", name);               \
        goto fail;                                              \
    }                                                           \
} while (0)

#define TRY_SYM_OPT(target, name) do {                          \
    *(void **)&(target) = dlsym(n->dl, name);                   \
    if (!(target))                                              \
        TLOG_V("libnvdc: optional symbol %s absent", name);     \
} while (0)

struct tegra_nvdc *tegra_nvdc_open(void)
{
    struct tegra_nvdc *n = calloc(1, sizeof(*n));
    if (!n) return NULL;

    n->dl = dlopen("libnvdc.so", RTLD_NOW | RTLD_LOCAL);
    if (!n->dl) {
        n->dl = dlopen("/usr/lib/libnvdc.so", RTLD_NOW | RTLD_LOCAL);
    }
    if (!n->dl) {
        TLOG("libnvdc: dlopen failed: %s", dlerror());
        goto fail;
    }

    /* Lifecycle. */
    TRY_SYM(n->Open,  "nvdcOpen");
    TRY_SYM(n->Close, "nvdcClose");

    /* Enumeration. */
    TRY_SYM(n->QueryNumHeads,   "nvdcQueryNumHeads");
    TRY_SYM(n->QueryDisplays,   "nvdcQueryDisplays");
    TRY_SYM(n->FreeDisplays,    "nvdcFreeDisplays");
    TRY_SYM(n->QueryHeadStatus, "nvdcQueryHeadStatus");
    TRY_SYM(n->GetCapabilities, "nvdcGetCapabilities");

    /* Window / plane. */
    TRY_SYM(n->GetWinmask, "nvdcGetWinmask");
    TRY_SYM(n->SetWinmask, "nvdcSetWinmask");
    TRY_SYM(n->GetWindow,  "nvdcGetWindow");
    TRY_SYM(n->PutWindow,  "nvdcPutWindow");

    /* Flip. */
    TRY_SYM(n->Flip,                "nvdcFlip");
    TRY_SYM(n->FlipSyncFd,          "nvdcFlipSyncFd");
    TRY_SYM_OPT(n->FlipSyncFd2,     "nvdcFlipSyncFd2");
    TRY_SYM_OPT(n->FlipProposeSafe, "nvdcFlipProposeSafe");

    /* Vblank. */
    TRY_SYM(n->EnableVblank,      "nvdcEnableVblank");
    TRY_SYM(n->QueryVblankSyncpt, "nvdcQueryVblankSyncpt");

    /* Events. */
    TRY_SYM(n->EventFds,     "nvdcEventFds");
    TRY_SYM(n->FreeEventFds, "nvdcFreeEventFds");
    TRY_SYM(n->EventData,    "nvdcEventData");

    TLOG("libnvdc: loaded successfully");
    return n;

fail:
    if (n->dl) dlclose(n->dl);
    free(n);
    return NULL;
}

void tegra_nvdc_close(struct tegra_nvdc *n)
{
    if (!n) return;
    if (n->dl) dlclose(n->dl);
    free(n);
}

/* --- Accessors used by tegra_present.c --------------------------------- */

void *tegra_nvdc_dev_open(struct tegra_nvdc *n, const char *path)
{
    if (!n || !n->Open) return NULL;
    return n->Open(path);
}

void tegra_nvdc_dev_close(struct tegra_nvdc *n, void *dev)
{
    if (!n || !n->Close || !dev) return;
    n->Close(dev);
}
