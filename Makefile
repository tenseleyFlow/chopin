# chopin - GNU make required (gmake on FreeBSD).
ifeq ($(filter else-if,$(.FEATURES)),)
$(error GNU make is required; use gmake on BSD systems)
endif

include config.mk

VERSION := $(shell sed -n 's/^CHOPIN_VERSION="\(.*\)"/\1/p' configure)

SRC = \
	src/backup.c \
	src/copy.c \
	src/copydata.c \
	src/forcelink.c \
	src/hashes.c \
	src/main.c \
	src/meta.c \
	src/options.c \
	src/parallel.c \
	src/plan.c \
	src/pool.c \
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

check: unit golden mined fuzz-smoke identity-smoke perf-smoke

mined: all
	sh tests/gnu-mined/run.sh || test $$? -eq 77

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

# Release depth: 3 seeds x 200 trials (sprint 08).
fuzz: all
	FUZZ_TRIALS=200 FUZZ_SEED=42 sh tests/fuzz/run.sh
	FUZZ_TRIALS=200 FUZZ_SEED=7 sh tests/fuzz/run.sh
	FUZZ_TRIALS=200 FUZZ_SEED=1234 sh tests/fuzz/run.sh

# Sprint 09C identity proof: parallel == serial byte-for-byte (both
# streams + manifest + rc), random worker counts, chunked lane
# included. Needs no oracle - runs on every platform.
identity-smoke: all
	FUZZ_IDENTITY=1 FUZZ_TRIALS=25 FUZZ_SEED=9001 sh tests/fuzz/run.sh || test $$? -eq 77

identity: all
	FUZZ_IDENTITY=1 FUZZ_TRIALS=200 FUZZ_SEED=9101 sh tests/fuzz/run.sh
	FUZZ_IDENTITY=1 FUZZ_TRIALS=200 FUZZ_SEED=9102 sh tests/fuzz/run.sh
	FUZZ_IDENTITY=1 FUZZ_CHUNKS=1 FUZZ_TRIALS=200 FUZZ_SEED=9103 sh tests/fuzz/run.sh

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
	FUZZ_IDENTITY=1 FUZZ_TRIALS=10 FUZZ_SEED=9001 sh tests/fuzz/run.sh
	FUZZ_IDENTITY=1 FUZZ_CHUNKS=1 FUZZ_TRIALS=10 FUZZ_SEED=9002 sh tests/fuzz/run.sh
	$(MAKE) clean
	$(MAKE) all

# The man page's DEVIATIONS section is GENERATED from the register
# (sprint 11 locked decision: one source of truth). `make man`
# re-splices it; `make man-check` fails if the in-tree page has
# drifted, so CI catches a register edit that never reached the page.
man: doc/chopin.1 doc/deviations.md scripts/gen-man-deviations.sh
	@sh scripts/gen-man-deviations.sh > build/dev.roff
	@awk '/^\.\\" BEGIN GENERATED DEVIATIONS/ { print; system("cat build/dev.roff"); skip=1; next } \
	      /^\.\\" END GENERATED DEVIATIONS/ { skip=0 } \
	      skip != 1 { print }' doc/chopin.1 > build/chopin.1.new
	@if cmp -s build/chopin.1.new doc/chopin.1; then \
		echo "man: doc/chopin.1 up to date"; \
	else \
		cp build/chopin.1.new doc/chopin.1; \
		echo "man: doc/chopin.1 regenerated from doc/deviations.md"; \
	fi

# Pure check: never mutates doc/chopin.1, so CI can run it.
man-check: doc/chopin.1 doc/deviations.md scripts/gen-man-deviations.sh
	@mkdir -p build
	@sh scripts/gen-man-deviations.sh > build/dev.roff
	@awk '/^\.\\" BEGIN GENERATED DEVIATIONS/ { print; system("cat build/dev.roff"); skip=1; next } \
	      /^\.\\" END GENERATED DEVIATIONS/ { skip=0 } \
	      skip != 1 { print }' doc/chopin.1 > build/chopin.1.check
	@cmp -s build/chopin.1.check doc/chopin.1 || { \
		echo "man-check: doc/chopin.1 is stale; run 'make man' and commit" >&2; \
		exit 1; }
	@echo "man-check: generated DEVIATIONS section matches the register"

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

.PHONY: all check unit golden fuzz fuzz-smoke identity identity-smoke \
	perf-smoke sanitize tsan dist distcheck install clean distclean

-include $(DEP)
