/* Stage A: manual restore from the local backup folder (docs/RESTORE.md).
 *
 * Jobs are queued by the settings page thread and executed by the main
 * thread between backup passes, so a restore never runs concurrently with a
 * backup pass. Plain files are copied back to their original path; a save is
 * rebuilt in a working copy of its image which then replaces the original
 * by rename, after an undo copy of the original was stored in the backup
 * folder. The original image is never modified in place. */
#ifndef RESTORE_H
#define RESTORE_H

#define UNDO_DIR          "restore_undo"
#define UNDO_KEEP         3            /* undo copies kept per save */
#define NEW_IMAGE_SUFFIX  ".ps5sbnew"  /* new image next to the original before the swap */

static pthread_mutex_t job_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct {
  job_kind_t kind;
  int queued;
  int running;
  char path[PATH_MAX];
  int ps5;
  char uid[64];
  char title[16];
  char save[256];
  char step[160];
  char result[256];
  char error[256];
  char last_label[320];
  time_t finished;
  int ok;
} job;


static int
job_pending(void) {
  int p;

  pthread_mutex_lock(&job_mutex);
  p = job.queued;
  pthread_mutex_unlock(&job_mutex);
  return p;
}


/* Queues one job. Returns -1 when another job is queued or running. */
static int
job_submit(job_kind_t kind, const char *path, int ps5, const char *uid,
           const char *title, const char *save) {
  pthread_mutex_lock(&job_mutex);
  if(job.queued || job.running) {
    pthread_mutex_unlock(&job_mutex);
    return -1;
  }
  job.kind = kind;
  job.queued = 1;
  snprintf(job.path, sizeof job.path, "%s", path ? path : "");
  job.ps5 = ps5;
  snprintf(job.uid, sizeof job.uid, "%s", uid ? uid : "");
  snprintf(job.title, sizeof job.title, "%s", title ? title : "");
  snprintf(job.save, sizeof job.save, "%s", save ? save : "");
  job.step[0] = job.result[0] = job.error[0] = 0;
  switch(kind) {
  case JOB_RESTORE_PATH:
    snprintf(job.last_label, sizeof job.last_label, "Restore %s", job.path);
    break;
  case JOB_RESTORE_SAVE:
    snprintf(job.last_label, sizeof job.last_label, "Restore %s save %s/%s (user %s)",
             ps5 ? "PS5" : "PS4", job.title, job.save, job.uid);
    break;
  case JOB_GDRIVE_DOWNLOAD:
    snprintf(job.last_label, sizeof job.last_label, "Download from Google Drive");
    break;
  default:
    break;
  }
  pthread_mutex_unlock(&job_mutex);
  ui_wake();
  return 0;
}


static void
job_step(const char *fmt, ...) {
  va_list ap;

  pthread_mutex_lock(&job_mutex);
  va_start(ap, fmt);
  vsnprintf(job.step, sizeof job.step, fmt, ap);
  va_end(ap);
  pthread_mutex_unlock(&job_mutex);
  logq("Restore: %s", job.step);
}


/* ------------------------------------------------------------------------
 * Plain files
 * ---------------------------------------------------------------------- */

/* Copy with fsync, through a .part file: the target is complete or absent. */
static int
file_copy_sync(const char *src, const char *dst, const struct stat *st) {
  char part[PATH_MAX];
  int in = -1, out = -1, ok = 0;
  ssize_t n;

  if(snprintf(part, sizeof part, "%s.part", dst) >= (int)sizeof part) return -1;
  if((in = open(src, O_RDONLY)) < 0) return -1;
  if((out = open(part, O_WRONLY | O_CREAT | O_TRUNC, 0666)) < 0) {
    close(in);
    return -1;
  }
  while((n = read(in, g_buf, COPY_BUF_SZ)) > 0) {
    char *p = g_buf;
    while(n > 0) {
      ssize_t w = write(out, p, n);
      if(w < 0) {
        if(errno == EINTR) continue;
        goto done;
      }
      p += w;
      n -= w;
    }
  }
  if(n < 0 || fsync(out)) goto done;
  ok = 1;
done:
  close(in);
  if(close(out)) ok = 0;
  if(ok) {
    struct timeval tv[2];
    tv[0].tv_sec = st->st_mtime;
    tv[0].tv_usec = 0;
    tv[1] = tv[0];
    utimes(part, tv);
    if(rename(part, dst)) ok = 0;
  }
  if(!ok) unlink(part);
  return ok ? 0 : -1;
}


