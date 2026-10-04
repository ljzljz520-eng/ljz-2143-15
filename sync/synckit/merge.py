"""Three-way merge over dotted leaf paths.

base -> server head and base -> client edits are compared leaf by leaf.
Leaf-level merge is the synchronisation default; the whole-document overwrite
mode is only used by an explicit admin publish action and still records every
collision for audit.
"""
from __future__ import annotations

from dataclasses import dataclass, field

from .schema import PROFILE, scope_of


@dataclass(frozen=True)
class Leaf:
    value: object
    stamp: int          # hybrid logical clock; monotonic per author
    author: str         # device id / user id / "admin"
    source: str = "user"  # user | admin | enforced | default | fallback

    def to_dict(self) -> dict:
        return {"value": self.value, "stamp": self.stamp,
                "author": self.author, "source": self.source}

    @staticmethod
    def from_dict(d: dict) -> "Leaf":
        return Leaf(d["value"], int(d["stamp"]), str(d["author"]),
                    d.get("source", "user"))


@dataclass
class MergeConflict:
    path: str
    base: object | None
    server: object | None
    client: object | None
    resolution: str
    winner: str
    resolved_value: object

    def to_dict(self) -> dict:
        return {
            "path": self.path,
            "base": self.base,
            "server": self.server,
            "client": self.client,
            "resolution": self.resolution,
            "winner": self.winner,
            "resolved_value": self.resolved_value,
        }


@dataclass
class MergeResult:
    leaves: dict[str, Leaf] = field(default_factory=dict)
    conflicts: list[MergeConflict] = field(default_factory=list)
    rejected: list[dict] = field(default_factory=list)

    def values(self) -> dict[str, object]:
        return {p: leaf.value for p, leaf in self.leaves.items()}


def flatten(doc: dict, prefix: str = "") -> dict[str, object]:
    """Nested dict -> dotted-path leaves. Arrays are opaque leaf values."""
    out: dict[str, object] = {}
    for key, val in doc.items():
        path = f"{prefix}.{key}" if prefix else key
        if isinstance(val, dict) and val:
            out.update(flatten(val, path))
        else:
            out[path] = val
    return out


def expand(flat: dict[str, object]) -> dict:
    root: dict = {}
    for path, value in flat.items():
        node = root
        parts = path.split(".")
        for part in parts[:-1]:
            node = node.setdefault(part, {})
        node[parts[-1]] = value
    return root


def _newer(a: Leaf | None, b: Leaf | None) -> Leaf | None:
    if a is None:
        return b
    if b is None:
        return a
    if a.stamp != b.stamp:
        return a if a.stamp > b.stamp else b
    return a if a.author >= b.author else b


def three_way_merge(
    base: dict[str, Leaf],
    server: dict[str, Leaf],
    client: dict[str, Leaf],
    enforced: frozenset[str] = frozenset(),
    mode: str = "merge",
    default_sources: frozenset[str] = frozenset({"default"}),
) -> MergeResult:
    """Merge three flat leaf maps.

    mode="merge"     : per-leaf LWW, enforced leaves locked to server, and
                       inherited *default* leaves never override an explicit
                       client value (that is a user edit, not a conflict);
    mode="overwrite" : admin snapshot publish, every server leaf wins,
                       collisions are still recorded in the audit trail.
    Device-scoped paths must never reach this function; callers filter them.
    """
    res = MergeResult()
    paths = set(base) | set(server) | set(client)

    for path in sorted(paths):
        if scope_of(path) == "device":  # defensive: device fields never merge
            continue
        b, s, c = base.get(path), server.get(path), client.get(path)
        bv = b.value if b else None
        sv = s.value if s else None
        cv = c.value if c else None

        # Administrative lock always wins; rejected client edits are reported,
        # never silently dropped: raw client value lands in the audit record.
        if path in enforced:
            if s is not None:
                res.leaves[path] = s
                if c is not None and c.value != s.value:
                    res.conflicts.append(MergeConflict(
                        path, bv, sv, cv, "enforced-admin-wins",
                        s.author, s.value))
                    res.rejected.append({"path": path, "value": cv,
                                         "reason": "field-is-enforced"})
            continue

        if mode == "overwrite" and s is not None:
            res.leaves[path] = s
            if b is not None and c is not None and cv != bv and cv != sv:
                res.conflicts.append(MergeConflict(
                    path, bv, sv, cv, "admin-overwrite", s.author, s.value))
            continue

        s_changed = s is not None and (
            b is None or sv != bv or s.stamp != b.stamp or s.author != b.author)
        c_changed = c is not None and (
            b is None or cv != bv or c.stamp != b.stamp or c.author != b.author)

        if sv == cv:
            # Same final value: newest metadata, no conflict.
            res.leaves[path] = _newer(s, c) if (s_changed or c_changed) \
                else (s or c or b)
            continue

        # One-sided changes.
        if s_changed and not c_changed:
            res.leaves[path] = s or b
            continue
        if c_changed and not s_changed:
            # User adopts an inherited default (base+server are the same
            # default, client carries the first explicit value).
            if b is not None and (s is None or sv == bv) \
                    and c.source not in default_sources \
                    and b.source in default_sources:
                res.leaves[path] = c
            else:
                res.leaves[path] = c or b
            continue
        # Both "changed" by stamps but the server only repeats the inherited
        # default VALUE (base->server is semantically a no-op): a client
        # adopting that field with an explicit value wins.
        if b is not None and s is not None and sv == bv \
                and s.source in default_sources \
                and c.source not in default_sources and cv != bv:
            res.leaves[path] = c
            continue
        # Enrollment-style: base may carry the default while the client sends
        # its first explicit value and the server fold re-stamped the default.
        if b is not None and s is not None and b.source in default_sources \
                and s.source in default_sources and sv == bv \
                and c.source not in default_sources and cv != bv:
            res.leaves[path] = c
            continue

        # Both sides changed differently. A user adopting a plain inherited
        # default keeps their value (the default was never an explicit edit).
        s_is_default = s is not None and s.source in default_sources
        c_is_explicit = c is not None and c.source not in default_sources
        base_was_default = b is None or b.source in default_sources
        if s_is_default and c_is_explicit and base_was_default:
            res.leaves[path] = c
            continue

        # Genuine divergence between two explicit writers: LWW, with the
        # author id as deterministic tie-break. Raw values are audited.
        winner = _newer(s, c)
        assert winner is not None
        res.leaves[path] = winner
        res.conflicts.append(MergeConflict(
            path, bv, sv, cv,
            "lww-client" if winner is c else "lww-server",
            winner.author, winner.value))


    return res
