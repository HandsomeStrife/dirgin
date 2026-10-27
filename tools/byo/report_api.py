#!/usr/bin/env python3
"""dirge-play report API: POST /api/report (proxied by nginx) -> one directory per report.

OPTIONAL. The viewer's Report button (R) posts here; without it the pages work and only
the Report button fails. tools/byo/serve.sh docker runs it next to nginx, with reports
written to tools/byo/serve-docker/reports/ (gitignored).

Body: JSON {meta: {...}, marked_png: <base64>, plain_png: <base64>}, at most MAX_BODY bytes.
Writes REPORTS/<id>/{meta.json, marked.png, plain.png} and answers {"id": "<id>"}.
<id> = UTC timestamp + 4 random hex chars, e.g. 20261026T163012Z-3fa1. Nothing is
written outside REPORTS; the directory is created with O_EXCL semantics (os.mkdir).
Runs as the uid serve.sh passes (the user who started it), so files are owned by that user.
"""
import base64, binascii, json, os, secrets, sys, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

REPORTS = os.environ.get("REPORTS_DIR", "/reports")
MAX_BODY = 20 * 1024 * 1024
PNG_MAGIC = b"\x89PNG\r\n\x1a\n"


def bad(msg):
    raise ValueError(msg)


def validate(doc):
    if not isinstance(doc, dict):
        bad("body must be a JSON object")
    meta = doc.get("meta")
    if not isinstance(meta, dict):
        bad("meta must be an object")
    for k, t in (("time", str), ("ua", str), ("note", str), ("camera", dict), ("rect", dict), ("canvas", dict)):
        if not isinstance(meta.get(k), t):
            bad(f"meta.{k} missing or not {t.__name__}")
    if len(meta["note"]) > 4000:
        bad("note longer than 4000 chars")
    cam = meta["camera"]
    for k in ("eye", "dir"):
        v = cam.get(k)
        if not (isinstance(v, list) and len(v) == 3 and all(isinstance(x, (int, float)) for x in v)):
            bad(f"camera.{k} must be 3 numbers")
    if not isinstance(cam.get("cam"), str) or len(cam["cam"]) > 200:
        bad("camera.cam must be a short string")
    px = meta["rect"].get("px")
    if not (isinstance(px, dict) and all(isinstance(px.get(k), (int, float)) for k in "xywh")):
        bad("rect.px must have numeric x y w h")
    if "picks" in meta and not isinstance(meta["picks"], list):
        bad("picks must be a list")
    imgs = {}
    for k in ("marked_png", "plain_png"):
        s = doc.get(k)
        if not isinstance(s, str):
            bad(f"{k} missing")
        try:
            b = base64.b64decode(s, validate=True)
        except (binascii.Error, ValueError):
            bad(f"{k} is not base64")
        if not b.startswith(PNG_MAGIC):
            bad(f"{k} is not a PNG")
        imgs[k] = b
    return meta, imgs


class H(BaseHTTPRequestHandler):
    server_version = "dirge-report/1"

    def reply(self, code, obj):
        b = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)

    def do_GET(self):
        if self.path.rstrip("/") == "/api/health":
            return self.reply(200, {"ok": True})
        self.reply(404, {"error": "not found"})

    def do_POST(self):
        if self.path.split("?")[0] != "/api/report":
            return self.reply(404, {"error": "not found"})
        # Only application/json: it forces a CORS preflight, so other web pages cannot post here.
        if self.headers.get("Content-Type", "").split(";")[0].strip().lower() != "application/json":
            return self.reply(415, {"error": "Content-Type must be application/json"})
        try:
            n = int(self.headers.get("Content-Length", ""))
        except ValueError:
            return self.reply(411, {"error": "Content-Length required"})
        if n <= 0 or n > MAX_BODY:
            return self.reply(413, {"error": f"body must be 1..{MAX_BODY} bytes"})
        raw = self.rfile.read(n)
        try:
            meta, imgs = validate(json.loads(raw))
        except (ValueError, RecursionError) as e:    # JSONDecodeError is a ValueError; deep nesting
            return self.reply(400, {"error": str(e)[:300]})
        rid = time.strftime("%Y%m%dT%H%M%SZ", time.gmtime()) + "-" + secrets.token_hex(2)
        d = os.path.join(REPORTS, rid)
        try:
            os.mkdir(d, 0o775)
        except FileExistsError:
            return self.reply(503, {"error": "id collision, retry"})
        meta = dict(meta, id=rid, received=time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                    client=self.headers.get("X-Real-IP", self.client_address[0]))
        with open(os.path.join(d, "marked.png"), "wb") as f:
            f.write(imgs["marked_png"])
        with open(os.path.join(d, "plain.png"), "wb") as f:
            f.write(imgs["plain_png"])
        with open(os.path.join(d, "meta.json"), "w") as f:
            json.dump(meta, f, indent=1)
        sys.stderr.write(f"report {rid}: {meta['note'][:80]!r}\n")
        self.reply(201, {"id": rid})


if __name__ == "__main__":
    os.makedirs(REPORTS, exist_ok=True)
    ThreadingHTTPServer(("0.0.0.0", 8000), H).serve_forever()   # container-internal only (not published)
