"""SQLite persistence for configuration versions, sessions, devices, audit."""
from __future__ import annotations

import json
import sqlite3
import threading
import time

SCHEMA = """
CREATE TABLE IF NOT EXISTS group_versions (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  group_id TEXT NOT NULL,
  version INTEGER NOT NULL,
  doc_json TEXT NOT NULL,
  enforced_json TEXT NOT NULL,
  mode TEXT NOT NULL DEFAULT 'merge',
  published_by TEXT NOT NULL,
  note TEXT NOT NULL DEFAULT '',
  rolled_back_from INTEGER,
  lamport INTEGER NOT NULL,
  created_at INTEGER NOT NULL,
  UNIQUE(group_id, version)
);
CREATE TABLE IF NOT EXISTS user_profile_versions (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  user_id TEXT NOT NULL,
  group_id TEXT NOT NULL,
  version INTEGER NOT NULL,
  base_group_version INTEGER NOT NULL,
  leaves_json TEXT NOT NULL,
  lamport INTEGER NOT NULL,
  created_at INTEGER NOT NULL,
  UNIQUE(user_id, group_id, version)
);
CREATE TABLE IF NOT EXISTS user_sessions (
  user_id TEXT PRIMARY KEY,
  version INTEGER NOT NULL,
  leaves_json TEXT NOT NULL,
  lamport INTEGER NOT NULL,
  updated_at INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS devices (
  device_id TEXT PRIMARY KEY,
  user_id TEXT NOT NULL DEFAULT '',
  group_id TEXT NOT NULL DEFAULT '',
  profile_base INTEGER NOT NULL DEFAULT 0,
  session_base INTEGER NOT NULL DEFAULT 0,
  fingerprint TEXT NOT NULL DEFAULT '',
  known_schema INTEGER NOT NULL DEFAULT 0,
  corrupt_count INTEGER NOT NULL DEFAULT 0,
  seen_at INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS profile_audit (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  ts INTEGER NOT NULL,
  group_id TEXT NOT NULL,
  user_id TEXT NOT NULL,
  device_id TEXT NOT NULL,
  path TEXT NOT NULL,
  base_json TEXT NOT NULL,
  server_json TEXT NOT NULL,
  client_json TEXT NOT NULL,
  resolution TEXT NOT NULL,
  winner TEXT NOT NULL,
  resolved_json TEXT NOT NULL,
  profile_version INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS corrupt_reports (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  ts INTEGER NOT NULL,
  device_id TEXT NOT NULL,
  user_id TEXT NOT NULL DEFAULT '',
  kind TEXT NOT NULL,
  payload_json TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS device_passthrough (
  device_id TEXT NOT NULL,
  path TEXT NOT NULL,
  leaf_json TEXT NOT NULL,
  updated_at INTEGER NOT NULL,
  PRIMARY KEY (device_id, path)
);
"""


def _now_ms() -> int:
    return int(time.time() * 1000)


