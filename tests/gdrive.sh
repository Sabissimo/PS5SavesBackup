#!/usr/bin/env bash
# Host test for the Google Drive destination, against tests/fake_google.py
# (run through "make test").
set -u

BIN=${1:?usage: gdrive.sh <host binary>}
BIN=$(readlink -f "$BIN")
T=${TEST_ROOT:-$(pwd)/tests/run}
T=$(readlink -f "$T" 2>/dev/null || echo "$T")
T=${T:?}
HERE=$(cd "$(dirname "$0")" && pwd)
PORT=19056
GPORT=19057
URL=http://127.0.0.1:$PORT
G=http://127.0.0.1:$GPORT
LOG=$T/log.txt
DATA=$T/data/homebrew/PS5SavesBackup
PASS=0; FAIL=0
PID=""; GPID=""

cleanup() {
  [ -n "$PID" ] && kill "$PID" 2>/dev/null && wait "$PID" 2>/dev/null; PID=""
  [ -n "$GPID" ] && kill "$GPID" 2>/dev/null && wait "$GPID" 2>/dev/null; GPID=""
}
trap cleanup EXIT
ok()  { PASS=$((PASS+1)); echo "  ok   - $1"; }
bad() { FAIL=$((FAIL+1)); echo "  FAIL - $1"; [ -n "${VERBOSE:-}" ] && sed 's/^/         | /' "$LOG"; }
check(){ if eval "$2"; then ok "$1"; else bad "$1"; fi; }
assert_log() { if grep -q -- "$1" "$LOG"; then ok "log: $1"; else bad "log: $1"; fi; }
assert_no_log() { if grep -q -- "$1" "$LOG"; then bad "no log: $1"; else ok "no log: $1"; fi; }
has() { echo "$1" | grep -q -- "$2"; }
tree() { (cd "$1" 2>/dev/null && find . -mindepth 1 | sort) ; }
wait_log() { for i in $(seq 1 ${2:-150}); do grep -q -- "$1" "$LOG" && return 0; sleep 0.1; done; return 1; }
count_log() { grep -c -- "$1" "$LOG"; }

start_google() {
  mkdir -p "$T/gstate"
  python3 -I "$HERE/fake_google.py" $GPORT "$T/gstate" &
  GPID=$!
  for i in $(seq 1 50); do curl -s "$G/_dump" >/dev/null 2>&1 && break; sleep 0.1; done
}
start() {
  TEST_GDRIVE_OAUTH_BASE=$G TEST_GDRIVE_API_BASE=$G "$BIN" >"$LOG" 2>/dev/null &
  PID=$!
  for i in $(seq 1 50); do curl -s "$URL/api/status" >/dev/null 2>&1 && break; sleep 0.1; done
}
stop() { [ -n "$PID" ] && kill "$PID" 2>/dev/null && wait "$PID" 2>/dev/null; PID=""; }
dump() { curl -s "$G/_dump"; }
# file_by_name <name> -> id
file_id() { dump | python3 -I -c "import json,sys; d=json.load(sys.stdin)['files']; print(next((i for i,f in d.items() if f['name']==sys.argv[1]), ''))" "$1"; }
file_content() { dump | python3 -I -c "import json,sys; d=json.load(sys.stdin)['files']; print(d.get(sys.argv[1],{}).get('content',''))" "$1"; }
file_parent() { dump | python3 -I -c "import json,sys; d=json.load(sys.stdin)['files']; print(d.get(sys.argv[1],{}).get('parent',''))" "$1"; }
settings() {   # settings <usb_id> <gdrive true/false> <folder> [paths...]
  mkdir -p "$DATA"
  { echo "backup_interval_seconds=1"; echo "usb_id=$1"; echo "gdrive_enabled=$2"; echo "gdrive_folder=$3"; shift 3
    echo "$T/src/"; for l in "$@"; do echo "$l"; done; } > "$DATA/backup.conf"
}

echo "Host binary: $BIN"
echo "Test root:   $T"

