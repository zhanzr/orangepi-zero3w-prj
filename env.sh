# Orange Pi Zero 3W cross toolchain environment
# source from anywhere:  . ~/opz3w-dev/env.sh
export OPZ3W_CROSS=aarch64-linux-gnu-
export OPZ3W_BOARD=opz3w   # ssh alias, see ~/.ssh/config
echo "opz3w toolchain ready: ${OPZ3W_CROSS}gcc ($( ${OPZ3W_CROSS}gcc -dumpversion ))"
