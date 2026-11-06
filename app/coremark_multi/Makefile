# coremark_multi - multi-core CoreMark for the Orange Pi Zero 3W (A733)
CROSS    ?= aarch64-linux-gnu-
BOARD    ?= opz3w
BOARD_DIR ?= coremark_multi
COREMARK_ITERATIONS ?= 160000
CFLAGS  := -I. -Ilinux -O3 -funroll-loops -flto -march=armv8-a \
           -DPERFORMANCE_RUN=1 -DSEED_METHOD=SEED_VOLATILE \
           -DMEM_METHOD=MEM_MALLOC -DITERATIONS=$(COREMARK_ITERATIONS) \
           -DCOMPILER_FLAGS="\"GCC15.2 -O3 -flto armv8-a\"" \
           -DMEM_LOCATION="\"DDR_HEAP\"" \
           -Wall
CORE_SRCS := core_list_join.c core_main.c core_matrix.c \
             core_state.c core_util.c linux/core_portme.c

all: coremark_multi

core_main.o: core_main.c coremark.h linux/core_portme.h
	$(CROSS)gcc $(CFLAGS) -Dmain=coremark_entry -c $< -o $@

linux_portme.o: linux/core_portme.c linux/core_portme.h coremark.h
	$(CROSS)gcc $(CFLAGS) -c $< -o $@

%.o: %.c coremark.h
	$(CROSS)gcc $(CFLAGS) -c $< -o $@

coremark_multi: coremark_multi.o core_main.o core_list_join.o \
                core_matrix.o core_state.o core_util.o linux_portme.o
	$(CROSS)gcc $(CFLAGS) -o $@ $^ -lrt

push: coremark_multi
	ssh $(BOARD) "mkdir -p $(BOARD_DIR)"
	rsync -az coremark_multi $(BOARD):$(BOARD_DIR)/
	ssh $(BOARD) "cd $(BOARD_DIR) && chmod 755 coremark_multi"

# schedutil quick run (governor may not reach max freq instantly)
run:
	ssh $(BOARD) "cd $(BOARD_DIR) && ./coremark_multi --round-robin --loops 1 --secs 10"

# all 8 cores at max frequency: performance governor on both clusters
# (cpu0-5 A55 @ 1794 MHz, cpu6-7 A76 @ 2002 MHz; we ssh in as root),
# schedutil restored afterwards; rc carries the app's exit status
run-perf:
	ssh $(BOARD) 'cd $(BOARD_DIR) && for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do echo performance > $$g; done && ./coremark_multi --round-robin --parallel --loops 2 --secs 10; rc=$$?; for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do echo schedutil > $$g; done; exit $$rc'

clean:
	rm -f *.o coremark_multi

.PHONY: all push run run-perf clean
