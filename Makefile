CC = gcc
CFLAGS = -DPANDORA -Iinclude -D_POSIX_C_SOURCE=200809L
LDFLAGS = -L../../lib/libacl/build -L../../lib/libcurl/build -lacl -lcurl 

BUILD_DIR = build
SRC_DIR = src

SRC = $(shell find $(SRC_DIR) -name '*.c')
OBJ = $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(SRC))

TARGET = build/pandora

.PHONY: all clean run crun

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR)
	rm -f $(TARGET)

run: all
	@./$(TARGET)

crun: clean run