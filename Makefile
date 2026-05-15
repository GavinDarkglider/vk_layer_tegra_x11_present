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
#   SYSCONFDIR        system config dir for profile.d (default /etc)
#   LAYERLIBDIR       where the shared object goes; defaults to a
#                     multiarch path under $(PREFIX)/lib but Lakka and
#                     other distros that don't use multiarch should
#                     override this to plain $(PREFIX)/lib
#   LAYERJSONDIR      where the implicit-layer JSON goes; default is
#                     /usr/share/vulkan/implicit_layer.d
#   PROFILEDDIR       where the shell profile fragment goes; default
#                     /etc/profile.d
#   DESTDIR           staging root for installation
#
# The library_path in the JSON is intentionally a bare filename, not an
# absolute path, so the Vulkan loader searches the standard library dirs
# and the same JSON works regardless of where LAYERLIBDIR puts the .so.

CROSS_COMPILE ?=
SYSROOT       ?=
PREFIX        ?= /usr
SYSCONFDIR    ?= /etc

# Default to Ubuntu/Debian multiarch. Lakka's package recipe overrides
# this to $(PREFIX)/lib.
ARCH          ?= aarch64-linux-gnu
LAYERLIBDIR   ?= $(PREFIX)/lib/$(ARCH)
LAYERJSONDIR  ?= $(PREFIX)/share/vulkan/implicit_layer.d
# Profile.d is the only reliable way to set __GL_YIELD before libGL caches
# the value. Setting it from a .so constructor runs too late on Tegra L4T.
PROFILEDDIR   ?= $(SYSCONFDIR)/profile.d

# CC selection:
# - If CC was set on the command line or in the environment, use that (this is
#   how Lakka and other cross-build recipes inject their toolchain).
# - Otherwise synthesize from CROSS_COMPILE (which is empty by default, giving
#   plain "gcc" — fine for native builds).
# - "make"'s built-in default of CC=cc counts as "default" origin, which is why
#   we test against that rather than using a plain ?= assignment.
ifeq ($(origin CC),default)
CC := $(CROSS_COMPILE)gcc
endif
INSTALL ?= install

ifneq ($(SYSROOT),)
SYSROOT_FLAGS := --sysroot=$(SYSROOT)
else
SYSROOT_FLAGS :=
endif

TARGET   := libVkLayer_tegra_x11_present.so
SRC      := vk_layer_tegra_x11_present.c
JSON     := vk_layer_tegra_x11_present.json
MAPFILE  := vk_layer_tegra_x11_present.map
PROFILESH := 99-tegra-x11-present.sh

CFLAGS  ?= -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
CFLAGS  += $(SYSROOT_FLAGS)

# Always required for a shared library, regardless of what the environment
# provides in CFLAGS:
# - -fPIC: position-independent code (mandatory for .so)
# - -fvisibility=hidden: hide internal symbols (only the three loader-resolved
#   entrypoints are exported via the version script)
LAYER_CFLAGS := -fPIC -fvisibility=hidden

# LDFLAGS handling:
# - The environment / build recipe may set LDFLAGS to inject sysroot, search
#   paths, hardening flags, etc. We want to combine with those, NOT replace.
# - But -shared and --version-script are NON-OPTIONAL for our build (without
#   -shared the linker builds an executable, looks for _start/main, and fails).
#   So they go into a separate LAYER_LDFLAGS that's always added.
LAYER_LDFLAGS := -shared -Wl,--no-undefined -Wl,--version-script=$(MAPFILE) $(SYSROOT_FLAGS)
LDLIBS  := -lvulkan -lGL -lX11 -lX11-xcb -lxcb -lpthread -ldl -lm

.PHONY: all clean install uninstall

all: $(TARGET)

$(TARGET): $(SRC) $(MAPFILE)
	$(CC) $(LAYER_CFLAGS) $(CFLAGS) $(LAYER_LDFLAGS) $(LDFLAGS) -o $@ $(SRC) $(LDLIBS)

install: all
	$(INSTALL) -d $(DESTDIR)$(LAYERLIBDIR)
	$(INSTALL) -m 0755 $(TARGET) $(DESTDIR)$(LAYERLIBDIR)/$(TARGET)
	$(INSTALL) -d $(DESTDIR)$(LAYERJSONDIR)
	$(INSTALL) -m 0644 $(JSON) $(DESTDIR)$(LAYERJSONDIR)/$(JSON)
	$(INSTALL) -d $(DESTDIR)$(PROFILEDDIR)
	$(INSTALL) -m 0644 $(PROFILESH) $(DESTDIR)$(PROFILEDDIR)/$(PROFILESH)

uninstall:
	rm -f $(DESTDIR)$(LAYERLIBDIR)/$(TARGET)
	rm -f $(DESTDIR)$(LAYERJSONDIR)/$(JSON)
	rm -f $(DESTDIR)$(PROFILEDDIR)/$(PROFILESH)

clean:
	rm -f $(TARGET)
