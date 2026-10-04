"""Terminal sync agent: talks to the backend on behalf of the C client.

Responsibilities:
  * read pending profile/session changes from the crash-safe StateStore;
  * while offline, do nothing destructive -- dirty leaves just wait;
  * on reconnect POST the 3-way merge request, apply the authoritative result;
  * preserve unknown/newer-schema fields ("unknown_*" bag) untouched;
  * upload corruption/quarantine telemetry without ever pushing the auto
    fallback default values to the group.
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from synckit import StateStore  # noqa: E402
from synckit.schema import KNOWN_SCHEMA, PROFILE, SESSION  # noqa: E402


class Offline(Exception):
    pass


class SyncAgent:
    def __init__(self, store: StateStore, server_url: str,
                 user_id: str = "user-1", group_id: str = "default",
                 transport=None):
        self.store = store
        self.server_url = server_url.rstrip("/")
        self.user_id = user_id
        self.group_id = group_id
        self._transport = transport or self._http_post
        self.store.state["user_id"] = user_id
        self.store.state["group_id"] = group_id

    # ------------------------------------------------------------------ #
    def _http_post(self, path: str, body: dict) -> dict:
        data = json.dumps(body, ensure_ascii=False).encode("utf-8")
        req = urllib.request.Request(
            self.server_url + path, data=data,
            headers={"Content-Type": "application/json"}, method="POST")
        with urllib.request.urlopen(req, timeout=5) as resp:
            return json.loads(resp.read().decode("utf-8"))

    def _pending_for(self, scope: str) -> dict:
        return {p: c for p, c in self.store.pending_changes().items()
                if _scope(p) == scope}

    def _collect_corrupt_reports(self) -> list[dict]:
        reports = []
        for ev in self.store.recovery_events:
            if ev["kind"] in ("quarantine", "fallback-defaults",
                              "journal-truncated-at-corrupt-record"):
                reports.append(ev)
        return reports

    # ------------------------------------------------------------------ #
    def sync_once(self) -> dict:
        st = self.store.state
        monitors = st.get("_last_monitors")
        request = {
            "device_id": st["device_id"],
            "user_id": self.user_id,
            "group_id": self.group_id,
            "known_schema": KNOWN_SCHEMA,
            "profile": {"base_version": st["baselines"][PROFILE],
                        "changes": self._pending_for(PROFILE)},
            "session": {"base_version": st["baselines"][SESSION],
                        "changes": self._pending_for(SESSION)},
            "displays": {"fingerprint": self.store.get("displays.fingerprint")
                         or "",
                         "monitors": monitors or [],
                         "diagnostic_only": True},
            "corrupt_reports": self._collect_corrupt_reports(),
        }
        # Device-scoped leaves are asserted absent from the wire payload.
        for seg in ("profile", "session"):
            for path in request[seg]["changes"]:
                assert _scope(path) in (PROFILE, SESSION), \
                    f"device field leaked into {seg} request: {path}"

        try:
            resp = self._transport("/api/sync", request)
        except (urllib.error.URLError, OSError, TimeoutError) as e:
            raise Offline(str(e)) from e

        self._apply(resp)
        return resp

    def _apply(self, resp: dict):
        st = self.store.state
        # Unknown (newer schema) passthrough leaves: keep verbatim in a bag the
        # old client never interprets; they round-trip on every later save.
        bag = st.setdefault("unknown_leaves", {})
        for path, leaf in (resp.get("passthrough") or {}).items():
            bag[path] = leaf

        # Apply authoritative documents leaf by leaf (field-level), preserving
        # every local unknown node because we never touch unknown_leaves.
        self.store.apply_server(
            PROFILE, resp["profile"]["leaves"],
            int(resp["profile"]["version"]),
            rejected=resp.get("rejected"))
        self.store.apply_server(
            SESSION, resp["session"]["leaves"],
            int(resp["session"]["version"]))
        st["last_sync_lamport"] = resp.get("lamport")

    def run_forever(self, interval: float = 2.0):
        while True:
            try:
                self.sync_once()
            except Offline:
                pass  # dirty leaves stay queued locally; nothing is dropped
            time.sleep(interval)


def _scope(path: str):
    from synckit.schema import scope_of
    return scope_of(path)


def main(argv=None):
    ap = argparse.ArgumentParser(description="visual-window sync agent")
    ap.add_argument("--state-dir",
                    default=os.environ.get(
                        "VW_STATE_DIR",
                        str(Path.home() / ".local/state/visual-window")))
    ap.add_argument("--server",
                    default=os.environ.get("VW_SYNC_URL",
                                           "http://127.0.0.1:8080"))
    ap.add_argument("--device", default=os.environ.get("VW_DEVICE_ID",
                                                       "device-local"))
    ap.add_argument("--user", default="user-1")
    ap.add_argument("--group", default="default")
    ap.add_argument("--once", action="store_true")
    args = ap.parse_args(argv)

    store = StateStore(args.state_dir, device_id=args.device)
    agent = SyncAgent(store, args.server, args.user, args.group)
    if args.once:
        try:
            resp = agent.sync_once()
        except Offline as e:
            print(json.dumps({"offline": True, "reason": str(e),
                              "queued": list(store.pending_changes())},
                             ensure_ascii=False))
            return 2
        print(json.dumps({
            "profile_version": resp["profile"]["version"],
            "session_version": resp["session"]["version"],
            "conflicts": resp["conflicts"],
            "rejected": resp["rejected"]}, ensure_ascii=False, indent=2))
        return 0
    agent.run_forever()


if __name__ == "__main__":
    raise SystemExit(main())
