/* Settings page: a small HTTP server on WEB_PORT, served by its own thread.
 *
 * The page (web/index.html, embedded as web_page.h) is opened in the console
 * browser or from any device on the network. It reads and writes the
 * settings file through a few JSON endpoints, lets the user pick the backup
 * USB partition (which writes the marker file on it) and shows the daemon
 * state and the recent log. There is no authentication, like the other PS5
 * homebrew services on the local network. */
#ifndef WEB_UI_H
#define WEB_UI_H

#include "web_page.h"

#ifndef WEB_PORT
#define WEB_PORT      9056
#endif
#define WEB_MAX_HEAD  (16 * 1024)
#define WEB_MAX_BODY  (256 * 1024)
#define WEB_TIMEOUT_MS 5000
#define UI_LOG_LINES  200
#define UI_LOG_WIDTH  256

static pthread_mutex_t ui_mutex = PTHREAD_MUTEX_INITIALIZER;
static char ui_log[UI_LOG_LINES][UI_LOG_WIDTH];
static unsigned ui_log_next, ui_log_count;
static struct {
  char state[32];
  char reason[192];
  char result[256];
  char usb_root[PATH_MAX];
  char conf_path[PATH_MAX];
  char usb_id[USB_ID_LEN + 1];
  int legacy;
  int usb_lost;
  int dest_internal;
  char backup_root[PATH_MAX];
  time_t result_at;
  double next_due;          /* monotonic, 0 = unknown */
} ui;


/* ------------------------------------------------------------------------
 * State shared with the main thread
 * ---------------------------------------------------------------------- */

static void
ui_log_push(const char *msg) {
  pthread_mutex_lock(&ui_mutex);
  snprintf(ui_log[ui_log_next], UI_LOG_WIDTH, "%s", msg);
  ui_log_next = (ui_log_next + 1) % UI_LOG_LINES;
  if(ui_log_count < UI_LOG_LINES) ui_log_count++;
  pthread_mutex_unlock(&ui_mutex);
}


static void
ui_set_state(const char *state, const char *reason) {
  pthread_mutex_lock(&ui_mutex);
  snprintf(ui.state, sizeof ui.state, "%s", state);
  snprintf(ui.reason, sizeof ui.reason, "%s", reason ? reason : "");
  pthread_mutex_unlock(&ui_mutex);
}


static void
ui_set_result(const char *fmt, ...) {
  va_list ap;

  pthread_mutex_lock(&ui_mutex);
  va_start(ap, fmt);
  vsnprintf(ui.result, sizeof ui.result, fmt, ap);
  va_end(ap);
  ui.result_at = wall_clock();
  pthread_mutex_unlock(&ui_mutex);
}


static void
ui_set_due(double due) {
  pthread_mutex_lock(&ui_mutex);
  ui.next_due = due;
  pthread_mutex_unlock(&ui_mutex);
}


/* Snapshot of the running titles for the page (the real check stays in the
 * main thread; title_activity.h is not thread safe). */
static char ui_active[MAX_ACTIVE_TITLES][10];
static size_t ui_active_count;
static int ui_active_unknown;

static void
ui_sync_activity(void) {
  activity_refresh(0);
  pthread_mutex_lock(&ui_mutex);
  ui_active_count = activity.count;
  ui_active_unknown = activity.unknown;
  memcpy(ui_active, activity.titles, sizeof ui_active);
  pthread_mutex_unlock(&ui_mutex);
}


static int
ui_title_active(const char *title) {
  int active;

  pthread_mutex_lock(&ui_mutex);
  active = ui_active_unknown;
  for(size_t i = 0; i < ui_active_count && !active; i++)
    active = !strcmp(ui_active[i], title);
  pthread_mutex_unlock(&ui_mutex);
  return active;
}


/* Copies the main thread's selection into the shared snapshot. */
static void
ui_sync(void) {
  pthread_mutex_lock(&ui_mutex);
  snprintf(ui.usb_root, sizeof ui.usb_root, "%s", g_usb_root);
  snprintf(ui.conf_path, sizeof ui.conf_path, "%s", g_conf_path);
  snprintf(ui.usb_id, sizeof ui.usb_id, "%s", g_usb_id);
  ui.legacy = g_legacy;
  ui.usb_lost = g_usb_lost;
  ui.dest_internal = g_dest_internal;
  snprintf(ui.backup_root, sizeof ui.backup_root, "%s", g_backup_root);
  pthread_mutex_unlock(&ui_mutex);
}


