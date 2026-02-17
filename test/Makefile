CC      = gcc
CFLAGS  = -Wall -Wextra -O2
BUILD   = ../build

PLAIN    = test_interpose verify_test_interpose spawn_hardened
HARDENED = hardened_test hardened_interp

all: $(PLAIN:%=$(BUILD)/%) $(HARDENED:%=$(BUILD)/%) $(BUILD)/hardened_spawner

$(PLAIN:%=$(BUILD)/%): $(BUILD)/%: %.c
	$(CC) $(CFLAGS) -o $@ $<

$(HARDENED:%=$(BUILD)/%): $(BUILD)/%: %.c
	$(CC) $(CFLAGS) -o $@ $<
	codesign --force -s - --options runtime $@

# hardened_spawner is spawn_hardened.c, codesigned
$(BUILD)/hardened_spawner: spawn_hardened.c
	$(CC) $(CFLAGS) -o $@ $<
	codesign --force -s - --options runtime $@

.PHONY: all
