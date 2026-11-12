#!/usr/bin/env bash
# Host test for stage A: restoring plain files and saves from the backup
# (run through "make test"). Save images are tar archives, see
# tests/host/stubs.c.
set -u

BIN=${1:?usage: restore.sh <host binary>}
BIN=$(readlink -f "$BIN")
T=${TEST_ROOT:-$(pwd)/tests/run}
T=$(readlink -f "$T" 2>/dev/null || echo "$T")
T=${T:?}
PORT=19056
URL=http://127.0.0.1:$PORT
LOG=$T/log.txt
DATA=$T/data/homebrew/PS5SavesBackup
HOME_ROOT=$T/home
UID1=1a2b3c4d
TITLE=PPSA12345
PASS=0; FAIL=0
PID=""

cleanup() { [ -n "$PID" ] && kill "$PID" 2>/dev/null && wait "$PID" 2>/dev/null; PID=""; }
trap cleanup EXIT
ok()  { PASS=$((PASS+1)); echo "  ok   - $1"; }
bad() { FAIL=$((FAIL+1)); echo "  FAIL - $1"; [ -n "${VERBOSE:-}" ] && sed 's/^/         | /' "$LOG"; }
check(){ if eval "$2"; then ok "$1"; else bad "$1"; fi; }
assert_log() { if grep -q -- "$1" "$LOG"; then ok "log: $1"; else bad "log: $1"; fi; }
has() { echo "$1" | grep -q -- "$2"; }
wait_log() { for i in $(seq 1 ${2:-150}); do grep -q -- "$1" "$LOG" && return 0; sleep 0.1; done; return 1; }
start() {
  "$BIN" >"$LOG" 2>/dev/null &
  PID=$!
  for i in $(seq 1 50); do curl -s "$URL/api/status" >/dev/null 2>&1 && break; sleep 0.1; done
}
stop() { [ -n "$PID" ] && kill "$PID" 2>/dev/null && wait "$PID" 2>/dev/null; PID=""; }
# make_image <dir with content> <image path>
make_image() { tar -cf "$2" -C "$1" . ; }
# image_file <image> <name> -> content of one file inside the image
image_file() { tar -xOf "$1" "./$2" 2>/dev/null; }
restore_wait() {   # restore_wait -> prints the final status JSON
  for i in $(seq 1 200); do
    st=$(curl -s "$URL/api/restore/status")
    has "$st" '"busy":false' && has "$st" '"finished_age":[0-9]' && break
    sleep 0.1
  done
  echo "$st"
}
settings() {   # settings <ps4 flag> <ps5 flag> [paths...]
  mkdir -p "$DATA"
  { echo "backup_interval_seconds=1"; echo "usb_id=0123456789abcdef0123456789abcdef"
    echo "backup_ps4_saves=$1"; echo "backup_ps5_saves=$2"; shift 2
    for l in "$@"; do echo "$l"; done; } > "$DATA/backup.conf"
}

echo "Host binary: $BIN"
echo "Test root:   $T"

