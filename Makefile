# Makefile for vk_layer_tegra_x11_present
#
# Builds an implicit Vulkan layer that fixes Tegra L4T r32.x's broken X11
# WSI presentation by routing it through GL/GLX. See README.md for the
# why and how.
#
# Variables (override on the make command line or in your build recipe):
#
#   CROSS_COMPILE     toolchain prefix, e.g. aarch64-linux-gnu-
#   SYSROOT           target sysroot for headers and libraries
#   PREFIX            install prefix (default /usr)
#   LAYERLIBDIR       where the shared object goes; defaults to a
#                     multiarch path under $(PREFIX)/lib but Lakka and
#                     other distros that don't use multiarch should
#                     override this to plain $(PREFIX)/lib
#   LAYERJSONDIR      where the implicit-layer JSON goes; default is
#                     /usr/share/vulkan/implicit_layer.d
#   DESTDIR           staging root for installation
#
# The library_path in the JSON is intentionally a bare filename, not an
# absolute path, so the Vulkan loader searches the standard library dirs
# and the same JSON works regardless of where LAYERLIBDIR puts the .so.

CROSS_COMPILE ?=
SYSROOT       ?=
PREFIX        ?= /usr

# Default to Ubuntu/Debian multiarch. Lakka's package recipe overrides
# this to $(PREFIX)/lib.
ARCH          ?= aarch64-linux-gnu
LAYERLIBDIR   ?= $(PREFIX)/lib/$(ARCH)
LAYERJSONDIR  ?= $(PREFIX)/share/vulkan/implicit_layer.d

CC      := $(CROSS_COMPILE)gcc
INSTALL := install

ifneq ($(SYSROOT),)
SYSROOT_FLAGS := --sysroot=$(SYSROOT)
else
SYSROOT_FLAGS :=
endif

CFLAGS  ?= -O2 -g -fPIC -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -fvisibility=hidden
CFLAGS  += $(SYSROOT_FLAGS)
LDFLAGS ?= -shared -Wl,--no-undefined -Wl,--version-script=$(MAPFILE) $(SYSROOT_FLAGS)
LDLIBS  := -lvulkan -lGL -lX11 -lX11-xcb -lxcb -lpthread -ldl -lm

TARGET  := libVkLayer_tegra_x11_present.so
SRC     := vk_layer_tegra_x11_present.c
JSON    := vk_layer_tegra_x11_present.json
MAPFILE := vk_layer_tegra_x11_present.map

.PHONY: all clean install uninstall

all: $(TARGET)

$(TARGET): $(SRC) $(MAPFILE)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRC) $(LDLIBS)

install: all
	$(INSTALL) -d $(DESTDIR)$(LAYERLIBDIR)
	$(INSTALL) -m 0755 $(TARGET) $(DESTDIR)$(LAYERLIBDIR)/$(TARGET)
	$(INSTALL) -d $(DESTDIR)$(LAYERJSONDIR)
	$(INSTALL) -m 0644 $(JSON) $(DESTDIR)$(LAYERJSONDIR)/$(JSON)

uninstall:
	rm -f $(DESTDIR)$(LAYERLIBDIR)/$(TARGET)
	rm -f $(DESTDIR)$(LAYERJSONDIR)/$(JSON)

clean:
	rm -f $(TARGET)