/* Asks the main loop to re-read the settings and run a pass now. */
static void
ui_wake(void) {
  if(g_wake_pipe[1] >= 0) {
    ssize_t n = write(g_wake_pipe[1], "w", 1);
    (void)n;
  }
}


/* ------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

typedef struct json_writer {
  char *buf;
  size_t len;
  size_t pos;
  int err;
} jw_t;


static void
jw_raw(jw_t *w, const char *fmt, ...) {
  va_list ap;
  int n;

  if(w->err) return;
  va_start(ap, fmt);
  n = vsnprintf(w->buf + w->pos, w->len - w->pos, fmt, ap);
  va_end(ap);
  if(n < 0 || (size_t)n >= w->len - w->pos) {
    w->err = 1;
    return;
  }
  w->pos += (size_t)n;
}


/* A JSON string literal, escaped. */
static void
jw_str(jw_t *w, const char *s) {
  size_t cap = strlen(s) * 6 + 1;
  char *esc = malloc(cap);

  if(!esc || !notification_json_escape(esc, cap, s)) {
    free(esc);
    w->err = 1;
    return;
  }
  jw_raw(w, "\"%s\"", esc);
  free(esc);
}


static void
jw_field_str(jw_t *w, const char *key, const char *value, int comma) {
  jw_raw(w, "%s\"%s\":", comma ? "," : "", key);
  jw_str(w, value);
}


static void
jw_field_num(jw_t *w, const char *key, long long value, int comma) {
  jw_raw(w, "%s\"%s\":%lld", comma ? "," : "", key, value);
}


static void
jw_field_bool(jw_t *w, const char *key, int value, int comma) {
  jw_raw(w, "%s\"%s\":%s", comma ? "," : "", key, value ? "true" : "false");
}


static uint64_t
fs_total(const char *path) {
  struct statfs sf;

  if(statfs(path, &sf)) {
    return 0;
  }
  return (uint64_t)sf.f_blocks * (uint64_t)sf.f_bsize;
}


/* The console's own address on the LAN, found by routing a UDP socket (no
 * packet is sent). */
