# Makefile for VK_LAYER_TEGRA_x11_present
#
# Build:    make
# Install:  sudo make install
# Clean:    make clean
#
# Builds two cooperating components from one tree:
#   - libVkLayer_tegra_x11_present.so : the Vulkan implicit layer
#   - tegra_present.so                : Xorg extension module that provides
#                                       DRI3 + Present hooks the layer needs
#
# The Xorg module is built only when xorg-server development headers are
# available (pkg-config --exists xorg-server). If they're absent, the layer
# still builds; the module silently skips.

CC      ?= gcc
CFLAGS  += -O2 -Wall -Wno-unused-function -fPIC -fvisibility=hidden -pthread
LDFLAGS += -shared -Wl,--no-undefined

LIBS = $(shell pkg-config --libs xcb xcb-dri3 xcb-present x11-xcb 2>/dev/null)
CFLAGS += $(shell pkg-config --cflags xcb xcb-dri3 xcb-present x11-xcb 2>/dev/null)

# Fallback if pkg-config is unavailable
ifeq ($(LIBS),)
LIBS = -lxcb -lxcb-dri3 -lxcb-present -lX11-xcb -lpthread
endif

PREFIX     ?= /usr
LIBDIR     ?= $(PREFIX)/lib/aarch64-linux-gnu
JSONDIR    ?= /etc/vulkan/implicit_layer.d
XORGCONFD  ?= /etc/X11/xorg.conf.d

# Path where Xorg loads extension modules from. Use pkg-config when
# possible; fall back to the conventional path.
XORG_MODULE_DIR ?= $(shell pkg-config --variable=moduledir xorg-server 2>/dev/null)/extensions
ifeq ($(XORG_MODULE_DIR),/extensions)
XORG_MODULE_DIR = $(PREFIX)/lib/xorg/modules/extensions
endif

LAYER_SO   = libVkLayer_tegra_x11_present.so
LAYER_JSON = VkLayer_tegra_x11_present.json
LAYER_SRC  = vk_layer_tegra_x11_present.c

# --- Xorg module ---------------------------------------------------------
# Detect whether xorg-server development headers are present. Both the
# build (build-XORG_MODULE) and install (install-XORG_MODULE) targets are
# driven off this flag.
HAVE_XORG_SERVER := $(shell pkg-config --exists xorg-server && echo yes)

XORG_MODULE_SO   = xorg_module/tegrapresent.so
XORG_MODULE_SRCS = xorg_module/tegra_present.c \
                   xorg_module/tegra_nvdc.c    \
                   xorg_module/tegra_gbm.c
XORG_MODULE_HDRS = xorg_module/tegra_present.h
XORG_CONF        = packaging/20-tegra-present.conf

XORG_CFLAGS  = $(shell pkg-config --cflags xorg-server 2>/dev/null) \
               -O2 -Wall -fPIC -fvisibility=hidden
# NOTE: do NOT pass --no-undefined here. Xorg modules have undefined
# symbols by design -- they are resolved at module-load time against the
# Xorg server binary itself (xf86MsgVerb, dixRegisterPrivateKey,
# xf86Screens, etc. live in the Xorg executable, not in a library).
XORG_LDFLAGS = -shared -Wl,-Bsymbolic-functions -ldl

ifeq ($(HAVE_XORG_SERVER),yes)
all: $(LAYER_SO) $(XORG_MODULE_SO)
else
all: $(LAYER_SO)
	@echo "NOTE: xorg-server headers not found via pkg-config;"
	@echo "      skipping xorg_module build. Install xorg-server-devel"
	@echo "      (or equivalent) and rerun 'make' to build it."
endif

$(LAYER_SO): $(LAYER_SRC)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIBS)

# Build the Xorg module as one unit. Each source file is compiled with
# xorg-server's include flags. We don't link against libnvdc/libnvgbm
# at build time -- they're dlopen'd at runtime.
$(XORG_MODULE_SO): $(XORG_MODULE_SRCS) $(XORG_MODULE_HDRS)
	$(CC) $(XORG_CFLAGS) -Ixorg_module $(XORG_MODULE_SRCS) \
	      $(XORG_LDFLAGS) -o $@

install: install-layer
ifeq ($(HAVE_XORG_SERVER),yes)
install: install-xorg-module
endif

install-layer: $(LAYER_SO) $(LAYER_JSON)
	install -d $(DESTDIR)$(LIBDIR)
	install -m 0755 $(LAYER_SO) $(DESTDIR)$(LIBDIR)/
	install -d $(DESTDIR)$(JSONDIR)
	install -m 0644 $(LAYER_JSON) $(DESTDIR)$(JSONDIR)/

install-xorg-module: $(XORG_MODULE_SO) $(XORG_CONF)
	install -d $(DESTDIR)$(XORG_MODULE_DIR)
	install -m 0755 $(XORG_MODULE_SO) $(DESTDIR)$(XORG_MODULE_DIR)/
	install -d $(DESTDIR)$(XORGCONFD)
	install -m 0644 $(XORG_CONF) $(DESTDIR)$(XORGCONFD)/

uninstall:
	rm -f $(DESTDIR)$(LIBDIR)/$(LAYER_SO)
	rm -f $(DESTDIR)$(JSONDIR)/$(LAYER_JSON)
	rm -f $(DESTDIR)$(XORG_MODULE_DIR)/tegrapresent.so
	rm -f $(DESTDIR)$(XORGCONFD)/20-tegra-present.conf

clean:
	rm -f $(LAYER_SO)
	rm -f $(XORG_MODULE_SO)

.PHONY: all install install-layer install-xorg-module uninstall clean