# --------------------------------------------------------------------------
echo "== Scenario: Google Drive without a USB, sign-in with the device code"
rm -rf "${T:?}"; mkdir -p "$T/mnt/usb0" "$T/src/sub"
echo alpha > "$T/src/a.txt"; echo beta > "$T/src/sub/b.txt"; printf 'x%.0s' $(seq 1 3000) > "$T/src/big.bin"
settings "" true PS5SavesBackup
start_google
start
wait_log "Backup destination: $DATA/backup (internal storage, for Google Drive)"
wait_log "Backup complete"
st=$(curl -s "$URL/api/status")
check "status: internal destination" "has \"\$st\" '\"dest_internal\":true,\"backup_root\":\"$DATA/backup\"'"
check "status: running" "has \"\$st\" '\"state\":\"running\"'"
check "local backup written" "[ -f '$DATA/backup$T/src/sub/b.txt' ]"
check "usb0 untouched" "[ -z \"\$(tree '$T/mnt/usb0')\" ]"
assert_no_log "Google Drive:"
g=$(curl -s "$URL/api/gdrive/status")
check "gdrive status: configured, not connected" "has \"\$g\" '\"configured\":true,\"connected\":false,\"pending\":false'"
page=$(curl -s "$URL/")
check "page has the Google Drive section" "has \"\$page\" 'Sign in to Google'"
g=$(curl -s -X POST "$URL/api/gdrive/connect")
check "connect: code shown" "has \"\$g\" '\"pending\":true,\"user_code\":\"ABCD-EFGH\",\"verification_url\":\"https://www.google.com/device\"'"
assert_log "Google Drive sign-in: enter code ABCD-EFGH at https://www.google.com/device"
g2=$(curl -s -X POST "$URL/api/gdrive/connect")
check "connect again: same pending code, no second flow" "has \"\$g2\" '\"user_code\":\"ABCD-EFGH\"'"
wait_log "Google Drive sign-in: signed in"
check "token file written" "grep -q '^refresh_token=ref-1$' '$DATA/gdrive.token'"
g=$(curl -s "$URL/api/gdrive/status")
check "gdrive status: connected" "has \"\$g\" '\"connected\":true,\"pending\":false'"
wait_log "file(s) uploaded"
assert_log "Google Drive: created folder PS5SavesBackup"
assert_log "Google Drive: uploading 3 file(s) to folder PS5SavesBackup"
assert_log "Google Drive: 3 file(s) uploaded"
root=$(file_id PS5SavesBackup)
check "root folder in My Drive" "[ -n '$root' ] && [ \"\$(file_parent $root)\" = root ]"
a=$(file_id a.txt); b=$(file_id b.txt); sub=$(file_id sub)
check "a.txt uploaded with content" "[ \"\$(file_content $a)\" = 'alpha' ]"
check "sub/b.txt in a sub folder chain" "[ -n '$sub' ] && [ \"\$(file_parent $b)\" = '$sub' ]"
check "folder chain mirrors the local path" "[ \"\$(dump | python3 -I -c \"import json,sys; d=json.load(sys.stdin)['files']; print(d['$sub']['parent'])\")\" != root ]"
check "index file written" "grep -qP \"^R\t$root\$\" '$DATA/gdrive.index' && grep -qP \"^F\t$a\t6\t\" '$DATA/gdrive.index' && grep -qP \"^D\t$sub\t\" '$DATA/gdrive.index'"
g=$(curl -s "$URL/api/gdrive/status")
check "gdrive status: last upload 3 files" "has \"\$g\" '\"last_files\":3,\"last_bytes\":3011,\"last_error\":\"\"'"

echo "== Scenario: nothing changed, changed file, file deleted in Drive"
sleep 2.5
assert_log "Google Drive up to date"
n=$(dump | python3 -I -c "import json,sys; print(json.load(sys.stdin)['counters']['api_calls'])")
echo alpha2 > "$T/src/a.txt"
for i in $(seq 1 100); do [ "$(file_content $a)" = "alpha2" ] && break; sleep 0.1; done
check "changed file re-uploaded to the same id" "[ \"\$(file_content $a)\" = 'alpha2' ]"
check "only one upload call for it" "[ \$(dump | python3 -I -c \"import json,sys; print(json.load(sys.stdin)['counters']['api_calls'])\") = \$((n+1)) ]"
curl -s -X POST "$G/_delete/$b" >/dev/null
echo beta2 > "$T/src/sub/b.txt"
for i in $(seq 1 100); do [ -n "$(file_id b.txt)" ] && [ "$(file_content $(file_id b.txt))" = "beta2" ] && break; sleep 0.1; done
b2=$(file_id b.txt)
check "file deleted in Drive is created again" "[ -n '$b2' ] && [ '$b2' != '$b' ] && [ \"\$(file_content $b2)\" = 'beta2' ]"
check "new id stored in the index" "grep -qP \"^F\t$b2\t\" '$DATA/gdrive.index'"

echo "== Scenario: server errors are retried, token refresh"
curl -s -X POST "$G/_fail/2" >/dev/null
echo gamma > "$T/src/c.txt"
for i in $(seq 1 150); do [ -n "$(file_id c.txt)" ] && [ "$(file_content $(file_id c.txt))" = "gamma" ] && break; sleep 0.1; done
check "upload succeeded after retries" "[ \"\$(file_content \$(file_id c.txt))\" = 'gamma' ]"
assert_no_log "Google Drive upload incomplete"
stop
python3 -I - "$DATA/gdrive.token" <<'EOF'
import sys,re
p=sys.argv[1]; s=open(p).read(); s=re.sub(r"expires_at=\d+", "expires_at=1", s); open(p,"w").write(s)
EOF
echo delta > "$T/src/d.txt"
start
wait_log "file(s) uploaded"
check "expired access token refreshed" "[ \$(dump | python3 -I -c \"import json,sys; print(json.load(sys.stdin)['counters']['refreshes'])\") -ge 1 ] && [ \"\$(file_content \$(file_id d.txt))\" = 'delta' ]"
stop

