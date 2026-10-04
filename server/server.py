"""状态同步后台：HTTP API + SQLite 存储 + Web 管理端页面。

端点：
  GET  /api/health
  GET  /api/pull?device_id&user&since_seq&epoch     增量 / 快照 / resync 判定
  POST /api/push                                    客户端 journal 推送（字段级合并）
  GET  /api/admin/defaults                          远程默认配置（admin 层）
  POST /api/admin/defaults                          Web 管理端保存默认配置
  GET  /api/admin/conflicts                         原始冲突与胜方
  GET  /api/admin/effective?device_id&user          生效配置与最终来源
  GET  /api/admin/devices                           设备列表
  GET  /api/admin/diagnostics                       损坏配置诊断副本列表
  GET  /api/admin/diagnostics/<id>                  诊断副本内容
  POST /api/diagnostics                             客户端上传诊断副本
  POST /api/test/prune                              （--allow-test-ops）裁剪变更日志
  GET  /                                            管理页面
"""

import argparse
import json
import signal
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlparse, parse_qs

from . import merge
from .migrate import migrate, CONFIG_SCHEMA, MigrationError
from .store import Store, utcnow

WEB_ROOT = Path(__file__).resolve().parent.parent / "web"


class SyncServer(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, addr, handler, store, allow_test_ops=False):
        super().__init__(addr, handler)
        self.store = store
        self.allow_test_ops = allow_test_ops
        self.io_lock = threading.Lock()


