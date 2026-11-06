# The RISC-V XuanTie E902 coprocessor (200 MHz) - programmability

Question: the A733 datasheet lists "single-core RISC-V E902, up to
200 MHz". Can the user program it? Short answer: **on this board's
vendor Armbian image - no**; it is reserved as the vendor power-
management core (the ARISC/CPUS analog), controlled from the secure
side. On mainline (T527/A527-proven, A733 in progress) it *can* be
taken over via remoteproc - a firmware/kernel bring-up project, not
something the current image exposes.

## What the E902 is (silicon facts)

- XuanTie E902, **RV32EMC** (32-bit, 16 GPRs, no FPU), up to 200 MHz -
  auxiliary real-time core, decoupled from the A76/A55 DynamIQ cluster.
- Executes out of **SRAM A2** (208 KB @ `0x00040000`); the reset vector
  is **hardwired to `0x00040000`** - whoever owns SRAM A2 at reset owns
  the core.
- 8-channel hardware mailbox (MSGBOX `0x03003000`) for A-core <-> E902
  messaging.
- Boot access is gated in **TF-A (BL31)**: the R_SPC (secure port)
  must be un-gated for the E902 boot vector.
- Sources: A733 Datasheet V0.93 (radxa dl), the "Unlocking the XuanTie
  RISC-V Core on Allwinner T527" series (Hackster.io, tcmichals), the
  `tcmichals/cubie-a5e` repo (T527/A527 **and A733** mainline BSP;
  verified: "TF-A BL31 security un-gating ... R_SPC Port 3 un-gated
  for E902 boot vector", "XuanTie E902 remoteproc: running out of
  System SRAM A2 (0x00040000, 208 KB), dynamic sysfs lifecycle
  verified"), ELCE 2026 mainline status (`CONFIG_SUNXI_REMOTEPROC`,
  `drivers/remoteproc/sunxi_rproc.c`: works for A523, **A733 still
  missing upstream**).

## What this board's vendor image does with it (verified 2026-11-06)

Kernel `6.6.98-vendor-sun60iw2`, Armbian 26.11.0-trunk.123:

- `/sys/class/remoteproc/` - **empty**: no remoteproc instance exists.
- Device tree: no `e902`/`riscv`/`sram A2`/DSP node for Linux at all.
  The only trace is the alias `arisc-config -> /arisc_config`, whose
  sole content is `s_uart_config` - the config blob for the **secure
  UART of the vendor PM firmware** running on the E902. (Allwinner's
  name for this layer on older SoCs: ARISC / CPUS.)
- `grep arisc /proc/kallsyms` -> exactly one symbol:
  `sunxi_smc_copy_arisc_paras` - Linux reaches the PM core only via
  **SMC calls through BL31** (ATF), never directly.
- No firmware file for it in `/lib/firmware`; nothing in `lsmod`;
  dmesg silent about it.
- `reserved-memory/bl31` shows ATF holds secure SRAM (which is what
  loads/owns the PM firmware at boot).

Conclusion: on this image the E902 boots Allwinner's PM/standby
firmware before/at Linux start and is owned by BL31; **there is no
userspace or kernel API to load code onto it**. It is not usable as a
general-purpose auxiliary core here, and poking its SRAM would fight
power management (standby/resume path).

## What it would take to program it (future work)

1. Move to (or build) a **mainline stack**: mainline U-Boot + TF-A
   with the R_SPC un-gate patch + kernel with `CONFIG_SUNXI_REMOTEPROC`
   + a DT node describing SRAM A2 + MSGBOX + the E902 (the T527 DT is
   the template; A733 remoteproc is listed missing upstream as of
   ELCE 2026).
2. Write RV32EMC firmware linked to SRAM A2 (`0x00040000` entry),
   loadable via `/sys/class/remoteproc/.../firmware` (the T527 project
   demonstrates the full lifecycle: start/stop, mailbox ping-pong).
   Toolchain: XuanTie/Andes rv32 gcc or clang with `-march=rv32emc`.
3. Accept the cost: on mainline there is no vendor PM firmware, so
   whatever standby/DVFS-assist work the E902 did is gone (and on a
   vendor+mainline hybrid, hijacking the core breaks power management).

None of this is possible from the shipped vendor image - documented so
nobody burns time looking for a hook that is not there.
