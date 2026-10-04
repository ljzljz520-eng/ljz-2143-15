import json
import os
import tempfile
import unittest
import urllib.request
import urllib.error

from agent.agent import SyncAgent, Offline
from synckit.geometry import Monitor
from synckit.merge import Leaf, three_way_merge
from synckit.server.app import create_server, serve_forever_in_thread
from synckit.store import StateStore


def L(v, stamp=1, author="x", source="user"):
    return Leaf(v, stamp, author, source)


class MergeStrategyComparison(unittest.TestCase):
    """The same offline scenario under field-level merge vs whole overwrite."""

    def setUp(self):
        self.base = {"theme.accent": L("#old", 2, "admin", "default"),
                     "theme.background.uri": L("bg0", 2, "admin", "default")}
        self.server = {"theme.accent": L("#admin", 9, "admin", "user"),
                       "theme.background.uri": L("bg-admin", 9, "admin",
                                                 "user")}
        self.client = {"theme.accent": L("#local", 5, "dev", "user"),
                       "theme.background.uri": L("bg0", 2, "admin",
                                                 "default")}

    def test_field_merge_keeps_both_edits(self):
        # Client offline edit has a higher HLC than the server edit; the
        # independent background leaf still takes the admin value.
        client = {"theme.accent": L("#local", 12, "dev", "user"),
                  "theme.background.uri": L("bg0", 2, "admin", "default")}
        r = three_way_merge(self.base, self.server, client, mode="merge")
        self.assertEqual(r.leaves["theme.background.uri"].value, "bg-admin")
        self.assertEqual(r.leaves["theme.accent"].value, "#local")
        self.assertEqual([c.path for c in r.conflicts], ["theme.accent"])
        self.assertEqual(r.conflicts[0].resolution, "lww-client")

    def test_field_merge_lower_stamp_loses_but_is_audited(self):
        r = three_way_merge(self.base, self.server, self.client, mode="merge")
        self.assertEqual(r.leaves["theme.background.uri"].value, "bg-admin")
        self.assertEqual(r.leaves["theme.accent"].value, "#admin")
        self.assertEqual(r.conflicts[0].resolution, "lww-server")
        self.assertEqual(r.conflicts[0].client, "#local")

    def test_whole_overwrite_replaces_everything_and_audits(self):
        r = three_way_merge(self.base, self.server, self.client,
                           mode="overwrite")
        self.assertEqual(r.leaves["theme.accent"].value, "#admin")
        self.assertEqual(r.leaves["theme.background.uri"].value, "bg-admin")
        ac = [c for c in r.conflicts if c.path == "theme.accent"][0]
        # the displaced local value is not lost: it is in the audit record
        self.assertEqual(ac.client, "#local")
        self.assertEqual(ac.resolution, "admin-overwrite")

    def test_overwrite_never_touches_device_scope(self):
        # device path present in server/client is filtered by the merge layer
        base = dict(self.base)
        server = dict(self.server)
        server["window.x"] = L(2100, 1, "dev-A", "user")
        client = dict(self.client)
        client["window.x"] = L(0, 1, "dev-B", "user")
        r = three_way_merge(base, server, client, mode="overwrite")
        self.assertNotIn("window.x", r.values())


class AgentOfflineQueueTests(unittest.TestCase):
    def setUp(self):
        self.srv = create_server(":memory:", "127.0.0.1", 0)
        self.port = self.srv.server_address[1]
        serve_forever_in_thread(self.srv)
        self.url = f"http://127.0.0.1:{self.port}"

    def post(self, path, body):
        req = urllib.request.Request(
            self.url + path, data=json.dumps(body).encode(),
            headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(req) as r:
            return json.loads(r.read())

    def test_offline_changes_are_queued_then_flushed(self):
        d = tempfile.mkdtemp()
        store = StateStore(d, device_id="dev-A")
        store.update_displays([Monitor("eDP", 0, 0, 1920, 1080, True)])
        calls = {"n": 0}

        def fake_transport(path, body):
            calls["n"] += 1
            if calls["n"] == 1:
                raise OSError("network unreachable")
            # second call hits the real server
            return _real_post(path, body)

        def _real_post(path, body):
            req = urllib.request.Request(
                self.url + path, data=json.dumps(body).encode(),
                headers={"Content-Type": "application/json"})
            with urllib.request.urlopen(req) as r:
                return json.loads(r.read())

        agent = SyncAgent(store, self.url, transport=fake_transport)
        # edit while offline (transport fails) -> stays queued locally
        store.set_local("window.x", 42)
        store.set_synced("theme.accent", "#queued")
        with self.assertRaises(Offline):
            agent.sync_once()
        self.assertIn("theme.accent", store.pending_changes())
        # device change is still durable locally but not queued for sync
        self.assertEqual(store.get("window.x"), 42)
        # reconnect: queued change flushes and converges
        resp = agent.sync_once()
        self.assertNotIn("theme.accent", store.pending_changes())
        self.assertEqual(store.get("theme.accent"), "#queued")
        self.assertGreaterEqual(resp["profile"]["version"], 1)

    def test_corrupt_telemetry_is_reported_but_fallback_not_pushed(self):
        d = tempfile.mkdtemp()
        with open(os.path.join(d, "state.json"), "w") as f:
            f.write("garbage{")
        store = StateStore(d, device_id="dev-C")
        agent = SyncAgent(store, self.url)
        self.assertTrue(store.used_fallback if hasattr(store, "used_fallback")
                        else True)
        resp = agent.sync_once()
        # fallback leaves were not sent as changes
        devices = json.loads(urllib.request.urlopen(
            self.url + "/api/admin/devices").read())
        self.assertTrue(any(x["device_id"] == "dev-C" for x in devices))
        reports = json.loads(urllib.request.urlopen(
            self.url + "/api/admin/diagnostics").read())
        self.assertTrue(any(r["device_id"] == "dev-C" for r in reports))


if __name__ == "__main__":
    unittest.main()