class ServerDB:
    def __init__(self, path: str = ":memory:"):
        self.path = path
        self._lock = threading.RLock()
        self._lamport = 1
        if path != ":memory:":
            import os
            os.makedirs(os.path.dirname(os.path.abspath(path)) or ".",
                        exist_ok=True)
        self.conn = sqlite3.connect(path, check_same_thread=False)
        self.conn.row_factory = sqlite3.Row
        self.conn.execute("PRAGMA journal_mode=WAL")
        self.conn.execute("PRAGMA foreign_keys=ON")
        self.conn.executescript(SCHEMA)
        self.conn.commit()

    def query(self, sql: str, params=()):
        with self._lock:
            return self.conn.execute(sql, params).fetchall()

    def execute(self, sql: str, params=()):
        with self._lock:
            cur = self.conn.execute(sql, params)
            self.conn.commit()
            return cur

    def next_lamport(self) -> int:
        with self._lock:
            self._lamport += 1
            return self._lamport

    # ---- group default versions --------------------------------------- #
    def group_head(self, group_id: str):
        rows = self.query(
            "SELECT * FROM group_versions WHERE group_id=? "
            "ORDER BY version DESC LIMIT 1", (group_id,))
        return rows[0] if rows else None

    def group_version(self, group_id: str, version: int):
        rows = self.query(
            "SELECT * FROM group_versions WHERE group_id=? AND version=?",
            (group_id, version))
        return rows[0] if rows else None

    def ensure_group(self, group_id: str, seed_doc: dict):
        with self._lock:
            if self.group_head(group_id) is None:
                self.publish_group(group_id, seed_doc, [], "system",
                                   "initial defaults", mode="merge")

    def publish_group(self, group_id, doc, enforced, by, note,
                      mode="merge", rolled_back_from=None):
        with self._lock:
            head = self.group_head(group_id)
            version = (int(head["version"]) + 1) if head else 1
            stamp = self.next_lamport()
            self.conn.execute(
                "INSERT INTO group_versions(group_id,version,doc_json,"
                "enforced_json,mode,published_by,note,rolled_back_from,"
                "lamport,created_at) VALUES (?,?,?,?,?,?,?,?,?,?)",
                (group_id, version, json.dumps(doc, ensure_ascii=False),
                 json.dumps(sorted(set(enforced))), mode, by, note,
                 rolled_back_from, stamp, _now_ms()))
            self.conn.commit()
            return version

    def rollback_group(self, group_id, target_version, by):
        """Rollback = publish old content as a NEW version (ids never reused)."""
        with self._lock:
            row = self.group_version(group_id, target_version)
            if row is None:
                raise KeyError(f"unknown version {target_version}")
            return self.publish_group(
                group_id, json.loads(row["doc_json"]),
                json.loads(row["enforced_json"]), by,
                f"rollback to v{target_version}", mode=row["mode"],
                rolled_back_from=target_version)

    # ---- user profile versions ---------------------------------------- #
    def profile_head(self, user_id, group_id):
        rows = self.query(
            "SELECT * FROM user_profile_versions WHERE user_id=? AND group_id=? "
            "ORDER BY version DESC LIMIT 1", (user_id, group_id))
        return rows[0] if rows else None

    def profile_version(self, user_id, group_id, version):
        rows = self.query(
            "SELECT * FROM user_profile_versions WHERE user_id=? AND group_id=? "
            "AND version=?", (user_id, group_id, version))
        return rows[0] if rows else None

    def save_profile(self, user_id, group_id, base_group_version, leaves,
                     lamport):
        with self._lock:
            head = self.profile_head(user_id, group_id)
            version = (int(head["version"]) + 1) if head else 1
            self.conn.execute(
                "INSERT INTO user_profile_versions(user_id,group_id,version,"
                "base_group_version,leaves_json,lamport,created_at) "
                "VALUES (?,?,?,?,?,?,?)",
                (user_id, group_id, version, base_group_version,
                 json.dumps(leaves, ensure_ascii=False), lamport, _now_ms()))
            self.conn.commit()
            return version

    # ---- user session prefs ------------------------------------------- #
    def get_session(self, user_id):
        rows = self.query("SELECT * FROM user_sessions WHERE user_id=?",
                          (user_id,))
        return rows[0] if rows else None

    def save_session(self, user_id, leaves, lamport):
        with self._lock:
            row = self.get_session(user_id)
            version = (int(row["version"]) + 1) if row else 1
            self.conn.execute(
                "INSERT INTO user_sessions(user_id,version,leaves_json,"
                "lamport,updated_at) VALUES (?,?,?,?,?) "
                "ON CONFLICT(user_id) DO UPDATE SET version=excluded.version,"
                "leaves_json=excluded.leaves_json,lamport=excluded.lamport,"
                "updated_at=excluded.updated_at",
                (user_id, version, json.dumps(leaves, ensure_ascii=False),
                 lamport, _now_ms()))
            self.conn.commit()
            return version

    # ---- devices ------------------------------------------------------ #
    def upsert_device(self, device_id, user_id, group_id,
                      profile_base, session_base, fingerprint, known_schema):
        with self._lock:
            row = self.query("SELECT device_id FROM devices WHERE device_id=?",
                             (device_id,))
            if row:
                self.conn.execute(
                    "UPDATE devices SET user_id=?,group_id=?,profile_base=?,"
                    "session_base=?,fingerprint=?,known_schema=?,seen_at=? "
                    "WHERE device_id=?",
                    (user_id, group_id, profile_base, session_base,
                     fingerprint, known_schema, _now_ms(), device_id))
            else:
                self.conn.execute(
                    "INSERT INTO devices(device_id,user_id,group_id,"
                    "profile_base,session_base,fingerprint,known_schema,"
                    "corrupt_count,seen_at) VALUES (?,?,?,?,?,?,?,?,?)",
                    (device_id, user_id, group_id, profile_base,
                     session_base, fingerprint, known_schema, 0, _now_ms()))
            self.conn.commit()

    def add_conflict(self, group_id, user_id, device_id, c, profile_version):
        with self._lock:
            self.conn.execute(
                "INSERT INTO profile_audit(ts,group_id,user_id,device_id,"
                "path,base_json,server_json,client_json,resolution,winner,"
                "resolved_json,profile_version) VALUES (?,?,?,?,?,?,?,?,?,?,?,?)",
                (_now_ms(), group_id, user_id, device_id, c.path,
                 json.dumps(c.base, ensure_ascii=False),
                 json.dumps(c.server, ensure_ascii=False),
                 json.dumps(c.client, ensure_ascii=False),
                 c.resolution, c.winner,
                 json.dumps(c.resolved_value, ensure_ascii=False),
                 profile_version))
            self.conn.commit()

    def add_corrupt_report(self, device_id, user_id, kind, payload):
        with self._lock:
            self.conn.execute(
                "INSERT INTO corrupt_reports(ts,device_id,user_id,kind,"
                "payload_json) VALUES (?,?,?,?,?)",
                (_now_ms(), device_id, user_id, kind,
                 json.dumps(payload, ensure_ascii=False)))
            self.conn.execute(
                "UPDATE devices SET corrupt_count=corrupt_count+1 "
                "WHERE device_id=?", (device_id,))
            self.conn.commit()

    # ---- unknown/newer-schema passthrough (per device) ---------------- #
    def get_passthrough(self, device_id) -> dict:
        rows = self.query(
            "SELECT path,leaf_json FROM device_passthrough WHERE device_id=?",
            (device_id,))
        return {r["path"]: json.loads(r["leaf_json"]) for r in rows}

    def set_passthrough(self, device_id, path, leaf):
        with self._lock:
            self.conn.execute(
                "INSERT INTO device_passthrough(device_id,path,leaf_json,"
                "updated_at) VALUES (?,?,?,?) "
                "ON CONFLICT(device_id,path) DO UPDATE SET "
                "leaf_json=excluded.leaf_json,updated_at=excluded.updated_at",
                (device_id, path, json.dumps(leaf, ensure_ascii=False),
                 _now_ms()))
            self.conn.commit()
