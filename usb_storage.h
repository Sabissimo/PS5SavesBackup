/* Runtime selection of the backup USB partition.
 *
 * The mount points /mnt/usb<N> are discovered, sorted by N and checked in
 * order. Two ways to recognise the backup partition:
 *
 *   - settings mode: the partition carries the marker file PS5SavesBackup.id
 *     whose content equals usb_id= from the settings file. The marker is
 *     written only when the user picks that partition on the settings page.
 *   - legacy mode: no settings file exists and the partition root holds a
 *     readable backup.conf (PS5Save v2 behaviour).
 *
 * Nothing is ever written to a mount point that was not recognised, there is
 * no fallback to /mnt/usb0, and the selected mount point is only exchanged
 * for another one when the user changes the selection in the settings. */
#ifndef USB_STORAGE_H
#define USB_STORAGE_H

#ifndef USB_MOUNT_DIR
#define USB_MOUNT_DIR   "/mnt"     /* where usb<N> mount points live */
#endif
#define USB_NAME_PREFIX "usb"
#define USB_PROBE_MAX   16         /* usb0..usb15 if USB_MOUNT_DIR is unreadable */
#define MAX_USB_MOUNTS  64
#define CONF_NAME       "backup.conf"
#define MARKER_NAME     "PS5SavesBackup.id"
#define BACKUP_DIR      "backup"
#define DAILY_DIR       "backup_daily"
#define USB_PROBE_NAME  ".ps5savesbackup-write-test"
#define USB_ID_LEN      32         /* hex characters in a marker id */
#ifndef USB_CHECK_SEC
#define USB_CHECK_SEC   1.0        /* how often presence is re-checked while copying */
#endif

static char g_usb_root[PATH_MAX];     /* e.g. /mnt/usb2; empty until selected */
static char g_dest_root[PATH_MAX];    /* local destination: the USB, or DATA_DIR */
static int g_dest_internal;           /* backups go to internal storage (Google Drive mode) */
static char g_conf_path[PATH_MAX];    /* settings file in use (internal or legacy USB) */
static char g_backup_root[PATH_MAX];  /* <usb>/backup */
static char g_daily_root[PATH_MAX];   /* <usb>/backup_daily */
static char g_usb_id[USB_ID_LEN + 1]; /* marker id the USB was selected by ("" = legacy) */
static dev_t g_usb_dev;               /* identity of the selected mount */
static ino_t g_usb_ino;
static int g_usb_lost;                /* selected USB disappeared; stop writing */
static double g_usb_checked;

typedef struct usb_mount {
  char name[32];
  unsigned long number;
} usb_mount_t;


/* usb<N>: the prefix followed by decimal digits only, nothing else. */
static int
usb_mount_name(const char *name, unsigned long *number) {
  size_t plen = strlen(USB_NAME_PREFIX);
  const char *p = name + plen;

  if(strncmp(name, USB_NAME_PREFIX, plen) || !*p || strlen(p) > 9) {
    return 0;
  }
  for(const char *q = p; *q; q++) {
    if(!isdigit((unsigned char)*q)) {
      return 0;
    }
  }
  *number = strtoul(p, NULL, 10);
  return 1;
}


static int
usb_mount_cmp(const void *a, const void *b) {
  const usb_mount_t *x = a, *y = b;

  if(x->number != y->number) {
    return x->number < y->number ? -1 : 1;
  }
  return strcmp(x->name, y->name);
}


/* Every usb<N> entry below USB_MOUNT_DIR, in numerical order. If the
 * directory cannot be listed, usb0..usb15 are probed one by one instead. */
static int
usb_list_mounts(usb_mount_t *mounts, int max) {
  struct dirent *dp;
  int n = 0;
  DIR *dir;

  if(!(dir = opendir(USB_MOUNT_DIR))) {
    for(int i = 0; i < USB_PROBE_MAX && n < max; i++, n++) {
      snprintf(mounts[n].name, sizeof mounts[n].name, "%s%d", USB_NAME_PREFIX, i);
      mounts[n].number = (unsigned long)i;
    }
    return n;
  }
  while((dp = readdir(dir))) {
    unsigned long number;

    if(!usb_mount_name(dp->d_name, &number)) {
      continue;
    }
    if(n == max) {
      logq("More than %d USB mount points; the rest are ignored", max);
      break;
    }
    snprintf(mounts[n].name, sizeof mounts[n].name, "%s", dp->d_name);
    mounts[n].number = number;
    n++;
  }
  closedir(dir);
  qsort(mounts, n, sizeof *mounts, usb_mount_cmp);
  return n;
}