static int
restore_bookkeeping_name(const char *name) {
  size_t len = strlen(name);

  return !strcmp(name, ".ps5save-index") || !strcmp(name, ".image_stamp") ||
         (len > 5 && !strcmp(name + len - 5, ".part"));
}


/* Title id of an application mentioned in path (/data/homebrew/PPSA12345/...),
 * when that application is running: restoring its files would corrupt them.
 * live=1 asks the kernel (main thread only); live=0 uses the page snapshot. */
static int
path_title_running(const char *path, char *why, size_t len, int live) {
  const char *p = path;

  while(*p) {
    const char *end;
    size_t clen;
    while(*p == '/') p++;
    if(!*p) break;
    end = strchr(p, '/');
    clen = end ? (size_t)(end - p) : strlen(p);
    if(clen >= 9) {
      char title[10];
      memcpy(title, p, 9);
      title[9] = 0;
      if(title_valid(title) && (clen == 9 || p[9] == '_' || p[9] == '-')) {
        if(live ? title_blocked(title, why, len, 1) : ui_title_active(title)) {
          if(!live) snprintf(why, len, "%s is running", title);
          return 1;
        }
      }
    }
    if(!end) break;
    p = end;
  }
  return 0;
}


static void
restore_tree(const char *src, const char *dst, unsigned *copied, unsigned *failed) {
  char spath[PATH_MAX], dpath[PATH_MAX];
  struct dirent *dp;
  struct stat st;
  DIR *dir;

  if(mkdir_p(dst)) {
    (*failed)++;
    return;
  }
  if(!(dir = opendir(src))) {
    (*failed)++;
    return;
  }
  while((dp = readdir(dir))) {
    if(!strcmp(dp->d_name, ".") || !strcmp(dp->d_name, "..") || restore_bookkeeping_name(dp->d_name)) continue;
    if(snprintf(spath, sizeof spath, "%s/%s", src, dp->d_name) >= (int)sizeof spath ||
       snprintf(dpath, sizeof dpath, "%s/%s", dst, dp->d_name) >= (int)sizeof dpath ||
       lstat(spath, &st)) {
      (*failed)++;
      continue;
    }
    if(S_ISDIR(st.st_mode)) {
      restore_tree(spath, dpath, copied, failed);
    } else if(S_ISREG(st.st_mode)) {
      if(file_copy_sync(spath, dpath, &st)) (*failed)++;
      else (*copied)++;
    }
  }
  closedir(dir);
}


/* Restores one path (file or folder) from backup/<path>. */
static int
restore_path(const char *path, char *err, size_t elen) {
  char norm[PATH_MAX], src[PATH_MAX], why[160];
  struct stat st;
  unsigned copied = 0, failed = 0;

  if(!path_normalize(path, norm, sizeof norm) || !strcmp(norm, "/")) {
    snprintf(err, elen, "invalid path");
    return -1;
  }
  if(source_overlaps_usb(norm)) {
    snprintf(err, elen, "%s contains the backup destination", norm);
    return -1;
  }
  if(path_title_running(norm, why, sizeof why, 1)) {
    snprintf(err, elen, "the application using %s is running (%s)", norm, why);
    return -1;
  }
  if(snprintf(src, sizeof src, "%s%s", g_backup_root, norm) >= (int)sizeof src) {
    snprintf(err, elen, "path too long");
    return -1;
  }
  if(lstat(src, &st)) {
    snprintf(err, elen, "%s is not in the backup", norm);
    return -1;
  }
  job_step("copying %s back", norm);
  if(S_ISDIR(st.st_mode)) {
    restore_tree(src, norm, &copied, &failed);
  } else if(S_ISREG(st.st_mode)) {
    char dir[PATH_MAX], *slash;
    snprintf(dir, sizeof dir, "%s", norm);
    if((slash = strrchr(dir, '/')) && slash != dir) {
      *slash = 0;
      mkdir_p(dir);
    }
    if(file_copy_sync(src, norm, &st)) failed++;
    else copied++;
  } else {
    snprintf(err, elen, "%s is not a file or folder", norm);
    return -1;
  }
  if(failed) {
    snprintf(err, elen, "%u file(s) restored, %u failed", copied, failed);
    return -1;
  }
  pthread_mutex_lock(&job_mutex);
  snprintf(job.result, sizeof job.result, "%u file(s) restored to %s", copied, norm);
  pthread_mutex_unlock(&job_mutex);
  return 0;
}


