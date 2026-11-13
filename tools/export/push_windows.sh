#!/bin/bash
# Sync the tree to the Windows folder a Drive client uploads from, and verify what landed.
#
# WSL cannot see a drive it has not mounted and this host has no passwordless sudo, so the last mile goes through
# powershell.exe: the bundle is written here (a Linux directory), copied to the Windows path, and then the remote file
# sizes and sha256 are compared with the local ones. A copy nobody checked is what the stale snapshot was.
#
# The destination is read from .drive-target (git-ignored, one line, e.g. G:\我的云端硬盘\AI_Code_Sync) or from
# $TOYQUANT_DRIVE_DIR. A missing destination is an error that names it -- never a silent skip.
set -u
cd "$(dirname "$0")/../.." || exit 1

TARGET_FILE=".drive-target"
DEST="${TOYQUANT_DRIVE_DIR:-}"
if [ -z "$DEST" ] && [ -f "$TARGET_FILE" ]; then
    DEST="$(sed -n 's/^[[:space:]]*//; s/[[:space:]]*$//; /^#/d; /^$/d; p; q' "$TARGET_FILE")"
fi
if [ -z "$DEST" ]; then
    echo "no destination: write it to $TARGET_FILE (one line) or set TOYQUANT_DRIVE_DIR" >&2
    exit 1
fi
if ! command -v powershell.exe >/dev/null 2>&1; then
    echo "$DEST is a Windows path and this host has no powershell.exe; on a POSIX host set TOYQUANT_DRIVE_DIR to a" \
         "directory the client syncs and run: python3 tools/export/bundle.py --sync --out \$TOYQUANT_DRIVE_DIR" >&2
    exit 1
fi

OUT="out/export"
python3 tools/export/bundle.py --sync --out "$OUT" || exit 1

UNC="$(wslpath -w "$(pwd)/$OUT")"
echo "copying $OUT -> $DEST"
# Every call into Windows is bounded. WSL interop can block indefinitely when the Windows side is busy (a virus scan of
# a 4 MB copy, a Drive FS flush), and a hung interop call hangs whatever shell or hook started it -- which is how this
# repository's terminal wedged once, with every later command queued behind it.
timeout 60 powershell.exe -NoProfile -Command "Copy-Item -Force -Path '$UNC\\*' -Destination '$DEST'" >/dev/null || {
    echo "copy to $DEST failed or timed out" >&2; exit 1; }

status=0
for name in code.txt docs.txt manifest.txt diff.patch; do
    [ -f "$OUT/$name" ] || continue
    local_size=$(stat -c %s "$OUT/$name")
    local_hash=$(sha256sum "$OUT/$name" | cut -d' ' -f1)
    remote_size=$(timeout 30 powershell.exe -NoProfile -Command "(Get-Item '$DEST\\$name').Length" 2>/dev/null \
        | tr -d '\r\n ')
    remote_hash=$(timeout 30 powershell.exe -NoProfile -Command "(Get-FileHash -Algorithm SHA256 '$DEST\\$name').Hash" \
        2>/dev/null | tr -d '\r\n ' | tr 'A-F' 'a-f')
    if [ "$local_size" != "$remote_size" ] || [ "$local_hash" != "$remote_hash" ]; then
        echo "FAIL $name: local ${local_size}B/${local_hash:0:12} remote ${remote_size:-?}B/${remote_hash:0:12}" >&2
        status=1
    else
        echo "ok $name ${local_size}B ${local_hash:0:12}"
    fi
done
if [ "$status" -eq 0 ]; then
    echo "synced $(git rev-parse --short HEAD) -> $DEST"
fi
exit "$status"
