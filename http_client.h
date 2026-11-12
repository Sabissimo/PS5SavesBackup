/* Minimal HTTPS client used for Google Drive: one request, whole response
 * in memory. On the console it is a thin wrapper around libSceHttp2 (with
 * libSceSsl for TLS, using the console's certificate store); the host test
 * build uses libcurl. All requests are serialised with a mutex, so the main
 * thread (uploads) and the settings page thread (sign-in) can share it. */
#ifndef HTTP_CLIENT_H
#define HTTP_CLIENT_H

#define HTTP_MAX_RESPONSE (4 * 1024 * 1024)
#define HTTP_USER_AGENT   APP_NAME "/" APP_VERSION
#define HTTP_TIMEOUT_SEC  60

typedef struct http_response {
  int status;          /* HTTP status, or -1 when no response was received */
  char *body;          /* NUL terminated, malloc'd; may be NULL */
  size_t len;
  char error[128];     /* transport error text when status < 0 */
} http_response_t;

static pthread_mutex_t http_mutex = PTHREAD_MUTEX_INITIALIZER;


static void
http_response_free(http_response_t *r) {
  free(r->body);
  r->body = NULL;
  r->len = 0;
}


#ifndef BACKUP_HOST_TEST
/* ---------------------------------------------------------------- PS5 --- */

int sceNetInit(void);
int sceNetPoolCreate(const char *, int, int);
int sceSslInit(size_t);
int sceHttp2Init(int, int, size_t, int);
int sceHttp2CreateTemplate(int, const char *, int, int);
int sceHttp2SetConnectTimeOut(int, uint32_t);
int sceHttp2SetSendTimeOut(int, uint32_t);
int sceHttp2SetRecvTimeOut(int, uint32_t);
int sceHttp2CreateRequestWithURL(int, const char *, const char *, uint64_t);
int sceHttp2DeleteRequest(int);
int sceHttp2AddRequestHeader(int, const char *, const char *, uint32_t);
int sceHttp2SetRequestContentLength(int, uint64_t);
int sceHttp2SendRequest(int, const void *, size_t);
int sceHttp2GetStatusCode(int, int *);
int sceHttp2ReadData(int, void *, size_t);

static int http_template = -1;

/* Called once, lazily, under http_mutex. */
static int
http_init_locked(void) {
  static int tried;
  int net, ssl, ctx;

  if(http_template >= 0) return 0;
  if(tried) return -1;
  tried = 1;
  if(sceNetInit()) {
    logq("Network: sceNetInit failed");
    return -1;
  }
  if((net = sceNetPoolCreate(APP_NAME, 64 * 1024, 0)) < 0) {
    logq("Network: sceNetPoolCreate failed (0x%08x)", (unsigned)net);
    return -1;
  }
  if((ssl = sceSslInit(512 * 1024)) < 0) {
    logq("Network: sceSslInit failed (0x%08x)", (unsigned)ssl);
    return -1;
  }
  if((ctx = sceHttp2Init(net, ssl, 512 * 1024, 2)) < 0) {
    logq("Network: sceHttp2Init failed (0x%08x)", (unsigned)ctx);
    return -1;
  }
  if((http_template = sceHttp2CreateTemplate(ctx, HTTP_USER_AGENT, 3, 1)) < 0) {
    logq("Network: sceHttp2CreateTemplate failed (0x%08x)", (unsigned)http_template);
    http_template = -1;
    return -1;
  }
  sceHttp2SetConnectTimeOut(http_template, 20 * 1000000u);
  sceHttp2SetSendTimeOut(http_template, HTTP_TIMEOUT_SEC * 1000000u);
  sceHttp2SetRecvTimeOut(http_template, HTTP_TIMEOUT_SEC * 1000000u);
  return 0;
}


