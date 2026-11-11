<img width="128" height="128" alt="PS5Save" src="https://github.com/user-attachments/assets/dbdbdd48-14d2-46fa-8a6f-189ede65da7b" /> 

# PS5Save - Automatic game save and file backups!
Easy to use USB save backup utility for jailbroken PS5s that helps you prevent data loss!  
- Fully automatic with daily snapshots, supports PS5 saves, PS4 saves, emulator saves and other files
<img width="1280" height="720" alt="PS5Save notification screenshot" src="https://github.com/user-attachments/assets/cbef0c27-b218-4da1-943c-f6c0da6e0ef4" />


# Guide
- Get a fast USB storage device that is big enough to fit all of your saves (formatted as exFAT)
- Create a `backup.conf` file on the root of your USB
- Fill `backup.conf` with your personalized values (look at Backup Configuration section)
- Plug in into your PS5
- Push the backup.elf payload to your PS5 using a network loader or an autoloader of your choice

The USB partition is found automatically: PS5Save scans `/mnt/usb0`, `/mnt/usb1`, `/mnt/usb2`, ... in numerical order and uses the first one whose root contains `backup.conf`. That partition is used for the configuration, the live backup (`backup/`) and the daily snapshots (`backup_daily/`). A multi-partition drive works too (for example a FAT32 boot partition on `usb0` and the exFAT backup partition on `usb2`): partitions without `backup.conf` are never written to. If no `backup.conf` is found, or the partition is read-only, the payload reports an error and exits without backing anything up. If the drive is unplugged while the payload runs, writing stops until the same partition is back; it never switches to another USB on its own (restart the payload for that).

Startup log (klog), for example:
```
[PS5Save] Scanning USB devices...
[PS5Save] Checking /mnt/usb0
[PS5Save] Checking /mnt/usb0/backup.conf ... NOT FOUND
[PS5Save] Checking /mnt/usb1
[PS5Save] Checking /mnt/usb1 ... not mounted
[PS5Save] Checking /mnt/usb2
[PS5Save] Checking /mnt/usb2/backup.conf ... FOUND
[PS5Save] Found backup.conf
[PS5Save] Selected USB: /mnt/usb2
[PS5Save] Backup destination: /mnt/usb2/backup
[PS5Save] Starting backup service...
```

PS4/PS5 saves will backed up while in-game except for the game you're currently playing it will be backed up after you exit the title *(since version v2)*

# Backup Configuration
Example: 
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

# File backup section with most popular emulators listed

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


## Credits
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
export LLVM_CONFIG=llvm-config-18
```

Open the PS5Save source directory, then run:

```sh
make
```

The compiled payload is `backup.elf`.

To rebuild after changing the source:

```sh
make clean
make
```

## Testing

The USB detection can be exercised on a Linux host without a console or the SDK (clang required). `main.c` is built with `-DBACKUP_HOST_TEST` against stubbed PS5 APIs (`tests/host/`), and `tests/usb_detection.sh` runs the scenarios below with fake mount points under `tests/run/mnt/usb<N>`:

```sh
make test
```

Covered: configuration on usb0, usb1, usb2 and usb10 (numeric order), several partitions with only one `backup.conf`, several partitions with a `backup.conf` (lowest number wins), no configuration at all, a read-only partition, a USB unplugged while the payload runs (and another USB appearing meanwhile), and backup sources that contain the destination (`/`, `/mnt`, the USB itself, `..` paths, an alias mount of the USB). The read-only and unplug scenarios use real tmpfs mounts and therefore need root; run `VERBOSE=1 make test` to print the payload log of a failed check.
