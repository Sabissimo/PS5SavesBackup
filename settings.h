/* Settings file model for the settings page.
 *
 * The backup daemon keeps reading backup.conf as plain text (backup_entry);
 * this module only loads the file into a struct for the page, validates
 * form input and writes the file back atomically. Comments in a hand-written
 * file are not preserved; the written file documents every key itself. */
#ifndef SETTINGS_H
#define SETTINGS_H

#ifndef DATA_DIR
#define DATA_DIR  "/data/homebrew/PS5SavesBackup"
#endif
#define DATA_CONF DATA_DIR "/" CONF_NAME
#define MAX_CONF_PATHS 256

typedef struct settings {
  long interval;
  int  ps4;
  int  ps5;
  long max_mb;
  int  daily;
  char usb_id[USB_ID_LEN + 1];
  int  gdrive;
  char gdrive_folder[256];
  int  npaths;
  char (*paths)[PATH_MAX];
} settings_t;


static void
settings_defaults(settings_t *s) {
  s->interval = DEF_INTERVAL;
  s->ps4 = 0;
  s->ps5 = 0;
  s->max_mb = DEF_GAME_MB;
  s->daily = 0;
  s->usb_id[0] = 0;
  s->gdrive = 0;
  snprintf(s->gdrive_folder, sizeof s->gdrive_folder, "%s", GDRIVE_DEFAULT_FOLDER);
  s->npaths = 0;
}


static settings_t *
settings_new(void) {
  settings_t *s = calloc(1, sizeof *s);
  if(!s) {
    return NULL;
  }
  s->paths = calloc(MAX_CONF_PATHS, sizeof *s->paths);
  if(!s->paths) {
    free(s);
    return NULL;
  }
  settings_defaults(s);
  return s;
}


static void
settings_free(settings_t *s) {
  if(s) {
    free(s->paths);
    free(s);
  }
}


static void
settings_add_path(settings_t *s, const char *path) {
  if(s->npaths == MAX_CONF_PATHS) {
    logq("More than %d backup paths; the rest are ignored", MAX_CONF_PATHS);
    return;
  }
  snprintf(s->paths[s->npaths++], PATH_MAX, "%s", path);
}


/* One backup.conf line, same keys and ranges as backup_entry(). Unknown
 * keys are ignored, invalid values keep the previous value. */
static void
settings_line(settings_t *s, char *line) {
  const char *value;
  size_t len;
  long v;

  while(*line == ' ' || *line == '\t') line++;
  len = strlen(line);
  while(len > 0 && (line[len-1] == '\n' || line[len-1] == '\r' ||
                    line[len-1] == ' '  || line[len-1] == '\t')) {
    line[--len] = 0;
  }
  if(len == 0 || line[0] == '#') {
    return;
  }
  if((value = config_value(line, "backup_interval_seconds", "interval"))) {
    if(config_number(value, MIN_INTERVAL, MAX_INTERVAL, &v)) s->interval = v;
  } else if((value = config_value(line, "max_title_size_mb", "max_game_mb"))) {
    if(config_number(value, MIN_GAME_MB, MAX_GAME_MB, &v)) s->max_mb = v;
  } else if((value = config_value(line, "daily_snapshots_to_keep", "dailybackups"))) {
    if(config_number(value, 0, MAX_DAILY, &v)) s->daily = (int)v;
  } else if((value = config_value(line, "backup_ps4_saves", "ps4saves"))) {
    parse_bool(value, &s->ps4);
  } else if((value = config_value(line, "backup_ps5_saves", "ps5saves"))) {
    parse_bool(value, &s->ps5);
  } else if((value = config_value(line, "usb_id", "usb_id"))) {
    if(usb_id_valid(value)) snprintf(s->usb_id, sizeof s->usb_id, "%s", value);
  } else if((value = config_value(line, "gdrive_enabled", "gdrive_enabled"))) {
    parse_bool(value, &s->gdrive);
  } else if((value = config_value(line, "gdrive_folder", "gdrive_folder"))) {
    if(gdrive_folder_valid(value)) snprintf(s->gdrive_folder, sizeof s->gdrive_folder, "%s", value);
  } else if(line[0] == '/') {
    settings_add_path(s, line);
  }
}


