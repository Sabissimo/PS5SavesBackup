#!/usr/bin/env bash
# Host test for the dynamic USB detection (run through "make test").
#
# The payload is built for Linux with -DBACKUP_HOST_TEST; all console paths
# live below TEST_ROOT and the USB mount points are TEST_ROOT/mnt/usb<N>.
# As root, the read-only and unplug scenarios use real tmpfs mounts;
# otherwise they fall back to plain folders where possible or are skipped.
set -u

BIN=${1:?usage: usb_detection.sh <host binary>}
BIN=$(readlink -f "$BIN")
T=${TEST_ROOT:-$(pwd)/tests/run}
T=$(readlink -f "$T" 2>/dev/null || echo "$T")
T=${T:?}
LOG=$T/log.txt
PASS=0; FAIL=0; SKIP=0
MOUNTS=()
PID=""

cleanup() {
  [ -n "$PID" ] && kill "$PID" 2>/dev/null && wait "$PID" 2>/dev/null
  PID=""
  for m in "${MOUNTS[@]:-}"; do [ -n "$m" ] && umount -l "$m" 2>/dev/null; done
  MOUNTS=()
}
trap cleanup EXIT

ok()   { PASS=$((PASS+1)); echo "  ok   - $1"; }
bad()  { FAIL=$((FAIL+1)); echo "  FAIL - $1"; [ -n "${VERBOSE:-}" ] && sed 's/^/         | /' "$LOG"; }
check(){ if eval "$2"; then ok "$1"; else bad "$1"; fi; }
log_has()    { grep -q -- "$1" "$LOG"; }
assert_log()    { if log_has "$1"; then ok "log: $1"; else bad "log: $1"; fi; }
assert_no_log() { if log_has "$1"; then bad "no log: $1"; else ok "no log: $1"; fi; }
tree() { (cd "$1" 2>/dev/null && find . -mindepth 1 | sort) ; }

fresh() {
  cleanup
  rm -rf "${T:?}"
  mkdir -p "$T/mnt" "$T/src/sub" "$T/home" "$T/data"
  echo "alpha" > "$T/src/a.txt"
  echo "beta"  > "$T/src/sub/b.txt"
}

# conf <usbN> [extra lines...]: a backup.conf that backs up TEST_ROOT/src
conf() {
  local d=$T/mnt/$1; shift
  mkdir -p "$d"
  { echo "backup_interval_seconds=1"; echo "$T/src/"; for l in "$@"; do echo "$l"; done; } > "$d/backup.conf"
}

run_once() {   # one backup pass, then exit
  TEST_MOUNT_DIR=$T/mnt TEST_CYCLES=1 timeout 60 "$BIN" >"$LOG" 2>/dev/null
  RC=$?
}

can_mount=0
if [ "$(id -u)" = 0 ]; then
  mkdir -p "$T/mnt/probe"
  if mount -t tmpfs -o size=256m tmpfs "$T/mnt/probe" 2>/dev/null; then
    umount "$T/mnt/probe"; can_mount=1
  fi
  rmdir "$T/mnt/probe" 2>/dev/null
fi
mount_usb() { mkdir -p "$T/mnt/$1"; mount -t tmpfs -o size=256m tmpfs "$T/mnt/$1" && MOUNTS+=("$T/mnt/$1"); }
unmount_usb() { umount -l "$T/mnt/$1"; }

echo "Host binary: $BIN"
echo "Test root:   $T (mounts available: $can_mount)"

# --------------------------------------------------------------------------
for n in 0 1 2; do
  echo "== Scenario: configuration on usb$n"
  fresh
  mkdir -p "$T/mnt/usb0" "$T/mnt/usb1" "$T/mnt/usb2"
  conf usb$n
  run_once
  check "exit status 0" "[ $RC = 0 ]"
  assert_log "Scanning USB devices..."
  assert_log "Checking $T/mnt/usb0"
  assert_log "Found backup.conf"
  assert_log "Selected USB: $T/mnt/usb$n"
  assert_log "Backup destination: $T/mnt/usb$n/backup"
  assert_log "Starting backup service..."
  assert_log "Backup complete"
  check "file backed up below usb$n/backup" "[ -f '$T/mnt/usb$n/backup$T/src/sub/b.txt' ]"
  for o in 0 1 2; do
    [ $o = $n ] && continue
    check "usb$o untouched" "[ -z \"\$(tree '$T/mnt/usb$o')\" ]"
  done
  if [ $n != 0 ]; then
    assert_log "Checking $T/mnt/usb0/backup.conf ... NOT FOUND"
    assert_no_log "Selected USB: $T/mnt/usb0"
  fi
