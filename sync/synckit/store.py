"""Crash-safe local state store, mirroring the C terminal's on-disk format.

Layout under STATE_DIR:
  state.json          current state (atomic tmp + rename)
  journal.jsonl       write-ahead log of mutations, pruned at checkpoint
  state.backup.json   durable pre-image of the last committed state
  restore.jsonl       append-only local recovery record
  diag/quarantine-*  copies of corrupt state/journal for diagnostics

PowerLoss can be injected at each stage to emulate "power cut halfway through
a write". A discarded state.json.tmp must never be adopted; a corrupt
state.json is quarantined, never overwritten in place.
"""
from __future__ import annotations

import contextlib
import fcntl
import json
import os
import shutil
import tempfile
import time
from pathlib import Path

from . import migrations
from .geometry import fingerprint, geometry_repair, Monitor
from .schema import (
    KNOWN_SCHEMA, DEVICE, PROFILE, SESSION, is_synced, scope_of,
)

STATE_NAME = "state.json"
TMP_NAME = "state.json.tmp"
JOURNAL_NAME = "journal.jsonl"
BACKUP_NAME = "state.backup.json"
RESTORE_NAME = "restore.jsonl"
DIAG_DIR = "diag"
LOCK_NAME = "state.lock"


class PowerLoss(Exception):
    """Injected fault: process dies at a specific write stage."""


class CorruptState(Exception):
    pass


def default_state() -> dict:
    return {
        "schema_version": KNOWN_SCHEMA,
        "device_id": None,
        "user_id": None,
        "group_id": None,
        "data": {
            DEVICE: {"window": {"x": -1, "y": -1, "width": 1280,
                                "height": 720, "maximized": False}},
            PROFILE: {"theme": {"background": {
                          "uri": "assets/background.png", "mode": "fill"},
                      "accent": "#2b6cb0", "dark": False}},
            SESSION: {"ui": {"density": "normal", "language": "en"}},
        },
        "stamps": {},                 # dotted path -> {"stamp", "author", "source"}
        "baselines": {"profile": 0, "session": 0},
        "dirty": [],                  # dotted paths awaiting sync
        "hlc": 1,
    }


def _dotted_get(data, path):
    node = data
    for part in path.split("."):
        if not isinstance(node, dict) or part not in node:
            return None, False
        node = node[part]
    return node, True


def _dotted_set(data, path, value):
    node = data
    parts = path.split(".")
    for part in parts[:-1]:
        node = node.setdefault(part, {})
    node[parts[-1]] = value


