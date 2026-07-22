#!/bin/bash
# Test rerun commands end-to-end: (rerun t), (rerun nil) parse and dispatch,
# and (rerun-once) actually causes a convergence pass. Uses -stream so
# commands are queued before compilation begins — no timing races. Uses
# refs.tex (which has TOC + cross-refs) so the first pass leaves aux dirty
# and (rerun-once) has real work to commit.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TEX_FILE="$SCRIPT_DIR/refs.tex"

if [ ! -f "$TEX_FILE" ]; then
  echo "FAIL: $TEX_FILE not found" >&2
  exit 1
fi

FIFO=$(mktemp -u /tmp/texpresso-fifo-XXXXXX)
STDERR_FILE=$(mktemp /tmp/texpresso-stderr-XXXXXX)
mkfifo "$FIFO"

cleanup() {
  rm -f "$FIFO" "$STDERR_FILE"
  kill "$PID" 2>/dev/null || true
}
trap cleanup EXIT

CONTENT=$(sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/	/\\t/g' "$TEX_FILE" | \
  awk '{ if (NR > 1) printf "\\n"; printf "%s", $0 }')

SDL_VIDEODRIVER=dummy build/texpresso -stream -test-initialize test/refs.tex \
  < "$FIFO" 2>"$STDERR_FILE" &
PID=$!

exec 3>"$FIFO"
# Exercise all three commands while paused (stream mode starts paused).
printf '(rerun t)\n' >&3
printf '(rerun nil)\n' >&3
printf '(rerun-once)\n' >&3
# Prime the VFS and start compilation.
printf '(register "%s")\n' "$TEX_FILE" >&3
printf '(open "%s" "%s")\n' "$TEX_FILE" "$CONTENT" >&3
printf '(resume)\n' >&3
exec 3>&-

if ! wait "$PID"; then
  echo "FAIL: texpresso exited with error"
  cat "$STDERR_FILE"
  exit 1
fi

FAIL=0
# Parse+dispatch assertions.
for msg in \
  '\[command\] rerun enabled' \
  '\[command\] rerun disabled' \
  '\[command\] rerun-once: pending immediate pass'
do
  if ! grep -qE "$msg" "$STDERR_FILE"; then
    echo "FAIL: missing stderr message matching: $msg"
    FAIL=1
  fi
done

# End-to-end assertion: (rerun-once) must actually fire a convergence pass.
# Emitted from the idle-block once aux_ready && rerun_once_pending are both set.
if ! grep -qE '\[rerun\] on-demand: finishing pass' "$STDERR_FILE"; then
  echo "FAIL: (rerun-once) never triggered a finishing pass"
  FAIL=1
fi

if [ $FAIL -eq 1 ]; then
  echo "--- stderr ---"
  cat "$STDERR_FILE"
  exit 1
fi

echo "PASS: rerun test"