/* ------------------------------------------------------------------------
 * Saves
 * ---------------------------------------------------------------------- */

static int
save_name_valid(const char *save) {
  size_t len = strlen(save);

  return len > 0 && len < 200 && !strchr(save, '/') && save[0] != '.';
}


/* Paths of the save image (and the PS4 sealed-key file) on the console. */
static void
save_image_paths(int ps5, const char *uid, const char *title, const char *save,
                 char *image, size_t ilen, char *bin, size_t blen) {
  if(ps5) {
    snprintf(image, ilen, "%s/%s/savedata_prospero/%s/%s", HOME_ROOT, uid, title, save);
    bin[0] = 0;
  } else {
    snprintf(image, ilen, "%s/%s/savedata/%s/sdimg_%s", HOME_ROOT, uid, title, save);
    snprintf(bin, blen, "%s/%s/savedata/%s/%s.bin", HOME_ROOT, uid, title, save);
  }
}


/* Keeps the UNDO_KEEP newest "<save>.<time>" copies in dir. */
static void
undo_prune(const char *dir, const char *save) {
  char (*names)[256] = malloc(64 * sizeof *names);
  char path[PATH_MAX];
  struct dirent *dp;
  size_t plen = strlen(save) + 1;
  int n = 0;
  DIR *d;

  if(!names) return;
  if((d = opendir(dir))) {
    while((dp = readdir(d)) && n < 64) {
      size_t len = strlen(dp->d_name);
      if(strncmp(dp->d_name, save, plen - 1) || dp->d_name[plen - 1] != '.') continue;
      if(len > 4 && !strcmp(dp->d_name + len - 4, ".bin")) continue;
      for(size_t i = plen; i < len; i++) if(!isdigit((unsigned char)dp->d_name[i])) goto next;
      snprintf(names[n++], 256, "%s", dp->d_name);
    next:;
    }
    closedir(d);
  }
  qsort(names, n, sizeof *names, cmp_desc);
  for(int i = UNDO_KEEP; i < n; i++) {
    snprintf(path, sizeof path, "%s/%s", dir, names[i]);
    unlink(path);
    snprintf(path, sizeof path, "%s/%s.bin", dir, names[i]);
    unlink(path);
  }
  free(names);
}


/* Empties the mounted save: everything below the mount point is removed. */
static int
mount_clear(void) {
  char path[PATH_MAX];
  struct dirent *dp;
  int rc = 0;
  DIR *d;

  if(!(d = opendir(MOUNT_POINT))) return -1;
  while((dp = readdir(d))) {
    if(!strcmp(dp->d_name, ".") || !strcmp(dp->d_name, "..")) continue;
    if(snprintf(path, sizeof path, "%s/%s", MOUNT_POINT, dp->d_name) >= (int)sizeof path ||
       remove_tree(path)) rc = -1;
  }
  closedir(d);
  return rc;
}


/**
 * Rebuilds one save from the backup. Runs with elevated privileges, never
 * while the title is running; see docs/RESTORE.md for the sequence.
 **/
