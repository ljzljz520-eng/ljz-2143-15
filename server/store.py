"""SQLite 存储层：设备配置版本、用户会话偏好、变更日志、冲突记录、诊断副本。

三层字段存储：
  layer='admin'  scope=组名        —— Web 管理端维护的远程默认配置
  layer='user'   scope=用户 id     —— 用户会话偏好（跨设备同步）
  layer='device' scope=设备 id     —— 机器相关状态（显示器坐标，绝不跨设备应用）
"""

import json
import sqlite3
import threading
import uuid
from datetime import datetime, timezone

SCHEMA = """
CREATE TABLE IF NOT EXISTS meta (
  key TEXT PRIMARY KEY,
  value TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS fields (
  layer TEXT NOT NULL,
  scope TEXT NOT NULL,
  field TEXT NOT NULL,
  value TEXT NOT NULL,
  version INTEGER NOT NULL,
  source TEXT NOT NULL,
  updated_at TEXT NOT NULL,
  PRIMARY KEY (layer, scope, field)
);
CREATE TABLE IF NOT EXISTS changes (
  seq INTEGER PRIMARY KEY AUTOINCREMENT,
  layer TEXT NOT NULL,
  scope TEXT NOT NULL,
  field TEXT NOT NULL,
  value TEXT NOT NULL,
  version INTEGER NOT NULL,
  source TEXT NOT NULL,
  ts TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS conflicts (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  ts TEXT NOT NULL,
  layer TEXT NOT NULL,
  scope TEXT NOT NULL,
  field TEXT NOT NULL,
  base_value TEXT,
  incoming_value TEXT,
  incoming_source TEXT,
  current_value TEXT,
  current_source TEXT,
  winner TEXT NOT NULL,
  reason TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS devices (
  device_id TEXT PRIMARY KEY,
  user TEXT,
  last_seen TEXT,
  last_seq INTEGER,
  schema_version INTEGER
);
CREATE TABLE IF NOT EXISTS diagnostics (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  ts TEXT NOT NULL,
  device_id TEXT,
  kind TEXT,
  filename TEXT,
  content TEXT
);
"""

DB_SCHEMA_VERSION = 1


def utcnow():
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds")


