"""Server-side three-way merge orchestration.

The server is authoritative for profile/session conflict resolution so that
offline edits from many devices converge identically. Device-scoped fields
never appear in this code path.
"""
from __future__ import annotations

import json

from ..merge import Leaf, three_way_merge, flatten, expand
from ..schema import PROFILE, SESSION, scope_of, KNOWN_SCHEMA, registry
from .db import ServerDB


def _leaves_from_flat(values: dict, author: str, stamp: int,
                      source: str = "user") -> dict[str, Leaf]:
    return {p: Leaf(v, stamp, author, source) for p, v in values.items()}


def _leaf_to_dict(leaf: Leaf) -> dict:
    d = leaf.to_dict()
    d["source"] = leaf.source
    return d


class SyncEngine:
    def __init__(self, db: ServerDB):
        self.db = db

    # ------------------------------------------------------------------ #
    def _seed_defaults(self, group_id: str) -> dict:
        from ..store import default_state
        d = default_state()
        return flatten(d["data"][PROFILE])

    def _fold_group_defaults(self, group_id: str, user_flat: dict,
                             group_row, prev_group_row=None):
        """Overlay current group defaults onto the user profile leaves.

        Enforced leaves are admin-locked. Leaves changed vs the previous
        publication are explicit admin edits. Unchanged, non-enforced leaves
        keep the user's adopted value if they have one, otherwise they are an
        inherited default. A rollback restores content rather than expressing
        new opinions, so restored leaves are treated as defaults too.
        """
        doc = json.loads(group_row["doc_json"])
        enforced = frozenset(json.loads(group_row["enforced_json"]))
        stamp = int(group_row["lamport"])
        if prev_group_row is None:
            prev_group_row = self.db.group_version(
                group_id, int(group_row["version"]) - 1)
        prev_doc = (json.loads(prev_group_row["doc_json"])
                    if prev_group_row is not None else {})
        rolled_from = group_row["rolled_back_from"]
        rollback_source_doc = {}
        if rolled_from:
            rrow = self.db.group_version(group_id, int(rolled_from))
            if rrow is not None:
                rollback_source_doc = json.loads(rrow["doc_json"])

        def keep_user_or_default(path, value):
            raw = user_flat.get(path)
            if raw is not None and raw.get("source") in ("user", "admin"):
                return Leaf(raw["value"], int(raw["stamp"]),
                            raw.get("author", "user"),
                            source=raw.get("source", "user"))
            return Leaf(value, stamp, "admin", source="default")

        leaves: dict[str, Leaf] = {}
        for path, value in doc.items():
            if path in enforced:
                leaves[path] = Leaf(value, stamp, "admin",
                                    source="enforced")
            elif prev_group_row is not None and (
                    path not in prev_doc or prev_doc[path] != value):
                # Explicit admin edit. Rollback-restored values are content
                # restoration (not a new opinion) -> treat as default so an
                # adopted user value survives without a forced conflict.
                restoring = (rolled_from is not None
                             and rollback_source_doc.get(path) == value)
                if restoring:
                    leaves[path] = keep_user_or_default(path, value)
                else:
                    leaves[path] = Leaf(value, stamp, "admin", source="user")
            else:
                # Leaf unchanged by this publication: keep the user's adopted
                # value; seed inherited default only if never adopted.
                leaves[path] = keep_user_or_default(path, value)

        for path, raw in user_flat.items():
            if path in enforced or path in leaves:
                continue
            leaves[path] = Leaf(raw["value"], int(raw["stamp"]),
                                raw.get("author", "user"),
                                source=raw.get("source", "user"))
        return leaves, stamp, enforced, group_row["mode"]

    # ------------------------------------------------------------------ #
    def _merge_segment(self, *, base_flat, base_stamps, server_flat,
                       client_changes, client_author, enforced, mode,
                       max_client_stamp):
        base = {p: Leaf(v, int(m.get("stamp", 0)), m.get("author", "user"),
                        m.get("source", "user"))
                for p, v in base_flat.items() for m in [base_stamps.get(p, {})]}
        server = {p: Leaf(v.value, v.stamp, v.author, v.source)
                  for p, v in server_flat.items()}
        client = {p: Leaf(v.value, v.stamp, v.author, v.source)
                  for p, v in base.items()}
        for path, ch in (client_changes or {}).items():
            client[path] = Leaf(ch["value"], int(ch["stamp"]),
                                ch.get("author", client_author), "user")
        res = three_way_merge(base, server, client, enforced=enforced,
                              mode=mode)
        new_lamport = max([max_client_stamp,
                           max((l.stamp for l in res.leaves.values()),
                               default=0)]) + 1
        return res, new_lamport

    def sync(self, req: dict) -> dict:
        device_id = req["device_id"]
        user_id = req["user_id"]
        group_id = req["group_id"]
        known_schema = int(req.get("known_schema", KNOWN_SCHEMA))
        disp = req.get("displays", {})
        fingerprint_str = disp.get("fingerprint", "")

        self.db.ensure_group(group_id, self._seed_defaults(group_id))
        group_head = self.db.group_head(group_id)

        client_changes = {PROFILE: dict((req.get("profile") or {})
                                        .get("changes", {})),
                          SESSION: dict((req.get("session") or {})
                                        .get("changes", {}))}
        base_versions = {PROFILE: int((req.get("profile") or {})
                                      .get("base_version", 0)),
                         SESSION: int((req.get("session") or {})
                                      .get("base_version", 0))}

        # ----- profile --------------------------------------------------
        prof_head = self.db.profile_head(user_id, group_id)
        enrollment = prof_head is None
        if enrollment:
            # First enrollment: the 3-way merge baseline IS the group
            # defaults, so a user value never loses to "inherited default".
            seed_leaves, _, _, _ = self._fold_group_defaults(
                group_id, {}, group_head)
            prof_version = self.db.save_profile(
                user_id, group_id, int(group_head["version"]),
                {p: _leaf_to_dict(l) for p, l in seed_leaves.items()},
                int(group_head["lamport"]))
            prof_head = self.db.profile_version(user_id, group_id,
                                                prof_version)

        prof_flat = {p: Leaf.from_dict(d)
                     for p, d in json.loads(prof_head["leaves_json"]).items()}

        base_group_version = int(prof_head["base_group_version"])
        if int(group_head["version"]) > base_group_version:
            # New group default publication: fold it in before merging edits.
            # "Explicit admin edit" is always relative to the immediately
            # preceding group version (rollback creates one such edit-set),
            # never relative to a particular device's baseline.
            prev_group_row = self.db.group_version(
                group_id, int(group_head["version"]) - 1)
            # Server view: explicit admin edits dominate; unchanged
            # non-enforced leaves keep the user's adopted value so a new
            # publication of unrelated leaves does not reset them.
            user_flat = {p: {"value": l.value, "stamp": l.stamp,
                             "author": l.author, "source": l.source}
                         for p, l in prof_flat.items()}
            server_flat, group_stamp, enforced, _mode = \
                self._fold_group_defaults(
                    group_id, user_flat, group_head, prev_group_row)
            # The base for the 3-way merge is the user's profile before the
            # new default publication.
            base_flat = {p: l.value for p, l in prof_flat.items()}
            base_stamps = {p: {"stamp": l.stamp, "author": l.author,
                               "source": l.source}
                           for p, l in prof_flat.items()}
            mode = "merge"  # default drift is field-level, never overwrite
        elif enrollment:
            # Baseline = group defaults as they were at enrollment time.
            server_flat = prof_flat
            seed_doc = json.loads(group_head["doc_json"])
            base_flat = dict(seed_doc)
            group_stamp = int(group_head["lamport"])
            base_stamps = {p: {"stamp": group_stamp, "author": "admin",
                               "source": "default"} for p in seed_doc}
            enforced = frozenset(json.loads(group_head["enforced_json"]))
            mode = "merge"
        else:
            server_flat = prof_flat
            base_row = (self.db.profile_version(
                user_id, group_id, base_versions[PROFILE])
                if base_versions[PROFILE] > 0 else None)
            if base_row is not None:
                base_leaves = {p: Leaf.from_dict(d) for p, d in
                               json.loads(base_row["leaves_json"]).items()}
                base_flat = {p: l.value for p, l in base_leaves.items()}
                base_stamps = {p: {"stamp": l.stamp, "author": l.author,
                                   "source": l.source}
                               for p, l in base_leaves.items()}
            else:
                # Ancient baseline pruned: empty base => union, never lossy.
                base_flat, base_stamps = {}, {}
            enforced = frozenset(json.loads(group_head["enforced_json"]))
            mode = "merge"

        # Unknown paths from a newer schema are unknown to registry but must
        # still round-trip: pull client-unknown leaves into the client change
        # map is impossible, so they live in the per-device passthrough bag.
        max_client_stamp = max(
            [int(c["stamp"]) for seg in client_changes.values()
             for c in seg.values()] + [0])

        base_leaf_map = {p: Leaf(v, int(base_stamps.get(p, {}).get("stamp", 0)),
                                base_stamps.get(p, {}).get("author", "user"),
                                base_stamps.get(p, {}).get("source", "user"))
                         for p, v in base_flat.items()}
        client_leaf_map = dict(base_leaf_map)
        for path, ch in (client_changes[PROFILE] or {}).items():
            client_leaf_map[path] = Leaf(ch["value"], int(ch["stamp"]),
                                         ch.get("author", device_id), "user")
        pres, new_lamport = self._merge_segment(
            base_flat=base_flat, base_stamps=base_stamps,
            server_flat=server_flat,
            client_changes=client_changes[PROFILE],
            client_author=device_id, enforced=enforced, mode=mode,
            max_client_stamp=max_client_stamp)

        merged_profile_values = pres.values()
        # Re-derive source for leaves the user never explicitly adopted: if
        # neither the base nor the client carried an explicit user value, an
        # inherited default stays marked "default" so later drift/rollback can
        # update it without being mistaken for a conflict.
        merged_leaves = dict(pres.leaves)
        client_explicit = {p for p, ch in
                           (client_changes[PROFILE] or {}).items()}
        for path, leaf in list(merged_leaves.items()):
            base_leaf = base_leaf_map.get(path)
            client_leaf = client_leaf_map.get(path)
            user_ever = (
                path in client_explicit
                or (base_leaf is not None
                    and base_leaf.source in ("user", "admin"))
                or (client_leaf is not None
                    and client_leaf.source in ("user", "admin")
                    and (base_leaf is None
                         or client_leaf.value != base_leaf.value
                         or client_leaf.stamp != base_leaf.stamp)))
            if not user_ever and leaf.source != "enforced":
                merged_leaves[path] = Leaf(leaf.value, leaf.stamp,
                                           leaf.author, "default")
        new_profile_version = self.db.save_profile(
            user_id, group_id, int(group_head["version"]),
            {p: _leaf_to_dict(l) for p, l in merged_leaves.items()},
            new_lamport)
        for c in pres.conflicts:
            self.db.add_conflict(group_id, user_id, device_id, c,
                                 new_profile_version)

        # ----- session --------------------------------------------------
        sess_head = self.db.get_session(user_id)
        if sess_head is None:
            from ..store import default_state
            seed = flatten(default_state()["data"][SESSION])
            sess_flat = _leaves_from_flat(seed, "default", 0, "default")
        else:
            sess_flat = {p: Leaf.from_dict(d)
                         for p, d in json.loads(
                             sess_head["leaves_json"]).items()}
        sres, sess_lamport = self._merge_segment(
            base_flat={p: l.value for p, l in sess_flat.items()},
            base_stamps={p: {"stamp": l.stamp, "author": l.author,
                             "source": l.source} for p, l in sess_flat.items()},
            server_flat=sess_flat,
            client_changes=client_changes[SESSION],
            client_author=device_id, enforced=frozenset(), mode="merge",
            max_client_stamp=max_client_stamp)
        new_session_version = self.db.save_session(
            user_id, {p: _leaf_to_dict(l) for p, l in sres.leaves.items()},
            sess_lamport)
        for c in sres.conflicts:
            self.db.add_conflict(group_id, user_id, device_id, c,
                                 new_session_version)

        # ----- device bookkeeping (display info is diagnostic only) ------
        self.db.upsert_device(device_id, user_id, group_id,
                              new_profile_version, new_session_version,
                              fingerprint_str, known_schema)

        # ----- forward-compatible passthrough ----------------------------
        for path, ch in client_changes[PROFILE].items():
            if scope_of(path) is None:
                self.db.set_passthrough(
                    device_id, path,
                    {"value": ch["value"], "stamp": ch["stamp"],
                     "author": ch.get("author", device_id)})
        passthrough = self.db.get_passthrough(device_id)

        # ----- optional corruption/quarantine telemetry ------------------
        for report in req.get("corrupt_reports", []):
            self.db.add_corrupt_report(device_id, user_id,
                                       report.get("kind", "unknown"), report)

        return {
            "profile": {"version": new_profile_version,
                        "group_version": int(group_head["version"]),
                        "doc": expand(merged_profile_values),
                        "leaves": {p: _leaf_to_dict(l)
                                   for p, l in merged_leaves.items()}},
            "session": {"version": new_session_version,
                        "doc": expand(sres.values()),
                        "leaves": {p: _leaf_to_dict(l)
                                   for p, l in sres.leaves.items()}},
            "enforced": sorted(enforced),
            "rejected": pres.rejected,
            "conflicts": [c.to_dict() for c in pres.conflicts]
                         + [c.to_dict() for c in sres.conflicts],
            "passthrough": passthrough,
            "lamport": new_lamport,
        }
