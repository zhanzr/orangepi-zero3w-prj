"""Reproducible CAM2 ISP-profile patch for libisp.so (A733, ISP602).

Applies to the libisp.so shipped in Radxa's libAWIspApi_602_1.0.0
(build of radxa/allwinner-isp6xx). See docs/csi-ov5647-notes.md,
sections "Incorrect color" and "CAM2 tuning fix". In short:

1. The imx219 1080p profile entry's fps 30 -> 1, matching the frame
   interval the pipeline actually reports (unset = 1; G_PARM is
   unsupported by the BSP vin driver), so CAM1 exact-matches the
   calibrated imx219_1080p profile.
2. The imx219_3280_2464 (8MP) entry is renamed to sensor "imx219_2"
   with 1920x1080@1, so CAM2 (sensor imx219_2) exact-matches the same
   calibrated tuning instead of falling into the dark 8MP profile.

Usage: point IN at the libisp.so to patch (default /tmp/libisp_so_radxa
= the pristine Radxa libAWIspApi_602_1.0.0 libisp.so); writes
/tmp/libisp_so_full. Deploy to /usr/lib/aarch64-linux-gnu/libisp.so
and ldconfig. Backups: keep the original before overwriting.
"""

import struct

DATA = bytearray(open("/tmp/libisp_so_radxa", "rb").read())

def find_entry(cfg_name_bytes):
    i = 0
    while True:
        i = DATA.find(cfg_name_bytes + b"\x00", i)
        if i < 0:
            return None
        sn = bytes(DATA[i - 20:i - 13])
        if sn[:6] == b"imx219" and sn[6:7] == b"\x00":
            return i - 20
        i += 1

e1080 = find_entry(b"imx219_1080p_isp_cfg")
e3280 = find_entry(b"imx219_3280_2464_isp_cfg")
if e1080 is None or e3280 is None:
    raise SystemExit("entries not found")

# 1. CAM1: 1080p entry fps 30 -> 1
struct.pack_into("<i", DATA, e1080 + 72 + 8, 1)

# 2. CAM2: rename the 3280_2464 entry into an imx219_2 1080p entry
DATA[e3280:e3280 + 20] = b"imx219_2".ljust(20, b"\x00")
DATA[e3280 + 20:e3280 + 70] = b"imx219_1080p_isp_cfg".ljust(50, b"\x00")
struct.pack_into("<5i", DATA, e3280 + 72, 1920, 1080, 1, 0, 0)

# 3. the entry's cfg pointer relocation -> the 1080p profile struct
e_phoff = struct.unpack_from("<Q", DATA, 0x20)[0]
e_phentsize = struct.unpack_from("<H", DATA, 0x36)[0]
e_phnum = struct.unpack_from("<H", DATA, 0x38)[0]
loads = []
for i in range(e_phnum):
    off = e_phoff + i * e_phentsize
    if struct.unpack_from("<I", DATA, off)[0] == 1:
        po, pv, pfs = struct.unpack_from("<QQQ", DATA, off + 8)
        loads.append((po, pv, pfs))

def off2v(off):
    for o, v, fs in loads:
        if o <= off < o + fs:
            return v + (off - o)
    return None

e_shoff = struct.unpack_from("<Q", DATA, 0x28)[0]
e_shentsize = struct.unpack_from("<H", DATA, 0x3A)[0]
e_shnum = struct.unpack_from("<H", DATA, 0x3C)[0]
rela_off = rela_sz = None
for i in range(e_shnum):
    off = e_shoff + i * e_shentsize
    if struct.unpack_from("<I", DATA, off + 4)[0] == 4:
        rela_off, rela_sz = struct.unpack_from("<QQ", DATA, off + 24)
        break

# the 1080p profile struct address = the st_value of the exported symbol
# imx219_1080p_isp_cfg (from the earlier .dynsym patch run: 0x3968d0)
V1080_STRUCT = 0x3968d0

v3280ptr = off2v(e3280 + 96)   # cfg pointer field (after 12 reserved bytes)
found = False
n = rela_sz // 24
for j in range(n):
    r_offset, _info, _addend = struct.unpack_from("<QQq", DATA, rela_off + j * 24)
    if r_offset == v3280ptr:
        struct.pack_into("<q", DATA, rela_off + j * 24 + 16, V1080_STRUCT)
        found = True
        print(f"re-pointed reloc @ {r_offset:#x} -> {V1080_STRUCT:#x}")
        break
if not found:
    print("WARN: no relocation at the cfg pointer field - dlsym path will resolve by cfg name")

open("/tmp/libisp_so_full", "wb").write(DATA)
print("wrote /tmp/libisp_so_full")