static void
local_ip(char *out, size_t len) {
  struct sockaddr_in to, me;
  socklen_t mlen = sizeof me;
  int s;

  snprintf(out, len, "ps5");
  if((s = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
    return;
  }
  memset(&to, 0, sizeof to);
  to.sin_family = AF_INET;
  to.sin_port = htons(53);
  to.sin_addr.s_addr = htonl(0x08080808);
  if(!connect(s, (struct sockaddr *)&to, sizeof to) &&
     !getsockname(s, (struct sockaddr *)&me, &mlen)) {
    inet_ntop(AF_INET, &me.sin_addr, out, (socklen_t)len);
  }
  close(s);
}


static void
web_url(char *out, size_t len) {
  char ip[64];

  local_ip(ip, sizeof ip);
  snprintf(out, len, "http://%s:%d/", ip, WEB_PORT);
}


/* 32 hex characters from /dev/urandom, mixed with time and pid as a fallback. */
static void
usb_new_id(char *out) {
  static const char hex[] = "0123456789abcdef";
  unsigned char raw[USB_ID_LEN / 2];
  int fd = open("/dev/urandom", O_RDONLY);
  ssize_t n = fd >= 0 ? read(fd, raw, sizeof raw) : -1;

  if(fd >= 0) close(fd);
  if(n != (ssize_t)sizeof raw) {
    uint64_t seed = (uint64_t)time(NULL) ^ ((uint64_t)getpid() << 32) ^
                    (uint64_t)(now_seconds() * 1e9);
    for(size_t i = 0; i < sizeof raw; i++) {
      seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
      raw[i] = (unsigned char)(seed >> 56);
    }
  }
  for(size_t i = 0; i < sizeof raw; i++) {
    out[2*i] = hex[raw[i] >> 4];
    out[2*i+1] = hex[raw[i] & 15];
  }
  out[USB_ID_LEN] = 0;
}


/* Makes root ours: keeps an existing valid marker, otherwise writes a new
 * one. This is the only write to a USB the user has not selected before,
 * and it happens only on the user's explicit request. */
static int
usb_mark(const char *root, char *id_out) {
  char path[PATH_MAX], part[PATH_MAX], content[128];
  FILE *f;
  int ok;

  if(usb_read_small(root, MARKER_NAME, content, sizeof content) && usb_id_valid(content)) {
    snprintf(id_out, USB_ID_LEN + 1, "%s", content);
    return 0;
  }
  usb_new_id(id_out);
  if(snprintf(path, sizeof path, "%s/%s", root, MARKER_NAME) >= (int)sizeof path ||
     snprintf(part, sizeof part, "%s.part", path) >= (int)sizeof part) {
    return -1;
  }
  if(!(f = fopen(part, "w"))) {
    return -1;
  }
  ok = fprintf(f, "%s\n", id_out) > 0;
  if(fflush(f) || fsync(fileno(f))) ok = 0;
  if(fclose(f)) ok = 0;
  if(!ok || rename(part, path)) {
    unlink(part);
    return -1;
  }
  logq("Marked %s as the backup USB (%s)", root, MARKER_NAME);
  return 0;
}


/* Restore jobs and listings (restore.h, included after this file). */
typedef enum { JOB_NONE, JOB_RESTORE_PATH, JOB_RESTORE_SAVE, JOB_GDRIVE_DOWNLOAD } job_kind_t;
static int job_submit(job_kind_t kind, const char *path, int ps5, const char *uid,
                      const char *title, const char *save, const char *source);
static void job_status_json(jw_t *w);
static int restore_source_valid(const char *source);
static void list_saves_json(jw_t *w);
static void list_paths_json(jw_t *w);


/* ------------------------------------------------------------------------
 * HTTP
 * ---------------------------------------------------------------------- */

static int
web_write_all(int c, const void *data, size_t len) {
  const char *p = data;

  while(len) {
    ssize_t n = send(c, p, len, 0);
    if(n < 0) {
      if(errno == EINTR) continue;
      return -1;
    }
    p += n;
    len -= (size_t)n;
  }
  return 0;
}


static int
web_send(int c, const char *status, const char *type, const void *body, size_t len) {
  char head[256];
  int n = snprintf(head, sizeof head,
                   "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                   "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                   status, type, len);
  if(n < 0 || n >= (int)sizeof head) {
    return -1;
  }
  if(web_write_all(c, head, (size_t)n)) {
    return -1;
  }
  return web_write_all(c, body, len);
}


static int
web_json(int c, const char *status, jw_t *w) {
  if(w->err) {
    return web_send(c, "500 Internal Server Error", "application/json",
                    "{\"error\":\"response too large\"}", 30);
  }
  return web_send(c, status, "application/json", w->buf, w->pos);
}


static void
web_message(int c, const char *status, const char *key, const char *msg) {
  char buf[1024];
  jw_t w = {buf, sizeof buf, 0, 0};

  jw_raw(&w, "{");
  jw_field_str(&w, key, msg, 0);
  jw_raw(&w, "}");
  web_json(c, status, &w);
}


/* Reads one request. Returns 0 with method, path, body (NUL terminated,
 * caller frees), or -1 after answering with an error. */
static int
web_read(int c, char *method, size_t mlen, char *path, size_t plen,
         char **body_out, size_t *blen_out) {
  char *buf = malloc(WEB_MAX_HEAD + 1);
  char *end = NULL, *line, *p, *body = NULL;
  size_t got = 0, hlen, blen = 0, have;

  *body_out = NULL;
  *blen_out = 0;
  if(!buf) {
    web_message(c, "500 Internal Server Error", "error", "not enough memory");
    return -1;
  }
  while(!end) {
    struct pollfd pfd = {c, POLLIN, 0};
    ssize_t n;
    if(got == WEB_MAX_HEAD) {
      web_message(c, "431 Request Header Fields Too Large", "error", "request too large");
      free(buf);
      return -1;
    }
    if(poll(&pfd, 1, WEB_TIMEOUT_MS) <= 0) {
      free(buf);
      return -1;
    }
    n = recv(c, buf + got, WEB_MAX_HEAD - got, 0);
    if(n <= 0) {
      free(buf);
      return -1;
    }
    got += (size_t)n;
    buf[got] = 0;
    end = strstr(buf, "\r\n\r\n");
  }
  hlen = (size_t)(end + 4 - buf);

  /* request line */
  p = strstr(buf, "\r\n");
  *p = 0;
  if(sscanf(buf, "%15s %1023s", method, path) != 2) {
    web_message(c, "400 Bad Request", "error", "malformed request");
    free(buf);
    return -1;
  }
  (void)mlen; (void)plen;
  line = p + 2;
  if((p = strchr(path, '?'))) *p = 0;

  /* headers: only Content-Length matters */
  while(line < end) {
    char *next = strstr(line, "\r\n");
    if(!next) break;
    *next = 0;
    if(!strncasecmp(line, "Content-Length:", 15)) {
      blen = strtoul(line + 15, NULL, 10);
    }
    line = next + 2;
  }
  if(blen > WEB_MAX_BODY) {
    web_message(c, "413 Content Too Large", "error", "request body too large");
    free(buf);
    return -1;
  }

  if(!(body = malloc(blen + 1))) {
    web_message(c, "500 Internal Server Error", "error", "not enough memory");
    free(buf);
    return -1;
  }
  have = got - hlen;
  if(have > blen) have = blen;
  memcpy(body, buf + hlen, have);
  while(have < blen) {
    struct pollfd pfd = {c, POLLIN, 0};
    ssize_t n;
    if(poll(&pfd, 1, WEB_TIMEOUT_MS) <= 0) {
      free(buf);
      free(body);
      return -1;
    }
    n = recv(c, body + have, blen - have, 0);
    if(n <= 0) {
      free(buf);
      free(body);
      return -1;
    }
    have += (size_t)n;
  }
  body[blen] = 0;
  free(buf);
  *body_out = body;
  *blen_out = blen;
  return 0;
}


/* ------------------------------------------------------------------------
 * API
 * ---------------------------------------------------------------------- */

static void
api_status(int c) {
  char *buf = malloc(16384);
  jw_t w = {buf, 16384, 0, 0};
  char url[128];
  struct stat st;
  time_t now = wall_clock();
  double mono = now_seconds();

  if(!buf) {
    web_message(c, "500 Internal Server Error", "error", "not enough memory");
    return;
  }
  web_url(url, sizeof url);
  pthread_mutex_lock(&ui_mutex);
  jw_raw(&w, "{");
  jw_field_str(&w, "app", APP_NAME, 0);
  jw_field_str(&w, "version", APP_VERSION, 1);
  jw_field_str(&w, "state", ui.state, 1);
  jw_field_str(&w, "reason", ui.reason, 1);
  jw_field_str(&w, "result", ui.result, 1);
  jw_field_num(&w, "result_age", ui.result_at ? (long long)(now - ui.result_at) : -1, 1);
  jw_field_num(&w, "next_in", ui.next_due > mono ? (long long)(ui.next_due - mono) : -1, 1);
  jw_field_str(&w, "usb_root", ui.usb_root, 1);
  jw_field_str(&w, "usb_id", ui.usb_id, 1);
  jw_field_bool(&w, "usb_lost", ui.usb_lost, 1);
  jw_field_bool(&w, "dest_internal", ui.dest_internal, 1);
  jw_field_str(&w, "backup_root", ui.backup_root, 1);
  jw_field_str(&w, "conf_path", ui.conf_path, 1);
  jw_field_bool(&w, "legacy", ui.legacy, 1);
  {
    const char *space = ui.dest_internal ? ui.backup_root : ui.usb_root;
    int have = space[0] && !ui.usb_lost;
    jw_field_num(&w, "usb_free", have ? (long long)fs_free(space, "TEST_USB_FREE") : 0, 1);
    jw_field_num(&w, "usb_total", have ? (long long)fs_total(space) : 0, 1);
  }
  pthread_mutex_unlock(&ui_mutex);
  jw_field_bool(&w, "settings_file", !stat(DATA_CONF, &st) && S_ISREG(st.st_mode), 1);
  jw_field_str(&w, "settings_path", DATA_CONF, 1);
  jw_field_str(&w, "url", url, 1);
  jw_raw(&w, "}");
  web_json(c, "200 OK", &w);
  free(buf);
}


static void
api_config_get(int c) {
  settings_t *s = settings_new();
  char conf[PATH_MAX];
  char *buf = malloc(WEB_MAX_BODY);
  jw_t w = {buf, WEB_MAX_BODY, 0, 0};
  int found;

  if(!s || !buf) {
    settings_free(s);
    free(buf);
    web_message(c, "500 Internal Server Error", "error", "not enough memory");
    return;
  }
  /* the file in use: the internal one, or the legacy USB file to migrate */
  pthread_mutex_lock(&ui_mutex);
  snprintf(conf, sizeof conf, "%s", ui.conf_path);
  pthread_mutex_unlock(&ui_mutex);
  found = !settings_load(DATA_CONF, s);
  if(!found && conf[0]) {
    found = !settings_load(conf, s);
  }
  jw_raw(&w, "{");
  jw_field_bool(&w, "found", found, 0);
  jw_field_num(&w, "interval", s->interval, 1);
  jw_field_bool(&w, "ps4", s->ps4, 1);
  jw_field_bool(&w, "ps5", s->ps5, 1);
  jw_field_num(&w, "max_mb", s->max_mb, 1);
  jw_field_num(&w, "daily", s->daily, 1);
  jw_field_num(&w, "history_days", s->history_days, 1);
  jw_field_str(&w, "usb_id", s->usb_id, 1);
  jw_field_bool(&w, "gdrive", s->gdrive, 1);
  jw_field_str(&w, "gdrive_folder", s->gdrive_folder, 1);
  jw_raw(&w, ",\"paths\":[");
  for(int i = 0; i < s->npaths; i++) {
    if(i) jw_raw(&w, ",");
    jw_str(&w, s->paths[i]);
  }
  jw_raw(&w, "]}");
  web_json(c, "200 OK", &w);
  settings_free(s);
  free(buf);
}


/* Saves s as the internal settings file. usb_id is kept from the existing
 * file; when migrating from a legacy USB backup.conf, that USB is marked. */
static int
settings_commit(settings_t *s, char *err, size_t len) {
  settings_t *cur = settings_new();
  char usb_root[PATH_MAX];
  int legacy;

  if(!cur) {
    snprintf(err, len, "not enough memory");
    return -1;
  }
  pthread_mutex_lock(&ui_mutex);
  snprintf(usb_root, sizeof usb_root, "%s", ui.usb_root);
  legacy = ui.legacy;
  pthread_mutex_unlock(&ui_mutex);

  if(!settings_load(DATA_CONF, cur)) {
    snprintf(s->usb_id, sizeof s->usb_id, "%s", cur->usb_id);
  } else if(legacy && usb_root[0]) {
    if(usb_mark(usb_root, s->usb_id)) {
      snprintf(err, len, "cannot write %s on %s", MARKER_NAME, usb_root);
      settings_free(cur);
      return -1;
    }
  }
  settings_free(cur);
  if(settings_save(DATA_CONF, s)) {
    snprintf(err, len, "cannot write %s (%s)", DATA_CONF, strerror(errno));
    return -1;
  }
  logq("Settings saved to %s", DATA_CONF);
  return 0;
}


static void
api_config_post(int c, const char *body) {
  settings_t *s = settings_new();
  char err[256];

  if(!s) {
    web_message(c, "500 Internal Server Error", "error", "not enough memory");
    return;
  }
  if(!settings_from_form(body, s, err, sizeof err) || settings_commit(s, err, sizeof err)) {
    web_message(c, "400 Bad Request", "error", err);
    settings_free(s);
    return;
  }
  settings_free(s);
  ui_wake();
  web_message(c, "200 OK", "ok", "Settings saved");
}


static void
api_usb(int c) {
  usb_mount_t mounts[MAX_USB_MOUNTS];
  struct stat parent, st;
  char selected[PATH_MAX];
  char *buf = malloc(WEB_MAX_BODY);
  jw_t w = {buf, WEB_MAX_BODY, 0, 0};
  int n;

  if(!buf) {
    web_message(c, "500 Internal Server Error", "error", "not enough memory");
    return;
  }
  pthread_mutex_lock(&ui_mutex);
  snprintf(selected, sizeof selected, "%s", ui.usb_root);
  pthread_mutex_unlock(&ui_mutex);
  if(stat(USB_MOUNT_DIR, &parent)) {
    memset(&parent, 0, sizeof parent);
  }
  n = usb_list_mounts(mounts, MAX_USB_MOUNTS);
  jw_raw(&w, "{\"mounts\":[");
  for(int i = 0; i < n; i++) {
    char root[PATH_MAX], marker[128], conf[128];
    int mounted;

    if(snprintf(root, sizeof root, "%s/%s", USB_MOUNT_DIR, mounts[i].name) >= (int)sizeof root) {
      continue;
    }
    mounted = usb_root_mounted(root, &parent, &st);
    if(i) jw_raw(&w, ",");
    jw_raw(&w, "{");
    jw_field_str(&w, "root", root, 0);
    jw_field_bool(&w, "mounted", mounted, 1);
    jw_field_bool(&w, "selected", mounted && !strcmp(root, selected), 1);
    if(mounted) {
      int has_marker = usb_read_small(root, MARKER_NAME, marker, sizeof marker) && usb_id_valid(marker);
      jw_field_str(&w, "marker", has_marker ? marker : "", 1);
      jw_field_bool(&w, "legacy_conf", usb_read_small(root, CONF_NAME, conf, sizeof conf), 1);
      jw_field_num(&w, "free", (long long)fs_free(root, "TEST_USB_FREE"), 1);
      jw_field_num(&w, "total", (long long)fs_total(root), 1);
    }
    jw_raw(&w, "}");
  }
  jw_raw(&w, "]}");
  web_json(c, "200 OK", &w);
  free(buf);
}


static void
api_usb_select(int c, const char *body) {
  usb_mount_t mounts[MAX_USB_MOUNTS];
  struct stat parent, st;
  char root[PATH_MAX], conf[PATH_MAX], err[256];
  settings_t *s;
  int n, known = 0, legacy;

  if(!form_get(body, "root", root, sizeof root) || !root[0]) {
    web_message(c, "400 Bad Request", "error", "root is missing");
    return;
  }
  /* only a mount point from the current listing can be chosen */
  if(stat(USB_MOUNT_DIR, &parent)) {
    memset(&parent, 0, sizeof parent);
  }
  n = usb_list_mounts(mounts, MAX_USB_MOUNTS);
  for(int i = 0; i < n && !known; i++) {
    char cand[PATH_MAX];
    if(snprintf(cand, sizeof cand, "%s/%s", USB_MOUNT_DIR, mounts[i].name) < (int)sizeof cand)
      known = !strcmp(cand, root);
  }
  if(!known || !usb_root_mounted(root, &parent, &st)) {
    web_message(c, "400 Bad Request", "error", "not a mounted USB partition");
    return;
  }
  if(!(s = settings_new())) {
    web_message(c, "500 Internal Server Error", "error", "not enough memory");
    return;
  }
  /* keep the settings in use: internal file, or the legacy USB file */
  pthread_mutex_lock(&ui_mutex);
  snprintf(conf, sizeof conf, "%s", ui.conf_path);
  legacy = ui.legacy;
  pthread_mutex_unlock(&ui_mutex);
  if(settings_load(DATA_CONF, s) && legacy && conf[0]) {
    settings_load(conf, s);
  }
  if(usb_mark(root, s->usb_id)) {
    snprintf(err, sizeof err, "cannot write %s on %s (%s)", MARKER_NAME, root, strerror(errno));
    web_message(c, "400 Bad Request", "error", err);
    settings_free(s);
    return;
  }
  if(settings_save(DATA_CONF, s)) {
    snprintf(err, sizeof err, "cannot write %s (%s)", DATA_CONF, strerror(errno));
    web_message(c, "500 Internal Server Error", "error", err);
    settings_free(s);
    return;
  }
  logq("Backup USB set to %s in settings", root);
  settings_free(s);
  ui_wake();
  web_message(c, "200 OK", "ok", "Backup USB selected");
}


static void
api_gdrive_status(int c) {
  char buf[4096];
  jw_t w = {buf, sizeof buf, 0, 0};
  int connected = gd_connected();
  time_t now = wall_clock();

  pthread_mutex_lock(&gd_mutex);
  jw_raw(&w, "{");
  jw_field_bool(&w, "configured", strcmp(GDRIVE_CLIENT_ID, "") != 0, 0);
  jw_field_bool(&w, "connected", connected, 1);
  jw_field_bool(&w, "pending", gd_state.pending, 1);
  jw_field_str(&w, "user_code", gd_state.pending ? gd_state.user_code : "", 1);
  jw_field_str(&w, "verification_url", gd_state.pending ? gd_state.verification_url : "", 1);
  jw_field_num(&w, "code_expires_in", gd_state.pending && gd_state.expires > now ? (long long)(gd_state.expires - now) : 0, 1);
  jw_field_str(&w, "message", gd_state.message, 1);
  jw_field_str(&w, "progress", gd_state.progress, 1);
  jw_field_num(&w, "last_sync_age", gd_state.last_sync ? (long long)(now - gd_state.last_sync) : -1, 1);
  jw_field_num(&w, "last_files", gd_state.last_files, 1);
  jw_field_num(&w, "last_bytes", (long long)gd_state.last_bytes, 1);
  jw_field_str(&w, "last_error", gd_state.last_error, 1);
  jw_raw(&w, "}");
  pthread_mutex_unlock(&gd_mutex);
  web_json(c, "200 OK", &w);
}


static void
api_gdrive_connect(int c) {
  char err[256];

  if(gd_auth_begin(err, sizeof err)) {
    web_message(c, "400 Bad Request", "error", err);
    return;
  }
  api_gdrive_status(c);
}


static void
api_restore_list(int c) {
  char *buf = malloc(WEB_MAX_BODY);
  jw_t w = {buf, WEB_MAX_BODY, 0, 0};

  if(!buf) {
    web_message(c, "500 Internal Server Error", "error", "not enough memory");
    return;
  }
  jw_raw(&w, "{\"saves\":");
  if(g_backup_root[0]) list_saves_json(&w); else jw_raw(&w, "[]");
  jw_raw(&w, ",\"files\":");
  if(g_backup_root[0]) list_paths_json(&w); else jw_raw(&w, "[]");
  jw_raw(&w, ",\"status\":");
  job_status_json(&w);
  jw_raw(&w, "}");
  web_json(c, "200 OK", &w);
  free(buf);
}


static void
api_restore(int c, const char *body) {
  char kind[16], path[PATH_MAX], ps[8], uid[64], title[16], save[256], source[160];
  int rc;

  form_get(body, "kind", kind, sizeof kind);
  form_get(body, "source", source, sizeof source);
  if(!restore_source_valid(source)) {
    web_message(c, "400 Bad Request", "error", "invalid source");
    return;
  }
  if(!strcmp(kind, "path")) {
    if(!form_get(body, "path", path, sizeof path) || path[0] != '/') {
      web_message(c, "400 Bad Request", "error", "path is missing");
      return;
    }
    rc = job_submit(JOB_RESTORE_PATH, path, 0, NULL, NULL, NULL, source);
  } else if(!strcmp(kind, "save")) {
    form_get(body, "ps", ps, sizeof ps);
    form_get(body, "uid", uid, sizeof uid);
    form_get(body, "title", title, sizeof title);
    form_get(body, "save", save, sizeof save);
    if((strcmp(ps, "ps4") && strcmp(ps, "ps5")) || !is_hex_name(uid) || !title_valid(title) ||
       !save[0] || strchr(save, '/') || save[0] == '.') {
      web_message(c, "400 Bad Request", "error", "invalid save reference");
      return;
    }
    rc = job_submit(JOB_RESTORE_SAVE, NULL, !strcmp(ps, "ps5"), uid, title, save, source);
  } else {
    web_message(c, "400 Bad Request", "error", "kind must be path or save");
    return;
  }
  if(rc) {
    web_message(c, "409 Conflict", "error", "another restore is still running");
    return;
  }
  web_message(c, "202 Accepted", "ok", "Restore queued");
}


static void
api_log(int c) {
  char *buf = malloc(UI_LOG_LINES * UI_LOG_WIDTH * 6 + 64);
  jw_t w = {buf, UI_LOG_LINES * UI_LOG_WIDTH * 6 + 64, 0, 0};

  if(!buf) {
    web_message(c, "500 Internal Server Error", "error", "not enough memory");
    return;
  }
  pthread_mutex_lock(&ui_mutex);
  jw_raw(&w, "{\"lines\":[");
  for(unsigned i = 0; i < ui_log_count; i++) {
    unsigned idx = (ui_log_next + UI_LOG_LINES - ui_log_count + i) % UI_LOG_LINES;
    if(i) jw_raw(&w, ",");
    jw_str(&w, ui_log[idx]);
  }
  jw_raw(&w, "]}");
  pthread_mutex_unlock(&ui_mutex);
  web_json(c, "200 OK", &w);
  free(buf);
}


static void
web_handle(int c) {
  char method[16], path[1024];
  char *body;
  size_t blen;

  if(web_read(c, method, sizeof method, path, sizeof path, &body, &blen)) {
    return;
  }
  if(!strcmp(method, "GET") && (!strcmp(path, "/") || !strcmp(path, "/index.html"))) {
    web_send(c, "200 OK", "text/html; charset=utf-8", web_index_html, web_index_html_len);
  } else if(!strcmp(method, "GET") && !strcmp(path, "/api/status")) {
    api_status(c);
  } else if(!strcmp(method, "GET") && !strcmp(path, "/api/config")) {
    api_config_get(c);
  } else if(!strcmp(method, "POST") && !strcmp(path, "/api/config")) {
    api_config_post(c, body);
  } else if(!strcmp(method, "GET") && !strcmp(path, "/api/usb")) {
    api_usb(c);
  } else if(!strcmp(method, "POST") && !strcmp(path, "/api/usb/select")) {
    api_usb_select(c, body);
  } else if(!strcmp(method, "POST") && !strcmp(path, "/api/backup")) {
    ui_wake();
    web_message(c, "200 OK", "ok", "Backup pass requested");
  } else if(!strcmp(method, "GET") && !strcmp(path, "/api/gdrive/status")) {
    api_gdrive_status(c);
  } else if(!strcmp(method, "POST") && !strcmp(path, "/api/gdrive/connect")) {
    api_gdrive_connect(c);
  } else if(!strcmp(method, "POST") && !strcmp(path, "/api/gdrive/disconnect")) {
    gd_disconnect();
    ui_wake();
    web_message(c, "200 OK", "ok", "Signed out of Google Drive");
  } else if(!strcmp(method, "GET") && !strcmp(path, "/api/restore/list")) {
    api_restore_list(c);
  } else if(!strcmp(method, "POST") && !strcmp(path, "/api/restore")) {
    api_restore(c, body);
  } else if(!strcmp(method, "GET") && !strcmp(path, "/api/restore/status")) {
    char sbuf[2048];
    jw_t w = {sbuf, sizeof sbuf, 0, 0};
    job_status_json(&w);
    web_json(c, "200 OK", &w);
  } else if(!strcmp(method, "POST") && !strcmp(path, "/api/gdrive/download")) {
    if(!gd_connected()) {
      web_message(c, "400 Bad Request", "error", "Google Drive is not signed in");
    } else if(job_submit(JOB_GDRIVE_DOWNLOAD, NULL, 0, NULL, NULL, NULL, "")) {
      web_message(c, "409 Conflict", "error", "another restore is still running");
    } else {
      web_message(c, "202 Accepted", "ok", "Download queued");
    }
  } else if(!strcmp(method, "GET") && !strcmp(path, "/api/log")) {
    api_log(c);
  } else {
    web_message(c, "404 Not Found", "error", "no such page");
  }
  free(body);
}


static void *
web_main(void *arg) {
  int listener = (int)(intptr_t)arg;

  for(;;) {
    int c = accept(listener, NULL, NULL);
    if(c < 0) {
      if(errno == EINTR || errno == ECONNABORTED) continue;
      logq("Settings page stopped: accept failed (%s)", strerror(errno));
      return NULL;
    }
    web_handle(c);
    close(c);
  }
}


/* Starts the settings page thread. Returns 0, or -1 if the port is busy. */
static int
web_start(void) {
  struct sockaddr_in addr;
  pthread_attr_t attr;
  pthread_t thread;
  int one = 1, s, rc;

  if((s = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    return -1;
  }
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_port = htons(WEB_PORT);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if(bind(s, (struct sockaddr *)&addr, sizeof addr) || listen(s, 4)) {
    close(s);
    return -1;
  }
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  rc = pthread_create(&thread, &attr, web_main, (void *)(intptr_t)s);
  pthread_attr_destroy(&attr);
  if(rc) {
    close(s);
    return -1;
  }
  return 0;
}


/* Opens the settings page in the console browser (first run only). */
#ifndef BACKUP_HOST_TEST
int sceUserServiceInitialize(void *);
int sceSystemServiceLaunchWebBrowser(const char *uri, void *);
#endif

static void
web_open_browser(const char *url) {
#ifndef BACKUP_HOST_TEST
  sceUserServiceInitialize(0);   /* may already be initialised */
  if(sceSystemServiceLaunchWebBrowser(url, 0)) {
    logq("Cannot open the settings page in the console browser");
  }
#else
  logq("Would open %s in the console browser", url);
#endif
}

#endif
