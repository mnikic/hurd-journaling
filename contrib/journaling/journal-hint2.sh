#!/usr/bin/env bash
set -euo pipefail

usage() { echo "Usage: $0 <image.img> {show|on|off}" >&2; exit 1; }
[[ $# -ge 2 ]] || usage

img="$1"; cmd="$2"

need() { command -v "$1" >/dev/null || { echo "need $1" >&2; exit 1; }; }
need fdisk; need awk; need xxd; need python3

# --- Parse fdisk once ---------------------------------------------------------
# Sector size (first number on the "Sector size" line)
sector_bytes=$(fdisk -l "$img" | awk -F'[: /]+' '/Sector size/{print $(NF-3)}')
if [[ -z "${sector_bytes:-}" || "$sector_bytes" -le 0 ]]; then
  echo "[ERROR] failed to get sector size from fdisk" >&2; exit 1
fi

# Grab partition 2 (the ext2 partition). We assume "…img2" line is the one we want.
base=$(basename "$img")
img2="$base"2
read start_sector total_sectors <<EOF
$(fdisk -l "$img" | awk -v pat="^${img2}$" '
  $1 ~ pat {print $2, $4; exit}')
EOF

if [[ -z "${start_sector:-}" || -z "${total_sectors:-}" ]]; then
  echo "[ERROR] could not find partition 2 in fdisk output" >&2; exit 1
fi

part_start_bytes=$(( start_sector * sector_bytes ))
sb_off=$(( part_start_bytes + 1024 ))
jh_off=$(( sb_off + 0x108 ))

# Verify EF53 at superblock + 0x38
magic_hex=$(xxd -p -s $((sb_off + 0x38)) -l 2 "$img")
if [[ "$magic_hex" != "53ef" ]]; then
  echo "[ERROR] ext2 magic not found at $((sb_off + 0x38)) (got $magic_hex). Aborting." >&2
  echo "        start_sector=$start_sector sector_bytes=$sector_bytes sb_off=$sb_off" >&2
  exit 1
fi

echo "[INFO] image: $img"
echo "[INFO] sector size:         $sector_bytes"
echo "[INFO] part2 start sector:  $start_sector"
echo "[INFO] part2 total sectors: $total_sectors"
echo "[INFO] superblock offset:   $sb_off"
echo "[INFO] journal_hint offset: $jh_off"

show_hint() {
  echo "[INFO] Hex @ journal_hint:"
  xxd -g1 -s "$jh_off" -l 16 "$img" || true
  echo "[INFO] Parsed:"
  IMG="$img" OFF="$jh_off" python3 - <<'PY'
import os, struct, sys
img=os.environ["IMG"]; off=int(os.environ["OFF"])
with open(img,"rb") as f:
    f.seek(off); d=f.read(16)
if len(d)<16: sys.exit("short read at journal_hint")
sb,bc,crc,mag = struct.unpack("<III4s", d)
print(f"  start_block=0x{sb:08x} ({sb})")
print(f"  block_count=0x{bc:08x} ({bc})")
print(f"  crc32=0x{crc:08x} ({crc})")
print(f"  magic={mag!r}")
PY
}

case "$cmd" in
  show)
    show_hint
    ;;

  on)
    # Last 8 MiB of partition: 8 MiB / 512 = 16384 sectors
    block_count=16384
    start_block=$(( total_sectors - block_count ))           # relative to partition start, in sectors
    journal_abs=$(( part_start_bytes + start_block * sector_bytes ))

    printf "[INFO] start_block (sectors rel->part): %d (0x%08x)\n" "$start_block" "$start_block"
    printf "[INFO] block_count (sectors):           %d (0x%08x)\n" "$block_count" "$block_count"
    printf "[INFO] journal start (abs bytes):       %d\n" "$journal_abs"

    # Write <start_block><block_count><crc32=0><magic='JNLH'>
    IMG="$img" OFF="$jh_off" SB="$start_block" BC="$block_count" \
    python3 - <<'PY'
import os, struct
img=os.environ["IMG"]; off=int(os.environ["OFF"])
sb=int(os.environ["SB"]); bc=int(os.environ["BC"])
blob=struct.pack("<III4s", sb, bc, 0, b"JNLH")
with open(img,"r+b") as f:
    f.seek(off); f.write(blob)
PY
    echo "[OK] journaling enabled (JNLH written using fdisk-based math)."
    show_hint
    ;;

  off)
    # Zero only magic
    dd if=/dev/zero of="$img" bs=1 seek=$((jh_off+12)) count=4 conv=notrunc status=none
    echo "[OK] journaling disabled (magic cleared)."
    show_hint
    ;;

  *)
    usage
    ;;
esac

