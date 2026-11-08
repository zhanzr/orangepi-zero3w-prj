TARGET    := csi_to_lcd
CROSS     ?= aarch64-linux-gnu-
CROSS_FLAGS := -O2 -Wall -Wextra -march=armv8-a -pthread
BOARD     ?= opz3w
BOARD_DIR ?= co5300_md196_368x448_chsc6417/csi_to_lcd
SOURCES   := csi_to_lcd.c lcd.c spi_hw.c touch.c log.c isp_replay.c \
             ref/lcd_fonts.c ref/lcd_font_1608.c

all: $(TARGET)

$(TARGET): csi_to_lcd.c lcd.c spi_hw.c touch.c log.c isp_replay.c \
           isp_replay.h spi_hw.h lcd.h touch.h log.h \
           ref/lcd_fonts.h ref/lcd_font_1608.h setup.sh
	$(CROSS)gcc $(CROSS_FLAGS) $(SOURCES) -o $@

push: $(TARGET)
	ssh $(BOARD) "mkdir -p $(BOARD_DIR)"
	rsync -az $(TARGET) setup.sh $(BOARD):$(BOARD_DIR)/
	ssh $(BOARD) "cd $(BOARD_DIR) && chmod 755 $(TARGET) setup.sh"

# run: requires the media pipeline configured (setup.sh, run once
# per boot as root after the VIN modules load). Loops forever, Ctrl-C.
run:
	ssh -t $(BOARD) "cd $(BOARD_DIR) && ./csi_to_lcd"

run-s:
	ssh -t $(BOARD) "cd $(BOARD_DIR) && timeout 30 ./csi_to_lcd -s $(SPEED)"
SPEED ?= 40000

clean:
	rm -f $(TARGET)

.PHONY: all push run run-s clean
