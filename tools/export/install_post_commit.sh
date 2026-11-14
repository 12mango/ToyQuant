#!/bin/bash
# Install (or repair) the post-commit hook that syncs this tree to the Drive folder after every commit.
#
# Run once per clone: `bash tools/export/install_post_commit.sh`. `.git/hooks` is not versioned, so this installer is
# what makes the rule in `.clinerules` section 6 reproducible on a fresh checkout -- and what repairs a hook whose sync
# block sits after the final `exit 0`, which is how the first version of this block was silently skipped.
#
# The hook runs after the commit and its exit status cannot change it, so a failed sync cannot lose a commit. What it
# can do is be silent, so it appends to a log, and the log is what the next reader checks:
#
#     tail -3 /tmp/toyquant-sync.log
set -eu
cd "$(dirname "$0")/../.." || exit 1
HOOK=".git/hooks/post-commit"
mkdir -p .git/hooks
[ -f "$HOOK" ] || printf '#!/bin/bash\n' > "$HOOK"

python3 - "$HOOK" <<'PY'
import pathlib
import sys

hook = pathlib.Path(sys.argv[1])
marker = "# toyquant drive sync"
block = """
# toyquant drive sync -- writes the bundle and copies it to the folder a Drive client uploads from.
# Never blocks the commit: a post-commit hook cannot undo one, and a failed sync is reported in the log.
{
    echo "=== $(date -Is) $(git rev-parse --short HEAD)"
    bash tools/export/push_windows.sh || echo "sync failed: $?"
} >> /tmp/toyquant-sync.log 2>&1
"""
text = hook.read_text()
if marker in text:
    print(f"already installed: {hook}")
    sys.exit(0)
lines = text.splitlines(keepends=True)
for index in range(len(lines) - 1, -1, -1):
    if lines[index].strip() == "exit 0":
        lines.insert(index, block)
        break
else:
    lines.append(block)
hook.write_text("".join(lines))
print(f"installed: {hook} (log: /tmp/toyquant-sync.log)")
PY
chmod +x "$HOOK"
