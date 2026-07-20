# chopin - GNU make required (gmake on FreeBSD).
ifeq ($(filter else-if,$(.FEATURES)),)
$(error GNU make is required; use gmake on BSD systems)
endif

include config.mk

VERSION := $(shell sed -n 's/^CHOPIN_VERSION="\(.*\)"/\1/p' configure)

SRC = \
	src/main.c \
	src/options.c \
	src/plan.c \
	src/quote.c \
	src/util.c

OBJ = $(SRC:.c=.o)
DEP = $(OBJ:.o=.d)

CPPFLAGS += -I. -Isrc -D_DEFAULT_SOURCE -D_FILE_OFFSET_BITS=64 $(EXTRA_CPPFLAGS)
CFLAGS ?= -O2
CFLAGS += -std=c11 -pthread -Wall -Wextra -Werror -Wpedantic -Wshadow \
	-Wstrict-prototypes -Wmissing-prototypes -Wconversion -Wwrite-strings
LDFLAGS += -pthread

PREFIX ?= /usr/local

all: config.h chopin cpn

chopin: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

# cpn is the same binary under a shorter name (argv[0] changes nothing but
# diagnostics). Copy in-tree; symlink at install time.
cpn: chopin
	@cmp -s chopin cpn 2>/dev/null || cp -f chopin cpn

config.mk config.h: configure
	./configure

%.o: %.c config.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

check: unit golden fuzz-smoke perf-smoke

# The manifest tool is shared by the unit and golden tiers; make owns
# the build so parallel `make check` cannot race on the artifact.
build/manifest: tests/manifest.c config.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wno-missing-prototypes -o $@ tests/manifest.c

unit: all build/manifest
	sh tests/unit/run.sh

golden: all build/manifest
	sh tests/golden/run.sh || test $$? -eq 77

# fuzz lands in sprint 08, perf smoke in sprint 10; tolerate absence
# until then so `make check` is runnable from the first commit.
fuzz-smoke: all
	@test ! -f tests/fuzz/run.sh || FUZZ_TRIALS=25 sh tests/fuzz/run.sh || test $$? -eq 77

fuzz: all
	@test -f tests/fuzz/run.sh || { echo "fuzz harness lands in sprint 08"; exit 1; }
	sh tests/fuzz/run.sh

perf-smoke: all
	@test ! -f bench/run-smoke.sh || sh bench/run-smoke.sh || test $$? -eq 77

sanitize:
	$(MAKE) clean
	$(MAKE) check CFLAGS='$(CFLAGS) $(SANITIZE_FLAGS)' LDFLAGS='$(LDFLAGS) $(SANITIZE_FLAGS)'
	$(MAKE) clean
	$(MAKE) all

# ThreadSanitizer over the parallel copy engine (meaningful from sprint 09;
# the harness forwards CHOPIN_PARALLEL_MIN through env -i from day one).
tsan:
	$(MAKE) clean
	$(MAKE) all CFLAGS='$(CFLAGS) -fsanitize=thread' LDFLAGS='$(LDFLAGS) -fsanitize=thread'
	CHOPIN_PARALLEL_MIN=1 sh tests/golden/run.sh || test $$? -eq 77
	@test ! -f tests/fuzz/run.sh || FUZZ_TRIALS=10 CHOPIN_PARALLEL_MIN=1 sh tests/fuzz/run.sh || test $$? -eq 77
	$(MAKE) clean
	$(MAKE) all

dist:
	git archive --format=tar.gz --prefix=chopin-$(VERSION)/ \
		-o chopin-$(VERSION).tar.gz HEAD

distcheck: dist
	rm -rf build/distcheck
	mkdir -p build/distcheck
	tar -xzf chopin-$(VERSION).tar.gz -C build/distcheck
	cd build/distcheck/chopin-$(VERSION) && ./configure && $(MAKE) && sh tests/unit/run.sh

install: all
	mkdir -p $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/share/man/man1
	install -m 0755 chopin $(DESTDIR)$(PREFIX)/bin/chopin
	ln -sf chopin $(DESTDIR)$(PREFIX)/bin/cpn
	install -m 0644 doc/chopin.1 $(DESTDIR)$(PREFIX)/share/man/man1/chopin.1
	install -m 0644 doc/cpn.1 $(DESTDIR)$(PREFIX)/share/man/man1/cpn.1

clean:
	rm -f chopin cpn $(OBJ) $(DEP)

distclean: clean
	rm -f config.h config.mk chopin-*.tar.gz
	rm -rf build

.PHONY: all check unit golden fuzz fuzz-smoke perf-smoke sanitize dist \
	distcheck install clean distclean

-include $(DEP)
