TARGET    := coremark
CROSS     ?= aarch64-linux-gnu-
BOARD     ?= opz3w
BOARD_DIR ?= coremark
ITERATIONS ?= 200000
SOURCES   := core_list_join.c core_main.c core_matrix.c core_state.c \
             core_util.c linux/core_portme.c linux/core_portme.h

FLAGS := -O3 -funroll-loops -flto \
         -march=armv8-a \
         -I. -Ilinux \
         -DFLAGS_STR=\"\" \
         -DSEED_METHOD=SEED_VOLATILE -DMEM_METHOD=MEM_MALLOC \
         -DPERFORMANCE_RUN=1 -DITERATIONS=$(ITERATIONS)

all: $(TARGET)

$(TARGET): $(SOURCES)
	$(CROSS)gcc $(FLAGS) $(SOURCES) -o $@

push: $(TARGET)
	ssh $(BOARD) "mkdir -p $(BOARD_DIR)"
	rsync -az $(TARGET) $(BOARD):$(BOARD_DIR)/
	ssh $(BOARD) "cd $(BOARD_DIR) && chmod 755 $(TARGET)"

# cpu6 = A76 (big cluster), cpu0 = A55 (little cluster)
run:
	ssh $(BOARD) "cd $(BOARD_DIR) && taskset -c 6 ./coremark && taskset -c 0 ./coremark"

# performance governor on both clusters (we ssh in as root),
# schedutil restored afterwards
run-perf:
	ssh $(BOARD) 'cd $(BOARD_DIR) && for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do echo performance > $$g; done && taskset -c 6 ./coremark && taskset -c 0 ./coremark; for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do echo schedutil > $$g; done'

size: $(TARGET)
	$(CROSS)size $(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all push run run-perf size clean
