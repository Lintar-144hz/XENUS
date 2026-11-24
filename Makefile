# XENUS - Solo Coding Desktop Environment
#
#   make              build xenus + XENUS BIOS + the X test helpers + plugins
#   make debug        build xenus-debug (-O0 -g) in build-debug/
#   make test         HAL tests, App API tests, BIOS tests, Xvfb suites
#   make run          run xenus (needs a running X server / $DISPLAY)
#   make bios         build only xenus-bios and the xfirm launcher
#   make test-bios    run the BIOS engine unit test
#   make test-bios-gui run the Xvfb BIOS end-to-end smoke test
#   make hal          build only the HAL static library
#   make plugins      build only the example/test plugin shared objects
#   make install      install into $(PREFIX), DESTDIR is honoured
#   make uninstall    remove the installed files
#   make clean        remove build artifacts (never sources or docs)
#
# No architecture specific flags (-march/-mcpu/...) are used: the project is
# compiled natively for the target (ARM64, x86_64, ...) with the system
# compiler.

CC        = gcc
AR       ?= ar

# always-on: language level + portability warnings
STD       := -std=c11 -D_POSIX_C_SOURCE=200809L
WARN      := -Wall -Wextra -Wpedantic
# optimisation; override with   make OPT=-O0   or just use  make debug
OPT       ?= -O2

CFLAGS    = $(STD) $(WARN) $(OPT) $(EXTRA_CFLAGS)
# public App API headers live in include/, the API itself in src/api + src/plugin
CPPFLAGS += -Iinclude
LDFLAGS   ?= $(EXTRA_LDFLAGS)
LDLIBS    ?= -lX11 -ldl -lcrypt

PREFIX    ?= /usr/local
BINDIR    ?= $(PREFIX)/bin
LIBDIR    ?= $(PREFIX)/lib
INCDIR    ?= $(PREFIX)/include
SHAREDIR  ?= $(PREFIX)/share/xenus
DESTDIR   ?=

BIN       = xenus
DEBUGBIN  = xenus-debug
BIOSBIN   = xenus-bios
XFIRMBIN  = xfirm
XRECOVERYBIN = xrecovery
OBJDIR    = build
DEBUGDIR  = build-debug

HALDIR    = hal
# hard limit for the Xvfb suite so `make test` can never hang
TEST_TIMEOUT ?= 300

SRC = src/main.c src/util.c src/ui.c src/config.c src/wm.c src/input.c \
      src/panel.c src/dock.c src/launcher.c src/apps.c src/msgbox.c src/fm.c \
      src/desktop.c src/security_gui.c src/text_editor.c src/taskbar.c src/system_center.c \
      src/settings.c src/settings_prefs.c src/control_center.c src/notification_center.c src/auth_screen.c
OBJ = $(patsubst %.c,$(OBJDIR)/%.o,$(SRC))

# the App API + plugin system: a static library shared by xenus and the tests
APISRC = src/cache.c src/api/common.c src/api/config.c src/api/event.c \
         src/api/notification.c src/api/window.c src/api/app.c src/api/net.c \
         src/config_manager.c \
         src/core/ucore.c src/core/auth.c src/core/sysinfo.c src/core/proc.c src/core/apps.c \
         src/bios/bios_config.c \
         src/plugin/loader.c src/plugin/manager.c \
         src/security/core.c src/security/scanner.c src/security/policy.c \
         src/security/audit.c src/security/capability.c \
         src/security/sandbox.c src/security/bait.c \
         src/security/mirage.c src/security/resource.c
APIOBJ = $(patsubst %.c,$(OBJDIR)/%.o,$(APISRC))
APILIB = $(OBJDIR)/libxenusapp.a

# XENUS BIOS (X-FIRM): standalone GUI + engine and the xfirm opener.
BIOSSRC = src/bios/bios_config.c src/bios/bios_backend.c
BIOSOBJ = $(patsubst %.c,$(OBJDIR)/%.o,$(BIOSSRC))
BIOSLIB = $(OBJDIR)/bios/libxenusbios.a
BIOSUIOBJ = $(OBJDIR)/src/bios/bios_ui.o
XFIRMOBJ = $(OBJDIR)/src/bios/xfirm.o
BIOSDEPS = include/xenus/bios.h src/bios/bios_backend.h

DEP = $(OBJ:.o=.d) $(APIOBJ:.o=.d) $(BIOSOBJ:.o=.d) $(BIOSUIOBJ:.o=.d) \
      $(XFIRMOBJ:.o=.d)

TESTBIN = tests/testclient tests/rootpixel tests/screenshot tests/api_test \
          tests/security_test tests/cache_test tests/ucore_test tests/bios_test \
          tests/config_manager_test
NETTEST  = tests/net_test

# plugin shared objects (needed by the API tests)
PLUGIN_DIR = $(OBJDIR)/plugins
EXAMPLE_SO  = $(PLUGIN_DIR)/libxenus-example.so
BADVER_SO   = $(PLUGIN_DIR)/test-badver.so
FAILINIT_SO = $(PLUGIN_DIR)/test-failinit.so
PLUGINS     = $(EXAMPLE_SO) $(BADVER_SO) $(FAILINIT_SO)

all: $(BIN) $(TESTBIN) $(BIOSBIN) $(XFIRMBIN) $(XRECOVERYBIN)

$(BIN): $(OBJ) $(APILIB)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ) $(APILIB) $(LDLIBS)

$(APILIB): $(APIOBJ)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $(APIOBJ)

$(BIOSLIB): $(BIOSOBJ)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $(BIOSOBJ)

