#!/bin/bash
# Build, launch the emery emulator headless, and install the watchface.
#
# pebble-tool (5.0.37/5.0.38) cannot launch this emulator reliably on a fast
# Linux host. Hard-won facts this script encodes:
#   * qemu's tcp serial chardevs treat a client's FIN as a disconnect, and a
#     plain `nc host port` with stdin at EOF sends FIN immediately -- so any
#     serial client must hold its stdin open or qemu silently drops it and
#     discards all guest output (this is why pebble-tool's launch usually
#     hangs on "Waiting for the firmware to boot": its serial connection
#     races the boot banner, and output sent while disconnected is lost).
#   * a monitor system_reset during the FIRST boot of a fresh SPI image
#     interrupts filesystem formatting and corrupts it -> silent boot loop;
#     every later handshake (pypkjs, install) then times out. First boot must
#     complete untouched. If the emulator misbehaves, delete $SPI_IMG.
#   * pypkjs must be the first bluetooth client of a fully-booted firmware or
#     its watch-version handshake times out and it dies.
# Sequence: boot with a held-open console reader attached -> see the boot
# marker -> reset (safe: filesystem initialized) -> second boot -> pypkjs
# attaches -> register the emulator in /tmp/pb-emulator.json -> pebble
# install adopts it. After this, normal commands work as usual:
# pebble screenshot / logs / emu-app-config / send-app-message / kill.
set -u

PLATFORM="${1:-emery}"

SDK_BASE="$HOME/.local/share/pebble-sdk"
VERSION=$(basename "$(readlink -f "$SDK_BASE/SDKs/current")")
SDK_QEMU_DIR="$SDK_BASE/SDKs/current/sdk-core/pebble/$PLATFORM/qemu"
TOOL_PY="$HOME/.local/share/uv/tools/pebble-tool/bin/python"
QEMU_BIN="$SDK_BASE/SDKs/current/toolchain/bin/qemu-pebble"
PERSIST_DIR="$SDK_BASE/$VERSION/$PLATFORM"
SPI_IMG="$PERSIST_DIR/qemu_spi_flash.bin"
CONSOLE_CAP="/tmp/qemu-console.bin"

# fixed ports (single emulator instance)
BT_PORT=12342 CON_PORT=12343 GDB_PORT=12344 MON_PORT=12345 PKJS_PORT=12346

# secrets.json is gitignored but required by the build (bundled via require())
if [ ! -f src/pkjs/secrets.json ]; then
  cp src/pkjs/secrets.json.example src/pkjs/secrets.json
  echo "Created src/pkjs/secrets.json from example -- fill in your TfNSW key and stop IDs."
fi

pebble build || exit 1

# clean slate (-x matches the process name exactly so we can't kill ourselves)
pkill -x qemu-pebble 2>/dev/null
pkill -x pypkjs 2>/dev/null
for p in $(pgrep -f 'python -m pypkjs'); do kill "$p" 2>/dev/null; done
for p in $(pgrep -x nc) $(pgrep -x ncat); do kill -9 "$p" 2>/dev/null; done
rm -f /tmp/pb-emulator.json "$CONSOLE_CAP"
sleep 1

FIRST_BOOT=""
if [ ! -f "$SPI_IMG" ]; then
  mkdir -p "$PERSIST_DIR"
  "$TOOL_PY" -c "import bz2, shutil; shutil.copyfileobj(bz2.open('$SDK_QEMU_DIR/qemu_spi_flash.bin.bz2'), open('$SPI_IMG', 'wb'))"
  echo "Created fresh SPI flash image."
  FIRST_BOOT=1
fi

SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-dummy}" "$QEMU_BIN" \
  -rtc base=localtime \
  -serial null \
  -serial "tcp::$BT_PORT,server=on,wait=off" \
  -serial "tcp::$CON_PORT,server=on,wait=off" \
  -kernel "$SDK_QEMU_DIR/qemu_micro_flash.bin" \
  -gdb "tcp::$GDB_PORT,server=on,wait=off" \
  -monitor "tcp::$MON_PORT,server=on,wait=off" \
  -machine "pebble-$PLATFORM" -cpu cortex-m33 \
  -drive "if=mtd,format=raw,file=$SPI_IMG" \
  -audio driver=sdl,id=audio0 -display sdl,show-cursor=on \
  > /tmp/qemu-pebble.log 2>&1 &
QEMU_PID=$!

# wait for qemu to listen (an ss probe does not consume the client slot),
# then attach a console reader whose stdin stays open (see header comment)
for _ in $(seq 1 300); do
  ss -tln 2>/dev/null | grep -q ":$CON_PORT " && break
  kill -0 "$QEMU_PID" 2>/dev/null || { echo "qemu died:"; tail -3 /tmp/qemu-pebble.log; exit 1; }
  sleep 0.1
done
nc localhost "$CON_PORT" > "$CONSOLE_CAP" < <(sleep 600) 2>/dev/null &
READER_PID=$!

wait_for_marker() {
  for _ in $(seq 1 120); do
    grep -aq -e "Ready for communication" -e "<SDK Home>" "$CONSOLE_CAP" 2>/dev/null && return 0
    kill -0 "$QEMU_PID" 2>/dev/null || return 1
    sleep 0.5
  done
  return 1
}

if ! wait_for_marker; then
  echo "Firmware did not boot within 60s. Delete $SPI_IMG and rerun (a corrupt" >&2
  echo "image from a hard-killed emulator boot-loops silently)." >&2
  kill "$QEMU_PID" "$READER_PID" 2>/dev/null
  exit 1
fi
echo "Firmware booted."
# on a fresh image, give the first boot time to finish filesystem writes
[ -n "$FIRST_BOOT" ] && sleep 8

# reboot so pypkjs becomes the first bluetooth client of a clean boot;
# the still-connected reader captures the second boot's marker
> "$CONSOLE_CAP"
( echo system_reset; sleep 2 ) | timeout 6 nc localhost "$MON_PORT" >/dev/null 2>&1
echo "Rebooting firmware..."
if ! wait_for_marker; then
  echo "Firmware did not come back after reset. Delete $SPI_IMG and rerun." >&2
  kill "$QEMU_PID" "$READER_PID" 2>/dev/null
  exit 1
fi
echo "Firmware rebooted."
kill "$READER_PID" 2>/dev/null

"$TOOL_PY" -m pypkjs \
  --qemu "localhost:$BT_PORT" --port "$PKJS_PORT" \
  --persist "$PERSIST_DIR" --layout "$SDK_QEMU_DIR/layouts.json" \
  > /tmp/pypkjs.log 2>&1 &
PKJS_PID=$!

for _ in $(seq 1 30); do
  ss -tln 2>/dev/null | grep -q ":$PKJS_PORT " && break
  kill -0 "$PKJS_PID" 2>/dev/null || { echo "pypkjs died:" >&2; tail -5 /tmp/pypkjs.log >&2; exit 1; }
  sleep 0.5
done
echo "pypkjs ready."

# hand the running emulator to pebble-tool
cat > /tmp/pb-emulator.json <<EOF
{"$PLATFORM": {"$VERSION": {
  "qemu":   {"pid": $QEMU_PID, "port": $BT_PORT, "serial": $CON_PORT, "gdb": $GDB_PORT, "monitor": $MON_PORT, "vnc": false},
  "pypkjs": {"pid": $PKJS_PID, "port": $PKJS_PORT},
  "version": "$VERSION"
}}}
EOF

pebble install --emulator "$PLATFORM" && { echo "Install OK."; exit 0; }
echo "First install attempt failed; retrying once..."
sleep 5
pebble install --emulator "$PLATFORM" && echo "Install OK on retry."
