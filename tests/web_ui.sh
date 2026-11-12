#!/usr/bin/env bash
# Host test for the settings page API (run through "make test").
set -u

BIN=${1:?usage: web_ui.sh <host binary>}
BIN=$(readlink -f "$BIN")
T=${TEST_ROOT:-$(pwd)/tests/run}
T=$(readlink -f "$T" 2>/dev/null || echo "$T")
T=${T:?}
PORT=19056
URL=http://127.0.0.1:$PORT
LOG=$T/log.txt
DATA=$T/data/homebrew/PS5SavesBackup
PASS=0; FAIL=0
PID=""

cleanup() { [ -n "$PID" ] && kill "$PID" 2>/dev/null && wait "$PID" 2>/dev/null; PID=""; }
trap cleanup EXIT
ok()  { PASS=$((PASS+1)); echo "  ok   - $1"; }
bad() { FAIL=$((FAIL+1)); echo "  FAIL - $1"; [ -n "${VERBOSE:-}" ] && sed 's/^/         | /' "$LOG"; }
check(){ if eval "$2"; then ok "$1"; else bad "$1"; fi; }
assert_log() { if grep -q -- "$1" "$LOG"; then ok "log: $1"; else bad "log: $1"; fi; }
has() { echo "$1" | grep -q -- "$2"; }   # has <text> <substring>
tree() { (cd "$1" 2>/dev/null && find . -mindepth 1 | sort) ; }

start() {
  "$BIN" >"$LOG" 2>/dev/null &
  PID=$!
  for i in $(seq 1 50); do curl -s "$URL/api/status" >/dev/null 2>&1 && break; sleep 0.1; done
}
wait_log() { for i in $(seq 1 100); do grep -q -- "$1" "$LOG" && return 0; sleep 0.1; done; return 1; }

echo "Host binary: $BIN"
echo "Test root:   $T"

# --------------------------------------------------------------------------
echo "== Scenario: page and status in legacy mode"
rm -rf "${T:?}"; mkdir -p "$T/mnt/usb0" "$T/mnt/usb2" "$T/src/sub"
echo alpha > "$T/src/a.txt"; echo beta > "$T/src/sub/b.txt"
printf 'backup_interval_seconds=1\n%s/src/\n' "$T" > "$T/mnt/usb2/backup.conf"
start
wait_log "Backup complete"
page=$(curl -s "$URL/")
check "page served" "has \"\$page\" '<title>PS5SavesBackup</title>'"
st=$(curl -s "$URL/api/status")
check "status: app name" "has \"\$st\" '\"app\":\"PS5SavesBackup\"'"
check "status: running" "has \"\$st\" '\"state\":\"running\"'"
check "status: legacy conf on usb2" "has \"\$st\" '\"conf_path\":\"$T/mnt/usb2/backup.conf\",\"legacy\":true'"
check "status: no settings file yet" "has \"\$st\" '\"settings_file\":false'"
usb=$(curl -s "$URL/api/usb")
check "usb list: usb2 selected with legacy conf" "has \"\$usb\" '\"root\":\"$T/mnt/usb2\",\"mounted\":true,\"selected\":true,\"marker\":\"\",\"legacy_conf\":true'"
check "usb list: usb0 available" "has \"\$usb\" '\"root\":\"$T/mnt/usb0\",\"mounted\":true,\"selected\":false'"
cfg=$(curl -s "$URL/api/config")
check "config read from legacy file" "has \"\$cfg\" '\"found\":true,\"interval\":1,' && has \"\$cfg\" '\"paths\":\[\"$T/src/\"\]'"
check "404 for unknown path" "has \"\$(curl -s $URL/nope)\" 'no such page'"
check "405-ish: POST to status rejected" "has \"\$(curl -s -X POST $URL/api/status)\" 'no such page'"