static int
restore_save(int ps5, const char *uid, const char *title, const char *save, char *err, size_t elen) {
  char image[PATH_MAX], bin[PATH_MAX], backup_dir[PATH_MAX], undo_dir[PATH_MAX];
  char undo[PATH_MAX], newimg[PATH_MAX], stamp[PATH_MAX], why[160], a[32];
  uint8_t sealed[0x60], key[0x20];
  struct stat ist, bst, st;
  priv_state_t old;
  MountOpt mopt;
  uint64_t bytes = 0;
  time_t now = wall_clock();
  int mounted = 0, have_tmp = 0, rc = -1, fd, ret;

  if(!is_hex_name(uid) || !title_valid(title) || !save_name_valid(save)) {
    snprintf(err, elen, "invalid save reference");
    return -1;
  }
  snprintf(backup_dir, sizeof backup_dir, "%s/saves/%s/%s/%s/%s", g_backup_root,
           ps5 ? "ps5" : "ps4", uid, title, save);
  if(stat(backup_dir, &st) || !S_ISDIR(st.st_mode)) {
    snprintf(err, elen, "this save is not in the backup");
    return -1;
  }
  save_image_paths(ps5, uid, title, save, image, sizeof image, bin, sizeof bin);

  priv_elevate(&old);

  if(g_need_cleanup || g_mount_stuck) {
    cleanup_stale();
    g_need_cleanup = 0;
  }
  if(g_mount_stuck) {
    snprintf(err, elen, "a previous save mount could not be removed; restart the payload");
    goto out;
  }
  if(stat(image, &ist) || !S_ISREG(ist.st_mode) || (!ps5 && (stat(bin, &bst) || !S_ISREG(bst.st_mode)))) {
    snprintf(err, elen, "the save slot does not exist on this console (start the game once and save)");
    goto out;
  }
  if(title_blocked(title, why, sizeof why, 1)) {
    snprintf(err, elen, "the game is running or suspended (%s)", why);
    goto out;
  }
  if(!enough_space(2 * (uint64_t)ist.st_size + SPACE_MARGIN)) {
    format_size(2 * (uint64_t)ist.st_size + SPACE_MARGIN, a, sizeof a);
    snprintf(err, elen, "not enough console storage (need %s)", a);
    goto out;
  }
  if(usb_free() < (uint64_t)ist.st_size + SPACE_MARGIN) {
    snprintf(err, elen, "not enough space in the backup folder for the undo copy");
    goto out;
  }

  /* 1. undo copy of the current image */
  job_step("saving an undo copy of the current save");
  snprintf(undo_dir, sizeof undo_dir, "%s/%s/%s/%s/%s", g_backup_root, UNDO_DIR,
           ps5 ? "ps5" : "ps4", uid, title);
  if(mkdir_p(undo_dir)) {
    snprintf(err, elen, "cannot create %s", undo_dir);
    goto out;
  }
  snprintf(undo, sizeof undo, "%s/%s.%lld", undo_dir, save, (long long)now);
  if(file_copy_sync(image, undo, &ist)) {
    snprintf(err, elen, "cannot write the undo copy");
    goto out;
  }
  if(!ps5) {
    char undo_bin[PATH_MAX];
    snprintf(undo_bin, sizeof undo_bin, "%s.bin", undo);
    if(file_copy_sync(bin, undo_bin, &bst)) {
      snprintf(err, elen, "cannot write the undo copy");
      goto out;
    }
  }
  undo_prune(undo_dir, save);

  /* 2. working copy, key, mount */
  job_step("preparing a working copy of the save");
  mkdir_p(TMP_DIR);
  unlink(TMP_IMG);
  have_tmp = 1;
  if(copy_plain(image, TMP_IMG)) {
    snprintf(err, elen, "cannot copy the save image");
    goto out;
  }
  if(ps5) {
    if((fd = open(image, O_RDONLY)) < 0) {
      snprintf(err, elen, "cannot read the save image");
      goto out;
    }
    ret = (int)pread(fd, sealed, sizeof sealed, 0x800);
    close(fd);
  } else {
    if((fd = open(bin, O_RDONLY)) < 0) {
      snprintf(err, elen, "cannot read the save key");
      goto out;
    }
    ret = (int)read(fd, sealed, sizeof sealed);
    close(fd);
  }
  if(ret != (int)sizeof sealed || decrypt_key(sealed, key)) {
    snprintf(err, elen, "cannot unseal the save key");
    goto out;
  }
  if(title_blocked(title, why, sizeof why, 1)) {
    snprintf(err, elen, "the game started meanwhile (%s)", why);
    goto out;
  }
  if(mkdir(PFS_ROOT, 0777) && errno != EEXIST) {
    snprintf(err, elen, "cannot create %s", PFS_ROOT);
    goto out;
  }
  mkdir(MOUNT_POINT, 0777);
  memset(&mopt, 0, sizeof mopt);
  sceFsInitMountSaveDataOpt(&mopt);
  mopt.budgetid = "system";
  if(sceFsMountSaveData(&mopt, TMP_IMG, MOUNT_POINT, key) < 0) {
    rmdir(MOUNT_POINT);
    snprintf(err, elen, "cannot mount the save image");
    goto out;
  }
  mounted = 1;

  /* 3. replace the content */
  job_step("writing the backup into the save");
  if(mount_clear() || copy_tree(backup_dir, MOUNT_POINT, &bytes)) {
    snprintf(err, elen, "cannot write the files into the save");
    goto out;
  }
  if(unmount_save()) {
    g_mount_stuck = 1;
    mounted = 0;
    snprintf(err, elen, "cannot unmount the save image; restart the payload");
    goto out;
  }
  mounted = 0;
  rmdir(MOUNT_POINT);

  /* 4. swap the image */
  job_step("replacing the save image");
  if(snprintf(newimg, sizeof newimg, "%s%s", image, NEW_IMAGE_SUFFIX) >= (int)sizeof newimg) {
    snprintf(err, elen, "path too long");
    goto out;
  }
  if(stat(TMP_IMG, &st) || file_copy_sync(TMP_IMG, newimg, &st)) {
    unlink(newimg);
    snprintf(err, elen, "cannot write the new save image");
    goto out;
  }
  chmod(newimg, ist.st_mode & 07777);
  if(chown(newimg, ist.st_uid, ist.st_gid)) {
    /* not fatal: the game checks the content, not the owner */
  }
#ifndef BACKUP_HOST_TEST
  chflags(newimg, ist.st_flags);
#endif
  if(title_blocked(title, why, sizeof why, 1)) {
    unlink(newimg);
    snprintf(err, elen, "the game started meanwhile (%s)", why);
    goto out;
  }
  if(rename(newimg, image)) {
    unlink(newimg);
    snprintf(err, elen, "cannot replace the save image (%s)", strerror(errno));
    goto out;
  }
  sync();

  /* 5. the next backup pass looks at this save again */
  snprintf(stamp, sizeof stamp, "%s/.image_stamp", backup_dir);
  unlink(stamp);
  format_size(bytes, a, sizeof a);
  pthread_mutex_lock(&job_mutex);
  snprintf(job.result, sizeof job.result, "%s save %s/%s restored (%s); previous save kept as %s",
           ps5 ? "PS5" : "PS4", title, save, a, undo + strlen(g_backup_root) + 1);
  pthread_mutex_unlock(&job_mutex);
  rc = 0;

out:
  if(mounted) {
    if(unmount_save()) g_mount_stuck = 1;
    else rmdir(MOUNT_POINT);
  }
  if(have_tmp) unlink(TMP_IMG);
  priv_restore(&old);
  return rc;
}


