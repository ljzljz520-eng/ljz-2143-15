"""Verifiable, ordered configuration migrations.

Rules:
  * schema_version only moves forward, one step per migration;
  * each step is a pure function with a structural verifier and an inverse on
    the canonical sample, so upgrade behaviour is provable in tests;
  * documents NEWER than this client are never migrated/downgraded and unknown
    leaves are preserved by the store (forward compatibility).
"""
from __future__ import annotations

from typing import Callable

from .schema import KNOWN_SCHEMA


def _v1_to_v2(doc: dict) -> dict:
    # window.w/h -> width/height ; profile.bg -> theme.background.uri
    win = doc.get("device", {}).get("window", {})
    if "w" in win:
        win["width"] = win.pop("w")
    if "h" in win:
        win["height"] = win.pop("h")

    profile = doc.get("profile", {})
    bg = profile.pop("bg", None)
    theme = profile.setdefault("theme", {})
    theme.setdefault("background", {})
    if bg is not None and "uri" not in theme["background"]:
        theme["background"]["uri"] = bg
    theme["background"].setdefault("mode", "fill")
    if "dark" in profile and "dark" not in theme:
        theme["dark"] = profile.pop("dark")
    else:
        profile.pop("dark", None)

    doc["schema_version"] = 2
    return doc


def _v2_to_v3(doc: dict) -> dict:
    theme = doc.get("profile", {}).setdefault("theme", {})
    theme.setdefault("accent", "#2b6cb0")
    theme.setdefault("background", {}).setdefault("mode", "fill")
    session = doc.setdefault("session", {})
    session.setdefault("ui", {})
    session["ui"].setdefault("density", "normal")
    session["ui"].setdefault("language", "en")
    doc["schema_version"] = 3
    return doc


def _v2_back(doc: dict) -> dict:  # inverse on canonical v1 sample only
    theme = doc.get("profile", {}).get("theme", {})
    bg = theme.get("background", {})
    profile = doc.get("profile", {})
    profile.pop("theme", None)
    if "uri" in bg:
        profile["bg"] = bg["uri"]
    if "dark" in theme:
        profile["dark"] = theme["dark"]
    win = doc.get("device", {}).get("window", {})
    if "width" in win:
        win["w"] = win.pop("width")
    if "height" in win:
        win["h"] = win.pop("height")
    doc["schema_version"] = 1
    return doc


def _v3_back(doc: dict) -> dict:
    theme = doc.get("profile", {}).get("theme", {})
    theme.pop("accent", None)
    bg = theme.get("background", {})
    bg.pop("mode", None)
    doc.get("session", {}).pop("ui", None)
    doc["schema_version"] = 2
    return doc


def _check_v2(doc: dict) -> None:
    win = doc["device"]["window"]
    assert "w" not in win and "h" not in win
    assert "width" in win and "height" in win
    theme = doc["profile"]["theme"]
    assert "background" in theme and "uri" in theme["background"]
    assert "mode" in theme["background"]
    assert "bg" not in doc["profile"]


def _check_v3(doc: dict) -> None:
    theme = doc["profile"]["theme"]
    assert "accent" in theme
    assert doc["session"]["ui"]["density"] in {
        "compact", "normal", "comfortable"}


# (to_version, up, down, verifier, canonical pre-image)
V1_SAMPLE = {
    "schema_version": 1,
    "device": {"window": {"x": -1, "y": -1, "w": 1280, "h": 720, "maximized": False}},
    "profile": {"bg": "assets/background.png", "dark": False},
    "session": {},
}

migration_chain: list[dict] = [
    {"to": 2, "up": _v1_to_v2, "down": _v2_back, "check": _check_v2},
    {"to": 3, "up": _v2_to_v3, "down": _v3_back, "check": _check_v3},
]


def migrate(doc: dict, target: int = KNOWN_SCHEMA) -> dict:
    """Run ordered migrations up to target. Newer docs are returned untouched."""
    version = int(doc.get("schema_version", 1))
    if version > KNOWN_SCHEMA:
        return doc  # forward-compatible: never downgrade, never rewrite
    for step in migration_chain:
        if version < step["to"] <= target:
            doc = step["up"](doc)
            version = int(doc["schema_version"])
    return doc


def verify_migration() -> None:
    """Upgrade the canonical sample step by step, verify, and round-trip back."""
    doc = V1_SAMPLE
    for step in migration_chain:
        before_version = int(doc["schema_version"])
        upgraded = step["up"](dict_deepcopy(doc))
        assert int(upgraded["schema_version"]) == before_version + 1
        step["check"](upgraded)
        # the step itself is idempotent (re-applying on its own output is safe)
        again = step["up"](dict_deepcopy(upgraded))
        assert again == upgraded
        # inverse round trip on the canonical sample
        back = step["down"](dict_deepcopy(upgraded))
        assert back == doc, f"round trip failed at v{step['to']}"
        doc = upgraded


def dict_deepcopy(d: dict) -> dict:
    import copy
    return copy.deepcopy(d)