class Store:
    def __init__(self, path):
        self._lock = threading.RLock()
        self._db = sqlite3.connect(path, check_same_thread=False)
        self._db.row_factory = sqlite3.Row
        self._db.execute("PRAGMA journal_mode=WAL")
        self._db.execute("PRAGMA synchronous=FULL")
        with self._lock:
            self._db.executescript(SCHEMA)
            if self.get_meta("db_schema_version") is None:
                self.set_meta("db_schema_version", str(DB_SCHEMA_VERSION))
            if self.get_meta("epoch") is None:
                # epoch 在库创建时生成；服务器回退到旧库时 epoch 会变，
                # 客户端据此识别“这不是我上次同步的那个服务器历史”。
                self.set_meta("epoch", uuid.uuid4().hex)
            self._db.commit()

    def close(self):
        with self._lock:
            self._db.close()

    # ---- meta ----
    def get_meta(self, key):
        with self._lock:
            row = self._db.execute("SELECT value FROM meta WHERE key=?", (key,)).fetchone()
            return row["value"] if row else None

    def set_meta(self, key, value):
        with self._lock:
            self._db.execute(
                "INSERT INTO meta(key,value) VALUES(?,?) "
                "ON CONFLICT(key) DO UPDATE SET value=excluded.value",
                (key, value),
            )
            self._db.commit()

    @property
    def epoch(self):
        return self.get_meta("epoch")

    # ---- fields ----
    def get_field(self, layer, scope, field):
        with self._lock:
            row = self._db.execute(
                "SELECT * FROM fields WHERE layer=? AND scope=? AND field=?",
                (layer, scope, field),
            ).fetchone()
            return dict(row) if row else None

    def put_field(self, layer, scope, field, value_json, source, ts):
        """写入字段（版本 +1）并记录变更日志，返回新版本号。"""
        with self._lock:
            cur = self.get_field(layer, scope, field)
            version = (cur["version"] + 1) if cur else 1
            self._db.execute(
                "INSERT INTO fields(layer,scope,field,value,version,source,updated_at)"
                " VALUES(?,?,?,?,?,?,?) "
                "ON CONFLICT(layer,scope,field) DO UPDATE SET"
                " value=excluded.value, version=excluded.version,"
                " source=excluded.source, updated_at=excluded.updated_at",
                (layer, scope, field, value_json, version, source, ts),
            )
            self._db.execute(
                "INSERT INTO changes(layer,scope,field,value,version,source,ts)"
                " VALUES(?,?,?,?,?,?,?)",
                (layer, scope, field, value_json, version, source, ts),
            )
            self._db.commit()
            return version

    def list_fields(self, layer, scope):
        with self._lock:
            rows = self._db.execute(
                "SELECT * FROM fields WHERE layer=? AND scope=? ORDER BY field",
                (layer, scope),
            ).fetchall()
            return [dict(r) for r in rows]

    # ---- change log ----
    def max_seq(self):
        with self._lock:
            row = self._db.execute("SELECT MAX(seq) AS s FROM changes").fetchone()
            return row["s"] or 0

    def min_seq(self):
        with self._lock:
            row = self._db.execute("SELECT MIN(seq) AS s FROM changes").fetchone()
            return row["s"] or 0

    def changes_since(self, seq, scopes):
        """scopes: list of (layer, scope)；返回 seq 之后、属于这些作用域的变更。"""
        with self._lock:
            out = []
            for layer, scope in scopes:
                rows = self._db.execute(
                    "SELECT * FROM changes WHERE seq>? AND layer=? AND scope=? ORDER BY seq",
                    (seq, layer, scope),
                ).fetchall()
                out.extend(dict(r) for r in rows)
            out.sort(key=lambda r: r["seq"])
            return out

    def prune_changes_before(self, seq):
        """测试用：裁剪变更日志，模拟长离线设备超出保留窗。"""
        with self._lock:
            self._db.execute("DELETE FROM changes WHERE seq<?", (seq,))
            self._db.commit()

    # ---- conflicts ----
    def log_conflict(self, layer, scope, field, base_value, incoming_value,
                     incoming_source, current_value, current_source, winner, reason):
        with self._lock:
            self._db.execute(
                "INSERT INTO conflicts(ts,layer,scope,field,base_value,incoming_value,"
                "incoming_source,current_value,current_source,winner,reason)"
                " VALUES(?,?,?,?,?,?,?,?,?,?,?)",
                (utcnow(), layer, scope, field, base_value, incoming_value,
                 incoming_source, current_value, current_source, winner, reason),
            )
            self._db.commit()

    def list_conflicts(self, limit=200):
        with self._lock:
            rows = self._db.execute(
                "SELECT * FROM conflicts ORDER BY id DESC LIMIT ?", (limit,)
            ).fetchall()
            return [dict(r) for r in rows]

    # ---- devices ----
    def touch_device(self, device_id, user, last_seq, schema_version):
        with self._lock:
            self._db.execute(
                "INSERT INTO devices(device_id,user,last_seen,last_seq,schema_version)"
                " VALUES(?,?,?,?,?) "
                "ON CONFLICT(device_id) DO UPDATE SET user=excluded.user,"
                " last_seen=excluded.last_seen, last_seq=excluded.last_seq,"
                " schema_version=excluded.schema_version",
                (device_id, user, utcnow(), last_seq, schema_version),
            )
            self._db.commit()

    def list_devices(self):
        with self._lock:
            rows = self._db.execute("SELECT * FROM devices ORDER BY device_id").fetchall()
            return [dict(r) for r in rows]

    # ---- diagnostics ----
    def add_diagnostic(self, device_id, kind, filename, content):
        with self._lock:
            self._db.execute(
                "INSERT INTO diagnostics(ts,device_id,kind,filename,content)"
                " VALUES(?,?,?,?,?)",
                (utcnow(), device_id, kind, filename, content[:65536]),
            )
            self._db.commit()

    def list_diagnostics(self, limit=100):
        with self._lock:
            rows = self._db.execute(
                "SELECT id,ts,device_id,kind,filename,length(content) AS size"
                " FROM diagnostics ORDER BY id DESC LIMIT ?",
                (limit,),
            ).fetchall()
            return [dict(r) for r in rows]

    def get_diagnostic(self, diag_id):
        with self._lock:
            row = self._db.execute(
                "SELECT * FROM diagnostics WHERE id=?", (diag_id,)
            ).fetchone()
            return dict(row) if row else None


def dumps_value(v):
    return json.dumps(v, ensure_ascii=False, sort_keys=True)