/* Returns 0 when the file was read, -1 when it cannot be opened. */
static int
settings_load(const char *path, settings_t *s) {
  char line[PATH_MAX + 16];
  FILE *f;

  settings_defaults(s);
  if(!(f = fopen(path, "r"))) {
    return -1;
  }
  while(fgets(line, sizeof line, f)) {
    settings_line(s, line);
  }
  fclose(f);
  return 0;
}


/* Written to a .part file first, then renamed, so the daemon never reads a
 * half-written settings file. Returns 0 on success. */
static int
settings_save(const char *path, const settings_t *s) {
  char dir[PATH_MAX], part[PATH_MAX];
  char *slash;
  FILE *f;
  int ok;

  snprintf(dir, sizeof dir, "%s", path);
  if((slash = strrchr(dir, '/')) && slash != dir) {
    *slash = 0;
    if(mkdir_p(dir)) {
      return -1;
    }
  }
  if(snprintf(part, sizeof part, "%s.part", path) >= (int)sizeof part) {
    return -1;
  }
  if(!(f = fopen(part, "w"))) {
    return -1;
  }
  ok = fprintf(f,
    "# PS5SavesBackup settings. Edit on the settings page or by hand.\n"
    "\n"
    "# Delay between backup passes in seconds (int, min %d max %d)\n"
    "backup_interval_seconds=%ld\n"
    "\n"
    "# Enable decrypted PS4 save backups (false, true)\n"
    "backup_ps4_saves=%s\n"
    "\n"
    "# Enable decrypted PS5 save backups (false, true)\n"
    "backup_ps5_saves=%s\n"
    "\n"
    "# Maximum combined save-image size per title in MB (int)\n"
    "max_title_size_mb=%ld\n"
    "\n"
    "# Number of dated snapshots retained, values 0 thru %d are valid; 0 means disabled (int)\n"
    "daily_snapshots_to_keep=%d\n"
    "\n"
    "# Id of the backup USB partition (content of its %s file); pick it on the settings page\n"
    "usb_id=%s\n"
    "\n"
    "# Upload the backup to Google Drive (false, true); sign in on the settings page\n"
    "gdrive_enabled=%s\n"
    "\n"
    "# Name of the folder in My Drive that receives the backup\n"
    "gdrive_folder=%s\n"
    "\n"
    "# Files and folders to back up, one absolute path per line\n",
    MIN_INTERVAL, MAX_INTERVAL, s->interval,
    s->ps4 ? "true" : "false", s->ps5 ? "true" : "false",
    s->max_mb, MAX_DAILY, s->daily, MARKER_NAME, s->usb_id,
    s->gdrive ? "true" : "false", s->gdrive_folder) > 0;
  for(int i = 0; i < s->npaths && ok; i++) {
    ok = fprintf(f, "%s\n", s->paths[i]) > 0;
  }
  if(fflush(f) || fsync(fileno(f))) ok = 0;
  if(fclose(f)) ok = 0;
  if(!ok || rename(part, path)) {
    unlink(part);
    return -1;
  }
  return 0;
}


/* ------------------------------------------------------------------------
 * application/x-www-form-urlencoded input from the settings page
 * ---------------------------------------------------------------------- */

