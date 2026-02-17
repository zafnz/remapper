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

all: $(BUILD)/interpose.dylib $(BUILD)/remapper

$(BUILD):
	mkdir -p $(BUILD)

# Shared code between the CLI and the interposer
$(BUILD)/rmp_shared.o: rmp_shared.c rmp_shared.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ rmp_shared.c

# Build the dylib first (intermediate artifact, not installed separately)
$(BUILD)/interpose.dylib: interpose.c $(BUILD)/rmp_shared.o rmp_shared.h | $(BUILD)
	$(CC) $(CFLAGS) -dynamiclib -o $@ interpose.c $(BUILD)/rmp_shared.o

# Embed interpose.dylib into the remapper binary as a Mach-O section.
# At runtime, remapper reads this section with getsectiondata() and writes
# the dylib out to $RMP_CONFIG/interpose.dylib on first run (or when stale).
# This means the user only needs to distribute/install a single file.
$(BUILD)/remapper: remapper.c $(BUILD)/rmp_shared.o $(BUILD)/interpose.dylib rmp_shared.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ remapper.c $(BUILD)/rmp_shared.o \
		-Wl,-sectcreate,__DATA,__interpose_lib,$(BUILD)/interpose.dylib

test: all
	$(MAKE) -C test BUILD=$(CURDIR)/$(BUILD)
	./test/test.sh

deploy:
	./deploy.sh $(VERSION)

clean:
	rm -rf $(BUILD)

.PHONY: all clean test deploy