done

# --------------------------------------------------------------------------
echo "== Scenario: configuration on usb10 (numeric sorting)"
fresh
mkdir -p "$T/mnt/usb1" "$T/mnt/usb9" "$T/mnt/usb10"
conf usb10
run_once
check "exit status 0" "[ $RC = 0 ]"
assert_log "Selected USB: $T/mnt/usb10"
order=$(grep -o "Checking $T/mnt/usb[0-9]*\$" "$LOG" | sed "s#.*/usb##" | tr '\n' ' ')
check "checked in numerical order (got: $order)" "[ \"$order\" = '1 9 10 ' ]"

# --------------------------------------------------------------------------
echo "== Scenario: multiple USB partitions, only one with backup.conf"
fresh
mkdir -p "$T/mnt/usb0/EFI/BOOT" "$T/mnt/usb1" "$T/mnt/usb2"
echo "boot" > "$T/mnt/usb0/EFI/BOOT/BOOTX64.EFI"    # FAT32 boot partition
echo "cfg"  > "$T/mnt/usb0/EFI/BOOT/grub.cfg"
conf usb2
before0=$(tree "$T/mnt/usb0")
run_once
check "exit status 0" "[ $RC = 0 ]"
assert_log "Checking $T/mnt/usb0/backup.conf ... NOT FOUND"
assert_log "Selected USB: $T/mnt/usb2"
check "boot partition untouched" "[ \"\$(tree '$T/mnt/usb0')\" = \"\$before0\" ]"
check "usb1 untouched" "[ -z \"\$(tree '$T/mnt/usb1')\" ]"
check "backup written on usb2" "[ -f '$T/mnt/usb2/backup$T/src/a.txt' ]"

# --------------------------------------------------------------------------
echo "== Scenario: multiple partitions with backup.conf (first numerical match wins)"
fresh
mkdir -p "$T/mnt/usb0"
conf usb2; conf usb3; conf usb10
run_once
check "exit status 0" "[ $RC = 0 ]"
assert_log "Selected USB: $T/mnt/usb2"
assert_no_log "Checking $T/mnt/usb3"
assert_no_log "Checking $T/mnt/usb10"
check "usb3 only holds backup.conf" "[ \"\$(tree '$T/mnt/usb3')\" = './backup.conf' ]"
check "usb10 only holds backup.conf" "[ \"\$(tree '$T/mnt/usb10')\" = './backup.conf' ]"
fresh
conf usb9; conf usb10
run_once
assert_log "Selected USB: $T/mnt/usb9"
check "usb10 only holds backup.conf" "[ \"\$(tree '$T/mnt/usb10')\" = './backup.conf' ]"

# --------------------------------------------------------------------------
echo "== Scenario: no configuration found"
fresh
mkdir -p "$T/mnt/usb0/EFI" "$T/mnt/usb1" "$T/mnt/usb2/photos"
echo x > "$T/mnt/usb2/photos/1.jpg"
mkdir -p "$T/mnt/usb3/backup.conf"              # a folder named backup.conf does not count
before=$(tree "$T/mnt")
run_once
check "exit status is failure" "[ $RC != 0 ]"
assert_log "No backup.conf found on any USB device (4 mount point(s) checked)"
assert_log "Checking $T/mnt/usb3/backup.conf ... not a regular file"
assert_log "backup.conf not found on any USB."
assert_no_log "Selected USB"
assert_no_log "Starting backup service"
check "nothing written to any mount point" "[ \"\$(tree '$T/mnt')\" = \"\$before\" ]"
fresh
run_once
check "empty /mnt: exit status is failure" "[ $RC != 0 ]"
assert_log "No usb<N> mount points found below $T/mnt"

# --------------------------------------------------------------------------
echo "== Scenario: configuration found but storage is read-only"
if [ $can_mount = 1 ]; then
  fresh
  mkdir -p "$T/mnt/usb0"
  mount_usb usb2 && conf usb2 && mount -o remount,ro "$T/mnt/usb2"
  run_once
  check "exit status is failure" "[ $RC != 0 ]"
  assert_log "Selected USB: $T/mnt/usb2"
  assert_log "is not writable"
  assert_log "Backup could not start: USB $T/mnt/usb2 is not writable."
  assert_no_log "Starting backup service"
  check "no backup folder created" "[ ! -e '$T/mnt/usb2/backup' ]"
  check "usb0 untouched" "[ -z \"\$(tree '$T/mnt/usb0')\" ]"
  cleanup
