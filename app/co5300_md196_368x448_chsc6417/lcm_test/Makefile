TARGET    := lcm_test
CROSS     ?= aarch64-linux-gnu-
CROSS_FLAGS := -O2 -Wall -Wextra -march=armv8-a
BOARD     ?= opz3w
BOARD_DIR ?= co5300_md196_368x448_chsc6417/lcm_test
SOURCES   := main.c lcd.c spi_hw.c touch.c log.c \
             ref/lcd_fonts.c ref/lcd_font_1608.c

all: $(TARGET)

$(TARGET): main.c lcd.c spi_hw.c touch.c log.c spi_hw.h lcd.h touch.h log.h \
           ref/lcd_fonts.h ref/lcd_font_1608.h logo64.h config.json
	$(CROSS)gcc $(CROSS_FLAGS) $(SOURCES) -o $@

push: $(TARGET)
	ssh $(BOARD) "mkdir -p $(BOARD_DIR)"
	rsync -az $(TARGET) config.json $(BOARD):$(BOARD_DIR)/
	ssh $(BOARD) "cd $(BOARD_DIR) && chmod 755 $(TARGET)"

# the demo loops forever - Ctrl-C to stop
run:
	ssh -t $(BOARD) "cd $(BOARD_DIR) && ./$(TARGET)"

# single run at a given speed, e.g.: make run-s SPEED=40000
SPEED ?= 40000
run-s:
	ssh -t $(BOARD) "cd $(BOARD_DIR) && timeout 60 ./$(TARGET) -s $(SPEED)"

deploy:
	ssh $(BOARD) "mkdir -p $(BOARD_DIR)"
	rsync -az main.c lcd.c spi_hw.c touch.c log.c spi_hw.h lcd.h touch.h \
	    log.h ref logo64.h config.json README.md $(BOARD):$(BOARD_DIR)/

build-remote:
	ssh $(BOARD) "cd $(BOARD_DIR) && make CC=gcc"

clean:
	rm -f $(TARGET)

.PHONY: all push run run-s deploy build-remote clean
