# Makefile for building remapper
# Copyright (c) 2026 Nick Clifford <nick@nickclifford.com>
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.

CC      = gcc
CFLAGS  = -Wall -Wextra -O2
BUILD   = build
UNAME_S := $(shell uname -s)

SHARED_HDR     = rmp_shared.h
INTERPOSE_HDR  = interpose.h $(SHARED_HDR)

##############################################################################
# Platform-specific targets
##############################################################################

ifeq ($(UNAME_S),Darwin)

all: $(BUILD)/interpose.dylib $(BUILD)/remapper

INTERPOSE_SRC = interpose.c interpose_fs.c interpose_exec.c

$(BUILD)/interpose.dylib: $(INTERPOSE_SRC) $(INTERPOSE_HDR) $(BUILD)/rmp_shared.o | $(BUILD)
	$(CC) $(CFLAGS) -dynamiclib -o $@ $(INTERPOSE_SRC) $(BUILD)/rmp_shared.o

$(BUILD)/remapper: remapper.c $(BUILD)/rmp_shared.o $(BUILD)/interpose.dylib $(SHARED_HDR) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ remapper.c $(BUILD)/rmp_shared.o \
		-Wl,-sectcreate,__DATA,__interpose_lib,$(BUILD)/interpose.dylib

test: all
	$(MAKE) -C test BUILD=$(CURDIR)/$(BUILD)
	./test/test.sh

else ifeq ($(UNAME_S),Linux)

all: $(BUILD)/remapper

INTERPOSE_SRC_LINUX = interpose.c interpose_fs_linux.c interpose_exec_linux.c

# Build the shared library (intermediate artifact — will be embedded)
$(BUILD)/interpose.so: $(INTERPOSE_SRC_LINUX) $(INTERPOSE_HDR) $(BUILD)/rmp_shared.o | $(BUILD)
	$(CC) $(CFLAGS) -shared -fPIC -o $@ $(INTERPOSE_SRC_LINUX) $(BUILD)/rmp_shared.o -ldl

# Wrap interpose.so into a relocatable object so it can be linked into remapper.
# This creates symbols: _binary_interpose_so_start, _binary_interpose_so_end
$(BUILD)/interpose_so.o: $(BUILD)/interpose.so | $(BUILD)
	cd $(BUILD) && ld -r -b binary -o interpose_so.o interpose.so

# Embed interpose.so into the remapper binary.
# At runtime, remapper reads _binary_interpose_so_start/end and writes
# the .so out to $RMP_CONFIG/interpose.so on first run (or when stale).
# This means the user only needs to distribute/install a single file.
$(BUILD)/remapper: remapper.c $(BUILD)/rmp_shared.o $(BUILD)/interpose_so.o $(SHARED_HDR) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ remapper.c $(BUILD)/rmp_shared.o $(BUILD)/interpose_so.o

test: all
	$(MAKE) -C test -f Makefile.linux BUILD=$(CURDIR)/$(BUILD)
	./test/test_linux.sh

endif

##############################################################################
# Shared rules
##############################################################################

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/rmp_shared.o: rmp_shared.c $(SHARED_HDR) | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ rmp_shared.c

deploy:
	./deploy.sh $(VERSION)

clean:
	rm -rf $(BUILD)

.PHONY: all clean test deploy