class Handler(BaseHTTPRequestHandler):
    server_version = "StateSync/1.0"

    # ---------- helpers ----------
    def _json_body(self):
        length = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(length) if length else b"{}"
        try:
            return json.loads(raw.decode("utf-8")), None
        except (ValueError, UnicodeDecodeError) as exc:
            return None, str(exc)

    def _send_json(self, obj, status=200):
        body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _send_text(self, text, status=200, content_type="text/html; charset=utf-8"):
        body = text.encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):  # 静默，测试输出更干净
        pass

    # ---------- routing ----------
    def do_GET(self):
        parsed = urlparse(self.path)
        path, qs = parsed.path, parse_qs(parsed.query)
        if path == "/":
            return self._send_text((WEB_ROOT / "admin.html").read_text(encoding="utf-8"))
        if path == "/api/health":
            st = self.server.store
            return self._send_json({
                "ok": True, "epoch": st.epoch, "server_seq": st.max_seq(),
                "schema_version": CONFIG_SCHEMA,
            })
        if path == "/api/pull":
            return self._handle_pull(qs)
        if path == "/api/admin/defaults":
            fields = self.server.store.list_fields("admin", merge.ADMIN_SCOPE)
            return self._send_json({"fields": fields, "server_seq": self.server.store.max_seq()})
        if path == "/api/admin/conflicts":
            return self._send_json({"conflicts": self.server.store.list_conflicts()})
        if path == "/api/admin/devices":
            return self._send_json({"devices": self.server.store.list_devices()})
        if path == "/api/admin/diagnostics":
            return self._send_json({"diagnostics": self.server.store.list_diagnostics()})
        if path.startswith("/api/admin/diagnostics/"):
            diag = self.server.store.get_diagnostic(int(path.rsplit("/", 1)[1]))
            if not diag:
                return self._send_json({"error": "not found"}, 404)
            return self._send_json(diag)
        if path == "/api/admin/effective":
            device_id = qs.get("device_id", [""])[0]
            user = qs.get("user", [""])[0]
            return self._send_json({
                "fields": merge.effective_fields(self.server.store, device_id, user),
                "server_seq": self.server.store.max_seq(),
                "epoch": self.server.store.epoch,
            })
        return self._send_json({"error": "not found"}, 404)

    def do_POST(self):
        parsed = urlparse(self.path)
        path = parsed.path
        body, err = self._json_body()
        if err:
            return self._send_json({"error": f"bad json: {err}"}, 400)
        if path == "/api/push":
            return self._handle_push(body)
        if path == "/api/admin/defaults":
            return self._handle_admin_save(body)
        if path == "/api/diagnostics":
            self.server.store.add_diagnostic(
                body.get("device_id", ""), body.get("kind", "corrupt"),
                body.get("filename", ""), body.get("content", ""),
            )
            return self._send_json({"ok": True})
        if path == "/api/test/prune" and self.server.allow_test_ops:
            self.server.store.prune_changes_before(int(body.get("before_seq", 0)))
            return self._send_json({"ok": True, "min_seq": self.server.store.min_seq()})
        return self._send_json({"error": "not found"}, 404)

    # ---------- sync protocol ----------
    def _handle_pull(self, qs):
        store = self.server.store
        device_id = qs.get("device_id", [""])[0]
        user = qs.get("user", [""])[0]
        since_seq = int(qs.get("since_seq", ["0"])[0])
        client_epoch = qs.get("epoch", [""])[0]
        max_seq = store.max_seq()

        with self.server.io_lock:
            resync, reason = self._needs_resync(client_epoch, since_seq, max_seq)
            if resync:
                payload = {
                    "resync_required": True,
                    "reason": reason,
                    "snapshot": merge.snapshot(store, device_id, user),
                }
            elif since_seq == 0:
                payload = {"resync_required": False,
                           "snapshot": merge.snapshot(store, device_id, user)}
            else:
                payload = {"resync_required": False,
                           "changed_fields": merge.delta_since(store, device_id, user, since_seq)}
            store.touch_device(device_id, user, max_seq, 0)
            payload.update({
                "epoch": store.epoch,
                "server_seq": max_seq,
                "schema_version": CONFIG_SCHEMA,
            })
        return self._send_json(payload)

    def _needs_resync(self, client_epoch, since_seq, max_seq):
        store = self.server.store
        if client_epoch and client_epoch != store.epoch:
            return True, "epoch_mismatch"          # 服务器换库 / 回退到另一份历史
        if since_seq > max_seq:
            return True, "server_rolled_back"      # 客户端基线比服务器还新：服务器回退
        min_seq = store.min_seq()
        if since_seq > 0 and min_seq > 0 and since_seq < min_seq - 1:
            return True, "delta_window_exceeded"   # 长离线，变更日志已裁剪
        return False, ""

    def _handle_push(self, body):
        store = self.server.store
        device_id = body.get("device_id", "")
        user = body.get("user", "")
        base_seq = int(body.get("base_seq") or 0)
        client_epoch = body.get("epoch", "")
        changes = body.get("changes", [])

        with self.server.io_lock:
            resync, reason = self._needs_resync(client_epoch, base_seq, store.max_seq())
            results = merge.apply_push(store, device_id, user, changes)
            store.touch_device(device_id, user, store.max_seq(),
                               int(body.get("schema_version") or 0))
            return self._send_json({
                "results": results,
                "server_seq": store.max_seq(),
                "epoch": store.epoch,
                "resync_required": resync,
                "reason": reason,
            })

    def _handle_admin_save(self, body):
        admin_id = body.get("admin", "web")
        changes = body.get("changes", [])
        with self.server.io_lock:
            results = merge.apply_admin(self.server.store, changes, admin_id)
            return self._send_json({
                "results": results,
                "server_seq": self.server.store.max_seq(),
            })


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--db", default="state_sync.db")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8000)
    parser.add_argument("--allow-test-ops", action="store_true")
    args = parser.parse_args()

    store = Store(args.db)
    server = SyncServer((args.host, args.port), Handler, store,
                        allow_test_ops=args.allow_test_ops)
    actual_port = server.server_address[1]
    print(f"PORT {actual_port}", flush=True)
    print(f"serving on http://{args.host}:{actual_port} db={args.db}", flush=True)

    def _stop(signum, frame):
        raise KeyboardInterrupt

    # 优雅关闭：确保 SQLite 最后一个连接关闭时 checkpoint WAL，
    # 否则直接 kill 会让最近写入只存在于 -wal 文件，文件级备份会丢数据。
    signal.signal(signal.SIGTERM, _stop)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
        store.close()


if __name__ == "__main__":
    main()
