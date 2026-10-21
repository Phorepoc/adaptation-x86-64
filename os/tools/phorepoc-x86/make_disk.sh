#!/usr/bin/env bash
# Copyright The Phorepoc Authors
# SPDX-License-Identifier: Apache-2.0

# make_disk.sh -- IN-CONTAINER: the Path C test hard disk (X4.22). A raw image with an MBR and one FAT32
# primary partition (type 06, from sector 2048), formatted with mtools, a README on it. The kernel's ATA
# driver (ekern/xk_ata.cpp) finds the partition and ESTART mounts it as D:.
#   out:   $KOUT/disk/phorepoc-disk.img  (QEMU: make_ekern.sh DISK=1 run; ph image vbox converts it to a VMDK)
# LABEL=: the FAT volume label (the memory card image is built with LABEL="MEMORY CARD", X4.111).
# An existing image is kept (it is the persistent disk); FRESH=1 rebuilds it.
set -eu
MB="${1:-64}"
OUT="${KOUT:-/out}/disk"
IMG="${IMG:-$OUT/phorepoc-disk.img}"   # IMG=: another image (app_regress.sh: its blank template)
mkdir -p "$OUT"
if [ -f "$IMG" ] && [ "${FRESH:-0}" != 1 ]; then echo "keeping $IMG (FRESH=1 to rebuild)"; exit 0; fi
rm -f "$IMG"
truncate -s "${MB}M" "$IMG"
python3 - "$IMG" "$MB" <<'EOF'
import struct, sys
img, mb = sys.argv[1], int(sys.argv[2])
total = mb * 2048
first, count = 2048, mb * 2048 - 2048
entry = struct.pack("<B3sB3sII", 0x80, b"\xfe\xff\xff", 0x0C, b"\xfe\xff\xff", first, count)   # 0x0C = FAT32 (LBA)
with open(img, "r+b") as f:
    f.seek(0x1BE); f.write(entry)
    f.seek(0x1FE); f.write(b"\x55\xaa")
EOF
# mtools addresses a partition by byte offset (@@). FAT32 (2026-10-12, user: FAT16 is out -- C: is FAT32 through efat32.fsy,
# tools/phorepoc-x86/board/estart.txt, as on the PinePhone): clusters from 512 B, doubled while there would be more than
# 262144 of them (64 MB: 512 B, 1 GB: 4 KB); FAT32 itself needs >= 65525 clusters, true from 32 MB at 512 B
C=1
while [ $(( (MB - 1) * 2048 / C )) -gt 262144 ] && [ $C -lt 64 ]; do C=$(( C * 2 )); done
mformat -i "$IMG@@1M" -F -v "${LABEL:-PHOREPOC}" -c $C -T $(( (MB - 1) * 2048 )) -h 16 -s 63 -H 2048 ::
printf 'Hello from a persistent disk.\r\nWritten on the host by make_disk.sh (mtools), read by EKA2 on amd64.\r\n' > "${IMG}.readme"
mcopy -i "$IMG@@1M" "${IMG}.readme" ::README.TXT
# the phone-memory root a factory C: image has (PathInfo::PhoneMemoryRootPath() = C:\Data\): S60/Qt code creates
# only the LAST level below it (Qt's tempPath() = C:\Data\temp\ -> Orbit's theme server lock file, S^4 S4.4)
rm -f "${IMG}.readme"
mmd -i "$IMG@@1M" ::/Data
mdir -i "$IMG@@1M" ::
ls -l "$IMG"