# --------------------------------------------------------------------------
echo "== Scenario: plain file and folder restore"
rm -rf "${T:?}"; mkdir -p "$T/mnt/usb2" "$T/src/sub" "$T/data/homebrew/PPSA99303/saves" "$T/home"
echo 0123456789abcdef0123456789abcdef > "$T/mnt/usb2/PS5SavesBackup.id"
echo alpha > "$T/src/a.txt"; echo beta > "$T/src/sub/b.txt"; echo emu > "$T/data/homebrew/PPSA99303/saves/game.sav"
settings false false "$T/src/" "$T/data/homebrew/PPSA99303/saves/"
start
wait_log "Backup complete"
r=$(curl -s "$URL/api/restore/list")
check "list: both paths in backup" "has \"\$r\" '\"path\":\"$T/src\",\"in_backup\":true,\"folder\":true,\"files\":2' && has \"\$r\" '\"path\":\"$T/data/homebrew/PPSA99303/saves\",\"in_backup\":true'"
check "list: no saves" "has \"\$r\" '\"saves\":\[\]'"
echo CHANGED > "$T/src/a.txt"; rm "$T/src/sub/b.txt"; echo extra > "$T/src/extra.txt"
sleep 2.5                                      # the change is backed up first; restore then brings alpha back
r=$(curl -s -X POST "$URL/api/restore" --data-urlencode "kind=path" --data-urlencode "path=$T/src/sub/b.txt")
check "single file restore queued" "has \"\$r\" 'Restore queued'"
st=$(restore_wait)
check "single file restored" "[ \"\$(cat '$T/src/sub/b.txt')\" = beta ] && has \"\$st\" '\"last_ok\":true' && has \"\$st\" '1 file(s) restored'"
check "no .part left" "[ ! -e '$T/src/sub/b.txt.part' ]"
# folder restore merges: a.txt back to the backup copy (CHANGED was backed up meanwhile), extra kept
cp "$T/mnt/usb2/backup$T/src/a.txt" "$T/a.backup"
r=$(curl -s -X POST "$URL/api/restore" --data-urlencode "kind=path" --data-urlencode "path=$T/src")
st=$(restore_wait)
check "folder restore ok" "has \"\$st\" '\"last_ok\":true' && has \"\$st\" 'restored to $T/src'"
check "folder restore keeps extra local file" "[ -f '$T/src/extra.txt' ]"
check "folder restore copies backup content" "cmp -s '$T/a.backup' '$T/src/a.txt'"
r=$(curl -s -X POST "$URL/api/restore" --data-urlencode "kind=path" --data-urlencode "path=$T/mnt/usb2/backup")
st=$(restore_wait)
check "overlapping path refused" "has \"\$st\" '\"last_ok\":false' && has \"\$st\" 'contains the backup destination'"
r=$(curl -s -X POST "$URL/api/restore" --data-urlencode "kind=path" --data-urlencode "path=$T/nowhere")
st=$(restore_wait)
check "unknown path refused" "has \"\$st\" 'is not in the backup'"
check "bad request without path" "has \"\$(curl -s -X POST $URL/api/restore --data kind=path)\" 'path is missing'"
stop

echo "== Scenario: application running: restore of its files refused"
printf 'PPSA99303\n' > "$T/active.txt"
TEST_ACTIVE_TITLES=$T/active.txt "$BIN" >"$LOG" 2>/dev/null &
PID=$!
for i in $(seq 1 50); do curl -s "$URL/api/status" >/dev/null 2>&1 && break; sleep 0.1; done
wait_log "Backup"
sleep 1.5
r=$(curl -s "$URL/api/restore/list")
check "list marks the emulator path as running" "has \"\$r\" '\"path\":\"$T/data/homebrew/PPSA99303/saves\",\"in_backup\":true,\"folder\":true,\"files\":1,\"bytes\":4,\"backed_up\":[0-9]*,\"running\":true'"
r=$(curl -s -X POST "$URL/api/restore" --data-urlencode "kind=path" --data-urlencode "path=$T/data/homebrew/PPSA99303/saves")
st=$(restore_wait)
check "restore refused while running" "has \"\$st\" '\"last_ok\":false' && has \"\$st\" 'is running'"
stop
rm -f "$T/active.txt"

