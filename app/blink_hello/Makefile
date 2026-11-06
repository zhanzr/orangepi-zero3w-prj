TARGET    := blink_hello
CROSS     ?= aarch64-linux-gnu-
CROSS_FLAGS := -O2 -Wall -Wextra -march=armv8-a
BOARD     ?= opz3w
BOARD_DIR ?= $(TARGET)
SOURCES   := $(TARGET).c Makefile README.md

all: $(TARGET)

$(TARGET): $(TARGET).c
	$(CROSS)gcc $(CROSS_FLAGS) -o $@ $<

push: $(TARGET)
	ssh $(BOARD) "mkdir -p $(BOARD_DIR)"
	rsync -az $(TARGET) $(BOARD):$(BOARD_DIR)/

# we ssh in as root - the LED sysfs nodes are root-owned (0644),
# so no sudo or udev rule is needed on this board
run:
	ssh -t $(BOARD) "./$(BOARD_DIR)/$(TARGET)"

stop:
	ssh $(BOARD) "pkill -x $(TARGET) || true"

deploy:
	ssh $(BOARD) "mkdir -p $(BOARD_DIR)"
	rsync -az $(SOURCES) $(BOARD):$(BOARD_DIR)/

build-remote:
	ssh $(BOARD) "cd $(BOARD_DIR) && make CC=gcc"

clean:
	rm -f $(TARGET)

.PHONY: all push run stop deploy build-remote clean