class StateStore:
    def __init__(self, state_dir: str | os.PathLike,
                 device_id: str | None = None,
                 inject: str | None = None,
                 clock=None):
        self.dir = Path(state_dir)
        self.dir.mkdir(parents=True, exist_ok=True)
        (self.dir / DIAG_DIR).mkdir(exist_ok=True)
        self.state_path = self.dir / STATE_NAME
        self.tmp_path = self.dir / TMP_NAME
        self.journal_path = self.dir / JOURNAL_NAME
        self.backup_path = self.dir / BACKUP_NAME
        self.restore_path = self.dir / RESTORE_NAME
        self.inject = inject  # "journal" | "tmp" | "rename"
        self._clock = clock or (lambda: int(time.time() * 1000))
        self.recovery_events: list[dict] = []
        self._lock_fd = os.open(self.dir / LOCK_NAME, os.O_RDWR | os.O_CREAT,
                                0o600)
        # Recovery is a read-modify-(re)write critical section shared with the
        # C terminal process; coordinate through the same flock.
        with self._locked():
            self.state = self._load_and_recover()
        if device_id and not self.state.get("device_id"):
            self.state["device_id"] = device_id

    # ------------------------------------------------------------------ #
    # clock
    # ------------------------------------------------------------------ #
    def tick(self) -> int:
        self.state["hlc"] = int(self.state.get("hlc", 0)) + 1
        return self.state["hlc"]

    @contextlib.contextmanager
    def _locked(self):
        fcntl.flock(self._lock_fd, fcntl.LOCK_EX)
        try:
            yield
        finally:
            fcntl.flock(self._lock_fd, fcntl.LOCK_UN)

    def _restore(self, kind: str, detail: dict):
        event = {"at": self._clock(), "kind": kind, **detail}
        self.recovery_events.append(event)
        with open(self.restore_path, "a", encoding="utf-8") as f:
            f.write(json.dumps(event, ensure_ascii=False) + "\n")

    def _quarantine(self, src: Path, why: str) -> Path:
        dst = self.dir / DIAG_DIR / f"quarantine-{int(self._clock())}-{src.name}"
        try:
            shutil.copy2(src, dst)
        except FileNotFoundError:
            return dst
        self._restore("quarantine", {"file": src.name, "copy": str(dst),
                                     "reason": why})
        return dst

    # ------------------------------------------------------------------ #
    # load / recovery
    # ------------------------------------------------------------------ #
    def _read_json(self, path: Path):
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f)

    def _load_and_recover(self) -> dict:
        # 1. A leftover tmp is proof of an interrupted rewrite: never adopt.
        if self.tmp_path.exists():
            self._quarantine(self.tmp_path, "discarded-tmp-from-interrupted-write")
            self.tmp_path.unlink()

        state = None
        if self.state_path.exists():
            try:
                state = self._read_json(self.state_path)
                self._validate(state)
            except (json.JSONDecodeError, ValueError, KeyError, OSError) as e:
                self._quarantine(self.state_path, f"corrupt-state:{e}")
                state = None

        if state is None and self.backup_path.exists():
            try:
                state = self._read_json(self.backup_path)
                self._validate(state)
                self._restore("recovered-from-backup", {})
            except (json.JSONDecodeError, ValueError, KeyError, OSError) as e:
                self._quarantine(self.backup_path, f"corrupt-backup:{e}")
                state = None

        if state is None:
            state = default_state()
            if not self.state_path.exists():
                self._restore("first-boot-defaults", {})
            else:
                self._restore("fallback-defaults",
                              {"note": "values marked source=fallback, "
                                         "not broadcast until user edits"})

        state = self._upgrade(state)

        # 2. Replay durable mutations logged after the last checkpoint.
        self._replay_journal(state)
        self.state = state
        # Persist the (possibly migrated/recovered/default) document so a
        # state.json and durable backup exist before the first mutation.
        self._flush()
        return state

    def _validate(self, state):
        for key in ("data", "stamps", "baselines"):
            if key not in state or not isinstance(state[key], dict):
                raise ValueError(f"missing section: {key}")

    def _upgrade(self, state):
        version = int(state.get("schema_version", 1))
        if version > KNOWN_SCHEMA:
            # Newer config on an older client: keep it whole, never downgrade.
            self._restore("forward-compatible-newer-schema",
                          {"version": version, "known": KNOWN_SCHEMA})
            return state
        if version < KNOWN_SCHEMA:
            upgraded = migrations.migrate(state)
            self._restore("schema-migrated",
                          {"from": version, "to": upgraded["schema_version"]})
            return upgraded
        return state

    def _replay_journal(self, state: dict):
        if not self.journal_path.exists():
            return
        replayed = 0
        bad_line = None
        with open(self.journal_path, "r", encoding="utf-8") as f:
            lines = f.readlines()
        for idx, line in enumerate(lines):
            line = line.strip()
            if not line or line.startswith("#"):
                if line.startswith("# checkpoint"):
                    replayed = 0  # everything before it was durably committed
                continue
            try:
                event = json.loads(line)
            except json.JSONDecodeError:
                bad_line = idx
                break
            if event.get("type") != "mut":
                continue
            try:
                _dotted_set(state["data"][event["scope"]],
                            event["path"], event["value"])
                stamps = state.setdefault("stamps", {})
                if event.get("stamped"):
                    stamps[event["path"]] = {
                        "stamp": event["stamp"], "author": event["author"],
                        "source": event.get("source", "user")}
                    dirty = state.setdefault("dirty", [])
                    if event["path"] not in dirty:
                        dirty.append(event["path"])
                replayed += 1
            except (KeyError, TypeError) as e:
                bad_line = idx
                self._restore("journal-event-failed", {"error": str(e)})
                break
        if replayed:
            self._restore("journal-replay", {"events": replayed})
        if bad_line is not None:
            # Keep the unreadable tail for diagnosis; drop it from the active
            # journal so the store can keep operating.
            diag = self.dir / DIAG_DIR / f"journal-tail-{int(self._clock())}.jsonl"
            with open(diag, "w", encoding="utf-8") as f:
                f.writelines(lines[bad_line:])
            with open(self.journal_path, "w", encoding="utf-8") as f:
                f.writelines(lines[:bad_line])
                f.write(f"# checkpoint {self._clock()}\n")
            self._restore("journal-truncated-at-corrupt-record",
                          {"line": bad_line, "copy": str(diag)})

    # ------------------------------------------------------------------ #
    # mutation + durable commit
    # ------------------------------------------------------------------ #
    def get(self, path: str):
        scope = scope_of(path)
        if scope is None:
            value, found = _dotted_get(self.state["data"].get(PROFILE, {}), path)
            if not found:
                value, found = _dotted_get(self.state["data"].get(SESSION, {}), path)
            return value if found else None
        value, _ = _dotted_get(self.state["data"][scope], path)
        return value

    def set_local(self, path: str, value, source: str = "user"):
        """Device-scope mutation: recorded locally, never marked for sync."""
        assert scope_of(path) == DEVICE, f"{path} is not device-scoped"
        self._commit_mut(DEVICE, path, value, stamped=False, source=source)
        _dotted_set(self.state["data"][DEVICE], path, value)

    def set_synced(self, path: str, value, author: str | None = None,
                   source: str = "user", stamp: int | None = None):
        scope = scope_of(path)
        assert scope in (PROFILE, SESSION), f"{path} is not syncable"
        stamp = stamp if stamp is not None else self.tick()
        author = author or self.state.get("device_id") or "local"
        self._commit_mut(scope, path, value, stamped=True, source=source,
                         stamp=stamp, author=author)
        _dotted_set(self.state["data"][scope], path, value)
        self.state["stamps"][path] = {"stamp": stamp, "author": author,
                                      "source": source}
        dirty = self.state.setdefault("dirty", [])
        if path not in dirty:
            dirty.append(path)

    def _commit_mut(self, scope, path, value, stamped, source,
                    stamp=None, author=None):
        event = {"type": "mut", "seq": self._clock(), "scope": scope,
                 "path": path, "value": value, "stamped": stamped,
                 "source": source}
        if stamped:
            event.update({"stamp": stamp, "author": author})
        self._durably_commit(event)

    def _durably_commit(self, event: dict):
        with self._locked():
            self._durably_commit_locked(event)

    def _durably_commit_locked(self, event: dict):
        # Stage 1: WAL append + fsync
        with open(self.journal_path, "a", encoding="utf-8") as f:
            f.write(json.dumps(event, ensure_ascii=False) + "\n")
            f.flush()
            os.fsync(f.fileno())
        if self.inject == "journal":
            stage = self.inject
            self.inject = None
            raise PowerLoss("power lost after journal append: " + stage)

        # Stage 2: full-state tmp + fsync + rename
        payload = json.dumps(self._snapshot_with_event(event),
                             ensure_ascii=False, indent=2)
        with open(self.tmp_path, "w", encoding="utf-8") as f:
            f.write(payload)
            f.flush()
            os.fsync(f.fileno())
        if self.inject == "tmp":
            self.inject = None
            raise PowerLoss("power lost while tmp written")

        # Keep a durable backup of the previous committed state.
        if self.state_path.exists():
            shutil.copy2(self.state_path, self.backup_path)
        os.replace(self.tmp_path, self.state_path)
        if self.inject == "rename":
            self.inject = None
            raise PowerLoss("power lost right after rename")

        # Stage 3: checkpoint -> prune replayed journal prefix
        with open(self.journal_path, "a", encoding="utf-8") as f:
            f.write(f"# checkpoint {self._clock()}\n")
            f.flush()
            os.fsync(f.fileno())

    def _snapshot_with_event(self, event: dict) -> dict:
        import copy
        snap = copy.deepcopy(self.state)
        _dotted_set(snap["data"][event["scope"]], event["path"], event["value"])
        if event.get("stamped"):
            snap.setdefault("stamps", {})[event["path"]] = {
                "stamp": event["stamp"], "author": event["author"],
                "source": event.get("source", "user")}
            dirty = snap.setdefault("dirty", [])
            if event["path"] not in dirty:
                dirty.append(event["path"])
        return snap

    # ------------------------------------------------------------------ #
    # display hotplug (device only)
    # ------------------------------------------------------------------ #
    def update_displays(self, monitors: list[Monitor],
                        win_paths=("window.x", "window.y", "window.width",
                                   "window.height")):
        """Detect screen topology change and repair the window onto it."""
        new_fp = fingerprint(monitors)
        old_fp = self.get("displays.fingerprint")
        win = tuple(self.get(p) if self.get(p) is not None else d
                    for p, d in zip(win_paths, (-1, -1, 1280, 720)))
        repaired, reason = geometry_repair(win, monitors)
        changed = old_fp != new_fp or repaired != win
        for p, v in zip(win_paths, repaired):
            if self.get(p) != v:
                self.set_local(p, v, source="repair")
        self.set_local("displays.fingerprint", new_fp, source="repair")
        if changed:
            self._restore("display-repair",
                          {"old_fp": old_fp, "new_fp": new_fp,
                           "reason": reason, "window": list(win),
                           "repaired": list(repaired)})
        return repaired, reason, changed

    # ------------------------------------------------------------------ #
    # synchronisation interface used by the agent
    # ------------------------------------------------------------------ #
    def pending_changes(self) -> dict[str, dict]:
        out = {}
        for path in list(self.state.get("dirty", [])):
            scope = scope_of(path)
            if scope not in (PROFILE, SESSION):
                continue
            value, found = _dotted_get(self.state["data"][scope], path)
            if not found:
                continue
            meta = self.state["stamps"].get(path, {})
            # Auto-fallback values must never be synced until the user edits.
            if meta.get("source") == "fallback":
                continue
            out[path] = {"value": value,
                         "stamp": meta.get("stamp", 0),
                         "author": meta.get("author",
                                           self.state.get("device_id"))}
        return out

    def apply_server(self, segment: str, leaves: dict, base_version: int,
                     rejected: list[dict] | None = None):
        import copy
        from .merge import expand
        assert segment in (PROFILE, SESSION)
        # Leaves arrive as {path: {"value","stamp","author","source"}}; the
        # data section holds plain values, stamps live in self.state["stamps"].
        values, stamps = {}, self.state.setdefault("stamps", {})
        for path, leaf in leaves.items():
            if isinstance(leaf, dict) and "value" in leaf:
                values[path] = leaf["value"]
                stamps[path] = {"stamp": leaf.get("stamp", 0),
                                "author": leaf.get("author", "server"),
                                "source": leaf.get("source", "user")}
            else:  # already a plain value
                values[path] = leaf
        self.state["data"][segment] = copy.deepcopy(expand(values))
        self.state["baselines"][segment] = base_version
        rejected_paths = {r["path"] for r in (rejected or [])}
        # Anything the server sent becomes clean; rejected local edits stay
        # dirty so the user can see/resolve them, they are not silently lost.
        self.state["dirty"] = [p for p in self.state.get("dirty", [])
                               if scope_of(p) != segment or p in rejected_paths]
        self._flush()

    def _flush(self):
        with self._locked():
            self._flush_locked()

    def _flush_locked(self):
        payload = json.dumps(self.state, ensure_ascii=False, indent=2)
        fd, tmp = tempfile.mkstemp(dir=str(self.dir), suffix=".tmp")
        try:
            with os.fdopen(fd, "w", encoding="utf-8") as f:
                f.write(payload)
                f.flush()
                os.fsync(f.fileno())
            if self.state_path.exists():
                shutil.copy2(self.state_path, self.backup_path)
            os.replace(tmp, self.state_path)
        finally:
            if os.path.exists(tmp):
                os.unlink(tmp)

    def mark_fallback(self, paths: list[str]):
        for path in paths:
            meta = self.state["stamps"].setdefault(
                path, {"stamp": 0, "author": "local", "source": "fallback"})
            meta["source"] = "fallback"
