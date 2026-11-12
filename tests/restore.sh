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
check "unknown path refused" "has \"\$st\" 'is not in the latest backup'"
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
check "unknown save refused" "has \"\$st\" 'not in the latest backup'"
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

# --------------------------------------------------------------------------
echo "== Scenario: save history: versions, restore from a version and a daily snapshot, pruning"
rm -rf "${T:?}"; mkdir -p "$T/mnt/usb2" "$HOME_ROOT/$UID1/savedata_prospero/$TITLE" "$T/fixture" "$T/src" "$DATA"
echo 0123456789abcdef0123456789abcdef > "$T/mnt/usb2/PS5SavesBackup.id"
echo "level 1" > "$T/fixture/progress.dat"; echo x > "$T/src/a.txt"
make_image "$T/fixture" "$HOME_ROOT/$UID1/savedata_prospero/$TITLE/slot1"
printf 'backup_interval_seconds=1\nusb_id=0123456789abcdef0123456789abcdef\nbackup_ps5_saves=true\ndaily_snapshots_to_keep=2\nhistory_days_to_keep=3\n%s/src/\n' "$T" > "$DATA/backup.conf"
start
wait_log "Daily snapshot saved"
wait_log "Backup complete"
LIVE=$T/mnt/usb2/backup/saves/ps5/$UID1/$TITLE/slot1
HIST=$T/mnt/usb2/backup/history/ps5/$UID1/$TITLE/slot1
today=$(ls "$T/mnt/usb2/backup_daily" | head -1)
check "daily snapshot holds the save, not the history" "[ -f '$T/mnt/usb2/backup_daily/$today/saves/ps5/$UID1/$TITLE/slot1/progress.dat' ] && [ ! -e '$T/mnt/usb2/backup_daily/$today/history' ]"
check "no history after the first backup" "[ ! -e '$HIST' ]"
sleep 1.1; echo "level 2" > "$T/fixture/progress.dat"; make_image "$T/fixture" "$HOME_ROOT/$UID1/savedata_prospero/$TITLE/slot1"
for i in $(seq 1 100); do [ "$(count_log 'Backup complete')" -ge 2 ] && break; sleep 0.1; done
sleep 1.1; echo "level 3" > "$T/fixture/progress.dat"; make_image "$T/fixture" "$HOME_ROOT/$UID1/savedata_prospero/$TITLE/slot1"
for i in $(seq 1 100); do [ "$(count_log 'Backup complete')" -ge 3 ] && break; sleep 0.1; done
sleep 0.5
check "live copy is the newest save" "[ \"\$(cat '$LIVE/progress.dat')\" = 'level 3' ]"
check "two versions in history" "[ \$(ls '$HIST' | wc -l) = 2 ]"
v1=$(ls "$HIST" | sort | head -1); v2=$(ls "$HIST" | sort | tail -1)
check "oldest version is level 1, newer is level 2" "[ \"\$(cat '$HIST/$v1/progress.dat')\" = 'level 1' ] && [ \"\$(cat '$HIST/$v2/progress.dat')\" = 'level 2' ]"
assert_log "Save version kept: history/ps5/$UID1/$TITLE/slot1/$v1"
check "no .part folder left" "[ ! -e '$LIVE.part' ]"
r=$(curl -s "$URL/api/restore/list")
check "list offers both versions and the daily snapshot" "has \"\$r\" '\"id\":\"history/$v2\"' && has \"\$r\" '\"id\":\"history/$v1\"' && has \"\$r\" '\"id\":\"daily/$today\",\"label\":\"daily snapshot $today\"'"
check "invalid source rejected" "has \"\$(curl -s -X POST $URL/api/restore --data 'kind=save&ps=ps5&uid=$UID1&title=$TITLE&save=slot1&source=history/../x')\" 'invalid source'"
r=$(curl -s -X POST "$URL/api/restore" --data-urlencode "kind=save" --data-urlencode "ps=ps5" --data-urlencode "uid=$UID1" --data-urlencode "title=$TITLE" --data-urlencode "save=slot1" --data-urlencode "source=history/$v1")
st=$(restore_wait)
check "restore from the oldest version ok" "has \"\$st\" '\"last_ok\":true' && has \"\$st\" 'restored from history/$v1'"
check "image is level 1 again" "[ \"\$(image_file '$HOME_ROOT/$UID1/savedata_prospero/$TITLE/slot1' progress.dat)\" = 'level 1' ]"
for i in $(seq 1 100); do [ "$(count_log 'Backup complete')" -ge 4 ] && break; sleep 0.1; done
sleep 0.5
check "restored image backed up as the new live copy, level 3 went to history" "[ \"\$(cat '$LIVE/progress.dat')\" = 'level 1' ] && [ \$(ls '$HIST' | wc -l) = 3 ]"
sleep 1.1; echo "level 4" > "$T/fixture/progress.dat"; make_image "$T/fixture" "$HOME_ROOT/$UID1/savedata_prospero/$TITLE/slot1"
for i in $(seq 1 100); do [ "$(count_log 'Backup complete')" -ge 5 ] && break; sleep 0.1; done
r=$(curl -s -X POST "$URL/api/restore" --data-urlencode "kind=save" --data-urlencode "ps=ps5" --data-urlencode "uid=$UID1" --data-urlencode "title=$TITLE" --data-urlencode "save=slot1" --data-urlencode "source=daily/$today")
st=$(restore_wait)
check "restore from the daily snapshot ok" "has \"\$st\" '\"last_ok\":true' && has \"\$st\" 'restored from daily/$today'"
check "image is the snapshot content" "[ \"\$(image_file '$HOME_ROOT/$UID1/savedata_prospero/$TITLE/slot1' progress.dat)\" = 'level 1' ]"
for i in $(seq 1 100); do [ "$(count_log 'Backup complete')" -ge 6 ] && break; sleep 0.1; done   # restored save backed up again
r=$(curl -s -X POST "$URL/api/restore" --data-urlencode "kind=path" --data-urlencode "path=$T/src/a.txt" --data-urlencode "source=history/$v1")
st=$(restore_wait)
check "files cannot be restored from a save version" "has \"\$st\" 'no version history'"
r=$(curl -s -X POST "$URL/api/restore" --data-urlencode "kind=path" --data-urlencode "path=$T/src/a.txt" --data-urlencode "source=daily/$today")
st=$(restore_wait)
check "file restored from the daily snapshot" "has \"\$st\" '\"last_ok\":true' && has \"\$st\" 'restored to $T/src/a.txt'"
# pruning: jump the clock 10 days ahead
echo $(( $(date +%s) + 10 * 86400 )) > "$T/now.txt"
stop
TEST_NOW_FILE=$T/now.txt "$BIN" >"$LOG" 2>/dev/null &
PID=$!
for i in $(seq 1 50); do curl -s "$URL/api/status" >/dev/null 2>&1 && break; sleep 0.1; done
wait_log "Save version removed"
sleep 1
check "old versions pruned, history folder gone" "[ ! -e '$HIST' ] && [ ! -e '$T/mnt/usb2/backup/history' ]"
check "live copy untouched by pruning" "[ -f '$LIVE/progress.dat' ]"
stop
# history off: a changed save replaces the live copy without keeping a version
printf 'backup_interval_seconds=1\nusb_id=0123456789abcdef0123456789abcdef\nbackup_ps5_saves=true\nhistory_days_to_keep=0\n%s/src/\n' "$T" > "$DATA/backup.conf"
start
wait_log "Backup"
sleep 1.1; echo "level 5" > "$T/fixture/progress.dat"; make_image "$T/fixture" "$HOME_ROOT/$UID1/savedata_prospero/$TITLE/slot1"
for i in $(seq 1 100); do [ "$(cat "$LIVE/progress.dat" 2>/dev/null)" = "level 5" ] && break; sleep 0.1; done
check "history off: live copy replaced" "[ \"\$(cat '$LIVE/progress.dat')\" = 'level 5' ]"
check "history off: no version kept" "[ ! -e '$T/mnt/usb2/backup/history' ]"
stop

echo
echo "passed: $PASS  failed: $FAIL"
[ $FAIL = 0 ]
