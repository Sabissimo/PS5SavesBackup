/* Google Drive destination.
 *
 * Sign-in uses the OAuth 2.0 flow for TV and limited-input devices: the
 * payload shows a short code, the user enters it on google.com/device from
 * a phone or PC, and the payload receives a refresh token which is stored in
 * DATA_DIR/gdrive.token. The scope is drive.file only: the app sees nothing
 * but the files and folders it created itself.
 *
 * Backups go to Drive as a mirror of the local backup folder (the USB
 * backup/ folder, or the internal one when no USB is used): after every
 * backup pass, files whose size or modification time changed since the last
 * upload are uploaded again, new files are created. The mapping from path
 * to Drive file id is kept in DATA_DIR/gdrive.index. Daily snapshots are not
 * mirrored (they would re-upload the whole backup every day). Files removed
 * locally are left in Drive. */
#ifndef GDRIVE_H
#define GDRIVE_H

#include "http_client.h"

/* The Google OAuth client ("TVs and Limited Input devices" type) is set at
 * build time: GDRIVE_CLIENT_ID and GDRIVE_CLIENT_SECRET make variables, or
 * a gdrive_credentials.mk file next to the Makefile (not tracked by git).
 * Without them the Google Drive sign-in is disabled in the build. */
#ifndef GDRIVE_CLIENT_ID
#define GDRIVE_CLIENT_ID     ""
#endif
#ifndef GDRIVE_CLIENT_SECRET
#define GDRIVE_CLIENT_SECRET ""
#endif
#define GDRIVE_SCOPE          "https://www.googleapis.com/auth/drive.file"
#define GDRIVE_TOKEN_FILE     DATA_DIR "/gdrive.token"
#define GDRIVE_INDEX_FILE     DATA_DIR "/gdrive.index"
#ifndef GDRIVE_DEFAULT_FOLDER
#define GDRIVE_DEFAULT_FOLDER "PS5SavesBackup"
#endif
#define GDRIVE_FOLDER_MIME    "application/vnd.google-apps.folder"
#define GDRIVE_INDEX_BUCKETS  4096
#define GDRIVE_INDEX_LIMIT    100000
#define GDRIVE_MAX_ERRORS     5      /* consecutive failures that end a sync */
#define GDRIVE_ID_LEN         128

static const char *
gd_oauth_base(void) {
#ifdef BACKUP_HOST_TEST
  const char *s = getenv("TEST_GDRIVE_OAUTH_BASE");
  if(s && *s) return s;
#endif
  return "https://oauth2.googleapis.com";
}


static const char *
gd_api_base(void) {
#ifdef BACKUP_HOST_TEST
  const char *s = getenv("TEST_GDRIVE_API_BASE");
  if(s && *s) return s;
#endif
  return "https://www.googleapis.com";
}


/* ------------------------------------------------------------------------
 * Small helpers: JSON reading (flat keys), URL encoding, text files
 * ---------------------------------------------------------------------- */

