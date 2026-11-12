# PS5SavesBackup roadmap and status

Last updated: 2026-11-12. Repository: https://github.com/Sabissimo/PS5SavesBackup

This file is the hand-over document: what is done, what is in progress, what
was decided and why. Update it with every milestone.

## Done

### v3.0, step 1: settings page and USB by marker (commit 7fbd695)
- Project renamed from PS5Save to PS5SavesBackup (notifications, klog prefix
  `[PS5SavesBackup]`, icon folder `/user/data/ps5savesbackup`, payload
  `ps5savesbackup.elf`).
- Settings file `/data/homebrew/PS5SavesBackup/backup.conf`, same keys as
  PS5Save v2 plus `usb_id=`, `gdrive_enabled=`, `gdrive_folder=`.
- Web settings page served by the payload on port 9056 (`web_ui.h`,
  `web/index.html` embedded as `web_page.h`). JSON API, see `web_ui.h`.
- Backup USB chosen on the page; it gets the marker file `PS5SavesBackup.id`
  and is found by it at every start whatever `/mnt/usbN` it gets
  (`usb_storage.h`). Legacy mode (backup.conf on the USB) still works and is
  migrated on the first save.
- Host test harness (`make test`): `tests/usb_detection.sh`, `tests/web_ui.sh`.

### v3.0, step 2: Google Drive destination (commit 4b83161)
- `gdrive.h`: device-code sign-in, token file, refresh, sign-out with revoke,
  mirror of the local backup folder into a Drive folder after every pass.
  Scope `drive.file` only. `http_client.h`: libSceHttp2 on the console,
  libcurl on the host.
- Without a USB the backup lives in `/data/homebrew/PS5SavesBackup/backup`.
- OAuth client id/secret are build-time values (`make GDRIVE_CLIENT_ID=...
  GDRIVE_CLIENT_SECRET=...` or `gdrive_credentials.mk`), never committed.
- Verified against the real Google API from a Linux host: sign-in, folder
  creation, upload of a tree, re-upload of a changed file to the same id,
  sign-out. `tests/gdrive.sh` against `tests/fake_google.py`.

### Google Cloud side (owner: Sabissimo)
- Project with Google Drive API enabled, consent screen published
  (External, scope `drive.file`, no logo), OAuth client of type "TVs and
  Limited Input devices". Branding links point to this repository and to
  `PRIVACY.md`.

## In progress

### Stage A: manual restore (design in docs/RESTORE.md)
Goal: "Restore" buttons on the settings page for anything in the backup:
plain files/folders and PS4/PS5 saves, from the local backup folder, plus
"Download from Google Drive" to fill the local backup folder on a console
that has nothing yet.

Status: implemented and host-tested (commit after babd224): `restore.h`,
Restore section on the page, Drive download, `tests/restore.sh`. Waiting for
the console checks listed in docs/RESTORE.md.

## Planned

### Stage B: automatic download ("cloud saves" through Drive)
After stage A is proven on a console: watch the Drive folder for changes
made by another console (`changes.list`), and when the Drive copy of a save
is newer than the local one and the game is not running, restore it
automatically. Rule: newest wins, the losing version is kept under
`conflicts/` in the backup folder. Needs a per-console id in the Drive
index and the restore code from stage A.

### Later
- Creating a save slot for a game that was never started on this console
  (garlic-savemgr has save creation code).
- Daily snapshots in Drive (currently only the live backup is uploaded).
- Deleting files in Drive that were removed locally.
- Game names instead of title ids on the page.

## Decisions worth remembering
- The original save image is never modified in place. Backup mounts a copy;
  restore writes a modified copy next to the original and swaps it by
  rename. A copy of the previous image is kept under `restore_undo/`.
- Nothing is ever written to a USB partition without our marker (or, in
  legacy mode, without backup.conf). No fallback to /mnt/usb0.
- The payload never switches to another USB on its own; only the settings
  page changes the destination.
- Change detection is by size and mtime (never content hashes), and a
  missing index means a safe re-copy / re-upload.
- Daily snapshots are not mirrored to Drive.
- The settings page has no authentication, like other PS5 homebrew
  services on the LAN; documented in README.

## Not verified on a console yet
- `libSceHttp2`/`libSceSsl` trusting Google's certificates from a payload.
- `sceSystemServiceLaunchWebBrowser` opening the settings page on first run.
- Mounting a save image read-write through `sceFsMountSaveData` (restore).

## How to resume work
1. Clone, `make test` (needs clang, libcurl-dev, curl, python3; root for the
   tmpfs scenarios).
2. Build for the console with the SDK: see README "Building"; pass the
   Google credentials to make.
3. Read docs/RESTORE.md for the open checklist.