echo "== Scenario: custom folder name, USB plus Google Drive mirror"
mkdir -p "$T/mnt/usb2"; echo 0123456789abcdef0123456789abcdef > "$T/mnt/usb2/PS5SavesBackup.id"
settings 0123456789abcdef0123456789abcdef true "My PS5 saves"
start
wait_log "Selected USB: $T/mnt/usb2"
wait_log "created folder My PS5 saves"
wait_log "file(s) uploaded"
assert_log "Google Drive folder changed; uploading the backup again"
root2=$(file_id "My PS5 saves")
check "second root folder created" "[ -n '$root2' ] && [ '$root2' != '$root' ]"
check "USB backup mirrored" "[ -f '$T/mnt/usb2/backup$T/src/a.txt' ]"
assert_log "Google Drive: uploading 5 file(s) to folder My PS5 saves"
check "no marker or probe uploaded" "[ -z \"\$(file_id PS5SavesBackup.id)\" ] && [ -z \"\$(file_id .ps5save-index)\" ]"
stop

echo "== Scenario: download the backup from Google Drive on a console without one"
rm -rf "${DATA:?}/backup" "$DATA/gdrive.index" "$T/mnt/usb2"
rm -rf "${T:?}/src"; mkdir -p "$T/src"; echo fresh > "$T/src/fresh.txt"
settings "" true "My PS5 saves"
start
wait_log "Backup complete"
r=$(curl -s -X POST "$URL/api/gdrive/download")
check "download queued (got: $r)" "has '$r' 'Download queued'"
for i in $(seq 1 200); do st=$(curl -s "$URL/api/restore/status"); has "$st" '"busy":false' && has "$st" 'Download from Google Drive' && break; sleep 0.1; done
check "download reported ok" "has '$st' '\"last_ok\":true' && has '$st' 'downloaded'"
assert_log "Google Drive: downloading folder My PS5 saves to $DATA/backup"
A=$DATA/backup$T/src
check "files downloaded with content" "[ \"\$(cat '$A/a.txt')\" = alpha2 ] && [ \"\$(cat '$A/sub/b.txt')\" = beta2 ] && [ -f '$A/big.bin' ]"
check "downloaded files have the index entries" "grep -qP '^F\t[^\t]+\t6\t' '$DATA/gdrive.index' && grep -q 'src/sub/b.txt' '$DATA/gdrive.index'"
uploads() { dump | python3 -I -c "import json,sys; print(json.load(sys.stdin)['counters'].get('uploads', 0))"; }
sleep 2.5
n=$(uploads)
sleep 2.5
check "downloaded files are not uploaded again" "[ \$(uploads) = $n ]"
r=$(curl -s -X POST "$URL/api/gdrive/download")
for i in $(seq 1 200); do st=$(curl -s "$URL/api/restore/status"); has "$st" '"busy":false' && has "$st" '0 file(s) downloaded' && break; sleep 0.1; done
check "second download finds everything present" "has '$st' '0 file(s) downloaded'"
check "local extra file kept" "[ -f '$A/fresh.txt' ]"
stop
mkdir -p "$T/mnt/usb2"; echo 0123456789abcdef0123456789abcdef > "$T/mnt/usb2/PS5SavesBackup.id"
settings 0123456789abcdef0123456789abcdef true "My PS5 saves"