# --------------------------------------------------------------------------
echo "== Scenario: PS5 save backup and restore"
rm -rf "${T:?}"; mkdir -p "$T/mnt/usb2" "$HOME_ROOT/$UID1/savedata_prospero/$TITLE" "$T/fixture/sce_sys" "$T/src"
echo 0123456789abcdef0123456789abcdef > "$T/mnt/usb2/PS5SavesBackup.id"
echo "level 5" > "$T/fixture/progress.dat"; printf 'icon' > "$T/fixture/sce_sys/icon0.png"; echo x > "$T/src/a.txt"
make_image "$T/fixture" "$HOME_ROOT/$UID1/savedata_prospero/$TITLE/slot1"
settings false true "$T/src/"
start
wait_log "Backup complete"
check "save files backed up" "[ \"\$(cat '$T/mnt/usb2/backup/saves/ps5/$UID1/$TITLE/slot1/progress.dat')\" = 'level 5' ] && [ -f '$T/mnt/usb2/backup/saves/ps5/$UID1/$TITLE/slot1/sce_sys/icon0.png' ]"
check "image stamp written" "[ -f '$T/mnt/usb2/backup/saves/ps5/$UID1/$TITLE/slot1/.image_stamp' ]"
r=$(curl -s "$URL/api/restore/list")
check "list: save with slot, not running" "has \"\$r\" '\"ps\":\"ps5\",\"uid\":\"$UID1\",\"title\":\"$TITLE\",\"save\":\"slot1\",\"files\":2,\"bytes\":12,\"backed_up\":[0-9]*,\"slot_exists\":true,\"running\":false'"
# the game "plays on": new progress, a new file; then the user wants the backup back
sleep 1.1
echo "level 9" > "$T/fixture/progress.dat"; echo junk > "$T/fixture/newfile.bin"
make_image "$T/fixture" "$HOME_ROOT/$UID1/savedata_prospero/$TITLE/slot1"
stop                                           # keep the backup at level 5: stop before the next pass
# the new process must not back up level 9 before the restore: saves off
printf 'backup_interval_seconds=1\nusb_id=0123456789abcdef0123456789abcdef\nbackup_ps5_saves=false\n%s/src/\n' "$T" > "$DATA/backup.conf"
start
wait_log "Backup"
sleep 1.5
before_size=$(stat -c %s "$HOME_ROOT/$UID1/savedata_prospero/$TITLE/slot1")
r=$(curl -s -X POST "$URL/api/restore" --data-urlencode "kind=save" --data-urlencode "ps=ps5" --data-urlencode "uid=$UID1" --data-urlencode "title=$TITLE" --data-urlencode "save=slot1")
check "save restore queued" "has \"\$r\" 'Restore queued'"
st=$(restore_wait)
check "save restore reported ok" "has \"\$st\" '\"last_ok\":true' && has \"\$st\" 'PS5 save $TITLE/slot1 restored'"
check "image content is the backup again" "[ \"\$(image_file '$HOME_ROOT/$UID1/savedata_prospero/$TITLE/slot1' progress.dat)\" = 'level 5' ]"
check "file added after the backup is gone" "[ -z \"\$(image_file '$HOME_ROOT/$UID1/savedata_prospero/$TITLE/slot1' newfile.bin)\" ]"
check "sce_sys restored" "[ \"\$(image_file '$HOME_ROOT/$UID1/savedata_prospero/$TITLE/slot1' sce_sys/icon0.png)\" = 'icon' ]"
undo=$(ls "$T/mnt/usb2/backup/restore_undo/ps5/$UID1/$TITLE/" 2>/dev/null | head -1)
check "undo copy kept" "[ -n \"\$undo\" ] && [ \"\$(image_file '$T/mnt/usb2/backup/restore_undo/ps5/$UID1/$TITLE/'\$undo progress.dat)\" = 'level 9' ]"
check "no temporary files left" "[ ! -e '$HOME_ROOT/$UID1/savedata_prospero/$TITLE/slot1.ps5sbnew' ] && [ ! -e '$T/data/_backup_tmp_image' ]"
check "image stamp removed" "[ ! -e '$T/mnt/usb2/backup/saves/ps5/$UID1/$TITLE/slot1/.image_stamp' ]"
assert_log "Restore: saving an undo copy of the current save"
assert_log "Restore: replacing the save image"
check "mount point cleaned" "[ ! -e '$T/pfs/savebackup_tmp' ] || [ -z \"\$(ls -A '$T/pfs/savebackup_tmp')\" ]"
# refusals
r=$(curl -s -X POST "$URL/api/restore" --data-urlencode "kind=save" --data-urlencode "ps=ps5" --data-urlencode "uid=$UID1" --data-urlencode "title=$TITLE" --data-urlencode "save=slot9")
st=$(restore_wait)
check "unknown save refused" "has \"\$st\" 'not in the backup'"
mkdir -p "$T/mnt/usb2/backup/saves/ps5/$UID1/$TITLE/slot2"; echo z > "$T/mnt/usb2/backup/saves/ps5/$UID1/$TITLE/slot2/f"
r=$(curl -s -X POST "$URL/api/restore" --data-urlencode "kind=save" --data-urlencode "ps=ps5" --data-urlencode "uid=$UID1" --data-urlencode "title=$TITLE" --data-urlencode "save=slot2")
st=$(restore_wait)
check "missing slot refused" "has \"\$st\" 'save slot does not exist'"
check "invalid reference rejected" "has \"\$(curl -s -X POST $URL/api/restore --data 'kind=save&ps=ps5&uid=zz&title=$TITLE&save=slot1')\" 'invalid save reference'"
stop

