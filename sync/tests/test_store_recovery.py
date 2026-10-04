import glob
import json
import os
import tempfile
import unittest
from pathlib import Path

from synckit.store import StateStore, PowerLoss
from synckit.geometry import Monitor


class StoreRecoveryTests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()

    def _open(self, **kw):
        return StateStore(self.dir, device_id="dev-1", **kw)

    def test_first_boot_writes_state(self):
        s = self._open()
        self.assertEqual(s.get("window.width"), 1280)
        self.assertTrue((Path(self.dir) / "state.json").exists())

    def test_device_change_is_durable_and_not_dirty(self):
        s = self._open()
        s.set_local("window.x", 2100)
        s.set_local("window.width", 1600)
        s2 = StateStore(self.dir, device_id="dev-1")
        self.assertEqual(s2.get("window.x"), 2100)
        self.assertNotIn("window.x", s2.pending_changes())

    def test_synced_change_is_dirty_with_stamp(self):
        s = self._open()
        s.set_synced("theme.accent", "#abcdef")
        pending = s.pending_changes()
        self.assertEqual(pending["theme.accent"]["value"], "#abcdef")
        self.assertIn("stamp", pending["theme.accent"])

    def test_power_loss_after_journal_recovers(self):
        s = self._open()
        s.set_synced("ui.language", "fr")
        s.inject = "journal"
        with self.assertRaises(PowerLoss):
            s.set_synced("ui.density", "compact")
        # reopen: journal replay yields the durable mutation set consistently
        s2 = StateStore(self.dir, device_id="dev-1")
        self.assertEqual(s2.get("ui.language"), "fr")
        kinds = [e["kind"] for e in s2.recovery_events]
        self.assertTrue({"tmp-discarded", "journal-replay"} & set(
            k.replace("schema-migrated", "") for k in kinds)
            or any("journal" in k or "tmp" in k for k in kinds))

    def test_power_loss_during_tmp_does_not_adopt_tmp(self):
        s = self._open()
        s.set_local("window.x", 123)
        s.inject = "tmp"
        with self.assertRaises(PowerLoss):
            s.set_local("window.y", 456)
        s2 = StateStore(self.dir, device_id="dev-1")
        # committed mutation durable; interrupted tmp never adopted as state
        self.assertEqual(s2.get("window.x"), 123)
        quarantined = glob.glob(os.path.join(self.dir, "diag",
                                             "quarantine-*tmp"))
        self.assertTrue(quarantined)

    def test_power_loss_after_rename_keeps_value(self):
        s = self._open()
        s.inject = "rename"
        with self.assertRaises(PowerLoss):
            s.set_local("window.x", 789)
        s2 = StateStore(self.dir, device_id="dev-1")
        self.assertEqual(s2.get("window.x"), 789)

    def test_corrupt_state_is_quarantined_and_fallback_not_broadcast(self):
        state = Path(self.dir) / "state.json"
        state.write_text("{broken json,,,,", encoding="utf-8")
        s = StateStore(self.dir, device_id="dev-1")
        self.assertEqual(s.get("window.width"), 1280)
        copies = list((Path(self.dir) / "diag").glob("quarantine-*state.json"))
        self.assertTrue(copies, "diagnostic copy must be retained")
        # original bytes preserved in the copy
        self.assertIn("broken", copies[0].read_text())
        # fallback values are not pushed to the sync line ...
        self.assertEqual(s.pending_changes(), {})
        # ... until the user really edits a leaf
        s.set_synced("theme.accent", "#111")
        self.assertIn("theme.accent", s.pending_changes())
        restore = (Path(self.dir) / "restore.jsonl").read_text()
        self.assertIn("quarantine", restore)

    def test_backup_recovery_path(self):
        s = self._open()
        s.set_synced("ui.language", "ja")
        # corrupt state.json -> backup is the durable pre-image
        Path(self.dir, "state.json").write_text("<<not json>>")
        s2 = StateStore(self.dir, device_id="dev-1")
        self.assertEqual(s2.get("ui.language"), "ja")
        kinds = [e["kind"] for e in s2.recovery_events]
        self.assertIn("recovered-from-backup", kinds)

    def test_hotplug_repair_persists_and_records(self):
        s = self._open()
        s.update_displays([Monitor("eDP", 0, 0, 1920, 1080, True),
                           Monitor("HDMI", 1920, 0, 1920, 1080)])
        s.set_local("window.x", 2100)
        s.set_local("window.y", 60)
        repaired, reason, changed = s.update_displays(
            [Monitor("eDP", 0, 0, 1366, 768, True)])
        self.assertTrue(changed)
        self.assertTrue(reason.startswith("moved-to-primary"))
        self.assertGreaterEqual(repaired[0], 0)
        self.assertLessEqual(repaired[0] + repaired[2], 1366)

    def test_newer_schema_unknown_leaves_kept(self):
        state = {
            "schema_version": 99,
            "device_id": "dev-1", "user_id": None, "group_id": None,
            "data": {
                "device": {"window": {"x": 10, "y": 20, "width": 400,
                                      "height": 300}},
                "profile": {"holographic": {"intensity": 0.9},
                            "theme": {"accent": "#fff"}},
                "session": {"ui": {"language": "en"}},
            },
            "stamps": {}, "baselines": {"profile": 0, "session": 0},
            "dirty": [], "hlc": 1,
        }
        Path(self.dir, "state.json").write_text(json.dumps(state))
        s = StateStore(self.dir, device_id="dev-1")
        # unknown subtree untouched, schema not downgraded
        self.assertEqual(s.state["schema_version"], 99)
        self.assertEqual(s.get("holographic.intensity"), 0.9)
        # editing a known leaf then saving preserves the unknown subtree
        s.set_synced("theme.accent", "#000")
        s2 = StateStore(self.dir, device_id="dev-1")
        self.assertEqual(s2.state["schema_version"], 99)
        self.assertEqual(s2.get("holographic.intensity"), 0.9)
        self.assertEqual(s2.get("theme.accent"), "#000")


if __name__ == "__main__":
    unittest.main()
