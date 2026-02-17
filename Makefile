CC      = gcc
CFLAGS  = -Wall -Wextra -O2
BUILD   = build

all: $(BUILD)/interpose.dylib $(BUILD)/remapper $(BUILD)/test_interpose $(BUILD)/verify_test_interpose $(BUILD)/hardened_test $(BUILD)/spawn_hardened

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/rmp_shared.o: rmp_shared.c rmp_shared.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ rmp_shared.c

$(BUILD)/interpose.dylib: interpose.c $(BUILD)/rmp_shared.o rmp_shared.h | $(BUILD)
	$(CC) $(CFLAGS) -dynamiclib -o $@ interpose.c $(BUILD)/rmp_shared.o

$(BUILD)/remapper: remapper.c $(BUILD)/rmp_shared.o rmp_shared.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ remapper.c $(BUILD)/rmp_shared.o

$(BUILD)/test_interpose: test_interpose.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $<

$(BUILD)/verify_test_interpose: verify_test_interpose.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $<

$(BUILD)/hardened_test: hardened_test.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $<
	codesign --force -s - --options runtime $@

$(BUILD)/spawn_hardened: spawn_hardened.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $<

test: all
	./test.sh

clean:
	rm -rf $(BUILD)

.PHONY: all clean test