echo "== Scenario: PS5 save restore refused while the game runs"
printf "$TITLE\n" > "$T/active.txt"
TEST_ACTIVE_TITLES=$T/active.txt "$BIN" >"$LOG" 2>/dev/null &
PID=$!
for i in $(seq 1 50); do curl -s "$URL/api/status" >/dev/null 2>&1 && break; sleep 0.1; done
wait_log "Backup"
sleep 1.5
r=$(curl -s "$URL/api/restore/list")
check "list marks the game as running" "has \"\$r\" '\"save\":\"slot1\",\"files\":2,\"bytes\":12,\"backed_up\":[0-9]*,\"slot_exists\":true,\"running\":true'"
echo "level 5" > "$T/fixture/progress.dat"; rm -f "$T/fixture/newfile.bin"
r=$(curl -s -X POST "$URL/api/restore" --data-urlencode "kind=save" --data-urlencode "ps=ps5" --data-urlencode "uid=$UID1" --data-urlencode "title=$TITLE" --data-urlencode "save=slot1")
st=$(restore_wait)
check "restore refused while the game runs" "has \"\$st\" '\"last_ok\":false' && has \"\$st\" 'running or suspended'"
check "no undo copy made" "[ \$(ls '$T/mnt/usb2/backup/restore_undo/ps5/$UID1/$TITLE/' | wc -l) = 1 ]"
stop
rm -f "$T/active.txt"

# --------------------------------------------------------------------------
echo "== Scenario: PS4 save backup and restore"
rm -rf "${T:?}"; mkdir -p "$T/mnt/usb2" "$HOME_ROOT/$UID1/savedata/CUSA54321" "$T/fixture" "$T/src"
echo 0123456789abcdef0123456789abcdef > "$T/mnt/usb2/PS5SavesBackup.id"
echo "ps4 progress 1" > "$T/fixture/save.bin"; echo x > "$T/src/a.txt"
make_image "$T/fixture" "$HOME_ROOT/$UID1/savedata/CUSA54321/sdimg_SAVEDATA01"
head -c 96 /dev/zero > "$HOME_ROOT/$UID1/savedata/CUSA54321/SAVEDATA01.bin"
settings true false "$T/src/"
start
wait_log "Backup complete"
check "ps4 save backed up" "[ \"\$(cat '$T/mnt/usb2/backup/saves/ps4/$UID1/CUSA54321/SAVEDATA01/save.bin')\" = 'ps4 progress 1' ]"
sleep 1.1
echo "ps4 progress 2" > "$T/fixture/save.bin"
make_image "$T/fixture" "$HOME_ROOT/$UID1/savedata/CUSA54321/sdimg_SAVEDATA01"
stop
printf 'backup_interval_seconds=1\nusb_id=0123456789abcdef0123456789abcdef\n%s/src/\n' "$T" > "$DATA/backup.conf"
start
wait_log "Backup"
sleep 1.5
r=$(curl -s -X POST "$URL/api/restore" --data-urlencode "kind=save" --data-urlencode "ps=ps4" --data-urlencode "uid=$UID1" --data-urlencode "title=CUSA54321" --data-urlencode "save=SAVEDATA01")
st=$(restore_wait)
check "ps4 restore ok" "has \"\$st\" '\"last_ok\":true' && has \"\$st\" 'PS4 save CUSA54321/SAVEDATA01 restored'"
check "ps4 image content restored" "[ \"\$(image_file '$HOME_ROOT/$UID1/savedata/CUSA54321/sdimg_SAVEDATA01' save.bin)\" = 'ps4 progress 1' ]"
check "ps4 key file untouched" "[ \$(stat -c %s '$HOME_ROOT/$UID1/savedata/CUSA54321/SAVEDATA01.bin') = 96 ]"
check "ps4 undo copy with key" "[ \$(ls '$T/mnt/usb2/backup/restore_undo/ps4/$UID1/CUSA54321/' | grep -c '^SAVEDATA01\.[0-9]*$') = 1 ] && [ \$(ls '$T/mnt/usb2/backup/restore_undo/ps4/$UID1/CUSA54321/' | grep -c '\.bin$') = 1 ]"
# the next pass backs the restored image up again (stamp was removed)
printf 'backup_interval_seconds=1\nusb_id=0123456789abcdef0123456789abcdef\nbackup_ps4_saves=true\n%s/src/\n' "$T" > "$DATA/backup.conf"
wait_log "Backup complete (" 300
sleep 2
check "restored save backed up again" "[ -f '$T/mnt/usb2/backup/saves/ps4/$UID1/CUSA54321/SAVEDATA01/.image_stamp' ]"
stop

echo
echo "passed: $PASS  failed: $FAIL"
[ $FAIL = 0 ]