static int
hex_value(int c) {
  if(c >= '0' && c <= '9') return c - '0';
  if(c >= 'a' && c <= 'f') return c - 'a' + 10;
  if(c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}


/* Percent-decodes src[0..n) into out; '+' becomes a space. */
static void
form_decode(const char *src, size_t n, char *out, size_t len) {
  size_t o = 0;

  for(size_t i = 0; i < n && o + 1 < len; i++) {
    int c = (unsigned char)src[i];
    if(c == '+') {
      c = ' ';
    } else if(c == '%' && i + 2 < n && hex_value(src[i+1]) >= 0 && hex_value(src[i+2]) >= 0) {
      c = hex_value(src[i+1]) * 16 + hex_value(src[i+2]);
      i += 2;
    }
    out[o++] = (char)c;
  }
  out[o] = 0;
}


/* Value of key in a form body; returns 1 if the key is present. */
static int
form_get(const char *body, const char *key, char *out, size_t len) {
  size_t klen = strlen(key);
  const char *p = body;

  out[0] = 0;
  while(*p) {
    const char *end = strchr(p, '&');
    size_t plen = end ? (size_t)(end - p) : strlen(p);
    if(plen > klen && !strncmp(p, key, klen) && p[klen] == '=') {
      form_decode(p + klen + 1, plen - klen - 1, out, len);
      return 1;
    }
    if(plen == klen && !strncmp(p, key, klen)) {
      return 1;
    }
    if(!end) break;
    p = end + 1;
  }
  return 0;
}


static int
form_number(const char *body, const char *key, long min, long max, long *out,
            char *err, size_t len) {
  char value[64];
  char *end;
  long v;

  if(!form_get(body, key, value, sizeof value) || !value[0]) {
    snprintf(err, len, "%s is missing", key);
    return 0;
  }
  errno = 0;
  v = strtol(value, &end, 10);
  if(errno || end == value || *end) {
    snprintf(err, len, "%s is not a number", key);
    return 0;
  }
  if(v < min || v > max) {
    snprintf(err, len, "%s must be between %ld and %ld", key, min, max);
    return 0;
  }
  *out = v;
  return 1;
}


/* A checkbox is present when checked; "false"/"0"/"off" also mean off. */
static int
form_flag(const char *body, const char *key) {
  char value[16];

  if(!form_get(body, key, value, sizeof value)) {
    return 0;
  }
  return strcmp(value, "false") && strcmp(value, "0") && strcmp(value, "off");
}


/**
 * Fills s from the settings form. usb_id is not part of the form and is
 * left untouched. Returns 1 on success, 0 with a message in err.
 **/
static int
settings_from_form(const char *body, settings_t *s, char *err, size_t len) {
  char *paths;
  size_t plen = strlen(body) + 1;
  long v;

  if(!form_number(body, "interval", MIN_INTERVAL, MAX_INTERVAL, &v, err, len)) return 0;
  s->interval = v;
  if(!form_number(body, "max_mb", MIN_GAME_MB, MAX_GAME_MB, &v, err, len)) return 0;
  s->max_mb = v;
  if(!form_number(body, "daily", 0, MAX_DAILY, &v, err, len)) return 0;
  s->daily = (int)v;
  s->ps4 = form_flag(body, "ps4");
  s->ps5 = form_flag(body, "ps5");
  s->gdrive = form_flag(body, "gdrive");
  if(form_get(body, "gdrive_folder", s->gdrive_folder, sizeof s->gdrive_folder)) {
    char *p = s->gdrive_folder;
    size_t l;
    while(*p == ' ') p++;
    memmove(s->gdrive_folder, p, strlen(p) + 1);
    l = strlen(s->gdrive_folder);
    while(l && s->gdrive_folder[l-1] == ' ') s->gdrive_folder[--l] = 0;
    if(!l) snprintf(s->gdrive_folder, sizeof s->gdrive_folder, "%s", GDRIVE_DEFAULT_FOLDER);
    if(!gdrive_folder_valid(s->gdrive_folder)) {
      snprintf(err, len, "Google Drive folder name must not contain / or \\");
      return 0;
    }
  }

  if(!(paths = malloc(plen))) {
    snprintf(err, len, "not enough memory");
    return 0;
  }
  form_get(body, "paths", paths, plen);
  s->npaths = 0;
  for(char *line = paths, *next; line; line = next) {
    size_t l;
    next = strchr(line, '\n');
    if(next) *next++ = 0;
    while(*line == ' ' || *line == '\t') line++;
    l = strlen(line);
    while(l > 0 && (line[l-1] == '\r' || line[l-1] == ' ' || line[l-1] == '\t')) line[--l] = 0;
    if(!l || line[0] == '#') continue;
    if(line[0] != '/') {
      snprintf(err, len, "paths must be absolute: %.64s", line);
      free(paths);
      return 0;
    }
    if(l >= PATH_MAX) {
      snprintf(err, len, "path too long: %.64s...", line);
      free(paths);
      return 0;
    }
    if(s->npaths == MAX_CONF_PATHS) {
      snprintf(err, len, "at most %d paths", MAX_CONF_PATHS);
      free(paths);
      return 0;
    }
    settings_add_path(s, line);
  }
  free(paths);
  return 1;
}

#endif