/* A usable mount point is a directory with a file system of its own: an
 * unmounted /mnt/usbN is a plain folder on internal storage. A host test
 * with plain directories (BACKUP_HOST_TEST) skips the mount check. */
static int
usb_root_mounted(const char *root, const struct stat *parent, struct stat *st) {
  if(stat(root, st) || !S_ISDIR(st->st_mode)) {
    return 0;
  }
#ifndef BACKUP_HOST_TEST
  if(parent && st->st_dev == parent->st_dev) {
    return 0;
  }
#else
  (void)parent;
#endif
  return 1;
}


/* Reads a small text file below root into buf (first line, trimmed).
 * Returns 1 if the file is a readable regular file. */
static int
usb_read_small(const char *root, const char *name, char *buf, size_t len) {
  char path[PATH_MAX];
  struct stat st;
  ssize_t n;
  int fd;

  if(snprintf(path, sizeof path, "%s/%s", root, name) >= (int)sizeof path) {
    return 0;
  }
  if(stat(path, &st) || !S_ISREG(st.st_mode)) {
    return 0;
  }
  if((fd = open(path, O_RDONLY)) < 0) {
    return 0;
  }
  n = read(fd, buf, len - 1);
  close(fd);
  if(n < 0) {
    return 0;
  }
  buf[n] = 0;
  buf[strcspn(buf, "\r\n")] = 0;
  return 1;
}


/* Exactly USB_ID_LEN lowercase hex characters. */
static int
usb_id_valid(const char *id) {
  if(strlen(id) != USB_ID_LEN) {
    return 0;
  }
  for(const char *p = id; *p; p++) {
    if(!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'))) {
      return 0;
    }
  }
  return 1;
}


/* Does this mount point belong to us? Settings mode compares the marker id,
 * legacy mode looks for a readable backup.conf. Logs the verdict. */
static int
usb_root_matches(const char *root, const char *usb_id) {
  char content[128];

  if(usb_id) {
    if(!usb_read_small(root, MARKER_NAME, content, sizeof content)) {
      logq("Checking %s/%s ... NOT FOUND", root, MARKER_NAME);
      return 0;
    }
    if(strcmp(content, usb_id)) {
      logq("Checking %s/%s ... another id", root, MARKER_NAME);
      return 0;
    }
    logq("Checking %s/%s ... FOUND", root, MARKER_NAME);
    return 1;
  }
  if(!usb_read_small(root, CONF_NAME, content, sizeof content)) {
    logq("Checking %s/%s ... NOT FOUND", root, CONF_NAME);
    return 0;
  }
  logq("Checking %s/%s ... FOUND", root, CONF_NAME);
  return 1;
}


/* The selected USB must accept writes: the backup folder is created and a
 * probe file is written and removed inside it. Nothing else is touched. */
static int
usb_writable(void) {
  char probe[PATH_MAX];
  int fd;

  if(mkdir_p(g_backup_root)) {
    logq("Selected USB %s is not writable: cannot create %s (%s)",
         g_usb_root, g_backup_root, strerror(errno));
    return 0;
  }
  if(snprintf(probe, sizeof probe, "%s/%s", g_backup_root, USB_PROBE_NAME) >=
     (int)sizeof probe) {
    return 0;
  }
  if((fd = open(probe, O_WRONLY | O_CREAT | O_TRUNC, 0666)) < 0) {
    logq("Selected USB %s is not writable (%s)", g_usb_root, strerror(errno));
    return 0;
  }
  if(write(fd, "ok\n", 3) != 3 || fsync(fd)) {
    logq("Selected USB %s is not writable (%s)", g_usb_root, strerror(errno));
    close(fd);
    unlink(probe);
    return 0;
  }
  close(fd);
  if(unlink(probe)) {
    logq("Selected USB %s is not writable: cannot remove probe file (%s)",
         g_usb_root, strerror(errno));
    return 0;
  }
  return 1;
}