$(BIOSBIN): $(BIOSUIOBJ) $(BIOSLIB) $(BIOSDEPS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(BIOSUIOBJ) $(BIOSLIB) -lX11

$(XRECOVERYBIN): src/recovery/xrecovery.c src/core/auth.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $^ -lcrypt

$(XFIRMBIN): $(XFIRMOBJ) $(OBJDIR)/src/bios/bios_config.o $(BIOSDEPS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(XFIRMOBJ) \
		$(OBJDIR)/src/bios/bios_config.o -lX11

bios: $(BIOSBIN) $(XFIRMBIN)

$(OBJDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

tests/%: tests/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

tests/cache_test: tests/cache_test.c src/cache.c src/cache.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/cache_test.c \
		src/cache.c $(LDLIBS)

tests/security_test: tests/security_test.c src/util.c $(APILIB)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/security_test.c \
		src/util.c $(APILIB) $(LDLIBS)

tests/ucore_test: tests/ucore_test.c $(APILIB)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/ucore_test.c \
		$(APILIB) $(LDLIBS)

tests/bios_test: tests/bios_test.c src/bios/bios_config.c $(BIOSDEPS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/bios_test.c \
		src/bios/bios_config.c $(LDLIBS)

tests/config_manager_test: tests/config_manager_test.c $(APILIB)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/config_manager_test.c \
		$(APILIB) $(LDLIBS)

tests/api_test: tests/api_test.c $(APILIB) $(PLUGINS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/api_test.c \
		$(APILIB) $(LDLIBS)

$(EXAMPLE_SO): plugins/example/example.c $(wildcard include/xenus/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -shared -o $@ $<

$(BADVER_SO): tests/plugins/badver.c $(wildcard include/xenus/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -shared -o $@ $<

$(FAILINIT_SO): tests/plugins/failinit.c $(wildcard include/xenus/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -shared -o $@ $<

plugins: $(PLUGINS)

$(NETTEST): tests/net_test.c $(APILIB)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/net_test.c \
		$(APILIB) $(LDLIBS)

test-net: $(NETTEST)
	./$(NETTEST)

# a separate object directory keeps the release build intact
debug:
	@$(MAKE) BIN=$(DEBUGBIN) OBJDIR=$(DEBUGDIR) OPT="-O0 -g" $(DEBUGBIN)

run: $(BIN)
	./$(BIN) $(ARGS)

hal:
	$(MAKE) -C $(HALDIR) all

test: $(BIN) $(TESTBIN) hal
	@echo "== HAL tests =="
	$(MAKE) -C $(HALDIR) test
	@echo "== XENUS cache tests =="
	./tests/cache_test
	@echo "== XENUS UCore tests =="
	./tests/ucore_test
	@echo "== XENUS BIOS engine tests =="
	./tests/bios_test
	@echo "== XENUS Config Manager tests =="
	./tests/config_manager_test
	@echo "== XENUS App API tests =="
	./tests/api_test $(EXAMPLE_SO) $(BADVER_SO) $(FAILINIT_SO)
	@echo "== XENUS security boundary tests =="
	./tests/security_test $(EXAMPLE_SO)
	@echo "== XENUS test suite (limit $(TEST_TIMEOUT)s) =="
	timeout -k 10 $(TEST_TIMEOUT) bash tests/run_tests.sh

test-bios: $(BIOSBIN) $(XFIRMBIN) tests/bios_test
	./tests/bios_test

test-bios-gui: $(BIOSBIN) $(XFIRMBIN)
	bash tests/bios_ui_test.sh

install: $(BIN) $(APILIB) $(BIOSBIN) $(XFIRMBIN) $(XRECOVERYBIN)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(BIN) $(DESTDIR)$(BINDIR)/$(BIN)
	install -m 755 $(BIOSBIN) $(DESTDIR)$(BINDIR)/$(BIOSBIN)
	install -m 755 $(XFIRMBIN) $(DESTDIR)$(BINDIR)/$(XFIRMBIN)
	install -m 755 $(XRECOVERYBIN) $(DESTDIR)$(BINDIR)/$(XRECOVERYBIN)
	install -d $(DESTDIR)$(SHAREDIR)
	install -m 644 config/xenus-config.sample $(DESTDIR)$(SHAREDIR)/config.sample
	install -d $(DESTDIR)$(INCDIR)/xenus
	install -m 644 include/xenus/*.h $(DESTDIR)$(INCDIR)/xenus/
	install -d $(DESTDIR)$(LIBDIR)
	install -m 644 $(APILIB) $(DESTDIR)$(LIBDIR)/libxenusapp.a

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(BIN)
	rm -f $(DESTDIR)$(BINDIR)/$(BIOSBIN)
	rm -f $(DESTDIR)$(BINDIR)/$(XFIRMBIN)
	rm -f $(DESTDIR)$(BINDIR)/$(XRECOVERYBIN)
	rm -f $(DESTDIR)$(SHAREDIR)/config.sample
	rm -rf $(DESTDIR)$(INCDIR)/xenus
	rm -f $(DESTDIR)$(LIBDIR)/libxenusapp.a
	-rmdir $(DESTDIR)$(SHAREDIR)

clean:
	rm -rf $(OBJDIR) $(DEBUGDIR)
	rm -f src/*.o $(BIN) $(DEBUGBIN) $(BIOSBIN) $(XFIRMBIN) $(XRECOVERYBIN) $(TESTBIN)
	$(MAKE) -C $(HALDIR) clean

-include $(DEP)

.PHONY: all debug run hal plugins test test-net test-bios test-bios-gui \
        bios install uninstall clean
