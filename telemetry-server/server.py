"""Somniel telemetry server: receives opt-in anonymous perf/crash reports, stores them in SQLite, shows a dashboard.

Standard library only (no pip). Config: config.json next to this file, created by setup.ps1:
  {"port": 8787, "ingest_key": "...", "admin_user": "admin", "admin_password": "..."}

POST /v1/ingest   (header X-Somniel-Key)  one JSON report from the app
GET  /dashboard   (HTTP basic auth)       stats page
GET  /health                              "ok"

Client IP addresses are never stored or logged.
"""
import base64
import hmac
import html
import json
import os
import re
import sqlite3
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
CFG = json.load(open(os.path.join(HERE, "config.json"), encoding="utf-8"))
DB_PATH = os.path.join(HERE, "data", "telemetry.db")
MAX_BODY = 8192
RATE_PER_HOUR = 60  # reports per install id per hour

os.makedirs(os.path.dirname(DB_PATH), exist_ok=True)
db = sqlite3.connect(DB_PATH, check_same_thread=False)
db_lock = threading.Lock()
with db_lock:
    db.executescript("""
    PRAGMA journal_mode=WAL;
    CREATE TABLE IF NOT EXISTS devices(
      install TEXT PRIMARY KEY, first_seen INTEGER, last_seen INTEGER, app TEXT, model TEXT, soc TEXT,
      gpu TEXT, vk_driver TEXT, android TEXT, ram_gb INTEGER);
    CREATE TABLE IF NOT EXISTS perf(
      id INTEGER PRIMARY KEY, ts INTEGER, install TEXT, app TEXT, gpu TEXT, soc TEXT, seconds INTEGER,
      fps_avg REAL, fps_low REAL, longest_ms INTEGER, over50 INTEGER, over100 INTEGER);
    CREATE TABLE IF NOT EXISTS crashes(
      id INTEGER PRIMARY KEY, ts INTEGER, install TEXT, app TEXT, gpu TEXT, soc TEXT, kind TEXT, signal TEXT,
      sig TEXT, frames TEXT);
    CREATE INDEX IF NOT EXISTS perf_ts ON perf(ts);
    CREATE INDEX IF NOT EXISTS crash_ts ON crashes(ts);
    """)

rate = {}  # install -> [window_start, count]
rate_lock = threading.Lock()


def s(v, n=120):
    """Bounded printable string from untrusted input."""
    return re.sub(r"[^\x20-\x7e]", "?", str(v if v is not None else ""))[:n]


def num(v, lo=0, hi=10**9):
    try:
        return max(lo, min(hi, float(v)))
    except (TypeError, ValueError):
        return 0.0


def allowed(install):
    now = time.time()
    with rate_lock:
        w = rate.get(install)
        if not w or now - w[0] > 3600:
            rate[install] = [now, 1]
            return True
        w[1] += 1
        return w[1] <= RATE_PER_HOUR


def ingest(o):
    if not isinstance(o, dict) or o.get("v") != 1:
        return 400
    install = s(o.get("install"), 40)
    if not re.fullmatch(r"[0-9a-f-]{36}", install):
        return 400
    if not allowed(install):
        return 429
    typ = o.get("type")
    d = o.get("device") if isinstance(o.get("device"), dict) else {}
    gpu = s(d.get("vk_name") or d.get("gl_renderer"))
    soc = s(d.get("soc") or d.get("hardware"))
    app = s(o.get("app"), 40)
    now = int(time.time())
    with db_lock:
        db.execute(
            "INSERT INTO devices(install,first_seen,last_seen,app,model,soc,gpu,vk_driver,android,ram_gb) "
            "VALUES(?,?,?,?,?,?,?,?,?,?) ON CONFLICT(install) DO UPDATE SET last_seen=excluded.last_seen,"
            "app=excluded.app,model=excluded.model,soc=excluded.soc,gpu=excluded.gpu,vk_driver=excluded.vk_driver,"
            "android=excluded.android,ram_gb=excluded.ram_gb",
            (install, now, now, app, s(d.get("model")), soc, gpu, s(d.get("vk_driver"), 20), s(d.get("android"), 20),
             int(num(d.get("ram_gb"), 0, 1024))))
        if typ == "perf" and isinstance(o.get("perf"), dict):
            p = o["perf"]
            db.execute("INSERT INTO perf(ts,install,app,gpu,soc,seconds,fps_avg,fps_low,longest_ms,over50,over100) "
                       "VALUES(?,?,?,?,?,?,?,?,?,?,?)",
                       (now, install, app, gpu, soc, int(num(p.get("seconds"), 0, 10**6)),
                        num(p.get("fps_avg"), 0, 1000), num(p.get("fps_low_window"), 0, 1000),
                        int(num(p.get("longest_frame_ms"), 0, 10**7)), int(num(p.get("over_50ms"))),
                        int(num(p.get("over_100ms")))))
        elif typ == "crash" and isinstance(o.get("crash"), dict):
            c = o["crash"]
            frames = [s(c.get("frame%d" % i), 80) for i in range(10) if c.get("frame%d" % i)]
            sig = "|".join(frames[:3])  # grouping key: top three frames
            db.execute("INSERT INTO crashes(ts,install,app,gpu,soc,kind,signal,sig,frames) VALUES(?,?,?,?,?,?,?,?,?)",
                       (now, install, app, gpu, soc, s(c.get("kind"), 10), s(c.get("signal"), 6), sig,
                        "\n".join(frames)))
        else:
            return 400
        db.commit()
    return 204


def q(sql, args=()):
    with db_lock:
        return db.execute(sql, args).fetchall()


