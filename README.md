<img width="128" height="128" alt="PS5SavesBackup" src="https://github.com/user-attachments/assets/dbdbdd48-14d2-46fa-8a6f-189ede65da7b" />

# PS5SavesBackup - Automatic game save backups for jailbroken PS5s

PS5SavesBackup is a background payload that keeps copies of your game saves and other files so you never lose them. It started as a fork of [PS5Save](https://github.com/heni0xyz/PS5Save) and grew into a save backup manager:

- Fully automatic: runs in the background, copies only what changed, keeps dated daily snapshots
- PS5 saves, PS4 saves (decrypted), emulator saves and any other files or folders you list
- **Settings page**: everything is configured from a web page served by the payload, opened in the console browser or from your phone or PC. No more editing files by hand.
- **Any USB layout**: the backup partition is picked on the settings page and found again at every start, whatever `/mnt/usbN` number it gets. Multi-partition drives (for example a Linux boot partition plus an exFAT backup partition) work.
- Backup destinations beyond USB (Google Drive) are planned; the settings page is where they will be set up.

<img width="1280" height="720" alt="Notification screenshot" src="https://github.com/user-attachments/assets/cbef0c27-b218-4da1-943c-f6c0da6e0ef4" />

# Quick start

1. Plug in a USB drive with an exFAT partition that has room for your saves.
2. Push `ps5savesbackup.elf` to your PS5 with a network loader or an autoloader.
3. A notification shows the address of the settings page, for example `http://192.168.1.23:9056/`. On a first run without any configuration the payload also opens it in the console browser.
4. On the page: pick the backup partition, enable PS4/PS5 saves, add any extra paths, save.

Backups start right away and repeat at the configured interval. The settings page shows the state, the last result, free space and the recent log. A "Back up now" button runs a pass immediately.

PS4/PS5 saves are backed up while in-game except for the game you are currently playing; it is backed up after you exit the title.

# How it works

**Settings** live in `/data/homebrew/PS5SavesBackup/backup.conf`. The page reads and writes this file; you can still edit it by hand (over FTP) in the same format as before. Changes take effect at the next pass.

**Backup USB.** When you pick a partition on the page, a small file `PS5SavesBackup.id` with a random id is written to its root and the id is stored in the settings (`usb_id=`). At every start the payload scans `/mnt/usb0`, `/mnt/usb1`, ... in numerical order and selects the partition whose marker matches. Partitions without the marker are never written to, there is no fallback to `/mnt/usb0`, and the destination changes only when you pick another partition. If the drive is unplugged while the payload runs, writing stops until the same partition is back.

**Compatibility with PS5Save.** Without a settings file the payload behaves like PS5Save v2: the first USB whose root holds a `backup.conf` is used. The settings page shows those settings; saving them once moves them to the internal file and marks that USB.

**On the USB:**
```
PS5SavesBackup.id     marker
backup/               live backup: <original path> for files, saves/ps5/<user>/<title>/... for saves
backup_daily/         dated snapshots YYYY-MM-DD (console local date)
```

Startup log (klog):
```
[PS5SavesBackup] Settings page: http://192.168.1.23:9056/
[PS5SavesBackup] Scanning USB devices...
[PS5SavesBackup] Checking /mnt/usb0
[PS5SavesBackup] Checking /mnt/usb0/PS5SavesBackup.id ... NOT FOUND
[PS5SavesBackup] Checking /mnt/usb1
[PS5SavesBackup] Checking /mnt/usb1 ... not mounted
[PS5SavesBackup] Checking /mnt/usb2
[PS5SavesBackup] Checking /mnt/usb2/PS5SavesBackup.id ... FOUND
[PS5SavesBackup] Selected USB: /mnt/usb2
[PS5SavesBackup] Backup destination: /mnt/usb2/backup
[PS5SavesBackup] Starting backup service...
```

# Settings reference

The same keys work on the page and in the file:

```
# Delay between backup passes in seconds (int, min 30 max 3600)
backup_interval_seconds=30

# Enable decrypted PS4 save backups (false, true)
backup_ps4_saves=true

# Enable decrypted PS5 save backups (false, true)
backup_ps5_saves=true

# Maximum combined save-image size per title in MB (int)
max_title_size_mb=4096

# Number of dated snapshots retained, values 0 thru 10 are valid; 0 means disabled (int)
daily_snapshots_to_keep=2

# Id of the backup USB partition (content of its PS5SavesBackup.id file); pick it on the settings page
usb_id=0123456789abcdef0123456789abcdef

# Files and folders to back up, one absolute path per line

# PS5SX2
/data/PCSX2/Mcd001.ps2
/data/PCSX2/Mcd002.ps2

# PS5SX3
/data/homebrew/PPSA99303/rpcs3/dev_hdd0/home/

# Porpoise
/data/porpoise/saves/

# ProsperoEden
/data/prosperoeden/user/nand

# PS5X360
/data/homebrew/PPSA50011/saves
```

Paths that contain the backup destination (`/`, `/mnt`, the USB itself, `..` tricks) are refused. Plain files larger than 100MB are skipped; a title's saves are backed up completely or not at all, within `max_title_size_mb`.

# Privacy

See [PRIVACY.md](PRIVACY.md): the software stores data only on your console, on the USB you select and, if you enable it, in your own Google Drive. Nothing is sent anywhere else.

# Network

The settings page listens on port 9056 on the console's LAN address. Like the other PS5 homebrew services (FTP, payload loaders) it has no password: anyone on your home network can open it. The payload makes no connections to the internet.

Port 9055 on the loopback interface is used to replace a running instance.

## Credits
- heni0xyz — [PS5Save](https://github.com/heni0xyz/PS5Save), the project this is based on
- n0llptr — [Playstation 5 Save Mounter](https://github.com/n0llptr/Playstation-5-Save-Mounter)
- earthonion — loading game titles code and save creation: [Garlic SaveMgr](https://git.etawen.dev/earthonion/garlic-savemgr)
- cow — save mounting research using internal PS5 `sceFs*` functions

## Building

Requires Linux (for example Ubuntu 24.04) and the [PS5 Payload SDK](https://github.com/ps5-payload-dev/sdk).

Install the build tools

```sh
sudo apt update
sudo apt install make clang-18 lld-18 llvm-18 wget unzip
```

Download and extract the SDK

```sh
wget https://github.com/ps5-payload-dev/sdk/releases/download/v0.44/ps5-payload-sdk.zip
unzip ps5-payload-sdk.zip
export PS5_PAYLOAD_SDK="$(pwd)/ps5-payload-sdk"
export LLVM_CONFIG=/usr/bin/llvm-config-18
```

Open the source directory, then run:

```sh
make
```

The compiled payload is `ps5savesbackup.elf`.

To rebuild after changing the source:

```sh
make clean
make
```

The settings page is `web/index.html`, embedded into the payload as `web_page.h`. After editing the page run `make web_page.h` (needs python3) and rebuild.

## Testing

The USB detection and the settings page can be exercised on a Linux host without a console or the SDK (clang and curl required). `main.c` is built with `-DBACKUP_HOST_TEST` against stubbed PS5 APIs (`tests/host/`); all console paths are redirected below `tests/run`:

```sh
make test
```

`tests/usb_detection.sh` covers: legacy configuration on usb0, usb1, usb2 and usb10 (numeric order), several partitions with only one `backup.conf`, several with one (lowest number wins), no configuration, a read-only partition, a USB unplugged while running, sources overlapping the destination, and selection by marker id (ignoring legacy files, marked USB not connected, no selection). `tests/web_ui.sh` covers the page and its API: status, migration from a legacy `backup.conf`, validation, switching the USB, "back up now", the log, and first start without any configuration. The read-only and unplug scenarios use real tmpfs mounts and therefore need root; run `VERBOSE=1 make test` to print the payload log of a failed check.
