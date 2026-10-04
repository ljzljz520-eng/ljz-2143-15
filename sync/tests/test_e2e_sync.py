import json
import tempfile
import unittest
import urllib.request

from agent.agent import SyncAgent
from synckit.geometry import Monitor
from synckit.server.app import create_server, serve_forever_in_thread
from synckit.store import StateStore


class ServerHarness:
    def __init__(self):
        self.srv = create_server(":memory:", "127.0.0.1", 0)
        self.port = self.srv.server_address[1]
        serve_forever_in_thread(self.srv)
        self.url = f"http://127.0.0.1:{self.port}"

    def post(self, path, body):
        req = urllib.request.Request(
            self.url + path,
            data=json.dumps(body).encode(),
            headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(req) as resp:
            return json.loads(resp.read().decode())

    def get(self, path):
        with urllib.request.urlopen(self.url + path) as resp:
            return json.loads(resp.read().decode())

    def publish(self, doc, mode="merge", enforced=None, note=""):
        return self.post("/api/admin/groups/default/publish",
                         {"doc": doc, "mode": mode,
                          "enforced": enforced or [], "by": "admin",
                          "note": note})["version"]

    def rollback(self, version):
        return self.post("/api/admin/groups/default/rollback",
                         {"version": version, "by": "admin"})["version"]


class E2EScenarios(unittest.TestCase):
    def setUp(self):
        self.h = ServerHarness()
        self.dual = [Monitor("eDP", 0, 0, 1920, 1080, True),
                     Monitor("HDMI", 1920, 0, 1920, 1080)]
        self.single = [Monitor("eDP", 0, 0, 1366, 768, True)]
        self.A = StateStore(tempfile.mkdtemp(), device_id="dev-A")
        self.B = StateStore(tempfile.mkdtemp(), device_id="dev-B")
        self.agA = SyncAgent(self.A, self.h.url)
        self.agB = SyncAgent(self.B, self.h.url)
        self.A.update_displays(self.dual)
        self.B.update_displays(self.single)

    def test_theme_crosses_devices_but_coords_do_not(self):
        self.A.set_local("window.x", 2100)
        self.A.set_local("window.width", 1600)
        self.A.set_synced("theme.accent", "#00ff88")
        self.agA.sync_once()
        self.agB.sync_once()
        self.assertEqual(self.B.get("theme.accent"), "#00ff88")
        # The dual-screen coordinate never reached device B's local state.
        self.assertNotEqual(self.B.get("window.x"), 2100)
        self.assertLess(self.B.get("window.x") if self.B.get("window.x") >= 0
                        else 0, 1366)

    def test_offline_window_move_and_admin_background_both_survive(self):
        self.A.set_synced("theme.accent", "#user")
        self.agA.sync_once(); self.agB.sync_once()
        # network outage window: local window move + local accent touch
        self.A.set_local("window.x", 2500)
        self.A.set_synced("theme.accent", "#local")
        self.h.publish({"theme.background.uri": "BG1",
                        "theme.background.mode": "fill",
                        "theme.accent": "#admin", "theme.dark": True},
                       note="seasonal")
        r = self.agA.sync_once()
        # device coord untouched by sync
        self.assertEqual(self.A.get("window.x"), 2500)
        # different profile leaf: admin background applied
        self.assertEqual(self.A.get("theme.background.uri"), "BG1")
        # same profile leaf both edited: conflict resolved + audited
        self.assertTrue(any(c["path"] == "theme.accent"
                            for c in r["conflicts"]))
        # second machine gets the background but never the coordinate
        self.agB.sync_once()
        self.assertEqual(self.B.get("theme.background.uri"), "BG1")
        bx = self.B.get("window.x")
        self.assertTrue(bx < 0 or bx < 1366)
        # audit shows raw base/client/server and final source
        rows = self.h.get("/api/admin/conflicts")
        row = [c for c in rows if c["path"] == "theme.accent"][-1]
        self.assertEqual(row["client_json"], "#local")
        self.assertEqual(row["server_json"], "#admin")
        self.assertIn(row["resolution"],
                      {"lww-client", "lww-server", "enforced-admin-wins"})

    def test_enforced_background_rejects_client_override(self):
        self.h.publish(
            {"theme.background.uri": "CORP",
             "theme.background.mode": "fill",
             "theme.accent": "#2b6cb0", "theme.dark": False},
            enforced=["theme.background.uri"], note="corp lock")
        self.A.set_synced("theme.background.uri", "personal")
        r = self.agA.sync_once()
        self.assertEqual(self.A.get("theme.background.uri"), "CORP")
        self.assertTrue(any(x["path"] == "theme.background.uri"
                            for x in r["rejected"]))
        self.assertTrue(any(c["resolution"] == "enforced-admin-wins"
                            for c in r["conflicts"]))

    def test_long_offline_device_returns_and_converges(self):
        # Device A syncs at version 1
        self.A.set_synced("ui.language", "de")
        self.agA.sync_once()
        first_version = self.A.state["baselines"]["profile"]
        # Many server changes happen while A is "offline for months"
        for i in range(5):
            self.h.publish({"theme.background.uri": f"BG{i}",
                            "theme.background.mode": "fill",
                            "theme.accent": "#2b6cb0", "theme.dark": bool(i % 2)},
                           note=f"wave{i}")
        # A has a local edit in that period too
        self.A.set_synced("ui.density", "compact")
        r = self.agA.sync_once()
        self.assertEqual(self.A.get("theme.background.uri"), "BG4")
        self.assertEqual(self.A.get("ui.density"), "compact")
        self.assertEqual(self.A.get("ui.language"), "de")
        self.assertGreater(self.A.state["baselines"]["profile"],
                           first_version)

    def test_server_rollback_publishes_new_version_never_reuses(self):
        v1 = self.h.publish({"theme.background.uri": "V1",
                             "theme.background.mode": "fill",
                             "theme.accent": "#2b6cb0",
                             "theme.dark": False}, note="v1")
        v2 = self.h.publish({"theme.background.uri": "V2",
                             "theme.background.mode": "fill",
                             "theme.accent": "#2b6cb0",
                             "theme.dark": False}, note="v2")
        self.agA.sync_once()
        self.assertEqual(self.A.get("theme.background.uri"), "V2")
        v3 = self.h.rollback(v1)
        self.assertGreater(v3, v2)  # monotonic
        self.agA.sync_once()
        self.assertEqual(self.A.get("theme.background.uri"), "V1")
        versions = self.h.get("/api/admin/groups/default/versions")
        self.assertEqual(versions[-1]["rolled_back_from"], v1)

    def test_rollback_vs_offline_local_edit_does_not_silently_wipe(self):
        # Establish user-accent on the profile first.
        self.A.set_synced("theme.accent", "#user-accent")
        self.agA.sync_once()
        v1_doc = {"theme.background.uri": "S0",
                  "theme.background.mode": "fill",
                  "theme.accent": "#2b6cb0", "theme.dark": False}
        v1 = self.h.publish(v1_doc)
        self.agA.sync_once()  # A has S0 background + its own accent
        self.assertEqual(self.A.get("theme.accent"), "#user-accent")
        # While A is offline: admin changes and rolls back; A also edits.
        self.h.publish({"theme.background.uri": "S1",
                        "theme.background.mode": "fit",
                        "theme.accent": "#red", "theme.dark": True})
        self.h.rollback(v1)
        self.A.set_synced("ui.language", "ja")
        self.agA.sync_once()
        # background content returns to S0 through the new rollback version
        self.assertEqual(self.A.get("theme.background.uri"), "S0")
        # independent user leaves survive the rollback
        self.assertEqual(self.A.get("ui.language"), "ja")
        self.assertEqual(self.A.get("theme.accent"), "#user-accent")

    def test_session_prefs_cross_devices_independent_of_group(self):
        self.A.set_synced("ui.density", "comfortable")
        self.agA.sync_once()
        self.agB.sync_once()
        self.assertEqual(self.B.get("ui.density"), "comfortable")

    def test_unknown_new_schema_round_trips_per_device(self):
        # A client that carries an unknown leaf reports it; the server stores
        # it per-device and echoes it back, never leaking it to device B.
        resp = self.h.post("/api/sync", {
            "device_id": "dev-A", "user_id": "user-1",
            "group_id": "default", "known_schema": 3,
            "profile": {"base_version": 0,
                        "changes": {"future.field":
                                    {"value": 42, "stamp": 5,
                                     "author": "dev-A"}}},
            "session": {"base_version": 0, "changes": {}},
            "displays": {}})
        self.assertEqual(resp["passthrough"]["future.field"]["value"], 42)
        rB = self.h.post("/api/sync", {
            "device_id": "dev-B", "user_id": "user-1",
            "group_id": "default", "known_schema": 3,
            "profile": {"base_version": 0, "changes": {}},
            "session": {"base_version": 0, "changes": {}},
            "displays": {}})
        self.assertNotIn("future.field", rB["passthrough"])

    def test_fallback_defaults_are_not_broadcast_to_group(self):
        # Simulate a device that recovered from corruption: no pending changes
        # with source=fallback. Make a corrupt store:
        import os
        d = tempfile.mkdtemp()
        with open(os.path.join(d, "state.json"), "w") as f:
            f.write("{not json")
        C = StateStore(d, device_id="dev-C")
        agC = SyncAgent(C, self.h.url)
        r = agC.sync_once()
        # nothing the client pushed must overwrite group theme
        self.assertEqual(C.get("theme.accent"), "#2b6cb0")  # group default
        devices = self.h.get("/api/admin/devices")
        self.assertTrue(any(x["device_id"] == "dev-C" for x in devices))

    def test_audit_page_shows_raw_conflict_and_final_source(self):
        # User adopts accent, then both sides explicitly diverge.
        self.A.set_synced("theme.accent", "#user")
        self.agA.sync_once()
        self.A.set_synced("theme.accent", "#local-new")
        self.h.publish({"theme.background.uri": "assets/background.png",
                        "theme.background.mode": "fill",
                        "theme.accent": "#admin-new",
                        "theme.dark": False}, note="accent change")
        r = self.agA.sync_once()
        self.assertTrue(any(c["path"] == "theme.accent"
                            for c in r["conflicts"]))
        rows = self.h.get("/api/admin/conflicts")
        rows = [row for row in rows
                if row["path"] == "theme.accent"
                and row["client_json"] == "#local-new"]
        self.assertTrue(rows)
        for row in rows:
            self.assertEqual(row["server_json"], "#admin-new")
            self.assertIn(row["resolution"],
                          {"lww-client", "lww-server",
                           "enforced-admin-wins", "admin-overwrite"})
            self.assertIn(row["winner"], {"dev-A", "admin"})
            self.assertTrue(all(k in row for k in (
                "base_json", "client_json", "server_json",
                "resolved_json", "winner", "resolution")))


if __name__ == "__main__":
    unittest.main()
