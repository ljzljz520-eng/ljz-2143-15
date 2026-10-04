import unittest

from synckit.merge import Leaf, three_way_merge, flatten, expand
from synckit.schema import DEVICE


def L(value, stamp=1, author="x", source="user"):
    return Leaf(value, stamp, author, source)


class MergeRuleTests(unittest.TestCase):
    def test_no_change(self):
        r = three_way_merge({"a": L("v")}, {"a": L("v")}, {"a": L("v")})
        self.assertEqual(r.values(), {"a": "v"})
        self.assertEqual(r.conflicts, [])

    def test_client_only_change(self):
        r = three_way_merge({"a": L("d")}, {"a": L("d")},
                            {"a": L("u", 2, "dev")})
        self.assertEqual(r.values(), {"a": "u"})

    def test_server_only_change(self):
        r = three_way_merge({"a": L("d", 2)}, {"a": L("X", 9, "admin")},
                            {"a": L("d", 2)})
        self.assertEqual(r.values(), {"a": "X"})
        self.assertEqual(r.conflicts, [])

    def test_both_change_same_value_no_conflict(self):
        r = three_way_merge({"a": L("d")}, {"a": L("Z", 9, "admin")},
                            {"a": L("Z", 8, "dev")})
        self.assertEqual(r.values(), {"a": "u" and "Z"})

    def test_divergent_lww_deterministic(self):
        r = three_way_merge({"a": L("b", 2)},
                            {"a": L("server", 10, "admin")},
                            {"a": L("client", 9, "dev")})
        self.assertEqual(r.values(), {"a": "server"})
        self.assertEqual(r.conflicts[0].resolution, "lww-server")
        raw = r.conflicts[0]
        self.assertEqual((raw.base, raw.server, raw.client),
                         ("b", "server", "client"))

    def test_lww_author_tie_break_is_deterministic(self):
        a = three_way_merge({"a": L("b")}, {"a": L("Z", 5, "aaa")},
                            {"a": L("Y", 5, "zzz")})
        b = three_way_merge({"a": L("b")}, {"a": L("Z", 5, "aaa")},
                            {"a": L("Y", 5, "zzz")})
        self.assertEqual(a.values(), b.values())
        # equal stamps -> deterministic author-id tie-break (higher author wins)
        self.assertEqual(a.values(), {"a": "Y"})
        self.assertEqual(a.conflicts[0].winner, "zzz")

    def test_enforced_admin_wins_and_records_rejection(self):
        r = three_way_merge({"a": L("u", 2, "dev")},
                            {"a": L("locked", 9, "admin", "enforced")},
                            {"a": L("u", 3, "dev")},
                            enforced=frozenset({"a"}))
        self.assertEqual(r.values(), {"a": "locked"})
        self.assertEqual(r.conflicts[0].resolution,
                         "enforced-admin-wins")
        self.assertEqual(r.rejected[0]["path"], "a")

    def test_default_never_overrides_user_adoption(self):
        r = three_way_merge(
            {"a": L("d", 2, "admin", "default")},
            {"a": L("d2", 9, "admin", "default")},
            {"a": L("mine", 3, "dev", "user")})
        self.assertEqual(r.values(), {"a": "mine"})
        self.assertEqual(r.conflicts, [])

    def test_default_drift_on_untouched_leaf_flows(self):
        r = three_way_merge(
            {"a": L("d1", 2, "admin", "default")},
            {"a": L("d2", 9, "admin", "default")},
            {"a": L("d1", 2, "admin", "default")})
        self.assertEqual(r.values(), {"a": "d2"})

    def test_overwrite_mode_audits_collisions(self):
        r = three_way_merge({"a": L("u", 2, "dev")},
                            {"a": L("NEW", 9, "admin")},
                            {"a": L("mine", 3, "dev")},
                            mode="overwrite")
        self.assertEqual(r.values(), {"a": "NEW"})
        self.assertEqual(r.conflicts[0].resolution, "admin-overwrite")

    def test_device_paths_are_not_merged(self):
        r = three_way_merge({}, {"window.x": L(2100, 1, "dev")},
                            {"window.x": L(0, 2, "dev")})
        self.assertNotIn("window.x", r.values())

    def test_flatten_expand_roundtrip(self):
        doc = {"theme": {"background": {"uri": "x"}, "accent": "y"}}
        self.assertEqual(expand(flatten(doc)), doc)


if __name__ == "__main__":
    unittest.main()