static void
usb_clear(void) {
  g_usb_root[0] = g_dest_root[0] = g_backup_root[0] = g_daily_root[0] = g_usb_id[0] = 0;
  g_usb_dev = 0;
  g_usb_ino = 0;
  g_usb_lost = 0;
  g_dest_internal = 0;
}


/**
 * Backups go to internal storage below dir (used with Google Drive and no
 * USB). The same presence/overlap guards apply to that folder.
 **/
static int
dest_select_internal(const char *dir) {
  struct stat st;

  usb_clear();
  if(mkdir_p(dir) || stat(dir, &st) || !S_ISDIR(st.st_mode)) {
    logq("Cannot create %s (%s)", dir, strerror(errno));
    return -1;
  }
  snprintf(g_dest_root, sizeof g_dest_root, "%s", dir);
  snprintf(g_backup_root, sizeof g_backup_root, "%s/%s", dir, BACKUP_DIR);
  snprintf(g_daily_root, sizeof g_daily_root, "%s/%s", dir, DAILY_DIR);
  g_usb_dev = st.st_dev;
  g_usb_ino = st.st_ino;
  g_dest_internal = 1;
  logq("Backup destination: %s (internal storage, for Google Drive)", g_backup_root);
  return mkdir_p(g_backup_root) ? -1 : 0;
}


/**
 * Scan the USB mount points in numerical order and select the first one
 * that matches: the marker id when usb_id is given, backup.conf otherwise.
 * Returns 0 on success, -1 if nothing matched, -2 if the matching USB cannot
 * be written to (g_usb_root then names it, for the error report).
 **/
static int
usb_select(const char *usb_id) {
  usb_mount_t mounts[MAX_USB_MOUNTS];
  struct stat parent, st;
  int n;

  usb_clear();
  logq("Scanning USB devices...");
  if(stat(USB_MOUNT_DIR, &parent)) {
    memset(&parent, 0, sizeof parent);
  }
  n = usb_list_mounts(mounts, MAX_USB_MOUNTS);
  if(n == 0) {
    logq("No %s<N> mount points found below %s", USB_NAME_PREFIX, USB_MOUNT_DIR);
  }

  for(int i = 0; i < n; i++) {
    char root[PATH_MAX];

    if(snprintf(root, sizeof root, "%s/%s", USB_MOUNT_DIR, mounts[i].name) >=
       (int)sizeof root) {
      continue;
    }
    logq("Checking %s", root);
    if(!usb_root_mounted(root, &parent, &st)) {
      logq("Checking %s ... not mounted", root);
      continue;
    }
    if(!usb_root_matches(root, usb_id)) {
      continue;
    }
    logq("Found %s", usb_id ? MARKER_NAME : CONF_NAME);
    snprintf(g_usb_root, sizeof g_usb_root, "%s", root);
    snprintf(g_dest_root, sizeof g_dest_root, "%s", root);
    snprintf(g_backup_root, sizeof g_backup_root, "%s/%s", root, BACKUP_DIR);
    snprintf(g_daily_root, sizeof g_daily_root, "%s/%s", root, DAILY_DIR);
    snprintf(g_usb_id, sizeof g_usb_id, "%s", usb_id ? usb_id : "");
    g_usb_dev = st.st_dev;
    g_usb_ino = st.st_ino;
    g_usb_lost = 0;
    logq("Selected USB: %s", g_usb_root);
    logq("Backup destination: %s", g_backup_root);
    if(!usb_writable()) {
      return -2;
    }
    return 0;
  }

  if(usb_id) {
    logq("No USB with %s id %.8s... connected (%d mount point(s) checked)",
         MARKER_NAME, usb_id, n);
  } else {
    logq("No %s found on any USB device (%d mount point(s) checked)", CONF_NAME, n);
  }
  return -1;
}


/**
 * Is the selected USB still there? The mount point must still be the same
 * file system and still hold its marker (or backup.conf in legacy mode).
 * Once it is gone, every copy loop stops at its next check; the loss is
 * logged once.
 **/
static int
usb_online(void) {
  struct stat st;
  char content[128];

  if(g_dest_internal) {
    return 1;
  }
  if(!g_usb_root[0]) {
    return 0;
  }
  if(stat(g_usb_root, &st) || !S_ISDIR(st.st_mode) ||
     st.st_dev != g_usb_dev || st.st_ino != g_usb_ino ||
     !usb_read_small(g_usb_root, g_usb_id[0] ? MARKER_NAME : CONF_NAME,
                     content, sizeof content)) {
    if(!g_usb_lost) {
      logq("USB %s is no longer available; stopping writes", g_usb_root);
    }
    g_usb_lost = 1;
    return 0;
  }
  return 1;
}


