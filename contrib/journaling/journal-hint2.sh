#!/usr/bin/env bash
# journal-hint2.sh — write/read an ext2 journaling hint in the primary superblock
# Supports:
#   - Whole-disk images (auto-detect ext2/3/4 partition; or --part N)
#   - Block devices/partitions (Linux or Hurd, e.g. /dev/hd0s6)
# Hint layout at SB+0x108: <start_block:u32><block_count:u32><crc32:u32><magic:'JNLH'>

set -euo pipefail

usage() {
  cat >&2 <<EOF
Usage:
  $0 <image-or-device> {show|on|off} [--size-mib N] [--part N]

Notes:
  - For whole-disk images, the script auto-detects the ext2/3/4 partition.
    Use --part N to override (N is the partition number shown by fdisk/parted).
  - For block device partitions (e.g., /dev/hd0s6 on Hurd or /dev/sda2 on Linux),
    the superblock is at +1024 bytes from the start of that node, so no partition
    scan is needed.
  - "on" places the journal at the last --size-mib of the *partition* and writes
    the hint. Default size if omitted is 8 MiB.
EOF
  exit 1
}

[[ $# -ge 2 ]] || usage

target="$1"; cmd="$2"; shift 2

# Defaults
size_mib=8
part_num=""

# Parse optional flags
while [[ $# -gt 0 ]]; do
  case "$1" in
    --size-mib) shift; size_mib="${1:-}"; [[ -n "$size_mib" ]] || usage; shift;;
    --part)     shift; part_num="${1:-}"; [[ -n "$part_num" ]] || usage; shift;;
    *) usage;;
  esac
done

need() { command -v "$1" >/dev/null || { echo "need $1" >&2; exit 1; }; }

need awk
need xxd
need python3

# --- helpers -------------------------------------------------------------

is_blockdev() { [[ -b "$1" ]]; }

# Return: sector_bytes start_sector total_sectors for a given (disk, part_num)
fdisk_info_for_part() {
  local disk="$1" pnum="$2"
  need fdisk
  local sector_bytes
  sector_bytes=$(fdisk -l "$disk" | awk -F'[: /]+' '/Sector size/{print $(NF-3); exit}')
  [[ -n "$sector_bytes" && "$sector_bytes" -gt 0 ]] || { echo "[ERR] fdisk sector size" >&2; return 1; }

  # The partition name as shown by fdisk:
  # - Linux: /dev/sda2
  # - Hurd : /dev/hd0s6
  local part_node
  if [[ "$disk" =~ ^/dev/hd[0-9]+$ ]]; then
    part_node="${disk}s${pnum}"
  else
    part_node="${disk}${pnum}"
  fi

  # Parse fdisk -l <disk> line that begins with the partition device name
  # shellcheck disable=SC2016
  read -r start total < <(fdisk -l "$disk" | awk -v pat="^${part_node}$" '
    $1 ~ pat { print $2, $4; exit }')

  [[ -n "${start:-}" && -n "${total:-}" ]] || { echo "[ERR] partition $part_node not found in fdisk" >&2; return 1; }
  echo "$sector_bytes" "$start" "$total"
}

# For a whole-disk image, auto-pick an ext2/3/4 partition (or use --part)
image_pick_ext_part() {
  local img="$1" want="$2"
  need parted
  # Scriptable output in bytes
  # Format of data lines: N:STARTB:ENDB:SIZEB:FS:NAME:FLAGS
  local lines; lines=$(parted -sm "$img" unit B print) || true
  [[ -n "$lines" ]] || { echo "[ERR] parted failed on $img" >&2; return 1; }

  local chosen_start chosen_size chosen_num
  while IFS= read -r ln; do
    [[ "$ln" == *:* ]] || continue
    # skip header lines
    [[ "$ln" =~ ^[0-9]+: ]] || continue
    IFS=':' read -r num startB endB sizeB fstype _ <<<"$ln"
    # strip trailing 'B'
    startB="${startB%B}"
    sizeB="${sizeB%B}"

    if [[ -n "$want" ]]; then
      if [[ "$num" == "$want" ]]; then
        chosen_num="$num"; chosen_start="$startB"; chosen_size="$sizeB"; break
      fi
    else
      # pick the first ext2/ext3/ext4 (ext* match)
      if [[ "$fstype" =~ ^ext(2|3|4)$ ]]; then
        chosen_num="$num"; chosen_start="$startB"; chosen_size="$sizeB"; break
      fi
    fi
  done <<< "$lines"

  [[ -n "${chosen_start:-}" && -n "${chosen_size:-}" ]] || { echo "[ERR] no suitable partition found in $img" >&2; return 1; }
  echo "$chosen_num" "$chosen_start" "$chosen_size"
}

# Superblock/journal-hint offsets given base offset (in bytes)
sb_off_from_base()  { echo $(( $1 + 1024 )); }
jh_off_from_base()  { echo $(( $(sb_off_from_base "$1") + 0x108 )); }

# Read & pretty-print current hint
show_hint_at() {
  local node="$1" off="$2"
  echo "[INFO] Hex @ journal_hint:"
  xxd -g1 -s "$off" -l 16 "$node" || true
  echo "[INFO] Parsed:"
  IMG="$node" OFF="$off" python3 - <<'PY'
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

# Write the 16-byte hint blob at given offset
write_hint_at() {
  local node="$1" off="$2" start_block="$3" block_count="$4"
  IMG="$node" OFF="$off" SB="$start_block" BC="$block_count" python3 - <<'PY'
import os, struct
img=os.environ["IMG"]; off=int(os.environ["OFF"])
sb=int(os.environ["SB"]); bc=int(os.environ["BC"])
blob=struct.pack("<III4s", sb, bc, 0, b"JNLH")
with open(img,"r+b") as f:
    f.seek(off); f.write(blob)
PY
}

# Clear only the magic (disable)
clear_magic_at() {
  local node="$1" off="$2"
  dd if=/dev/zero of="$node" bs=1 seek=$((off+12)) count=4 conv=notrunc status=none
}

# --- Decide target type & compute base offset and partition size --------

base_off=0
part_bytes=0
node="$target"

if is_blockdev "$target"; then
  # Block device / already a single partition node
  # base_off = 0; we still want the total size of this partition for "on"
  # Try to get the byte size from a few sources
  if command -v blockdev >/dev/null 2>&1; then
    part_bytes=$(blockdev --getsize64 "$target" 2>/dev/null || echo 0)
  fi
  if [[ -z "$part_bytes" || "$part_bytes" -eq 0 ]]; then
    # fdisk -l on the *disk* not the partition: we need to map /dev/hd0s6 -> /dev/hd0, 6
    if [[ "$target" =~ ^(/dev/hd[0-9]+)s([0-9]+)$ ]]; then
      disk="${BASH_REMATCH[1]}"; pnum="${BASH_REMATCH[2]}"
    else
      # Linux-style /dev/sda2
      disk="${target%%[0-9]*}"
      pnum="${target#$disk}"
    fi
    if [[ -n "${disk:-}" && -n "${pnum:-}" ]]; then
      read -r sec_bytes start_sec tot_sec < <(fdisk_info_for_part "$disk" "$pnum") || true
      if [[ -n "${sec_bytes:-}" && -n "${tot_sec:-}" ]]; then
        part_bytes=$(( tot_sec * sec_bytes ))
      fi
    fi
  fi
  if [[ -z "$part_bytes" || "$part_bytes" -eq 0 ]]; then
    echo "[WARN] Could not determine partition size; 'on' will refuse without --size-mib." >&2
  fi
else
  # Whole-disk image: find the ext* partition
  need parted
  read -r picked_num startB sizeB < <(image_pick_ext_part "$target" "$part_num")
  echo "[INFO] image: $target"
  echo "[INFO] picked partition: $picked_num"
  base_off="$startB"
  part_bytes="$sizeB"
fi

sb_off=$(sb_off_from_base "$base_off")
jh_off=$(jh_off_from_base "$base_off")

# --- Commands ----------------------------------------------------------

case "$cmd" in
  show)
    echo "[INFO] node:            $node"
    [[ "$base_off" -gt 0 ]] && echo "[INFO] partition offset: $base_off"
    echo "[INFO] superblock off : $sb_off"
    echo "[INFO] journal_hint off: $jh_off"
    # Verify EF53
    magic_hex=$(xxd -p -s $((sb_off + 0x38)) -l 2 "$node")
    [[ "$magic_hex" == "53ef" ]] || { echo "[ERROR] ext2 magic not found at $((sb_off + 0x38)) (got $magic_hex)" >&2; exit 1; }
    show_hint_at "$node" "$jh_off"
    ;;

  on)
    # Pick a size (MiB -> sectors/blocks). Our hint stores *block* indexes relative to partition start,
    # but we don’t actually need the logical ext block size here; we store counts in *sectors* as before.
    # We only need total partition bytes to ensure it fits.
    [[ "$part_bytes" -gt 0 ]] || { echo "[ERROR] cannot determine partition size; pass a whole-disk image or a partition node I can size." >&2; exit 1; }

    # Journal size and start at the tail of the partition:
    # Use 512-byte sectors to match your earlier math (fdisk math). Aligns with your existing hint usage.
    journal_bytes=$(( size_mib * 1024 * 1024 ))
    sectors=$(( journal_bytes / 512 ))
    [[ "$sectors" -gt 0 ]] || { echo "[ERROR] --size-mib too small" >&2; exit 1; }

    total_sectors=$(( part_bytes / 512 ))
    start_sector=$(( total_sectors - sectors ))
    [[ "$start_sector" -ge 0 ]] || { echo "[ERROR] requested size exceeds partition" >&2; exit 1; }

    echo "[INFO] partition bytes : $part_bytes"
    echo "[INFO] journal bytes   : $journal_bytes"
    echo "[INFO] start_sector    : $start_sector"
    echo "[INFO] sector_count    : $sectors"

    # Verify EF53 before we write
    magic_hex=$(xxd -p -s $((sb_off + 0x38)) -l 2 "$node")
    [[ "$magic_hex" == "53ef" ]] || { echo "[ERROR] ext2 magic not found at $((sb_off + 0x38)) (got $magic_hex)" >&2; exit 1; }

    write_hint_at "$node" "$jh_off" "$start_sector" "$sectors"
    echo "[OK] journaling enabled (JNLH written)."
    show_hint_at "$node" "$jh_off"
    ;;

  off)
    # Verify EF53 before we clear
    magic_hex=$(xxd -p -s $((sb_off + 0x38)) -l 2 "$node")
    [[ "$magic_hex" == "53ef" ]] || { echo "[ERROR] ext2 magic not found at $((sb_off + 0x38)) (got $magic_hex)" >&2; exit 1; }

    clear_magic_at "$node" "$jh_off"
    echo "[OK] journaling disabled (magic cleared)."
    show_hint_at "$node" "$jh_off"
    ;;

  *)
    usage;;
esac

