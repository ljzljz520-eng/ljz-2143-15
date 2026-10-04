import unittest

from synckit.geometry import (Monitor, geometry_repair, fingerprint,
                              is_visible_and_operable)


class GeometryTests(unittest.TestCase):
    def setUp(self):
        self.dual = [Monitor("eDP", 0, 0, 1920, 1080, True),
                     Monitor("HDMI", 1920, 0, 1920, 1080)]
        self.single = [Monitor("eDP", 0, 0, 1366, 768, True)]

    def test_fingerprint_detects_unplug(self):
        self.assertNotEqual(fingerprint(self.dual), fingerprint(self.single))
        self.assertIn("1920x1080@1920,0", fingerprint(self.dual))

    def test_dual_screen_position_invalid_on_single(self):
        win = (2100, 60, 1600, 900)
        self.assertFalse(is_visible_and_operable(win, self.single))

    def test_unplug_moves_window_to_primary(self):
        win = (2100, 60, 1600, 900)
        repaired, reason = geometry_repair(win, self.single)
        self.assertTrue(is_visible_and_operable(repaired, self.single))
        self.assertTrue(reason.startswith("moved-to-primary"))
        self.assertGreaterEqual(repaired[2], 320)
        self.assertGreaterEqual(repaired[3], 240)
        self.assertLessEqual(repaired[0] + repaired[2], 1366)

    def test_stays_on_intersecting_second_monitor(self):
        repaired, reason = geometry_repair((2100, 60, 1600, 900), self.dual)
        self.assertTrue(reason.startswith("unchanged-on-monitor:HDMI"))
        self.assertTrue(is_visible_and_operable(repaired, self.dual))

    def test_oversized_window_shrinks_and_centers(self):
        repaired, _ = geometry_repair((-1, -1, 4000, 4000), self.single)
        self.assertTrue(is_visible_and_operable(repaired, self.single))
        self.assertEqual(repaired[2], 1366)

    def test_no_monitors_keeps_virtual_safe_rect(self):
        repaired, reason = geometry_repair((10, 10, 500, 500), [])
        self.assertEqual(reason, "no-monitor")
        self.assertGreaterEqual(repaired[2], 320)


if __name__ == "__main__":
    unittest.main()
