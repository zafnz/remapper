CC      = gcc
CFLAGS  = -Wall -Wextra -O2
BUILD   = build

all: $(BUILD)/interpose.dylib $(BUILD)/remapper $(BUILD)/test_interpose $(BUILD)/verify_test_interpose

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

test: all
	@TMPDIR=$$(mktemp -d) && \
	echo "--- Running test_interpose under interposer ---" && \
	$(BUILD)/remapper "$$TMPDIR" "$$HOME/.dummy*" -- $(BUILD)/test_interpose && \
	echo "" && \
	echo "--- Running verify_test_interpose (no interposer) ---" && \
	$(BUILD)/verify_test_interpose "$$TMPDIR" "$$HOME" && \
	rm -rf "$$TMPDIR" && \
	echo "--- All tests passed ---"

clean:
	rm -rf $(BUILD)

.PHONY: all clean test
