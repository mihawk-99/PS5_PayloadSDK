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

HOST_CC ?= cc
HOST_CFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Werror -Iinclude -Isrc -pthread
HOST_SRCS := $(filter-out src/probe.c,$(SRCS)) tests/host_kernel.c tests/test_platform.c

.PHONY: all clean install test

all: build/ps5/libps5platform.a

build/ps5/%.o: src/%.c $(HEADERS)
	@mkdir -p build/ps5
	$(CC) $(CFLAGS) -c $< -o $@

build/ps5/libps5platform.a: $(OBJS)
	rm -f $@
	$(AR) -rsc $@ $^

build/host/test_platform: $(HOST_SRCS) $(HEADERS) $(wildcard tests/*.h)
	@mkdir -p build/host
	$(HOST_CC) $(HOST_CFLAGS) $(HOST_SRCS) -o $@

test: build/host/test_platform
	build/host/test_platform

clean:
	rm -rf build

install: build/ps5/libps5platform.a
	install -d $(DESTDIR)/target/lib $(DESTDIR)/target/include/ps5platform
	install -m 0644 build/ps5/libps5platform.a $(DESTDIR)/target/lib
	install -m 0644 include/ps5platform/*.h $(DESTDIR)/target/include/ps5platform