else
  SKIP=$((SKIP+1)); echo "  skip - needs root to mount a read-only tmpfs"
fi

# --------------------------------------------------------------------------
echo "== Scenario: USB disconnected during operation"
fresh
mkdir -p "$T/mnt/usb0" "$T/mnt/usb1"
for i in $(seq 1 40); do echo "file $i" > "$T/src/f$i.txt"; done
if [ $can_mount = 1 ]; then mount_usb usb2; fi
conf usb2
TEST_MOUNT_DIR=$T/mnt TEST_COPY_DELAY_MS=100 "$BIN" >"$LOG" 2>/dev/null &
PID=$!
for i in $(seq 1 100); do log_has "Backing up, don't unplug" && break; sleep 0.1; done
assert_log "Selected USB: $T/mnt/usb2"
sleep 0.5
if [ $can_mount = 1 ]; then unmount_usb usb2; else rm -rf "${T:?}/mnt/usb2"; fi
for i in $(seq 1 100); do log_has "Backup pass aborted" && break; sleep 0.1; done
assert_log "USB $T/mnt/usb2 is no longer available; stopping writes"
assert_log "Backup pass aborted: USB $T/mnt/usb2 unavailable"
if [ $can_mount = 1 ]; then
  check "nothing written to the bare mount point" "[ -z \"\$(tree '$T/mnt/usb2')\" ]"
fi
# another USB with a backup.conf shows up meanwhile: it must not be used
conf usb5
sleep 4
check "service still running" "kill -0 $PID"
assert_no_log "Selected USB: $T/mnt/usb5"
check "usb5 only holds backup.conf" "[ \"\$(tree '$T/mnt/usb5')\" = './backup.conf' ]"
check "usb0/usb1 untouched" "[ -z \"\$(tree '$T/mnt/usb0')\$(tree '$T/mnt/usb1')\" ]"
# the same USB comes back at the same mount point: backups resume there
if [ $can_mount = 1 ]; then mount_usb usb2; fi
conf usb2
for i in $(seq 1 150); do log_has "Backup complete" && grep -q "available again" "$LOG" && break; sleep 0.1; done
assert_log "USB $T/mnt/usb2 is available again"
check "backup resumed on usb2" "grep -c 'Backup complete' '$LOG' | grep -q '^1\$' && [ -f '$T/mnt/usb2/backup$T/src/f40.txt' ]"
check "usb5 still untouched" "[ \"\$(tree '$T/mnt/usb5')\" = './backup.conf' ]"
cleanup

# --------------------------------------------------------------------------
echo "== Scenario: backup source overlaps the destination"
fresh
mkdir -p "$T/mnt/usb0" "$T/mnt/usb2" "$T/src/alias"
conf usb2 "/" "/*" "$T/mnt" "$T/mnt/*" "$T/mnt/usb2" "$T/mnt/usb2/backup" \
  "$T/mnt/usb2/../usb2/backup" "$T/src/../mnt" "$T/src//sub/" "$T/src/./sub"
alias_mounted=0
if [ $can_mount = 1 ] && mount --bind "$T/mnt/usb2" "$T/src/alias" 2>/dev/null; then
  MOUNTS+=("$T/src/alias"); alias_mounted=1
fi
run_once
check "exit status 0" "[ $RC = 0 ]"
for p in "/" "$T/mnt" "$T/mnt/usb2" "$T/mnt/usb2/backup" "$T/mnt/usb2/../usb2/backup" "$T/src/../mnt" "$T/src/./sub"; do
  assert_log "Refusing to back up $p: it contains the backup USB $T/mnt/usb2"
done
check "legit source still backed up" "[ -f '$T/mnt/usb2/backup$T/src/a.txt' ]"
check "double slash entry still backed up" "[ -f '$T/mnt/usb2/backup$T/src//sub/b.txt' ]"
check "no copy of /mnt inside the backup" "[ ! -e '$T/mnt/usb2/backup$T/mnt' ]"
check "backup does not contain itself" "[ -z \"\$(find '$T/mnt/usb2/backup' -path '*backup/*/backup' -o -name backup.conf)\" ]"
if [ $alias_mounted = 1 ]; then
  assert_log "Skipping $T/src//alias: it is the backup USB $T/mnt/usb2"
  check "alias of the USB not copied" "[ ! -e '$T/mnt/usb2/backup$T/src/alias' ]"
fi
cleanup

# --------------------------------------------------------------------------
echo
echo "passed: $PASS  failed: $FAIL  skipped: $SKIP"
[ $FAIL = 0 ]