def table(headers, rows):
    out = ["<table><tr>" + "".join("<th>%s</th>" % html.escape(h) for h in headers) + "</tr>"]
    for r in rows:
        out.append("<tr>" + "".join("<td>%s</td>" % html.escape(str(x)) for x in r) + "</tr>")
    out.append("</table>")
    return "".join(out)


def dashboard():
    day = int(time.time()) - 86400
    week = int(time.time()) - 7 * 86400
    n_dev = q("SELECT COUNT(*) FROM devices")[0][0]
    n_act = q("SELECT COUNT(*) FROM devices WHERE last_seen>?", (week,))[0][0]
    n_cr = q("SELECT COUNT(*) FROM crashes WHERE ts>?", (week,))[0][0]
    parts = ["<h1>Somniel telemetry</h1>",
             "<p>%d devices ever, %d active in 7 days, %d crashes in 7 days</p>" % (n_dev, n_act, n_cr)]
    parts.append("<h2>Performance by GPU (7 days, time-weighted)</h2>")
    parts.append(table(["GPU", "Devices", "Hours", "Avg FPS", "Worst 5 s window", "Frames >50 ms /h"], [
        (r[0], r[1], round(r[2] / 3600, 1), round(r[3], 1), round(r[4], 1), round(r[5], 1)) for r in q(
            "SELECT gpu, COUNT(DISTINCT install), SUM(seconds), SUM(fps_avg*seconds)/SUM(seconds), MIN(fps_low), "
            "SUM(over50)*3600.0/SUM(seconds) FROM perf WHERE ts>? AND seconds>0 GROUP BY gpu "
            "ORDER BY SUM(seconds) DESC", (week,))]))
    parts.append("<h2>Chipsets (devices seen in 30 days)</h2>")
    parts.append(table(["SoC", "GPU", "Devices"], q(
        "SELECT soc, gpu, COUNT(*) FROM devices WHERE last_seen>? GROUP BY soc, gpu ORDER BY COUNT(*) DESC",
        (int(time.time()) - 30 * 86400,))))
    parts.append("<h2>Crashes grouped by location (30 days)</h2>")
    parts.append(table(["Count", "Devices", "Kind", "Signal", "Top frames", "GPUs", "Last seen"], [
        (r[0], r[1], r[2], r[3], r[4].replace("|", "  <-  "), r[5], time.strftime("%Y-%m-%d %H:%M", time.gmtime(r[6])))
        for r in q("SELECT COUNT(*), COUNT(DISTINCT install), kind, signal, sig, GROUP_CONCAT(DISTINCT gpu), MAX(ts) "
                   "FROM crashes WHERE ts>? GROUP BY kind, signal, sig ORDER BY COUNT(*) DESC LIMIT 50",
                   (int(time.time()) - 30 * 86400,))]))
    parts.append("<h2>Recent performance reports</h2>")
    parts.append(table(["UTC", "App", "GPU", "FPS", "Low", "Longest ms", ">50", ">100"], [
        (time.strftime("%m-%d %H:%M", time.gmtime(r[0])), r[1], r[2], r[3], r[4], r[5], r[6], r[7]) for r in q(
            "SELECT ts, app, gpu, fps_avg, fps_low, longest_ms, over50, over100 FROM perf ORDER BY ts DESC LIMIT 30")]))
    css = ("body{font:14px system-ui,sans-serif;margin:24px;background:#10131c;color:#e6e9f2}"
           "table{border-collapse:collapse;margin:8px 0 24px}th,td{border:1px solid #2c3345;padding:4px 10px;"
           "text-align:left}th{background:#1a2030}h1,h2{font-weight:600}")
    return ("<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width'>"
            "<title>Somniel telemetry</title><style>%s</style>%s" % (css, "".join(parts))).encode()


class H(BaseHTTPRequestHandler):
    server_version = "somniel-telemetry"

    def log_message(self, *a):  # no access log: it would contain client addresses
        pass

    def send(self, code, body=b"", ctype="text/plain", extra=None):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def do_GET(self):
        if self.path == "/health":
            return self.send(200, b"ok")
        if self.path.startswith("/dashboard"):
            auth = self.headers.get("Authorization", "")
            ok = False
            if auth.startswith("Basic "):
                try:
                    u, _, p = base64.b64decode(auth[6:]).decode().partition(":")
                    ok = hmac.compare_digest(u, CFG["admin_user"]) and hmac.compare_digest(p, CFG["admin_password"])
                except Exception:
                    ok = False
            if not ok:
                return self.send(401, b"login required", extra={"WWW-Authenticate": 'Basic realm="somniel"'})
            return self.send(200, dashboard(), "text/html; charset=utf-8")
        self.send(404)

    def do_POST(self):
        if self.path != "/v1/ingest":
            return self.send(404)
        if not hmac.compare_digest(self.headers.get("X-Somniel-Key", ""), CFG["ingest_key"]):
            return self.send(403)
        try:
            n = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            return self.send(400)
        if n <= 0 or n > MAX_BODY:
            return self.send(413)
        try:
            o = json.loads(self.rfile.read(n))
        except Exception:
            return self.send(400)
        try:
            code = ingest(o)
        except Exception as e:  # never let one bad report take the server down
            print("ingest error:", e, file=sys.stderr)
            code = 400
        self.send(code)


if __name__ == "__main__":
    port = int(CFG.get("port", 8787))
    print("Somniel telemetry listening on 127.0.0.1:%d (db %s)" % (port, DB_PATH))
    ThreadingHTTPServer(("127.0.0.1", port), H).serve_forever()