/* Cheap periodic presence check for the copy loops. */
static void
usb_watch(void) {
  double t;

  if(g_usb_lost) {
    return;
  }
  t = now_seconds();
  if(t - g_usb_checked < USB_CHECK_SEC) {
    return;
  }
  g_usb_checked = t;
  usb_online();
}


/**
 * Called at the start of every backup pass. The same mount point is checked
 * again (after rest mode the drive may have been re-mounted, which changes
 * its file system identity); another mount point is never considered here.
 **/
static int
usb_cycle_begin(void) {
  struct stat parent, st;

  if(g_dest_internal) {
    return 1;
  }
  if(!g_usb_root[0]) {
    return 0;
  }
  if(stat(USB_MOUNT_DIR, &parent)) {
    memset(&parent, 0, sizeof parent);
  }
  if(!usb_root_mounted(g_usb_root, &parent, &st)) {
    if(!g_usb_lost) {
      logq("USB %s is no longer available; stopping writes", g_usb_root);
    }
    g_usb_lost = 1;
    return 0;
  }
  if(st.st_dev != g_usb_dev || st.st_ino != g_usb_ino) {
    char content[128];
    const char *name = g_usb_id[0] ? MARKER_NAME : CONF_NAME;
    if(!usb_read_small(g_usb_root, name, content, sizeof content) ||
       (g_usb_id[0] && strcmp(content, g_usb_id))) {
      if(!g_usb_lost) {
        logq("USB %s is no longer available; stopping writes", g_usb_root);
      }
      g_usb_lost = 1;
      return 0;
    }
    logq("USB %s was re-mounted; continuing with the same mount point", g_usb_root);
    g_usb_dev = st.st_dev;
    g_usb_ino = st.st_ino;
  }
  if(g_usb_lost) {
    logq("USB %s is available again", g_usb_root);
  }
  g_usb_lost = 0;
  g_usb_checked = now_seconds();
  return usb_online();
}


/* path equals base or lies below it ("/" contains everything). */
static int
path_within(const char *path, const char *base) {
  size_t blen = strlen(base);

  if(!strcmp(base, "/")) {
    return path[0] == '/';
  }
  return !strncmp(path, base, blen) && (path[blen] == 0 || path[blen] == '/');
}


/* Copy of an absolute path with repeated and trailing slashes removed.
 * Fails on relative paths and on "." or ".." components. */
static int
path_normalize(const char *in, char *out, size_t len) {
  size_t n = 0;
  const char *p = in;

  if(in[0] != '/' || len < 2) {
    return 0;
  }
  while(*p) {
    const char *end;
    size_t clen;

    while(*p == '/') p++;
    if(!*p) break;
    end = strchr(p, '/');
    clen = end ? (size_t)(end - p) : strlen(p);
    if((clen == 1 && p[0] == '.') || (clen == 2 && p[0] == '.' && p[1] == '.')) {
      return 0;
    }
    if(n + 1 + clen >= len) {
      return 0;
    }
    out[n++] = '/';
    memcpy(out + n, p, clen);
    n += clen;
    p += clen;
  }
  if(n == 0) {
    out[n++] = '/';
  }
  out[n] = 0;
  return 1;
}


/**
 * A configured source must never contain the backup destination: no "/",
 * no parent of the USB mount point, nothing on the USB itself, and no
 * ".." tricks that could resolve to one of those.
 **/
static int
source_overlaps_usb(const char *path) {
  char norm[PATH_MAX];

  if(!path_normalize(path, norm, sizeof norm) || !strcmp(norm, "/")) {
    return 1;
  }
  return path_within(norm, g_dest_root) || path_within(g_dest_root, norm);
}


/* The selected mount point seen through any other path (an alias or a
 * mount of the same file system below a source folder). */
static int
is_usb_root_dir(const struct stat *st) {
  return g_dest_root[0] && S_ISDIR(st->st_mode) &&
         st->st_dev == g_usb_dev && st->st_ino == g_usb_ino;
}

#endif