/* Pointer to the value of the first "key": in json, or NULL. */
static const char *
json_value(const char *json, const char *key) {
  size_t klen = strlen(key);
  const char *p = json;

  while((p = strstr(p, key))) {
    if(p > json && p[-1] == '"' && p[klen] == '"') {
      const char *q = p + klen + 1;
      while(*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
      if(*q == ':') {
        q++;
        while(*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
        return q;
      }
    }
    p += klen;
  }
  return NULL;
}


/* String or number value of key. Escapes are decoded; \uXXXX becomes '?'. */
static int
json_get_string(const char *json, const char *key, char *out, size_t len) {
  const char *v = json_value(json, key);
  size_t o = 0;

  out[0] = 0;
  if(!v) return 0;
  if(*v != '"') {
    while(*v && *v != ',' && *v != '}' && *v != ']' && *v != ' ' && o + 1 < len) out[o++] = *v++;
    out[o] = 0;
    return o > 0;
  }
  v++;
  while(*v && *v != '"' && o + 1 < len) {
    char c = *v++;
    if(c == '\\' && *v) {
      c = *v++;
      switch(c) {
      case 'n': c = '\n'; break;
      case 't': c = '\t'; break;
      case 'r': c = '\r'; break;
      case 'u':
        for(int i = 0; i < 4 && *v; i++) v++;
        c = '?';
        break;
      default: break;             /* \" \\ \/ */
      }
    }
    out[o++] = c;
  }
  out[o] = 0;
  return 1;
}


static long
json_get_number(const char *json, const char *key, long def) {
  char buf[32];

  if(!json_get_string(json, key, buf, sizeof buf)) return def;
  return strtol(buf, NULL, 10);
}


/* JSON string literal (quotes included) of s into out. */
static int
json_quote(const char *s, char *out, size_t len) {
  size_t o = 0;

  if(len < 3) return 0;
  out[o++] = '"';
  for(; *s; s++) {
    unsigned char c = (unsigned char)*s;
    if(c == '"' || c == '\\') {
      if(o + 3 >= len) return 0;
      out[o++] = '\\';
      out[o++] = (char)c;
    } else if(c < 0x20) {
      if(o + 8 >= len) return 0;
      o += (size_t)snprintf(out + o, len - o, "\\u%04x", c);
    } else {
      if(o + 2 >= len) return 0;
      out[o++] = (char)c;
    }
  }
  out[o++] = '"';
  out[o] = 0;
  return 1;
}


static int
url_encode(const char *s, char *out, size_t len) {
  static const char hex[] = "0123456789ABCDEF";
  size_t o = 0;

  for(; *s; s++) {
    unsigned char c = (unsigned char)*s;
    if(isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      if(o + 2 > len) return 0;
      out[o++] = (char)c;
    } else {
      if(o + 4 > len) return 0;
      out[o++] = '%';
      out[o++] = hex[c >> 4];
      out[o++] = hex[c & 15];
    }
  }
  out[o] = 0;
  return 1;
}


/* key=value&... body piece; returns 0 when out is too small. */
static int
form_add(char *out, size_t len, const char *key, const char *value) {
  char enc[2048];
  size_t used = strlen(out);

  if(!url_encode(value, enc, sizeof enc)) return 0;
  if(snprintf(out + used, len - used, "%s%s=%s", used ? "&" : "", key, enc) >= (int)(len - used)) return 0;
  return 1;
}


/* Atomic write of a small text file. */
static int
text_file_write(const char *path, const char *text) {
  char part[PATH_MAX];
  FILE *f;
  int ok;

  if(snprintf(part, sizeof part, "%s.part", path) >= (int)sizeof part) return -1;
  mkdir_p(DATA_DIR);
  if(!(f = fopen(part, "w"))) return -1;
  ok = fputs(text, f) >= 0;
  if(fflush(f) || fsync(fileno(f))) ok = 0;
  if(fclose(f)) ok = 0;
  if(!ok || rename(part, path)) {
    unlink(part);
    return -1;
  }
  return 0;
}


/* ------------------------------------------------------------------------
 * Tokens and sign-in state
 * ---------------------------------------------------------------------- */

static pthread_mutex_t gd_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct {
  int loaded;
  char refresh[1024];
  char access[4096];
  time_t expires_at;
  char folder_id[GDRIVE_ID_LEN];     /* cached root folder id */
  char folder_name[256];             /* name the id belongs to */
} gd_tok;

static struct {
  int pending;                       /* device flow in progress */
  char device_code[1024];
  char user_code[64];
  char verification_url[256];
  int interval;
  time_t expires;
  char message[256];                 /* last sign-in message */
  time_t last_sync;
  char last_error[256];
  unsigned last_files;
  uint64_t last_bytes;
  char progress[128];
} gd_state;


static void
gd_token_load_locked(void) {
  char line[4352];
  FILE *f;

  if(gd_tok.loaded) return;
  gd_tok.loaded = 1;
  memset(&gd_tok, 0, sizeof gd_tok);
  gd_tok.loaded = 1;
  if(!(f = fopen(GDRIVE_TOKEN_FILE, "r"))) return;
  while(fgets(line, sizeof line, f)) {
    char *eq = strchr(line, '=');
    if(!eq) continue;
    *eq++ = 0;
    eq[strcspn(eq, "\r\n")] = 0;
    if(!strcmp(line, "refresh_token")) snprintf(gd_tok.refresh, sizeof gd_tok.refresh, "%s", eq);
    else if(!strcmp(line, "access_token")) snprintf(gd_tok.access, sizeof gd_tok.access, "%s", eq);
    else if(!strcmp(line, "expires_at")) gd_tok.expires_at = (time_t)strtoll(eq, NULL, 10);
    else if(!strcmp(line, "folder_id")) snprintf(gd_tok.folder_id, sizeof gd_tok.folder_id, "%s", eq);
    else if(!strcmp(line, "folder_name")) snprintf(gd_tok.folder_name, sizeof gd_tok.folder_name, "%s", eq);
  }
  fclose(f);
}


static int
gd_token_save_locked(void) {
  char *text = malloc(8192);
  int rc;

  if(!text) return -1;
  snprintf(text, 8192,
           "# Google Drive sign-in for " APP_NAME ". Delete this file to sign out.\n"
           "refresh_token=%s\naccess_token=%s\nexpires_at=%lld\nfolder_id=%s\nfolder_name=%s\n",
           gd_tok.refresh, gd_tok.access, (long long)gd_tok.expires_at,
           gd_tok.folder_id, gd_tok.folder_name);
  rc = text_file_write(GDRIVE_TOKEN_FILE, text);
  free(text);
  return rc;
}


/* Signed in: a refresh token is stored. */
static int
gd_connected(void) {
  int c;

  pthread_mutex_lock(&gd_mutex);
  gd_token_load_locked();
  c = gd_tok.refresh[0] != 0;
  pthread_mutex_unlock(&gd_mutex);
  return c;
}


/* POST form to the token endpoint; returns 0 and the response JSON. */
static int
gd_token_request(const char *form, http_response_t *resp, char *err, size_t elen) {
  static const char *const headers[] = {"Content-Type: application/x-www-form-urlencoded"};
  char url[512];

  snprintf(url, sizeof url, "%s/token", gd_oauth_base());
  if(http_request("POST", url, headers, 1, form, strlen(form), resp)) {
    snprintf(err, elen, "Google not reachable: %s", resp->error);
    return -1;
  }
  return 0;
}


/* Valid access token into out (refreshing it when needed). */
static int
gd_access_token(char *out, size_t len, char *err, size_t elen) {
  char form[4096] = "";
  http_response_t resp;
  int rc = -1;

  pthread_mutex_lock(&gd_mutex);
  gd_token_load_locked();
  if(!gd_tok.refresh[0]) {
    snprintf(err, elen, "Google Drive is not signed in");
    pthread_mutex_unlock(&gd_mutex);
    return -1;
  }
  if(gd_tok.access[0] && gd_tok.expires_at > wall_clock() + 120) {
    snprintf(out, len, "%s", gd_tok.access);
    pthread_mutex_unlock(&gd_mutex);
    return 0;
  }
  form_add(form, sizeof form, "client_id", GDRIVE_CLIENT_ID);
  form_add(form, sizeof form, "client_secret", GDRIVE_CLIENT_SECRET);
  form_add(form, sizeof form, "refresh_token", gd_tok.refresh);
  form_add(form, sizeof form, "grant_type", "refresh_token");
  pthread_mutex_unlock(&gd_mutex);

  if(gd_token_request(form, &resp, err, elen)) return -1;

  pthread_mutex_lock(&gd_mutex);
  if(resp.status == 200 && json_get_string(resp.body, "access_token", gd_tok.access, sizeof gd_tok.access)) {
    gd_tok.expires_at = wall_clock() + json_get_number(resp.body, "expires_in", 3600);
    gd_token_save_locked();
    snprintf(out, len, "%s", gd_tok.access);
    rc = 0;
  } else {
    char e[128] = "";
    json_get_string(resp.body, "error", e, sizeof e);
    if(!strcmp(e, "invalid_grant")) {
      /* access was revoked or the token expired: sign in again */
      gd_tok.refresh[0] = gd_tok.access[0] = 0;
      gd_token_save_locked();
      snprintf(err, elen, "Google Drive access was revoked: sign in again");
    } else {
      snprintf(err, elen, "Google token refresh failed (HTTP %d%s%s)", resp.status, e[0] ? ": " : "", e);
    }
  }
  pthread_mutex_unlock(&gd_mutex);
  http_response_free(&resp);
  return rc;
}


/* Polls the token endpoint until the user has entered the code. */
static void *
gd_auth_poll(void *arg) {
  (void)arg;
  for(;;) {
    char form[4096] = "", err[256], e[128] = "";
    http_response_t resp;
    int interval;
    time_t expires;

    pthread_mutex_lock(&gd_mutex);
    interval = gd_state.interval;
    expires = gd_state.expires;
    form_add(form, sizeof form, "client_id", GDRIVE_CLIENT_ID);
    form_add(form, sizeof form, "client_secret", GDRIVE_CLIENT_SECRET);
    form_add(form, sizeof form, "device_code", gd_state.device_code);
    form_add(form, sizeof form, "grant_type", "urn:ietf:params:oauth:grant-type:device_code");
    pthread_mutex_unlock(&gd_mutex);

    sleep((unsigned)interval);
    if(wall_clock() > expires) {
      pthread_mutex_lock(&gd_mutex);
      gd_state.pending = 0;
      snprintf(gd_state.message, sizeof gd_state.message, "The code expired; start the sign-in again");
      pthread_mutex_unlock(&gd_mutex);
      logq("Google Drive sign-in: code expired");
      return NULL;
    }
    if(gd_token_request(form, &resp, err, sizeof err)) {
      logq("Google Drive sign-in: %s (retrying)", err);
      continue;
    }
    pthread_mutex_lock(&gd_mutex);
    if(resp.status == 200 && json_get_string(resp.body, "refresh_token", gd_tok.refresh, sizeof gd_tok.refresh)) {
      json_get_string(resp.body, "access_token", gd_tok.access, sizeof gd_tok.access);
      gd_tok.expires_at = wall_clock() + json_get_number(resp.body, "expires_in", 3600);
      gd_tok.folder_id[0] = gd_tok.folder_name[0] = 0;
      gd_tok.loaded = 1;
      gd_token_save_locked();
      gd_state.pending = 0;
      snprintf(gd_state.message, sizeof gd_state.message, "Signed in to Google Drive");
      pthread_mutex_unlock(&gd_mutex);
      http_response_free(&resp);
      logq("Google Drive sign-in: signed in");
      notify("Google Drive connected.");
      ui_wake();
      return NULL;
    }
    json_get_string(resp.body, "error", e, sizeof e);
    if(!strcmp(e, "authorization_pending")) {
      pthread_mutex_unlock(&gd_mutex);
    } else if(!strcmp(e, "slow_down")) {
      gd_state.interval += 5;
      pthread_mutex_unlock(&gd_mutex);
    } else {
      gd_state.pending = 0;
      if(!strcmp(e, "access_denied")) {
        snprintf(gd_state.message, sizeof gd_state.message, "Sign-in was declined on the Google page");
      } else if(!strcmp(e, "expired_token")) {
        snprintf(gd_state.message, sizeof gd_state.message, "The code expired; start the sign-in again");
      } else {
        snprintf(gd_state.message, sizeof gd_state.message, "Sign-in failed (HTTP %d%s%s)",
                 resp.status, e[0] ? ": " : "", e);
      }
      logq("Google Drive sign-in: %s", gd_state.message);
      pthread_mutex_unlock(&gd_mutex);
      http_response_free(&resp);
      return NULL;
    }
    http_response_free(&resp);
  }
}


/**
 * Starts the device sign-in. On success user_code and verification_url are
 * filled and a thread waits for the user to enter the code.
 **/
static int
gd_auth_begin(char *err, size_t elen) {
  static const char *const headers[] = {"Content-Type: application/x-www-form-urlencoded"};
  char form[1024] = "", url[512];
  http_response_t resp;
  pthread_attr_t attr;
  pthread_t thread;
  int rc;

  pthread_mutex_lock(&gd_mutex);
  if(gd_state.pending) {
    pthread_mutex_unlock(&gd_mutex);
    return 0;                        /* already waiting; the page shows the code */
  }
  pthread_mutex_unlock(&gd_mutex);

  if(!strcmp(GDRIVE_CLIENT_ID, "") || !strcmp(GDRIVE_CLIENT_SECRET, "")) {
    snprintf(err, elen, "This build has no Google client id");
    return -1;
  }
  form_add(form, sizeof form, "client_id", GDRIVE_CLIENT_ID);
  form_add(form, sizeof form, "scope", GDRIVE_SCOPE);
  snprintf(url, sizeof url, "%s/device/code", gd_oauth_base());
  if(http_request("POST", url, headers, 1, form, strlen(form), &resp)) {
    snprintf(err, elen, "Google not reachable: %s", resp.error);
    return -1;
  }
  pthread_mutex_lock(&gd_mutex);
  if(resp.status != 200 ||
     !json_get_string(resp.body, "device_code", gd_state.device_code, sizeof gd_state.device_code) ||
     !json_get_string(resp.body, "user_code", gd_state.user_code, sizeof gd_state.user_code)) {
    char e[128] = "";
    json_get_string(resp.body, "error", e, sizeof e);
    snprintf(err, elen, "Google refused the sign-in request (HTTP %d%s%s)", resp.status, e[0] ? ": " : "", e);
    pthread_mutex_unlock(&gd_mutex);
    http_response_free(&resp);
    return -1;
  }
  if(!json_get_string(resp.body, "verification_url", gd_state.verification_url, sizeof gd_state.verification_url)) {
    snprintf(gd_state.verification_url, sizeof gd_state.verification_url, "https://www.google.com/device");
  }
  gd_state.interval = (int)json_get_number(resp.body, "interval", 5);
  if(gd_state.interval < 1) gd_state.interval = 5;
  gd_state.expires = wall_clock() + json_get_number(resp.body, "expires_in", 1800);
  gd_state.pending = 1;
  snprintf(gd_state.message, sizeof gd_state.message, "Waiting for the code to be entered");
  http_response_free(&resp);

  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  rc = pthread_create(&thread, &attr, gd_auth_poll, NULL);
  pthread_attr_destroy(&attr);
  if(rc) {
    gd_state.pending = 0;
    snprintf(err, elen, "cannot start the sign-in (%s)", strerror(rc));
    pthread_mutex_unlock(&gd_mutex);
    return -1;
  }
  logq("Google Drive sign-in: enter code %s at %s", gd_state.user_code, gd_state.verification_url);
  pthread_mutex_unlock(&gd_mutex);
  return 0;
}


/* Signs out: revokes the token at Google (best effort) and forgets it. */
static void
gd_disconnect(void) {
  static const char *const headers[] = {"Content-Type: application/x-www-form-urlencoded"};
  char form[2048] = "", url[512];
  http_response_t resp;

  pthread_mutex_lock(&gd_mutex);
  gd_token_load_locked();
  if(gd_tok.refresh[0]) {
    form_add(form, sizeof form, "token", gd_tok.refresh);
  }
  memset(&gd_tok, 0, sizeof gd_tok);
  gd_tok.loaded = 1;
  gd_state.pending = 0;
  snprintf(gd_state.message, sizeof gd_state.message, "Signed out");
  gd_state.last_error[0] = 0;
  pthread_mutex_unlock(&gd_mutex);
  unlink(GDRIVE_TOKEN_FILE);
  unlink(GDRIVE_INDEX_FILE);
  if(form[0]) {
    snprintf(url, sizeof url, "%s/revoke", gd_oauth_base());
    if(!http_request("POST", url, headers, 1, form, strlen(form), &resp)) {
      http_response_free(&resp);
    }
  }
  logq("Google Drive: signed out");
}


/* ------------------------------------------------------------------------
 * Drive API
 * ---------------------------------------------------------------------- */

/* One Drive API call with the bearer token and up to 3 retries on
 * rate limits and server errors. Returns 0 with resp filled. */
static int
gd_api(const char *method, const char *url, const char *content_type,
       const void *body, size_t body_len, http_response_t *resp, char *err, size_t elen) {
  char auth[4200], token[4096];
  const char *headers[2];

  if(gd_access_token(token, sizeof token, err, elen)) return -1;
  snprintf(auth, sizeof auth, "Authorization: Bearer %s", token);
  headers[0] = auth;
  headers[1] = content_type;
  for(int attempt = 0; ; attempt++) {
    if(http_request(method, url, headers, content_type ? 2 : 1, body, body_len, resp)) {
      snprintf(err, elen, "Google Drive not reachable: %s", resp->error);
      return -1;
    }
    if((resp->status == 429 || resp->status >= 500) && attempt < 3) {
      http_response_free(resp);
      sleep(2u << attempt);
      continue;
    }
    if(resp->status == 401) {
      /* token no longer accepted: forget it and let the next call refresh */
      pthread_mutex_lock(&gd_mutex);
      gd_tok.access[0] = 0;
      gd_tok.expires_at = 0;
      pthread_mutex_unlock(&gd_mutex);
    }
    return 0;
  }
}


/* Id of the folder called name below parent ("root" for My Drive), or "". */
static int
gd_find_folder(const char *parent, const char *name, char *id, char *err, size_t elen) {
  char q[1024], qenc[3072], url[4096], nq[600];
  http_response_t resp;

  id[0] = 0;
  /* name inside the query is a single-quoted string with \ and ' escaped */
  {
    size_t o = 0;
    for(const char *p = name; *p && o + 3 < sizeof nq; p++) {
      if(*p == '\\' || *p == '\'') nq[o++] = '\\';
      nq[o++] = *p;
    }
    nq[o] = 0;
  }
  snprintf(q, sizeof q, "name = '%s' and mimeType = '" GDRIVE_FOLDER_MIME
           "' and '%s' in parents and trashed = false", nq, parent);
  url_encode(q, qenc, sizeof qenc);
  snprintf(url, sizeof url, "%s/drive/v3/files?q=%s&fields=files(id)&pageSize=1&spaces=drive",
           gd_api_base(), qenc);
  if(gd_api("GET", url, NULL, NULL, 0, &resp, err, elen)) return -1;
  if(resp.status != 200) {
    snprintf(err, elen, "Google Drive folder lookup failed (HTTP %d)", resp.status);
    http_response_free(&resp);
    return -1;
  }
  json_get_string(resp.body, "id", id, GDRIVE_ID_LEN);
  http_response_free(&resp);
  return 0;
}


/* Creates a file or folder (mime NULL = plain file) below parent. */
static int
gd_create(const char *parent, const char *name, const char *mime, char *id, char *err, size_t elen) {
  char body[1200], qname[600], url[512];
  http_response_t resp;

  if(!json_quote(name, qname, sizeof qname)) {
    snprintf(err, elen, "name too long");
    return -1;
  }
  snprintf(body, sizeof body, "{\"name\":%s,\"parents\":[\"%s\"]%s%s%s}", qname, parent,
           mime ? ",\"mimeType\":\"" : "", mime ? mime : "", mime ? "\"" : "");
  snprintf(url, sizeof url, "%s/drive/v3/files?fields=id", gd_api_base());
  if(gd_api("POST", url, "Content-Type: application/json; charset=UTF-8",
            body, strlen(body), &resp, err, elen)) return -1;
  if(resp.status != 200 || !json_get_string(resp.body, "id", id, GDRIVE_ID_LEN)) {
    snprintf(err, elen, "Google Drive create failed (HTTP %d)", resp.status);
    http_response_free(&resp);
    return -1;
  }
  http_response_free(&resp);
  return 0;
}


/* Replaces the content of file id. Returns 1 when the file no longer
 * exists in Drive, 0 on success, -1 on failure. */
static int
gd_upload(const char *id, const void *data, size_t len, char *err, size_t elen) {
  char url[512];
  http_response_t resp;

  snprintf(url, sizeof url, "%s/upload/drive/v3/files/%s?uploadType=media&fields=id", gd_api_base(), id);
  if(gd_api("PATCH", url, "Content-Type: application/octet-stream", data, len, &resp, err, elen)) return -1;
  if(resp.status == 404) {
    http_response_free(&resp);
    return 1;
  }
  if(resp.status != 200) {
    snprintf(err, elen, "Google Drive upload failed (HTTP %d)", resp.status);
    http_response_free(&resp);
    return -1;
  }
  http_response_free(&resp);
  return 0;
}


/* Id of the backup folder in My Drive, created when missing; cached. */
static int
gd_root_folder(const char *name, char *id, char *err, size_t elen) {
  char cached[GDRIVE_ID_LEN] = "";

  pthread_mutex_lock(&gd_mutex);
  gd_token_load_locked();
  if(gd_tok.folder_id[0] && !strcmp(gd_tok.folder_name, name)) {
    snprintf(cached, sizeof cached, "%s", gd_tok.folder_id);
  }
  pthread_mutex_unlock(&gd_mutex);
  if(cached[0]) {
    snprintf(id, GDRIVE_ID_LEN, "%s", cached);
    return 0;
  }
  if(gd_find_folder("root", name, id, err, elen)) return -1;
  if(!id[0]) {
    if(gd_create("root", name, GDRIVE_FOLDER_MIME, id, err, elen)) return -1;
    logq("Google Drive: created folder %s", name);
  }
  pthread_mutex_lock(&gd_mutex);
  snprintf(gd_tok.folder_id, sizeof gd_tok.folder_id, "%s", id);
  snprintf(gd_tok.folder_name, sizeof gd_tok.folder_name, "%s", name);
  gd_token_save_locked();
  pthread_mutex_unlock(&gd_mutex);
  return 0;
}


/* ------------------------------------------------------------------------
 * Upload index: relative path -> Drive id (+ size/mtime for files)
 * ---------------------------------------------------------------------- */

typedef struct gd_entry {
  struct gd_entry *next;
  char *path;                        /* relative to the local backup root */
  char id[GDRIVE_ID_LEN];
  int64_t size, mtime;
  int dir;
} gd_entry_t;
static gd_entry_t *gd_index[GDRIVE_INDEX_BUCKETS];
static unsigned gd_index_count;
static int gd_index_dirty;


static gd_entry_t *
gd_index_get(const char *path, int create) {
  unsigned b = index_bucket(path) % GDRIVE_INDEX_BUCKETS;
  gd_entry_t *e;

  for(e = gd_index[b]; e; e = e->next)
    if(!strcmp(e->path, path)) return e;
  if(!create || gd_index_count >= GDRIVE_INDEX_LIMIT) return NULL;
  if(!(e = calloc(1, sizeof *e))) return NULL;
  if(!(e->path = strdup(path))) {
    free(e);
    return NULL;
  }
  e->next = gd_index[b];
  gd_index[b] = e;
  gd_index_count++;
  return e;
}


static void
gd_index_clear(void) {
  for(unsigned i = 0; i < GDRIVE_INDEX_BUCKETS; i++) {
    while(gd_index[i]) {
      gd_entry_t *e = gd_index[i];
      gd_index[i] = e->next;
      free(e->path);
      free(e);
    }
  }
  gd_index_count = 0;
  gd_index_dirty = 0;
}


/* The index belongs to one Drive root folder: with another folder (renamed
 * in the settings, or a new sign-in) everything is uploaded again. */
static void
gd_index_load(const char *root_id) {
  char line[PATH_MAX + 256];
  FILE *f;
  int header = 0;

  gd_index_clear();
  if(!(f = fopen(GDRIVE_INDEX_FILE, "r"))) return;
  while(fgets(line, sizeof line, f)) {
    char *f1, *f2, *f3, *f4, *path;
    gd_entry_t *e;
    line[strcspn(line, "\r\n")] = 0;
    f1 = strchr(line, '\t');
    if(!f1) continue;
    *f1++ = 0;
    if(!strcmp(line, "R")) {
      header = 1;
      if(strcmp(f1, root_id)) {
        logq("Google Drive folder changed; uploading the backup again");
        break;
      }
    } else if(!header) {
      break;
    } else if(!strcmp(line, "D")) {
      path = strchr(f1, '\t');
      if(!path) continue;
      *path++ = 0;
      if(!(e = gd_index_get(path, 1))) break;
      snprintf(e->id, sizeof e->id, "%s", f1);
      e->dir = 1;
    } else if(!strcmp(line, "F")) {
      f2 = strchr(f1, '\t');
      if(!f2) continue;
      *f2++ = 0;
      f3 = strchr(f2, '\t');
      if(!f3) continue;
      *f3++ = 0;
      f4 = strchr(f3, '\t');
      if(!f4) continue;
      *f4++ = 0;
      if(!(e = gd_index_get(f4, 1))) break;
      snprintf(e->id, sizeof e->id, "%s", f1);
      e->size = strtoll(f2, NULL, 10);
      e->mtime = strtoll(f3, NULL, 10);
      e->dir = 0;
    }
  }
  fclose(f);
  gd_index_dirty = 0;
}


static void
gd_index_save(const char *root_id) {
  char part[PATH_MAX];
  FILE *f;
  int ok;

  if(!gd_index_dirty) return;
  snprintf(part, sizeof part, "%s.part", GDRIVE_INDEX_FILE);
  mkdir_p(DATA_DIR);
  if(!(f = fopen(part, "w"))) {
    logq("Cannot save the Google Drive index; files may be uploaded again");
    return;
  }
  ok = fprintf(f, "R\t%s\n", root_id) > 0;
  for(unsigned i = 0; i < GDRIVE_INDEX_BUCKETS && ok; i++) {
    for(gd_entry_t *e = gd_index[i]; e && ok; e = e->next) {
      if(e->dir) ok = fprintf(f, "D\t%s\t%s\n", e->id, e->path) > 0;
      else ok = fprintf(f, "F\t%s\t%lld\t%lld\t%s\n", e->id, (long long)e->size,
                        (long long)e->mtime, e->path) > 0;
    }
  }
  if(fflush(f) || fsync(fileno(f))) ok = 0;
  if(fclose(f)) ok = 0;
  if(!ok || rename(part, GDRIVE_INDEX_FILE)) {
    unlink(part);
    logq("Cannot commit the Google Drive index");
  } else {
    gd_index_dirty = 0;
  }
}


/* Drive id of the folder for the relative directory rel ("" = root),
 * creating the chain of folders as needed. */
static int
gd_folder_id(const char *root_id, const char *rel, char *id, char *err, size_t elen) {
  char parent_rel[PATH_MAX], parent_id[GDRIVE_ID_LEN];
  const char *slash, *name;
  gd_entry_t *e;

  if(!rel[0]) {
    snprintf(id, GDRIVE_ID_LEN, "%s", root_id);
    return 0;
  }
  if((e = gd_index_get(rel, 0)) && e->dir && e->id[0]) {
    snprintf(id, GDRIVE_ID_LEN, "%s", e->id);
    return 0;
  }
  slash = strrchr(rel, '/');
  if(slash) {
    snprintf(parent_rel, sizeof parent_rel, "%.*s", (int)(slash - rel), rel);
    name = slash + 1;
  } else {
    parent_rel[0] = 0;
    name = rel;
  }
  if(gd_folder_id(root_id, parent_rel, parent_id, err, elen)) return -1;
  if(gd_find_folder(parent_id, name, id, err, elen)) return -1;
  if(!id[0] && gd_create(parent_id, name, GDRIVE_FOLDER_MIME, id, err, elen)) return -1;
  if((e = gd_index_get(rel, 1))) {
    snprintf(e->id, sizeof e->id, "%s", id);
    e->dir = 1;
    gd_index_dirty = 1;
  }
  return 0;
}


/* Bookkeeping files of the local backup that are not part of the mirror. */
static int
gd_skip_name(const char *name) {
  size_t len = strlen(name);

  return !strcmp(name, ".ps5save-index") || !strcmp(name, ".image_stamp") ||
         !strcmp(name, MARKER_NAME) || !strcmp(name, USB_PROBE_NAME) ||
         (len > 5 && !strcmp(name + len - 5, ".part"));
}


/* Whole file into memory (files are capped at MAX_FILE_SZ by the backup). */
static char *
gd_read_file(const char *path, const struct stat *st, size_t *len) {
  char *buf;
  size_t got = 0;
  int fd;

  if(st->st_size < 0 || (uint64_t)st->st_size > MAX_FILE_SZ) return NULL;
  if(!(buf = malloc((size_t)st->st_size + 1))) return NULL;
  if((fd = open(path, O_RDONLY)) < 0) {
    free(buf);
    return NULL;
  }
  while(got < (size_t)st->st_size) {
    ssize_t n = read(fd, buf + got, (size_t)st->st_size - got);
    if(n < 0 && errno == EINTR) continue;
    if(n <= 0) break;
    got += (size_t)n;
  }
  close(fd);
  if(got != (size_t)st->st_size) {
    free(buf);
    return NULL;
  }
  *len = got;
  return buf;
}


/* Logs an upload error and shows it once per distinct message. */
static void
gd_report_error(const char *err) {
  static char reported[256];

  pthread_mutex_lock(&gd_mutex);
  snprintf(gd_state.last_error, sizeof gd_state.last_error, "%s", err);
  pthread_mutex_unlock(&gd_mutex);
  logq("Google Drive upload failed: %s", err);
  if(strcmp(reported, err)) {
    notify("Google Drive upload failed: %s", err);
    snprintf(reported, sizeof reported, "%s", err);
  }
}


typedef struct gd_sync_stats {
  unsigned planned, uploaded, failed, skipped;
  uint64_t bytes;
  int errors_in_a_row;
  char err[256];
} gd_sync_stats_t;


/* One file: upload when new or changed since the index entry. */
static int
gd_sync_file(const char *root_id, const char *root, const char *path, const char *rel,
             const struct stat *st, gd_sync_stats_t *stats, int plan) {
  gd_entry_t *e = gd_index_get(rel, 0);
  char folder[GDRIVE_ID_LEN], dir_rel[PATH_MAX];
  const char *slash = strrchr(rel, '/');
  size_t len = 0;
  char *data;
  int rc;

  (void)root;
  if(e && !e->dir && e->id[0] && e->size == st->st_size && e->mtime == (int64_t)st->st_mtime) {
    return 0;                        /* uploaded before, unchanged */
  }
  if(plan) {
    stats->planned++;
    return 0;
  }
  if((uint64_t)st->st_size > MAX_FILE_SZ) {
    stats->skipped++;
    return 0;
  }
  if(!(data = gd_read_file(path, st, &len))) {
    stats->failed++;
    stats->errors_in_a_row++;
    snprintf(stats->err, sizeof stats->err, "cannot read %s", rel);
    return -1;
  }
  if(slash) snprintf(dir_rel, sizeof dir_rel, "%.*s", (int)(slash - rel), rel);
  else dir_rel[0] = 0;
  if(gd_folder_id(root_id, dir_rel, folder, stats->err, sizeof stats->err)) {
    free(data);
    stats->failed++;
    stats->errors_in_a_row++;
    return -1;
  }
  rc = 1;
  if(e && e->id[0]) {
    rc = gd_upload(e->id, data, len, stats->err, sizeof stats->err);
  }
  if(rc == 1) {
    /* new file, or the old one was deleted in Drive */
    char id[GDRIVE_ID_LEN];
    if(gd_create(folder, slash ? slash + 1 : rel, NULL, id, stats->err, sizeof stats->err)) {
      rc = -1;
    } else if((rc = gd_upload(id, data, len, stats->err, sizeof stats->err)) == 1) {
      snprintf(stats->err, sizeof stats->err, "Google Drive lost the new file %s", rel);
      rc = -1;
    }
    if(rc) {
      free(data);
      stats->failed++;
      stats->errors_in_a_row++;
      return -1;
    }
    if(!e) e = gd_index_get(rel, 1);
    if(e) snprintf(e->id, sizeof e->id, "%s", id);
  }
  free(data);
  if(rc < 0) {
    stats->failed++;
    stats->errors_in_a_row++;
    return -1;
  }
  if(e) {
    e->dir = 0;
    e->size = st->st_size;
    e->mtime = (int64_t)st->st_mtime;
    gd_index_dirty = 1;
  }
  stats->uploaded++;
  stats->bytes += (uint64_t)st->st_size;
  stats->errors_in_a_row = 0;
  if(stats->uploaded % 25 == 0) gd_index_save(root_id);
  pthread_mutex_lock(&gd_mutex);
  snprintf(gd_state.progress, sizeof gd_state.progress, "Uploading %u/%u", stats->uploaded, stats->planned);
  pthread_mutex_unlock(&gd_mutex);
  return 0;
}


static int
gd_sync_tree(const char *root_id, const char *root, const char *dir, gd_sync_stats_t *stats, int plan) {
  char path[PATH_MAX];
  struct dirent *dp;
  struct stat st;
  DIR *d;
  int rc = 0;

  if(!(d = opendir(dir))) return 0;
  while((dp = readdir(d)) && !rc) {
    if(!strcmp(dp->d_name, ".") || !strcmp(dp->d_name, "..") || gd_skip_name(dp->d_name)) continue;
    if(snprintf(path, sizeof path, "%s/%s", dir, dp->d_name) >= (int)sizeof path || lstat(path, &st)) continue;
    if(S_ISDIR(st.st_mode)) {
      rc = gd_sync_tree(root_id, root, path, stats, plan);
    } else if(S_ISREG(st.st_mode)) {
      const char *rel = path + strlen(root) + 1;
      if(gd_sync_file(root_id, root, path, rel, &st, stats, plan) && !plan && !gd_connected()) {
        stats->errors_in_a_row = GDRIVE_MAX_ERRORS;   /* signed out: stop at once */
      }
      if(stats->errors_in_a_row >= GDRIVE_MAX_ERRORS || g_usb_lost) rc = -1;
    }
  }
  closedir(d);
  return rc;
}


/**
 * Mirrors the local backup folder into Drive. Returns 0 when everything is
 * up to date, -1 when the sync stopped early (reason logged).
 **/
static int
gd_sync(const char *root, const char *folder_name) {
  gd_sync_stats_t stats;
  char root_id[GDRIVE_ID_LEN], size[32], err[256];
  double start = now_seconds();
  int rc;

  memset(&stats, 0, sizeof stats);
  if(!gd_connected()) return 0;
  if(!folder_name[0]) folder_name = GDRIVE_DEFAULT_FOLDER;
  /* the root folder id is cached after the first lookup: no network here
   * on later passes */
  if(gd_root_folder(folder_name, root_id, err, sizeof err)) {
    gd_report_error(err);
    return -1;
  }
  gd_index_load(root_id);
  gd_sync_tree(root_id, root, root, &stats, 1);      /* count first, without network */
  if(!stats.planned) {
    gd_index_clear();
    pthread_mutex_lock(&gd_mutex);
    gd_state.last_sync = wall_clock();
    gd_state.last_error[0] = 0;
    gd_state.last_files = 0;
    gd_state.last_bytes = 0;
    gd_state.progress[0] = 0;
    pthread_mutex_unlock(&gd_mutex);
    logq("Google Drive up to date");
    return 0;
  }
  logq("Google Drive: uploading %u file(s) to folder %s", stats.planned, folder_name);
  notify("Uploading %u file(s) to Google Drive...", stats.planned);
  rc = gd_sync_tree(root_id, root, root, &stats, 0);
  gd_index_save(root_id);
  gd_index_clear();
  format_size(stats.bytes, size, sizeof size);
  pthread_mutex_lock(&gd_mutex);
  gd_state.last_sync = wall_clock();
  gd_state.last_files = stats.uploaded;
  gd_state.last_bytes = stats.bytes;
  gd_state.progress[0] = 0;
  gd_state.last_error[0] = 0;
  pthread_mutex_unlock(&gd_mutex);
  if(rc || stats.failed) {
    char summary[320];
    logq("Google Drive: %u uploaded (%s), %u failed, %u skipped in %.1f seconds",
         stats.uploaded, size, stats.failed, stats.skipped, now_seconds() - start);
    snprintf(summary, sizeof summary, "%s (%u file(s) not uploaded)", stats.err, stats.failed);
    gd_report_error(summary);
    return -1;
  }
  logq("Google Drive: %u file(s) uploaded (%s) in %.1f seconds", stats.uploaded, size,
       now_seconds() - start);
  notify("Google Drive backup complete (%s).", size);
  return 0;
}


/* ------------------------------------------------------------------------
 * Download: Drive folder -> local backup folder (docs/RESTORE.md)
 * ---------------------------------------------------------------------- */

/* Calls fn for every object of the JSON array at key (one nesting level);
 * obj is NUL terminated for the duration of the call. */
static void
json_each_object(const char *json, const char *key,
                 void (*fn)(const char *obj, void *arg), void *arg) {
  const char *v = json_value(json, key), *start = NULL;
  int depth = 0, in_str = 0;

  if(!v || *v != '[') return;
  for(const char *p = v + 1; *p; p++) {
    char c = *p;
    if(in_str) {
      if(c == '\\' && p[1]) p++;
      else if(c == '"') in_str = 0;
      continue;
    }
    if(c == '"') in_str = 1;
    else if(c == '{') {
      if(depth++ == 0) start = p;
    } else if(c == '}') {
      if(--depth == 0 && start) {
        size_t len = (size_t)(p + 1 - start);
        char *obj = malloc(len + 1);
        if(obj) {
          memcpy(obj, start, len);
          obj[len] = 0;
          fn(obj, arg);
          free(obj);
        }
        start = NULL;
      }
    } else if(c == ']' && depth == 0) break;
  }
}


/* "2026-11-12T10:00:00.000Z" -> time_t (UTC); 0 when unparsable. */
static time_t
parse_rfc3339(const char *s) {
  struct tm tm;
  int y, mo, d, h, mi, sec;

  if(sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &sec) != 6) return 0;
  memset(&tm, 0, sizeof tm);
  tm.tm_year = y - 1900;
  tm.tm_mon = mo - 1;
  tm.tm_mday = d;
  tm.tm_hour = h;
  tm.tm_min = mi;
  tm.tm_sec = sec;
  return timegm(&tm);
}


typedef struct gd_dl {
  const char *root;
  const char *rel;              /* relative path of the folder being listed */
  gd_sync_stats_t *stats;
  int depth;
} gd_dl_t;

static int gd_dl_dir(const char *root, const char *folder_id, const char *rel,
                     gd_sync_stats_t *stats, int depth);


/* Writes resp body to path through .part + rename, keeping mtime. */
static int
gd_write_file(const char *path, const char *data, size_t len, time_t mtime) {
  char part[PATH_MAX], dir[PATH_MAX], *slash;
  int fd, ok = 0;
  size_t pos = 0;

  snprintf(dir, sizeof dir, "%s", path);
  if((slash = strrchr(dir, '/')) && slash != dir) {
    *slash = 0;
    if(mkdir_p(dir)) return -1;
  }
  if(snprintf(part, sizeof part, "%s.part", path) >= (int)sizeof part) return -1;
  if((fd = open(part, O_WRONLY | O_CREAT | O_TRUNC, 0666)) < 0) return -1;
  while(pos < len) {
    ssize_t w = write(fd, data + pos, len - pos);
    if(w < 0) {
      if(errno == EINTR) continue;
      break;
    }
    pos += (size_t)w;
  }
  if(pos == len && !fsync(fd)) ok = 1;
  if(close(fd)) ok = 0;
  if(ok && mtime) {
    struct timeval tv[2];
    tv[0].tv_sec = mtime;
    tv[0].tv_usec = 0;
    tv[1] = tv[0];
    utimes(part, tv);
  }
  if(!ok || rename(part, path)) {
    unlink(part);
    return -1;
  }
  return 0;
}


static void
gd_dl_entry(const char *obj, void *arg) {
  gd_dl_t *ctx = arg;
  char id[GDRIVE_ID_LEN], name[512], mime[128], sizestr[32], modified[64], rel[PATH_MAX], local[PATH_MAX];
  gd_entry_t *e;
  struct stat st;
  long long size;
  time_t mtime;

  if(ctx->stats->errors_in_a_row >= GDRIVE_MAX_ERRORS) return;
  if(!json_get_string(obj, "id", id, sizeof id) || !json_get_string(obj, "name", name, sizeof name)) return;
  if(!name[0] || strchr(name, '/') || !strcmp(name, ".") || !strcmp(name, "..")) return;
  json_get_string(obj, "mimeType", mime, sizeof mime);
  if(snprintf(rel, sizeof rel, "%s%s%s", ctx->rel, ctx->rel[0] ? "/" : "", name) >= (int)sizeof rel ||
     snprintf(local, sizeof local, "%s/%s", ctx->root, rel) >= (int)sizeof local) return;

  if(!strcmp(mime, GDRIVE_FOLDER_MIME)) {
    if(ctx->depth > 64 || mkdir_p(local)) return;
    if((e = gd_index_get(rel, 1))) {
      snprintf(e->id, sizeof e->id, "%s", id);
      e->dir = 1;
      gd_index_dirty = 1;
    }
    gd_dl_dir(ctx->root, id, rel, ctx->stats, ctx->depth + 1);
    return;
  }
  json_get_string(obj, "size", sizestr, sizeof sizestr);
  size = strtoll(sizestr, NULL, 10);
  json_get_string(obj, "modifiedTime", modified, sizeof modified);
  mtime = parse_rfc3339(modified);
  e = gd_index_get(rel, 0);
  if(e && !e->dir && !strcmp(e->id, id) && e->size == size && !stat(local, &st) &&
     S_ISREG(st.st_mode) && st.st_size == size) {
    ctx->stats->skipped++;
    return;                                  /* this very file was uploaded from here */
  }
  if(size > (long long)MAX_FILE_SZ) {
    logq("Google Drive: %s is larger than the per-file limit; not downloaded", rel);
    ctx->stats->skipped++;
    return;
  }
  {
    char url[512], err[256];
    http_response_t resp;
    snprintf(url, sizeof url, "%s/drive/v3/files/%s?alt=media", gd_api_base(), id);
    if(gd_api("GET", url, NULL, NULL, 0, &resp, err, sizeof err) || resp.status != 200 ||
       (long long)resp.len != size || gd_write_file(local, resp.body, resp.len, mtime)) {
      if(resp.status >= 0 && resp.status != 200) snprintf(err, sizeof err, "download failed (HTTP %d)", resp.status);
      else if(resp.status == 200 && (long long)resp.len != size) snprintf(err, sizeof err, "short download of %s", rel);
      else if(resp.status == 200) snprintf(err, sizeof err, "cannot write %s", rel);
      snprintf(ctx->stats->err, sizeof ctx->stats->err, "%s", err);
      ctx->stats->failed++;
      ctx->stats->errors_in_a_row++;
      http_response_free(&resp);
      return;
    }
    http_response_free(&resp);
  }
  if(!stat(local, &st) && (e || (e = gd_index_get(rel, 1)))) {
    snprintf(e->id, sizeof e->id, "%s", id);
    e->dir = 0;
    e->size = st.st_size;
    e->mtime = (int64_t)st.st_mtime;
    gd_index_dirty = 1;
  }
  ctx->stats->uploaded++;                    /* counts downloaded files here */
  ctx->stats->bytes += (uint64_t)size;
  ctx->stats->errors_in_a_row = 0;
  pthread_mutex_lock(&gd_mutex);
  snprintf(gd_state.progress, sizeof gd_state.progress, "Downloading, %u file(s) so far", ctx->stats->uploaded);
  pthread_mutex_unlock(&gd_mutex);
}


/* Lists one Drive folder (paged) and handles every entry. */
static int
gd_dl_dir(const char *root, const char *folder_id, const char *rel, gd_sync_stats_t *stats, int depth) {
  char token[256] = "";
  gd_dl_t ctx = {root, rel, stats, depth};

  for(;;) {
    char q[256], qenc[768], url[1536], tenc[768], err[256];
    http_response_t resp;
    snprintf(q, sizeof q, "'%s' in parents and trashed = false", folder_id);
    url_encode(q, qenc, sizeof qenc);
    url_encode(token, tenc, sizeof tenc);
    snprintf(url, sizeof url, "%s/drive/v3/files?q=%s&fields=nextPageToken,files(id,name,mimeType,size,modifiedTime)"
             "&pageSize=1000&spaces=drive%s%s", gd_api_base(), qenc, token[0] ? "&pageToken=" : "", tenc);
    if(gd_api("GET", url, NULL, NULL, 0, &resp, err, sizeof err)) {
      snprintf(stats->err, sizeof stats->err, "%s", err);
      stats->failed++;
      stats->errors_in_a_row = GDRIVE_MAX_ERRORS;
      return -1;
    }
    if(resp.status != 200) {
      snprintf(stats->err, sizeof stats->err, "Google Drive listing failed (HTTP %d)", resp.status);
      stats->failed++;
      stats->errors_in_a_row = GDRIVE_MAX_ERRORS;
      http_response_free(&resp);
      return -1;
    }
    json_each_object(resp.body, "files", gd_dl_entry, &ctx);
    json_get_string(resp.body, "nextPageToken", token, sizeof token);
    http_response_free(&resp);
    if(!token[0] || stats->errors_in_a_row >= GDRIVE_MAX_ERRORS) break;
  }
  return stats->errors_in_a_row >= GDRIVE_MAX_ERRORS ? -1 : 0;
}


/**
 * Fills the local backup folder from the Drive folder: files that are not
 * known to be identical locally are downloaded. Local extras are kept.
 **/
static int
gd_download(const char *root, const char *folder_name, char *err, size_t elen) {
  gd_sync_stats_t stats;
  char root_id[GDRIVE_ID_LEN], size[32];
  double start = now_seconds();
  int rc;

  memset(&stats, 0, sizeof stats);
  if(!gd_connected()) {
    snprintf(err, elen, "Google Drive is not signed in");
    return -1;
  }
  if(!folder_name[0]) folder_name = GDRIVE_DEFAULT_FOLDER;
  if(gd_root_folder(folder_name, root_id, err, elen)) return -1;
  gd_index_load(root_id);
  logq("Google Drive: downloading folder %s to %s", folder_name, root);
  rc = gd_dl_dir(root, root_id, "", &stats, 0);
  gd_index_save(root_id);
  gd_index_clear();
  format_size(stats.bytes, size, sizeof size);
  pthread_mutex_lock(&gd_mutex);
  gd_state.progress[0] = 0;
  pthread_mutex_unlock(&gd_mutex);
  logq("Google Drive: %u file(s) downloaded (%s), %u already present, %u failed in %.1f seconds",
       stats.uploaded, size, stats.skipped, stats.failed, now_seconds() - start);
  if(rc || stats.failed) {
    snprintf(err, elen, "%s (%u downloaded, %u failed)", stats.err, stats.uploaded, stats.failed);
    return -1;
  }
  snprintf(err, elen, "%u file(s) downloaded (%s), %u already present", stats.uploaded, size, stats.skipped);
  return 0;
}

#endif
