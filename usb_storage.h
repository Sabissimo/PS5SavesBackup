/* Runtime selection of the backup USB partition.
 *
 * The mount points /mnt/usb<N> are discovered at startup, sorted by N, and
 * the first one whose root holds a readable regular backup.conf is used for
 * the whole process: configuration, live backup, daily snapshots, change
 * cache and retention. Nothing is ever written to a mount point without a
 * backup.conf, there is no fallback to /mnt/usb0, and the selected mount
 * point is never exchanged for another one while the payload runs. */
#ifndef USB_STORAGE_H
#define USB_STORAGE_H

#ifndef USB_MOUNT_DIR
#define USB_MOUNT_DIR   "/mnt"     /* where usb<N> mount points live */
#endif
#define USB_NAME_PREFIX "usb"
#define USB_PROBE_MAX   16         /* usb0..usb15 if USB_MOUNT_DIR is unreadable */
#define MAX_USB_MOUNTS  64
#define CONF_NAME       "backup.conf"
#define BACKUP_DIR      "backup"
#define DAILY_DIR       "backup_daily"
#define USB_PROBE_NAME  ".ps5save-write-test"
#ifndef USB_CHECK_SEC
#define USB_CHECK_SEC   1.0        /* how often presence is re-checked while copying */
#endif

static char g_usb_root[PATH_MAX];     /* e.g. /mnt/usb2; empty until selected */
static char g_conf_path[PATH_MAX];    /* <usb>/backup.conf */
static char g_backup_root[PATH_MAX];  /* <usb>/backup */
static char g_daily_root[PATH_MAX];   /* <usb>/backup_daily */
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
    logq("Cannot list %s (%s); probing %s0..%s%d", USB_MOUNT_DIR, strerror(errno),
         USB_NAME_PREFIX, USB_NAME_PREFIX, USB_PROBE_MAX - 1);
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


/* A mount point is only usable if it is a directory with a file system of
 * its own (an unmounted /mnt/usbN is a plain folder on internal storage) and
 * backup.conf in its root is a readable regular file. A host test with plain
 * directories (BACKUP_HOST_TEST) skips the mount check. */
static int
usb_conf_present(const char *root, const struct stat *parent, struct stat *st) {
  char conf[PATH_MAX];
  struct stat cs;
  int fd;

  if(stat(root, st) || !S_ISDIR(st->st_mode)) {
    logq("Checking %s ... not accessible", root);
    return 0;
  }
#ifndef BACKUP_HOST_TEST
  if(parent && st->st_dev == parent->st_dev) {
    logq("Checking %s ... not mounted", root);
    return 0;
  }
#else
  (void)parent;
#endif
  if(snprintf(conf, sizeof conf, "%s/%s", root, CONF_NAME) >= (int)sizeof conf) {
    return 0;
  }
  if(stat(conf, &cs)) {
    logq("Checking %s ... NOT FOUND", conf);
    return 0;
  }
  if(!S_ISREG(cs.st_mode)) {
    logq("Checking %s ... not a regular file", conf);
    return 0;
  }
  if((fd = open(conf, O_RDONLY)) < 0) {
    logq("Checking %s ... not readable (%s)", conf, strerror(errno));
    return 0;
  }
  close(fd);
  logq("Checking %s ... FOUND", conf);
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


/**
 * Scan the USB mount points in numerical order and select the first one
 * that holds backup.conf. Returns 0 on success, -1 if no configuration was
 * found anywhere, -2 if the selected USB cannot be written to (g_usb_root
 * then names it, for the error report).
 **/
static int
usb_select(void) {
  usb_mount_t mounts[MAX_USB_MOUNTS];
  struct stat parent, st;
  int n;

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
    if(!usb_conf_present(root, &parent, &st)) {
      continue;
    }
    logq("Found %s", CONF_NAME);
    snprintf(g_usb_root, sizeof g_usb_root, "%s", root);
    snprintf(g_conf_path, sizeof g_conf_path, "%s/%s", root, CONF_NAME);
    snprintf(g_backup_root, sizeof g_backup_root, "%s/%s", root, BACKUP_DIR);
    snprintf(g_daily_root, sizeof g_daily_root, "%s/%s", root, DAILY_DIR);
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

  logq("No %s found on any USB device (%d mount point(s) checked)", CONF_NAME, n);
  return -1;
}


/**
 * Is the selected USB still there? The mount point must still be the same
 * file system and still hold backup.conf. Once it is gone, every copy loop
 * stops at its next check; the loss is logged once.
 **/
static int
usb_online(void) {
  struct stat st, cs;

  if(!g_usb_root[0]) {
    return 0;
  }
  if(stat(g_usb_root, &st) || !S_ISDIR(st.st_mode) ||
     st.st_dev != g_usb_dev || st.st_ino != g_usb_ino ||
     stat(g_conf_path, &cs) || !S_ISREG(cs.st_mode)) {
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
 * its file system identity); another mount point is never considered.
 **/
static int
usb_cycle_begin(void) {
  struct stat parent, st;

  if(!g_usb_root[0]) {
    return 0;
  }
  if(stat(USB_MOUNT_DIR, &parent)) {
    memset(&parent, 0, sizeof parent);
  }
  if(stat(g_usb_root, &st) || !S_ISDIR(st.st_mode)
#ifndef BACKUP_HOST_TEST
     || st.st_dev == parent.st_dev
#endif
     ) {
    if(!g_usb_lost) {
      logq("USB %s is no longer available; stopping writes", g_usb_root);
    }
    g_usb_lost = 1;
    return 0;
  }
  if(st.st_dev != g_usb_dev || st.st_ino != g_usb_ino) {
    struct stat cs;
    if(stat(g_conf_path, &cs) || !S_ISREG(cs.st_mode)) {
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
  return path_within(norm, g_usb_root) || path_within(g_usb_root, norm);
}


/* The selected mount point seen through any other path (an alias or a
 * mount of the same file system below a source folder). */
static int
is_usb_root_dir(const struct stat *st) {
  return g_usb_root[0] && S_ISDIR(st->st_mode) &&
         st->st_dev == g_usb_dev && st->st_ino == g_usb_ino;
}

#endif
