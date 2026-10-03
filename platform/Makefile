#   Copyright (C) 2026 Mihawk
#
# This file is free software; you can redistribute it and/or modify it
# under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; see the file COPYING. If not see
# <http://www.gnu.org/licenses/>.

# The PS5 platform layer: libps5platform.a and its headers (README.md).
#
#   make            the console library, against this tree's headers
#   make install    target/lib/libps5platform.a and target/include/ps5platform
#   make test       the host unit tests

include ../Makefile.inc

# The flags prospero-clang gives a consumer's objects, with this tree's
# headers in place of an installed SDK's.
CFLAGS := -target x86_64-sie-ps5 -fPIC -fno-plt -fno-stack-protector -femulated-tls
CFLAGS += -fvisibility-nodllstorageclass=default
CFLAGS += -isystem ../include/freebsd -Iinclude
CFLAGS += -std=c11 -O2 -Wall -Wextra -Werror

SRCS := $(wildcard src/*.c)
OBJS := $(SRCS:src/%.c=build/ps5/%.o)
HEADERS := $(wildcard include/ps5platform/*.h src/*.h) ../include/freebsd/sys/_ucontext.h

# musl's regex engine (src/regex/, MIT), built as imported: its own warnings
# are not this tree's to fix.
REGEX_SRCS := $(wildcard src/regex/*.c)
REGEX_OBJS := $(REGEX_SRCS:src/regex/%.c=build/ps5/regex/%.o)
REGEX_HEADERS := $(wildcard src/regex/*.h)
REGEX_CFLAGS := $(filter-out -Wall -Wextra -Werror,$(CFLAGS)) -w

# dlmalloc (src/dlmalloc/malloc.c, MIT), configured by src/dlmalloc/mspace.c
# and built as imported, like the regex engine.
HEAP_OBJ := build/ps5/dlmalloc/mspace.o
HEAP_HEADERS := src/dlmalloc/malloc.c

HOST_CC ?= cc
HOST_CFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Werror -Iinclude -Isrc -pthread
HOST_SRCS := $(filter-out src/probe.c,$(SRCS)) tests/host_kernel.c tests/host_libc.c tests/test_platform.c
HOST_REGEX_OBJS := $(REGEX_SRCS:src/regex/%.c=build/host/regex/%.o)
HOST_HEAP_OBJ := build/host/dlmalloc/mspace.o

.PHONY: all clean install test

all: build/ps5/libps5platform.a

build/ps5/%.o: src/%.c $(HEADERS)
	@mkdir -p build/ps5
	$(CC) $(CFLAGS) -c $< -o $@

build/ps5/regex/%.o: src/regex/%.c $(REGEX_HEADERS)
	@mkdir -p build/ps5/regex
	$(CC) $(REGEX_CFLAGS) -c $< -o $@

build/ps5/dlmalloc/mspace.o: src/dlmalloc/mspace.c $(HEAP_HEADERS)
	@mkdir -p build/ps5/dlmalloc
	$(CC) $(REGEX_CFLAGS) -c $< -o $@

build/ps5/libps5platform.a: $(OBJS) $(REGEX_OBJS) $(HEAP_OBJ)
	rm -f $@
	$(AR) -rsc $@ $^

build/host/regex/%.o: src/regex/%.c $(REGEX_HEADERS)
	@mkdir -p build/host/regex
	$(HOST_CC) -std=gnu11 -O1 -g -w -c $< -o $@

build/host/dlmalloc/mspace.o: src/dlmalloc/mspace.c $(HEAP_HEADERS)
	@mkdir -p build/host/dlmalloc
	$(HOST_CC) -std=gnu11 -O1 -g -w -c $< -o $@

build/host/test_platform: $(HOST_SRCS) $(HOST_REGEX_OBJS) $(HOST_HEAP_OBJ) $(HEADERS) $(wildcard tests/*.h)
	@mkdir -p build/host
	$(HOST_CC) $(HOST_CFLAGS) $(HOST_SRCS) $(HOST_REGEX_OBJS) $(HOST_HEAP_OBJ) -Wl,--wrap=pthread_create \
		-Wl,--wrap=pthread_join -Wl,--wrap=pthread_detach -Wl,--wrap=localeconv \
		-Wl,--wrap=fclose -Wl,--wrap=fflush -o $@

build/host/test_directory_records: src/directory.c tests/test_directory_records.c $(HEADERS)
	@mkdir -p build/host
	$(HOST_CC) $(HOST_CFLAGS) src/directory.c tests/test_directory_records.c \
		-Wl,--wrap=open -Wl,--wrap=close -o $@

build/host/test_memory: src/memory.c tests/test_memory.c $(HEADERS)
	@mkdir -p build/host
	$(HOST_CC) $(HOST_CFLAGS) src/memory.c tests/test_memory.c -o $@

build/host/test_fdv: src/fdv.c tests/test_fdv.c $(HEADERS)
	@mkdir -p build/host
	$(HOST_CC) $(HOST_CFLAGS) src/fdv.c tests/test_fdv.c -o $@

build/host/test_elevation: src/elevation.c tests/test_elevation.c $(HEADERS)
	@mkdir -p build/host
	$(HOST_CC) $(HOST_CFLAGS) src/elevation.c tests/test_elevation.c -o $@

test: build/host/test_platform build/host/test_directory_records build/host/test_memory build/host/test_fdv build/host/test_elevation
	build/host/test_platform
	build/host/test_directory_records
	build/host/test_memory
	build/host/test_fdv
	build/host/test_elevation

clean:
	rm -rf build

install: build/ps5/libps5platform.a
	install -d $(DESTDIR)/target/lib $(DESTDIR)/target/include/ps5platform
	install -m 0644 build/ps5/libps5platform.a $(DESTDIR)/target/lib
	install -m 0644 include/ps5platform/*.h $(DESTDIR)/target/include/ps5platform
