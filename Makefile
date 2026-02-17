CC      = gcc
CFLAGS  = -Wall -Wextra -O2
BUILD   = build

all: $(BUILD)/interpose.dylib $(BUILD)/remapper

$(BUILD):
	mkdir -p $(BUILD)

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

$(BUILD)/test_interpose: test_interpose.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $<

$(BUILD)/verify_test_interpose: verify_test_interpose.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $<

$(BUILD)/hardened_test: hardened_test.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $<
	codesign --force -s - --options runtime $@

$(BUILD)/spawn_hardened: spawn_hardened.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $<

$(BUILD)/hardened_interp: hardened_interp.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $<
	codesign --force -s - --options runtime $@

$(BUILD)/hardened_spawner: spawn_hardened.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $<
	codesign --force -s - --options runtime $@

test: all $(BUILD)/test_interpose $(BUILD)/verify_test_interpose $(BUILD)/hardened_test $(BUILD)/spawn_hardened $(BUILD)/hardened_interp $(BUILD)/hardened_spawner
	./test.sh

deploy:
	./deploy.sh

clean:
	rm -rf $(BUILD)

.PHONY: all clean test deploy