static int
http_request_locked(const char *method, const char *url, const char *const *headers,
                    int nheaders, const void *body, size_t body_len,
                    http_response_t *resp) {
  int req, rc, status = 0;
  size_t cap = 0;

  if(http_init_locked()) {
    snprintf(resp->error, sizeof resp->error, "network not available");
    return -1;
  }
  if((req = sceHttp2CreateRequestWithURL(http_template, method, url, body_len)) < 0) {
    snprintf(resp->error, sizeof resp->error, "cannot create request (0x%08x)", (unsigned)req);
    return -1;
  }
  for(int i = 0; i < nheaders; i++) {
    const char *colon = strchr(headers[i], ':');
    char name[128];
    if(!colon || (size_t)(colon - headers[i]) >= sizeof name) continue;
    memcpy(name, headers[i], (size_t)(colon - headers[i]));
    name[colon - headers[i]] = 0;
    colon++;
    while(*colon == ' ') colon++;
    sceHttp2AddRequestHeader(req, name, colon, 0);
  }
  if(body_len) sceHttp2SetRequestContentLength(req, body_len);
  if((rc = sceHttp2SendRequest(req, body, body_len)) < 0) {
    snprintf(resp->error, sizeof resp->error, "send failed (0x%08x)", (unsigned)rc);
    sceHttp2DeleteRequest(req);
    return -1;
  }
  if((rc = sceHttp2GetStatusCode(req, &status)) < 0) {
    snprintf(resp->error, sizeof resp->error, "no status (0x%08x)", (unsigned)rc);
    sceHttp2DeleteRequest(req);
    return -1;
  }
  resp->status = status;
  for(;;) {
    int n;
    if(resp->len + 65536 + 1 > cap) {
      size_t ncap = cap ? cap * 2 : 128 * 1024;
      char *nb;
      if(ncap > HTTP_MAX_RESPONSE + 1) ncap = HTTP_MAX_RESPONSE + 1;
      if(ncap <= resp->len + 1) {
        snprintf(resp->error, sizeof resp->error, "response too large");
        sceHttp2DeleteRequest(req);
        return -1;
      }
      if(!(nb = realloc(resp->body, ncap))) {
        snprintf(resp->error, sizeof resp->error, "not enough memory");
        sceHttp2DeleteRequest(req);
        return -1;
      }
      resp->body = nb;
      cap = ncap;
    }
    n = sceHttp2ReadData(req, resp->body + resp->len, cap - resp->len - 1);
    if(n < 0) {
      snprintf(resp->error, sizeof resp->error, "read failed (0x%08x)", (unsigned)n);
      sceHttp2DeleteRequest(req);
      return -1;
    }
    if(n == 0) break;
    resp->len += (size_t)n;
  }
  if(!resp->body && !(resp->body = malloc(1))) {
    snprintf(resp->error, sizeof resp->error, "not enough memory");
    sceHttp2DeleteRequest(req);
    return -1;
  }
  resp->body[resp->len] = 0;
  sceHttp2DeleteRequest(req);
  return 0;
}

#else
/* --------------------------------------------------------- host: curl --- */
#include <curl/curl.h>

static size_t
http_curl_write(char *data, size_t size, size_t n, void *arg) {
  http_response_t *resp = arg;
  size_t add = size * n;
  char *nb;

  if(resp->len + add > HTTP_MAX_RESPONSE) return 0;
  if(!(nb = realloc(resp->body, resp->len + add + 1))) return 0;
  resp->body = nb;
  memcpy(resp->body + resp->len, data, add);
  resp->len += add;
  resp->body[resp->len] = 0;
  return add;
}


static int
http_request_locked(const char *method, const char *url, const char *const *headers,
                    int nheaders, const void *body, size_t body_len,
                    http_response_t *resp) {
  static int inited;
  struct curl_slist *list = NULL;
  CURL *c;
  CURLcode rc;
  long status = 0;

  if(!inited) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    inited = 1;
  }
  if(!(c = curl_easy_init())) {
    snprintf(resp->error, sizeof resp->error, "curl init failed");
    return -1;
  }
  for(int i = 0; i < nheaders; i++) list = curl_slist_append(list, headers[i]);
  curl_easy_setopt(c, CURLOPT_URL, url);
  curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method);
  curl_easy_setopt(c, CURLOPT_HTTPHEADER, list);
  curl_easy_setopt(c, CURLOPT_USERAGENT, HTTP_USER_AGENT);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, (long)HTTP_TIMEOUT_SEC * 4);
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, http_curl_write);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, resp);
  curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
  if(body_len || !strcmp(method, "POST") || !strcmp(method, "PATCH") || !strcmp(method, "PUT")) {
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body ? body : "");
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)body_len);
  }
  rc = curl_easy_perform(c);
  if(rc != CURLE_OK) {
    snprintf(resp->error, sizeof resp->error, "%s", curl_easy_strerror(rc));
    curl_slist_free_all(list);
    curl_easy_cleanup(c);
    return -1;
  }
  curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
  resp->status = (int)status;
  curl_slist_free_all(list);
  curl_easy_cleanup(c);
  if(!resp->body && !(resp->body = calloc(1, 1))) {
    snprintf(resp->error, sizeof resp->error, "not enough memory");
    return -1;
  }
  return 0;
}
#endif


/**
 * Performs one request. Returns 0 with resp->status and resp->body set, or
 * -1 with resp->error. The caller frees resp with http_response_free().
 **/
static int
http_request(const char *method, const char *url, const char *const *headers,
             int nheaders, const void *body, size_t body_len, http_response_t *resp) {
  int rc;

  memset(resp, 0, sizeof *resp);
  resp->status = -1;
  pthread_mutex_lock(&http_mutex);
  rc = http_request_locked(method, url, headers, nheaders, body, body_len, resp);
  pthread_mutex_unlock(&http_mutex);
  if(rc) {
    http_response_free(resp);
  }
  return rc;
}

#endif
