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
RELEASE = release
UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)

SHARED_HDR     = rmp_shared.h
INTERPOSE_HDR  = interpose.h $(SHARED_HDR)

##############################################################################
# Platform-specific targets
##############################################################################

ifeq ($(UNAME_S),Darwin)

all: $(BUILD)/interpose.dylib $(BUILD)/remapper $(RELEASE)/remapper-$(UNAME_S)-$(UNAME_M)

INTERPOSE_SRC = interpose.c interpose_fs.c interpose_exec.c

$(BUILD)/interpose.dylib: $(INTERPOSE_SRC) $(INTERPOSE_HDR) $(BUILD)/rmp_shared.o | $(BUILD)
	$(CC) $(CFLAGS) -dynamiclib -o $@ $(INTERPOSE_SRC) $(BUILD)/rmp_shared.o

$(BUILD)/remapper: remapper_darwin.c $(BUILD)/rmp_shared.o $(BUILD)/interpose.dylib $(SHARED_HDR) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ remapper_darwin.c $(BUILD)/rmp_shared.o \
		-Wl,-sectcreate,__DATA,__interpose_lib,$(BUILD)/interpose.dylib

test: all
	$(MAKE) -C test -f Makefile.darwin BUILD=$(CURDIR)/$(BUILD)
	./test/test_darwin.sh
	@if command -v docker >/dev/null 2>&1 && docker info >/dev/null 2>&1; then \
		echo "=== Docker available — running Linux tests ==="; \
		$(MAKE) docker-test; \
		$(MAKE) apparmor-test; \
	else \
		echo "=== Docker not available — skipping Linux tests ==="; \
	fi

else ifeq ($(UNAME_S),Linux)

all: $(BUILD)/remapper $(RELEASE)/remapper-$(UNAME_S)-$(UNAME_M)

$(BUILD)/remapper: remapper_linux.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ remapper_linux.c

test: all
	./test/test_linux.sh

endif

##############################################################################
# Shared rules
##############################################################################

$(BUILD):
	mkdir -p $(BUILD)

$(RELEASE):
	mkdir -p $(RELEASE)

$(RELEASE)/remapper-$(UNAME_S)-$(UNAME_M): $(BUILD)/remapper | $(RELEASE)
	cp $(BUILD)/remapper $@

$(BUILD)/rmp_shared.o: rmp_shared.c $(SHARED_HDR) | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ rmp_shared.c

##############################################################################
# Docker-based Linux testing (usable from any host with Docker)
##############################################################################

DOCKER_TEST_IMAGE = remapper-test

docker-image:
	docker build -q -f Dockerfile.test -t $(DOCKER_TEST_IMAGE) .

docker-test: docker-image
	@echo "=== Running Linux tests in Docker ==="
	docker run --rm --privileged -v "$(CURDIR)":/src -w /src $(DOCKER_TEST_IMAGE) \
		bash -c "make clean && make && make test"

apparmor-test: docker-image
	@echo "=== Running AppArmor tests in Docker ==="
	docker run --rm -v "$(CURDIR)":/src -w /src $(DOCKER_TEST_IMAGE) \
		./test/test_apparmor.sh

##############################################################################

deploy:
	./deploy-from-macos.sh $(VERSION)

clean:
	rm -rf $(BUILD)
	rm -f $(RELEASE)/remapper-$(UNAME_S)-$(UNAME_M)

.PHONY: all clean test deploy docker-image docker-test apparmor-test