echo "== Scenario: save history uploaded to Drive and pruned there"
HOME_ROOT=$T/home; UID1=1a2b3c4d; TITLE=PPSA12345
mkdir -p "$HOME_ROOT/$UID1/savedata_prospero/$TITLE" "$T/fixture"
echo "level 1" > "$T/fixture/progress.dat"; tar -cf "$HOME_ROOT/$UID1/savedata_prospero/$TITLE/slot1" -C "$T/fixture" .
printf 'backup_interval_seconds=1\nusb_id=0123456789abcdef0123456789abcdef\ngdrive_enabled=true\ngdrive_folder=My PS5 saves\nbackup_ps5_saves=true\nhistory_days_to_keep=3\n%s/src/\n' "$T" > "$DATA/backup.conf"
start
wait_log "Backup complete"
wait_log "file(s) uploaded"
check "save uploaded to Drive" "[ -n \"\$(file_id progress.dat)\" ]"
sleep 1.1; echo "level 2" > "$T/fixture/progress.dat"; tar -cf "$HOME_ROOT/$UID1/savedata_prospero/$TITLE/slot1" -C "$T/fixture" .
wait_log "Save version kept"
for i in $(seq 1 100); do [ -n "$(file_id history)" ] && sleep 1 && break; sleep 0.1; done
hist=$(file_id history)
check "history folder in Drive" "[ -n '$hist' ]"
nver=$(dump | python3 -I -c "import json,sys; d=json.load(sys.stdin)['files']; print(sum(1 for f in d.values() if f['parent']==sys.argv[1]))" "$(file_id slot1)")
check "one version folder uploaded" "[ \$(dump | python3 -I -c \"import json,sys; d=json.load(sys.stdin)['files']; print(sum(1 for f in d.values() if f['name'].startswith('20') and f['mime'].endswith('folder')))\") -ge 1 ]"
check "undo copies never uploaded" "[ -z \"\$(file_id restore_undo)\" ]"
stop
echo $(( $(date +%s) + 10 * 86400 )) > "$T/now.txt"
TEST_NOW_FILE=$T/now.txt TEST_GDRIVE_OAUTH_BASE=$G TEST_GDRIVE_API_BASE=$G "$BIN" >"$LOG" 2>/dev/null &
PID=$!
for i in $(seq 1 50); do curl -s "$URL/api/status" >/dev/null 2>&1 && break; sleep 0.1; done
wait_log "Save version removed"
wait_log "Google Drive: removed old save version"
sleep 1
check "version folder deleted in Drive" "[ \$(dump | python3 -I -c \"import json,sys; d=json.load(sys.stdin)['files']; print(sum(1 for f in d.values() if f['name'].startswith('20') and f['mime'].endswith('folder')))\") = 0 ] && [ \$(dump | python3 -I -c \"import json,sys; print(json.load(sys.stdin)['counters'].get('deletes',0))\") -ge 1 ]"
stop
rm -f "$T/now.txt"
printf 'backup_interval_seconds=1\nusb_id=0123456789abcdef0123456789abcdef\ngdrive_enabled=true\ngdrive_folder=My PS5 saves\n%s/src/\n' "$T" > "$DATA/backup.conf"

echo "== Scenario: revoked token"
touch "$T/gstate/revoked"
python3 -I - "$DATA/gdrive.token" <<'EOF'
import sys,re
p=sys.argv[1]; s=open(p).read(); s=re.sub(r"expires_at=\d+", "expires_at=1", s); open(p,"w").write(s)
EOF
echo epsilon > "$T/src/e.txt"
start
wait_log "Google Drive access was revoked: sign in again"
assert_log "Google Drive upload failed: Google Drive access was revoked"
check "token forgotten" "grep -q '^refresh_token=$' '$DATA/gdrive.token'"
g=$(curl -s "$URL/api/gdrive/status")
check "gdrive status: not connected with error" "has \"\$g\" '\"connected\":false' && has \"\$g\" 'sign in again'"
check "local backup still fine" "[ -f '$T/mnt/usb2/backup$T/src/e.txt' ]"
rm -f "$T/gstate/revoked"
stop

echo "== Scenario: sign-in declined, sign out"
touch "$T/gstate/deny"
start
curl -s -X POST "$URL/api/gdrive/connect" >/dev/null
wait_log "Sign-in was declined"
g=$(curl -s "$URL/api/gdrive/status")
check "declined reported" "has \"\$g\" '\"pending\":false' && has \"\$g\" 'declined'"
rm -f "$T/gstate/deny"
curl -s -X POST "$URL/api/gdrive/connect" >/dev/null
wait_log "Google Drive sign-in: signed in" 300
r=$(curl -s -X POST "$URL/api/gdrive/disconnect")
check "sign out accepted" "has \"\$r\" 'Signed out'"
check "token and index removed" "[ ! -e '$DATA/gdrive.token' ] && [ ! -e '$DATA/gdrive.index' ]"
g=$(curl -s "$URL/api/gdrive/status")
check "gdrive status: not connected" "has \"\$g\" '\"connected\":false'"
stop

echo "== Scenario: Google unreachable"
kill "$GPID" 2>/dev/null; wait "$GPID" 2>/dev/null; GPID=""
settings 0123456789abcdef0123456789abcdef true PS5SavesBackup
printf 'refresh_token=ref-1\naccess_token=\nexpires_at=1\n' > "$DATA/gdrive.token"
echo zeta > "$T/src/f.txt"
start
wait_log "Google Drive upload failed: Google not reachable"
check "local backup still succeeds" "[ -f '$T/mnt/usb2/backup$T/src/f.txt' ]"
stop

echo
echo "passed: $PASS  failed: $FAIL"
[ $FAIL = 0 ]
