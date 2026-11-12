#!/usr/bin/env python3
"""A stand-in for Google's OAuth and Drive endpoints, for the host test.

Usage: fake_google.py <port> <state dir>

OAuth: POST /device/code, POST /token, POST /revoke.
Drive: GET /drive/v3/files?q=..., POST /drive/v3/files,
       PATCH /upload/drive/v3/files/<id>?uploadType=media.
Test controls: GET /_dump (store as JSON), POST /_delete/<id>, POST /_fail/<n>
(next n Drive calls answer 500), and files in the state dir: "revoked"
makes refresh fail with invalid_grant, "deny" makes the device flow fail.
"""
import json, os, re, sys, threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

PORT = int(sys.argv[1])
STATE = sys.argv[2]
lock = threading.Lock()
store = {}          # id -> {name, parent, mime, content (bytes)}
counters = {"polls": 0, "refreshes": 0, "fail": 0, "ids": 0, "api_calls": 0}
tokens = {"access": None, "refresh": None}


def new_id():
    counters["ids"] += 1
    return "id-%d" % counters["ids"]


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def send_json(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def body(self):
        n = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(n) if n else b""

    def authed(self):
        auth = self.headers.get("Authorization", "")
        with lock:
            ok = tokens["access"] is not None and auth == "Bearer " + tokens["access"]
        if not ok:
            self.send_json(401, {"error": {"code": 401, "message": "Invalid Credentials"}})
        return ok

    def failing(self):
        with lock:
            if counters["fail"] > 0:
                counters["fail"] -= 1
                return True
        return False

    def do_POST(self):
        u = urlparse(self.path)
        if u.path == "/device/code":
            form = parse_qs(self.body().decode())
            assert form.get("client_id") and form.get("scope") == ["https://www.googleapis.com/auth/drive.file"], form
            return self.send_json(200, {"device_code": "dev-1", "user_code": "ABCD-EFGH",
                                        "verification_url": "https://www.google.com/device",
                                        "expires_in": 1800, "interval": 1})
        if u.path == "/token":
            form = parse_qs(self.body().decode())
            grant = form.get("grant_type", [""])[0]
            if grant == "urn:ietf:params:oauth:grant-type:device_code":
                assert form.get("client_secret"), "client_secret missing"
                if os.path.exists(os.path.join(STATE, "deny")):
                    return self.send_json(403, {"error": "access_denied"})
                with lock:
                    counters["polls"] += 1
                    if counters["polls"] < 3:
                        return self.send_json(428, {"error": "authorization_pending"})
                    tokens["access"], tokens["refresh"] = "acc-1", "ref-1"
                return self.send_json(200, {"access_token": "acc-1", "refresh_token": "ref-1",
                                            "expires_in": 3600, "token_type": "Bearer"})
            if grant == "refresh_token":
                if os.path.exists(os.path.join(STATE, "revoked")):
                    return self.send_json(400, {"error": "invalid_grant", "error_description": "Token has been expired or revoked."})
                with lock:
                    if form.get("refresh_token", [""])[0] != tokens["refresh"]:
                        return self.send_json(400, {"error": "invalid_grant"})
                    counters["refreshes"] += 1
                    tokens["access"] = "acc-%d" % (counters["refreshes"] + 1)
                    acc = tokens["access"]
                return self.send_json(200, {"access_token": acc, "expires_in": 3600, "token_type": "Bearer"})
            return self.send_json(400, {"error": "unsupported_grant_type"})
        if u.path == "/revoke":
            return self.send_json(200, {})
        if u.path == "/drive/v3/files":
            if not self.authed():
                return
            if self.failing():
                return self.send_json(500, {"error": {"code": 500, "message": "Backend Error"}})
            meta = json.loads(self.body().decode())
            with lock:
                counters["api_calls"] += 1
                fid = new_id()
                store[fid] = {"name": meta["name"], "parent": meta.get("parents", ["root"])[0],
                              "mime": meta.get("mimeType", "application/octet-stream"), "content": b""}
            return self.send_json(200, {"id": fid})
        if u.path.startswith("/_delete/"):
            with lock:
                store.pop(u.path.split("/")[-1], None)
            return self.send_json(200, {})
        if u.path.startswith("/_fail/"):
            with lock:
                counters["fail"] = int(u.path.split("/")[-1])
            return self.send_json(200, {})
        self.send_json(404, {"error": "no such endpoint"})

    def do_PATCH(self):
        u = urlparse(self.path)
        m = re.match(r"^/upload/drive/v3/files/([^/]+)$", u.path)
        if not m or "uploadType=media" not in (u.query or ""):
            return self.send_json(404, {"error": "no such endpoint"})
        if not self.authed():
            return
        if self.failing():
            return self.send_json(500, {"error": {"code": 500, "message": "Backend Error"}})
        data = self.body()
        with lock:
            counters["api_calls"] += 1
            f = store.get(m.group(1))
            if not f:
                return self.send_json(404, {"error": {"code": 404, "message": "File not found"}})
            f["content"] = data
        return self.send_json(200, {"id": m.group(1)})

    def do_GET(self):
        u = urlparse(self.path)
        if u.path == "/drive/v3/files":
            if not self.authed():
                return
            if self.failing():
                return self.send_json(500, {"error": {"code": 500, "message": "Backend Error"}})
            q = parse_qs(u.query).get("q", [""])[0]
            name = re.search(r"name = '((?:[^'\\]|\\.)*)'", q)
            parent = re.search(r"'([^']+)' in parents", q)
            name = name.group(1).replace("\\'", "'").replace("\\\\", "\\") if name else None
            parent = parent.group(1) if parent else "root"
            with lock:
                counters["api_calls"] += 1
                found = [{"id": i} for i, f in store.items()
                         if f["name"] == name and f["parent"] == parent and f["mime"].endswith("folder")]
            return self.send_json(200, {"files": found[:1]})
        if u.path == "/_dump":
            with lock:
                out = {"counters": dict(counters), "tokens": dict(tokens), "files": {}}
                for i, f in store.items():
                    out["files"][i] = {"name": f["name"], "parent": f["parent"], "mime": f["mime"],
                                       "size": len(f["content"]),
                                       "content": f["content"].decode("utf-8", "replace") if len(f["content"]) < 200 else None}
            return self.send_json(200, out)
        self.send_json(404, {"error": "no such endpoint"})


class Server(ThreadingHTTPServer):
    def handle_error(self, request, client_address):
        pass            # a client that hung up mid-request is not interesting


Server(("127.0.0.1", PORT), H).serve_forever()