/* ------------------------------------------------------------------------
 * Listing for the page
 * ---------------------------------------------------------------------- */

/* Files and bytes below dir (bookkeeping files excluded). */
static void
dir_stats(const char *dir, unsigned *files, uint64_t *bytes, time_t *newest) {
  char path[PATH_MAX];
  struct dirent *dp;
  struct stat st;
  DIR *d;

  if(!(d = opendir(dir))) return;
  while((dp = readdir(d))) {
    if(!strcmp(dp->d_name, ".") || !strcmp(dp->d_name, "..") || restore_bookkeeping_name(dp->d_name)) continue;
    if(snprintf(path, sizeof path, "%s/%s", dir, dp->d_name) >= (int)sizeof path || lstat(path, &st)) continue;
    if(S_ISDIR(st.st_mode)) {
      dir_stats(path, files, bytes, newest);
    } else if(S_ISREG(st.st_mode)) {
      (*files)++;
      *bytes += (uint64_t)st.st_size;
      if(st.st_mtime > *newest) *newest = st.st_mtime;
    }
  }
  closedir(d);
}


/* JSON array of the saves in the backup, with slot and activity state. */
static void
list_saves_json(jw_t *w) {
  const char *ps[2] = {"ps4", "ps5"};
  int first = 1;

  jw_raw(w, "[");
  for(int p = 0; p < 2 && !w->err; p++) {
    char base[PATH_MAX];
    struct dirent *ud;
    DIR *users;

    snprintf(base, sizeof base, "%s/saves/%s", g_backup_root, ps[p]);
    if(!(users = opendir(base))) continue;
    while((ud = readdir(users))) {
      char udir[PATH_MAX];
      struct dirent *td;
      DIR *titles;
      if(!is_hex_name(ud->d_name)) continue;
      snprintf(udir, sizeof udir, "%s/%s", base, ud->d_name);
      if(!(titles = opendir(udir))) continue;
      while((td = readdir(titles))) {
        char tdir[PATH_MAX];
        struct dirent *sd;
        DIR *saves;
        if(!title_valid(td->d_name)) continue;
        snprintf(tdir, sizeof tdir, "%s/%s", udir, td->d_name);
        if(!(saves = opendir(tdir))) continue;
        while((sd = readdir(saves))) {
          char sdir[PATH_MAX], image[PATH_MAX], bin[PATH_MAX];
          struct stat st, ist;
          unsigned files = 0;
          uint64_t bytes = 0;
          time_t newest = 0, stamped = 0;
          if(!save_name_valid(sd->d_name)) continue;
          if(snprintf(sdir, sizeof sdir, "%s/%s", tdir, sd->d_name) >= (int)sizeof sdir ||
             stat(sdir, &st) || !S_ISDIR(st.st_mode)) continue;
          dir_stats(sdir, &files, &bytes, &newest);
          snprintf(image, sizeof image, "%s/.image_stamp", sdir);
          if(!stat(image, &st)) stamped = st.st_mtime;
          save_image_paths(p, ud->d_name, td->d_name, sd->d_name, image, sizeof image, bin, sizeof bin);
          jw_raw(w, first ? "{" : ",{");
          first = 0;
          jw_field_str(w, "ps", ps[p], 0);
          jw_field_str(w, "uid", ud->d_name, 1);
          jw_field_str(w, "title", td->d_name, 1);
          jw_field_str(w, "save", sd->d_name, 1);
          jw_field_num(w, "files", files, 1);
          jw_field_num(w, "bytes", (long long)bytes, 1);
          jw_field_num(w, "backed_up", (long long)(stamped ? stamped : newest), 1);
          jw_field_bool(w, "slot_exists", !stat(image, &ist) && S_ISREG(ist.st_mode), 1);
          jw_field_bool(w, "running", ui_title_active(td->d_name), 1);
          jw_raw(w, "}");
        }
        closedir(saves);
      }
      closedir(titles);
    }
    closedir(users);
  }
  jw_raw(w, "]");
}