echo "== Scenario: saving settings migrates to the internal file and marks the USB"
r=$(curl -s -X POST "$URL/api/config" --data-urlencode "interval=2" --data-urlencode "max_mb=1024" \
    --data-urlencode "daily=1" --data-urlencode "history_days=3" --data "ps5=on" --data-urlencode "paths=$T/src/
# comment
$T/src/a.txt")
check "save accepted" "has \"\$r\" 'Settings saved'"
check "settings file written" "[ -f '$DATA/backup.conf' ]"
check "settings file content" "grep -q '^backup_interval_seconds=2$' '$DATA/backup.conf' && grep -q '^backup_ps5_saves=true$' '$DATA/backup.conf' && grep -q '^daily_snapshots_to_keep=1$' '$DATA/backup.conf' && grep -q \"^$T/src/a.txt\$\" '$DATA/backup.conf'"
id=$(cat "$T/mnt/usb2/PS5SavesBackup.id" 2>/dev/null)
check "marker written on usb2" "[ \${#id} = 32 ]"
check "usb_id stored in settings" "grep -q \"^usb_id=\$id\$\" '$DATA/backup.conf'"
wait_log "Backup destination changed in settings"
sleep 2
st=$(curl -s "$URL/api/status")
check "status: internal settings in use" "has \"\$st\" '\"conf_path\":\"$DATA/backup.conf\",\"legacy\":false'"
check "status: same usb2 selected by id" "has \"\$st\" '\"usb_root\":\"$T/mnt/usb2\",\"usb_id\":\"$id\"'"
check "daily snapshot made on usb2" "[ -d '$T/mnt/usb2/backup_daily' ]"
check "usb0 untouched" "[ -z \"\$(tree '$T/mnt/usb0')\" ]"

echo "== Scenario: validation"
check "interval out of range" "has \"\$(curl -s -X POST $URL/api/config --data 'interval=99999&max_mb=1024&daily=1&history_days=3&paths=/x')\" 'interval must be between'"
check "daily out of range" "has \"\$(curl -s -X POST $URL/api/config --data 'interval=5&max_mb=1024&daily=99&history_days=3&paths=/x')\" 'daily must be between'"
check "relative path refused" "has \"\$(curl -s -X POST $URL/api/config --data 'interval=5&max_mb=1024&daily=1&history_days=3&paths=relative')\" 'paths must be absolute'"
check "missing field" "has \"\$(curl -s -X POST $URL/api/config --data 'interval=5')\" 'max_mb is missing'"
check "settings unchanged after errors" "grep -q '^backup_interval_seconds=2$' '$DATA/backup.conf'"
check "unknown USB refused" "has \"\$(curl -s -X POST $URL/api/usb/select --data-urlencode root=$T/mnt/usb7)\" 'not a mounted USB partition'"
check "arbitrary path refused" "has \"\$(curl -s -X POST $URL/api/usb/select --data-urlencode root=$T/src)\" 'not a mounted USB partition'"
check "no marker written anywhere else" "[ ! -e '$T/mnt/usb0/PS5SavesBackup.id' ] && [ ! -e '$T/src/PS5SavesBackup.id' ]"

echo "== Scenario: switching the backup USB from the page"
r=$(curl -s -X POST "$URL/api/usb/select" --data-urlencode "root=$T/mnt/usb0")
check "selection accepted" "has \"\$r\" 'Backup USB selected'"
id0=$(cat "$T/mnt/usb0/PS5SavesBackup.id" 2>/dev/null)
check "marker written on usb0" "[ \${#id0} = 32 ] && [ \"\$id0\" != \"$id\" ]"
check "settings now point to usb0" "grep -q \"^usb_id=\$id0\$\" '$DATA/backup.conf'"
check "other settings kept" "grep -q '^backup_interval_seconds=2$' '$DATA/backup.conf'"
for i in $(seq 1 100); do grep -q "Selected USB: $T/mnt/usb0" "$LOG" && grep -c "Backup complete" "$LOG" | grep -q '^2$' && break; sleep 0.1; done
assert_log "Backup USB set to $T/mnt/usb0 in settings"
assert_log "Selected USB: $T/mnt/usb0"
check "backup written on usb0" "[ -f '$T/mnt/usb0/backup$T/src/a.txt' ]"
check "usb2 keeps its old backup and marker" "[ -f '$T/mnt/usb2/backup$T/src/a.txt' ] && [ \"\$(cat '$T/mnt/usb2/PS5SavesBackup.id')\" = \"$id\" ]"
st=$(curl -s "$URL/api/status")
check "status: usb0 selected" "has \"\$st\" '\"usb_root\":\"$T/mnt/usb0\",\"usb_id\":\"$id0\"'"

echo "== Scenario: back up now and log"
echo gamma > "$T/src/c.txt"
n=$(grep -c "Backup complete" "$LOG")
r=$(curl -s -X POST "$URL/api/backup")
check "backup requested" "has \"\$r\" 'Backup pass requested'"
for i in $(seq 1 50); do [ "$(grep -c 'Backup complete' "$LOG")" -gt "$n" ] && break; sleep 0.1; done
check "new file backed up on request" "[ -f '$T/mnt/usb0/backup$T/src/c.txt' ]"
lg=$(curl -s "$URL/api/log")
check "log endpoint has recent lines" "has \"\$lg\" 'Backup complete' && has \"\$lg\" 'Selected USB'"
cleanup

echo "== Scenario: fresh start with the settings file selects the marked USB"
start
wait_log "Selected USB: $T/mnt/usb0"
assert_log "Checking $T/mnt/usb0/PS5SavesBackup.id ... FOUND"
assert_log "Starting backup service..."
cleanup

echo "== Scenario: unconfigured console"
rm -rf "${T:?}"; mkdir -p "$T/mnt/usb0" "$T/mnt/usb1"
start
wait_log "No backup USB configured"
st=$(curl -s "$URL/api/status")
check "status: waiting with reason" "has \"\$st\" '\"state\":\"waiting\",\"reason\":\"No backup USB configured: open the settings page.\"'"
check "status: no settings file" "has \"\$st\" '\"settings_file\":false'"
assert_log "Would open http://"
cfg=$(curl -s "$URL/api/config")
check "config shows defaults" "has \"\$cfg\" '\"found\":false,\"interval\":60,\"ps4\":false,\"ps5\":false,\"max_mb\":4096,\"daily\":0'"
r=$(curl -s -X POST "$URL/api/usb/select" --data-urlencode "root=$T/mnt/usb1")
check "usb1 selected" "has \"\$r\" 'Backup USB selected'"
wait_log "Selected USB: $T/mnt/usb1"
check "settings file created with defaults" "grep -q '^backup_interval_seconds=60$' '$DATA/backup.conf' && grep -q '^usb_id=' '$DATA/backup.conf'"
wait_log "Backup up to date"
check "usb0 untouched" "[ -z \"\$(tree '$T/mnt/usb0')\" ]"
cleanup

echo
echo "passed: $PASS  failed: $FAIL"
[ $FAIL = 0 ]
