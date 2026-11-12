# PS5SavesBackup payload: requires PS5_PAYLOAD_SDK (see README). The host
# test below builds main.c for Linux with stubbed PS5 APIs and needs no SDK.
ifdef PS5_PAYLOAD_SDK
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
endif

ELF    := ps5savesbackup.elf
CFLAGS := -O2 -Wall -Wextra -Werror
LDLIBS := -lSceFsInternalForVsh -lSceNotification -lSceSystemService -lSceUserService -lpthread

HEADERS := notification_icon.h backup_progress.h title_activity.h change_index.h \
           usb_storage.h settings.h web_ui.h web_page.h

all: $(ELF)

$(ELF): main.c $(HEADERS)
ifndef PS5_PAYLOAD_SDK
	$(error Set PS5_PAYLOAD_SDK to your installed ps5-payload-sdk directory)
endif
	$(CC) $(CFLAGS) main.c $(LDLIBS) -o $@

# The settings page is embedded into the payload. Regenerate after editing
# web/index.html (python3 required only for that).
web_page.h: web/index.html tools/embed.py
	python3 tools/embed.py web/index.html web_index_html > $@

# ---------------------------------------------------------------------------
# Host test (Linux, clang): make test
# All console paths are redirected below TEST_ROOT; USB mount points are
# plain folders (or tmpfs mounts when run as root) under TEST_ROOT/mnt.
# ---------------------------------------------------------------------------
HOST_CC   ?= clang
TEST_ROOT ?= $(CURDIR)/tests/run
HOST_BIN  := tests/host/ps5savesbackup_host
HOST_CFLAGS := -O1 -g -Wall -Wextra -Werror -DBACKUP_HOST_TEST \
  -Dst_mtimespec=st_mtim -Dst_ctimespec=st_ctim -Itests/host/include \
  -DUSB_MOUNT_DIR='"$(TEST_ROOT)/mnt"' -DDATA_DIR='"$(TEST_ROOT)/data/homebrew/PS5SavesBackup"' \
  -DHOME_ROOT='"$(TEST_ROOT)/home"' -DTMP_DIR='"$(TEST_ROOT)/data"' \
  -DPFS_ROOT='"$(TEST_ROOT)/pfs"' -DPFSMGR_DEV='"$(TEST_ROOT)/pfsmgr"' \
  -DNOTIFICATION_ICON_DIR='"$(TEST_ROOT)/icon"' \
  -DLOCK_PORT=19055 -DWEB_PORT=19056 -DMIN_INTERVAL=1 -DSLICE_SEC=1

$(HOST_BIN): main.c $(HEADERS) tests/host/stubs.c
	$(HOST_CC) $(HOST_CFLAGS) main.c tests/host/stubs.c -lpthread -o $@

test: $(HOST_BIN)
	TEST_ROOT=$(TEST_ROOT) bash tests/usb_detection.sh $(HOST_BIN)
	TEST_ROOT=$(TEST_ROOT) bash tests/web_ui.sh $(HOST_BIN)

.PHONY: all clean test
clean:
	rm -f $(ELF) $(HOST_BIN)
