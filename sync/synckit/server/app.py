"""HTTP front end: /api/sync, admin JSON APIs, and the web admin page."""
from __future__ import annotations

import json
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlparse

try:
    from .db import ServerDB
    from .engine import SyncEngine
except ImportError:  # allow `python synckit/server/app.py` as well
    import os, sys
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__)))))
    from synckit.server.db import ServerDB
    from synckit.server.engine import SyncEngine

STATIC_DIR = Path(__file__).parent / "static"


def _json_series(rows):
    return [dict(r) for r in rows]


class _Handler(BaseHTTPRequestHandler):
    server_version = "StateSync/1.0"

    def log_message(self, fmt, *args):  # quiet by default
        if getattr(self.server, "verbose", False):
            super().log_message(fmt, *args)

    # ------------------------------------------------------------------ #
    def _send(self, code, payload, ctype="application/json; charset=utf-8"):
        body = (payload if isinstance(payload, bytes)
                else json.dumps(payload, ensure_ascii=False).encode("utf-8"))
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _read_json(self):
        length = int(self.headers.get("Content-Length", 0))
        if not length:
            return {}
        return json.loads(self.rfile.read(length).decode("utf-8"))

    def _error(self, code, message):
        self._send(code, {"error": message})

    # ------------------------------------------------------------------ #
    def do_GET(self):
        url = urlparse(self.path)
        path = url.path
        try:
            if path == "/":
                return self._serve_static("index.html", "text/html; charset=utf-8")
            if path.startswith("/static/"):
                return self._serve_static(path.split("/static/", 1)[1])
            if path == "/api/health":
                return self._send(200, {"ok": True})
            if path == "/api/admin/devices":
                return self._send(200, _json_series(
                    self.db.query("SELECT * FROM devices ORDER BY seen_at DESC")))
            if path.startswith("/api/admin/groups/") and \
                    path.endswith("/defaults"):
                group_id = path.split("/")[4]
                head = self.db.group_head(group_id)
                if head is None:
                    return self._send(404, {"error": "unknown group"})
                return self._send(200, {
                    "group_id": group_id,
                    "version": head["version"],
                    "doc": json.loads(head["doc_json"]),
                    "enforced": json.loads(head["enforced_json"]),
                    "mode": head["mode"], "note": head["note"],
                    "rolled_back_from": head["rolled_back_from"],
                    "created_at": head["created_at"]})
            if path.startswith("/api/admin/groups/") and \
                    path.endswith("/versions"):
                group_id = path.split("/")[4]
                rows = self.db.query(
                    "SELECT version,doc_json,enforced_json,mode,published_by,"
                    "note,rolled_back_from,created_at FROM group_versions "
                    "WHERE group_id=? ORDER BY version", (group_id,))
                out = []
                for r in rows:
                    d = dict(r)
                    d["doc_json"] = json.loads(d["doc_json"])
                    d["enforced_json"] = json.loads(d["enforced_json"])
                    out.append(d)
                return self._send(200, out)
            if path == "/api/admin/conflicts":
                rows = self.db.query(
                    "SELECT * FROM profile_audit ORDER BY id DESC LIMIT 200")
                out = []
                for r in rows:
                    d = dict(r)
                    for k in ("base_json", "server_json", "client_json",
                              "resolved_json"):
                        d[k] = json.loads(d[k])
                    out.append(d)
                return self._send(200, out)
            if path == "/api/admin/diagnostics":
                rows = self.db.query(
                    "SELECT * FROM corrupt_reports ORDER BY id DESC LIMIT 200")
                out = []
                for r in rows:
                    d = dict(r)
                    d["payload_json"] = json.loads(d["payload_json"])
                    out.append(d)
                return self._send(200, out)
            if path.startswith("/api/admin/users/") and \
                    path.endswith("/profiles"):
                parts = path.split("/")
                user_id = parts[4]
                rows = self.db.query(
                    "SELECT p.* FROM user_profile_versions p "
                    "WHERE user_id=? ORDER BY p.version DESC LIMIT 50",
                    (user_id,))
                return self._send(200, _json_series(rows))
            return self._error(404, f"not found: {path}")
        except Exception as e:  # noqa: BLE001
            return self._error(500, str(e))

    def do_POST(self):
        path = urlparse(self.path).path
        try:
            body = self._read_json()
            if path == "/api/sync":
                return self._send(200, self.engine.sync(body))
            if path.startswith("/api/admin/groups/") and \
                    path.endswith("/publish"):
                group_id = path.split("/")[4]
                version = self.db.publish_group(
                    group_id, body["doc"], body.get("enforced", []),
                    body.get("by", "admin"), body.get("note", ""),
                    mode=body.get("mode", "merge"))
                return self._send(200, {"group_id": group_id,
                                        "version": version})
            if path.startswith("/api/admin/groups/") and \
                    path.endswith("/rollback"):
                group_id = path.split("/")[4]
                version = self.db.rollback_group(
                    group_id, int(body["version"]), body.get("by", "admin"))
                return self._send(200, {"group_id": group_id,
                                        "version": version})
            if path == "/api/corrupt-report":
                self.db.add_corrupt_report(
                    body.get("device_id", "unknown"),
                    body.get("user_id", ""), body.get("kind", "unknown"),
                    body.get("payload", body))
                return self._send(200, {"ok": True})
            return self._error(404, f"not found: {path}")
        except KeyError as e:
            return self._error(400, f"missing field: {e}")
        except json.JSONDecodeError:
            return self._error(400, "invalid json")
        except Exception as e:  # noqa: BLE001
            return self._error(500, str(e))

    def _serve_static(self, name, ctype="application/javascript"):
        target = (STATIC_DIR / name).resolve()
        if not str(target).startswith(str(STATIC_DIR.resolve())) or \
                not target.is_file():
            return self._error(404, f"no such asset: {name}")
        self._send(200, target.read_bytes(), ctype)

    @property
    def db(self) -> ServerDB:
        return self.server.db  # type: ignore[attr-defined]

    @property
    def engine(self) -> SyncEngine:
        return self.server.engine  # type: ignore[attr-defined]


def create_server(db_path: str = ":memory:", host: str = "127.0.0.1",
                  port: int = 8080, verbose: bool = False):
    db = ServerDB(db_path)
    engine = SyncEngine(db)
    httpd = ThreadingHTTPServer((host, port), _Handler)
    httpd.db = db            # type: ignore[attr-defined]
    httpd.engine = engine    # type: ignore[attr-defined]
    httpd.verbose = verbose  # type: ignore[attr-defined]
    return httpd


def serve_forever_in_thread(httpd) -> threading.Thread:
    t = threading.Thread(target=httpd.serve_forever, daemon=True)
    t.start()
    return t


if __name__ == "__main__":
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", default="data/state-sync.db")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8080)
    args = ap.parse_args()
    srv = create_server(args.db, args.host, args.port, verbose=True)
    print(f"state-sync server on http://{args.host}:{args.port}")
    srv.serve_forever()
