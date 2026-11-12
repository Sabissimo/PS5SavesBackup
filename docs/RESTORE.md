# Stage A: manual restore

## What the user sees
A "Restore" section on the settings page lists what the backup contains:

- PS4/PS5 saves: platform, user id, title id, save name, number of files,
  size, when it was backed up, whether the save slot exists on this console
  and whether the game is running right now.
- Plain files and folders from the path list: path, size, file count.

Each row has a "Restore" button with a confirmation. A "Download from Google
Drive" button fills the local backup folder from the Drive folder (for a
console that has no local backup yet). Progress and the result are shown in
the same section; the log shows every step.

## Where the data is
Local backup folder (`<usb>/backup` or `/data/homebrew/PS5SavesBackup/backup`):

```
backup/<original path>                        plain files, as backed up
backup/saves/ps4/<uid>/<title>/<save>/...     decrypted files of a PS4 save
backup/saves/ps5/<uid>/<title>/<save>/...     decrypted files of a PS5 save
backup/saves/.../<save>/.image_stamp          size/mtime of the image backed up
backup/restore_undo/...                       copies of images replaced by a restore
```

Save images on the console:

```
PS4: /user/home/<uid>/savedata/<title>/sdimg_<save>  +  <save>.bin (sealed key)
PS5: /user/home/<uid>/savedata_prospero/<title>/<save>  (sealed key inside, at 0x800)
```

## Restore of a plain file or folder
1. Refuse paths that overlap the backup destination (same check as backup).
2. If the path contains a title id of an application that is running or
   suspended, refuse (emulator saves while the emulator runs).
3. Copy `backup/<path>` to `<path>`: each file to `<file>.part` first, then
   rename. Folders are merged: files present in the backup overwrite, other
   files are left alone.
4. Report copied/failed counts.

## Restore of a save
Done by the main thread (never concurrently with a backup pass), with the
same privilege elevation as the backup.

1. The save slot must exist: the image file (and the .bin for PS4) is there.
   Creating missing slots is not part of stage A.
2. The title must not be running or suspended (`title_blocked`); unknown
   activity state refuses, like the backup does.
3. Free space: internal storage needs the image size twice (working copy and
   the new image), the backup destination needs one image size for the undo
   copy.
4. Undo copy: the current image (and .bin) is copied to
   `backup/restore_undo/<ps>/<uid>/<title>/<save>.<unix time>` before
   anything else. The three newest undo copies per save are kept.
5. Working copy: the image is copied to `TMP_DIR/_restore_tmp_image`, the key
   is unsealed through pfsmgr like for a backup, and the copy is mounted at
   `PFS_ROOT/saverestore_tmp` (read-write).
6. The mounted content is replaced by the backup: every file below the
   mount is removed (except nothing: sce_sys is in the backup as well), then
   the backup files are copied in. Files are copied to `.part` first.
7. Unmount, `sync()`.
8. Swap: the working copy is copied next to the original as
   `<image>.ps5sbnew`, fsynced, then renamed over the original. The PS4
   `.bin` is not touched. If anything before the rename fails, the original
   is untouched; the undo copy covers a failure after it.
9. The `.image_stamp` of that save is removed so the next backup pass backs
   the restored image up again (content identical, bookkeeping fresh).

Rejected alternative: mounting the original image read-write and editing in
place. Simpler, but an interrupted restore leaves a half-edited save.

## Download from Google Drive
Reverse mirror of the upload: the Drive folder tree is listed
(`files.list` with `'<id>' in parents`, paged), and every file whose id,
size or modifiedTime differs from the local index entry is downloaded
(`files/<id>?alt=media`) to `backup/<relative path>` via `.part` + rename.
Files that exist locally but not in Drive are left alone. The index is
updated so the next upload pass does not re-upload what was just downloaded.

## API
- `GET /api/restore/list` -> `{saves:[...], files:[...], busy, last}`
- `POST /api/restore` form `kind=save&ps=ps5&uid=..&title=..&save=..` or
  `kind=path&path=/data/...` -> queued; `409` while a job runs
- `POST /api/gdrive/download` -> queued
- `GET /api/restore/status` -> `{busy, job, step, result, error}`

Jobs are queued by the page thread and executed by the main thread between
backup passes (the main loop is woken through the wake pipe).

## Host test plan (tests/restore.sh)
The host stubs simulate save images: an image file `X` with a sibling folder
`X.contents/`; "mounting" copies the folder to the mount point, "unmounting"
copies the mount point back. `decrypt_key` is a no-op on the host.

- plain file restore: changed source restored from backup, `.part` cleanup
- folder restore merges, leaves extra files, refuses overlapping paths
- refuse while the title id in the path is running (TEST_ACTIVE_TITLES)
- PS5 save: backup, modify the "image" contents, restore, contents equal the
  backup, undo copy exists, `.image_stamp` removed, next pass re-backs up
- PS4 save: same with sdimg_/.bin
- refuse: missing slot, running title, not enough space
- interrupted restore (kill between working copy and swap): original intact
- Google Drive download: fresh console gets the files, index updated, second
  download does nothing, upload after download does nothing

## Checklist
- [ ] docs committed
- [ ] restore.h: job queue, plain file restore
- [ ] restore.h: save restore with undo copy and atomic swap
- [ ] host stubs: simulated save mount, decrypt no-op
- [ ] web UI: Restore section, list and buttons
- [ ] gdrive.h: download (reverse mirror), fake server endpoints
- [ ] tests/restore.sh, tests/gdrive.sh download scenario
- [ ] README section "Restoring"
- [ ] console check: PS5 save restore on a throw-away game
- [ ] console check: PS4 save restore
- [ ] console check: download from Drive on a console without a backup
