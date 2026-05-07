# Makefile for VK_LAYER_TEGRA_x11_present
#
# Build:    make
# Install:  sudo make install
# Clean:    make clean

CC      ?= gcc
CFLAGS  += -O2 -Wall -Wno-unused-function -fPIC -fvisibility=hidden -pthread
LDFLAGS += -shared -Wl,--no-undefined

LIBS = $(shell pkg-config --libs xcb xcb-dri3 xcb-present x11-xcb 2>/dev/null)
CFLAGS += $(shell pkg-config --cflags xcb xcb-dri3 xcb-present x11-xcb 2>/dev/null)

# Fallback if pkg-config is unavailable
ifeq ($(LIBS),)
LIBS = -lxcb -lxcb-dri3 -lxcb-present -lX11-xcb -lpthread
endif

PREFIX  ?= /usr
LIBDIR  ?= $(PREFIX)/lib/aarch64-linux-gnu
JSONDIR ?= /etc/vulkan/implicit_layer.d

LAYER_SO   = libVkLayer_tegra_x11_present.so
LAYER_JSON = VkLayer_tegra_x11_present.json
LAYER_SRC  = vk_layer_tegra_x11_present.c

all: $(LAYER_SO)

$(LAYER_SO): $(LAYER_SRC)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIBS)

install: $(LAYER_SO) $(LAYER_JSON)
	install -d $(DESTDIR)$(LIBDIR)
	install -m 0755 $(LAYER_SO) $(DESTDIR)$(LIBDIR)/
	install -d $(DESTDIR)$(JSONDIR)
	install -m 0644 $(LAYER_JSON) $(DESTDIR)$(JSONDIR)/

uninstall:
	rm -f $(DESTDIR)$(LIBDIR)/$(LAYER_SO)
	rm -f $(DESTDIR)$(JSONDIR)/$(LAYER_JSON)

clean:
	rm -f $(LAYER_SO)

.PHONY: all install uninstall clean
