# MaxEco build.
#   make            tray without libc (x86_64) plus the root helper
#   make LIBC=1     tray linked to the system C library (any architecture)
#   make install DESTDIR=... PREFIX=/usr

PREFIX ?= /usr
BINDIR ?= $(PREFIX)/bin
LIBDIR ?= $(PREFIX)/lib
DATADIR ?= $(PREFIX)/share
UNITDIR ?= $(LIBDIR)/systemd/system
CC ?= cc

WARN = -Wall -Wextra -Wno-unused-function -Wshadow -Wformat=2
TRAY_BASE = -Os -std=gnu11 $(WARN) -fno-asynchronous-unwind-tables -fno-unwind-tables \
            -fno-ident -ffunction-sections -fdata-sections
ifeq ($(LIBC),1)
TRAY_CFLAGS = $(TRAY_BASE) -DMAXECO_LIBC -D_FORTIFY_SOURCE=2 -fstack-protector-strong
TRAY_LDFLAGS = -Wl,--gc-sections -Wl,-z,relro,-z,now -s
else
TRAY_CFLAGS = $(TRAY_BASE) -ffreestanding -fno-builtin -fno-stack-protector -fno-pie \
              -fno-tree-loop-distribute-patterns -fcf-protection=none
TRAY_LDFLAGS = -nostdlib -static -no-pie -Wl,--gc-sections -Wl,--build-id=none \
               -Wl,-z,noexecstack -Wl,-z,norelro -s
endif
HELPER_CFLAGS = -O2 -std=gnu11 $(WARN) -fstack-protector-strong -fPIE
ifeq ($(findstring FORTIFY,$(CPPFLAGS) $(CFLAGS)),)
HELPER_CFLAGS += -D_FORTIFY_SOURCE=2
endif
HELPER_LDFLAGS = -pie -Wl,-z,relro,-z,now -ldl

all: build/maxeco build/maxeco-helper

build/maxeco: src/maxeco.c src/rt.h src/dbus.h
	@mkdir -p build
	$(CC) $(TRAY_CFLAGS) -o $@ src/maxeco.c $(TRAY_LDFLAGS)

build/maxeco-helper: src/maxeco-helper.c
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(HELPER_CFLAGS) -o $@ src/maxeco-helper.c $(LDFLAGS) $(HELPER_LDFLAGS)

install: all
	install -Dm755 build/maxeco $(DESTDIR)$(BINDIR)/maxeco
	install -Dm755 build/maxeco-helper $(DESTDIR)$(LIBDIR)/maxeco/maxeco-helper
	install -Dm644 data/maxeco.service $(DESTDIR)$(UNITDIR)/maxeco.service
	install -Dm644 data/maxeco-level@.service $(DESTDIR)$(UNITDIR)/maxeco-level@.service
	install -Dm644 data/maxeco-resume.service $(DESTDIR)$(UNITDIR)/maxeco-resume.service
	for t in suspend hibernate hybrid-sleep suspend-then-hibernate; do \
	  install -dm755 $(DESTDIR)$(UNITDIR)/$$t.target.wants; \
	  ln -sf ../maxeco-resume.service $(DESTDIR)$(UNITDIR)/$$t.target.wants/maxeco-resume.service; \
	done
	install -Dm644 data/50-maxeco.rules $(DESTDIR)$(DATADIR)/polkit-1/rules.d/50-maxeco.rules
	install -Dm644 data/maxeco.desktop $(DESTDIR)$(DATADIR)/applications/maxeco.desktop
	install -Dm644 data/icons/maxeco-on.svg $(DESTDIR)$(DATADIR)/icons/hicolor/scalable/apps/maxeco-on.svg
	install -Dm644 data/icons/maxeco-eco.svg $(DESTDIR)$(DATADIR)/icons/hicolor/scalable/apps/maxeco-eco.svg
	install -Dm644 data/icons/maxeco-off.svg $(DESTDIR)$(DATADIR)/icons/hicolor/scalable/apps/maxeco-off.svg
	install -Dm644 LICENSE $(DESTDIR)$(DATADIR)/licenses/maxeco/LICENSE

clean:
	rm -rf build

.PHONY: all install clean