/* JSON array of the configured paths and what the backup holds for them. */
static void
list_paths_json(jw_t *w) {
  settings_t *s = settings_new();
  char conf[PATH_MAX];
  int first = 1;

  jw_raw(w, "[");
  if(!s) {
    jw_raw(w, "]");
    return;
  }
  snprintf(conf, sizeof conf, "%s", g_conf_path);
  if(!conf[0] || settings_load(conf, s)) {
    settings_free(s);
    jw_raw(w, "]");
    return;
  }
  for(int i = 0; i < s->npaths && !w->err; i++) {
    char norm[PATH_MAX], src[PATH_MAX], why[160];
    struct stat st;
    unsigned files = 0;
    uint64_t bytes = 0;
    time_t newest = 0;
    size_t len = strlen(s->paths[i]);
    if(len && s->paths[i][len-1] == '*') s->paths[i][len-1] = 0;
    if(!path_normalize(s->paths[i], norm, sizeof norm) || !strcmp(norm, "/")) continue;
    if(snprintf(src, sizeof src, "%s%s", g_backup_root, norm) >= (int)sizeof src) continue;
    jw_raw(w, first ? "{" : ",{");
    first = 0;
    jw_field_str(w, "path", norm, 0);
    if(lstat(src, &st)) {
      jw_field_bool(w, "in_backup", 0, 1);
    } else {
      jw_field_bool(w, "in_backup", 1, 1);
      if(S_ISDIR(st.st_mode)) dir_stats(src, &files, &bytes, &newest);
      else {
        files = 1;
        bytes = (uint64_t)st.st_size;
        newest = st.st_mtime;
      }
      jw_field_bool(w, "folder", S_ISDIR(st.st_mode), 1);
      jw_field_num(w, "files", files, 1);
      jw_field_num(w, "bytes", (long long)bytes, 1);
      jw_field_num(w, "backed_up", (long long)newest, 1);
      jw_field_bool(w, "running", path_title_running(norm, why, sizeof why, 0), 1);
    }
    jw_raw(w, "}");
  }
  jw_raw(w, "]");
  settings_free(s);
}


/* ------------------------------------------------------------------------
 * Execution (main thread)
 * ---------------------------------------------------------------------- */

static void
job_run(void) {
  job_kind_t kind;
  char path[PATH_MAX], uid[64], title[16], save[256], err[256] = "";
  int ps5, rc;

  pthread_mutex_lock(&job_mutex);
  if(!job.queued) {
    pthread_mutex_unlock(&job_mutex);
    return;
  }
  job.queued = 0;
  job.running = 1;
  kind = job.kind;
  snprintf(path, sizeof path, "%s", job.path);
  ps5 = job.ps5;
  snprintf(uid, sizeof uid, "%s", job.uid);
  snprintf(title, sizeof title, "%s", job.title);
  snprintf(save, sizeof save, "%s", job.save);
  pthread_mutex_unlock(&job_mutex);

  logq("Restore: %s", job.last_label);
  ui_set_state("restoring", "");
  if(!g_backup_root[0] || (kind != JOB_GDRIVE_DOWNLOAD && !usb_online())) {
    snprintf(err, sizeof err, "the backup destination is not available");
    rc = -1;
  } else switch(kind) {
  case JOB_RESTORE_PATH:
    rc = restore_path(path, err, sizeof err);
    break;
  case JOB_RESTORE_SAVE:
    rc = restore_save(ps5, uid, title, save, err, sizeof err);
    break;
  case JOB_GDRIVE_DOWNLOAD:
    rc = gd_download(g_backup_root, g_gdrive_folder, err, sizeof err);
    if(rc == 0) {
      pthread_mutex_lock(&job_mutex);
      snprintf(job.result, sizeof job.result, "Google Drive: %s", err);
      pthread_mutex_unlock(&job_mutex);
    }
    break;
  default:
    rc = -1;
    snprintf(err, sizeof err, "unknown job");
  }
  pthread_mutex_lock(&job_mutex);
  job.running = 0;
  job.ok = rc == 0;
  job.finished = wall_clock();
  job.step[0] = 0;
  snprintf(job.error, sizeof job.error, "%s", rc ? err : "");
  if(rc == 0 && !job.result[0]) snprintf(job.result, sizeof job.result, "done");
  pthread_mutex_unlock(&job_mutex);
  if(rc) {
    logq("Restore failed: %s", err);
    notify("Restore failed: %s", err);
  } else {
    logq("Restore done: %s", job.result);
    notify("%s", job.result);
  }
}


static void
job_status_json(jw_t *w) {
  time_t now = wall_clock();

  pthread_mutex_lock(&job_mutex);
  jw_raw(w, "{");
  jw_field_bool(w, "busy", job.queued || job.running, 0);
  jw_field_str(w, "job", job.queued || job.running ? job.last_label : "", 1);
  jw_field_str(w, "step", job.running ? job.step : "", 1);
  jw_field_str(w, "last", job.finished ? job.last_label : "", 1);
  jw_field_bool(w, "last_ok", job.ok, 1);
  jw_field_str(w, "result", job.finished && !job.running ? job.result : "", 1);
  jw_field_str(w, "error", job.finished && !job.running ? job.error : "", 1);
  jw_field_num(w, "finished_age", job.finished ? (long long)(now - job.finished) : -1, 1);
  jw_raw(w, "}");
  pthread_mutex_unlock(&job_mutex);
}

#endif
