"""Field registry: owner / scope / sync classification.

The single source of truth for "what is machine-specific and must never leave
the device" vs "what is a portable theme preference". The C terminal carries a
mirror of this table (src/config_registry.h); server-side writes of device
fields are rejected.
"""
from __future__ import annotations

from dataclasses import dataclass

DEVICE = "device"
PROFILE = "profile"
SESSION = "session"

CLIENT = "client"
USER = "user"
ADMIN = "admin"

# Bumped by ordered migrations in migrations.py. Old clients MUST preserve
# unknown leaves from newer schema versions (forward compatibility).
KNOWN_SCHEMA = 3


@dataclass(frozen=True)
class FieldSpec:
    path: str
    scope: str
    owner: str
    sync: bool
    description: str


# Leaf path -> spec. Anything not listed here is "unknown" and therefore
# preserved opaquely by older clients; never dropped on save.
_REGISTRY: dict[str, FieldSpec] = {
    # ---- machine-specific: never synchronised ---------------------------
    "window.x": FieldSpec("window.x", DEVICE, CLIENT, False,
                          "window left edge in display coordinates"),
    "window.y": FieldSpec("window.y", DEVICE, CLIENT, False,
                          "window top edge in display coordinates"),
    "window.width": FieldSpec("window.width", DEVICE, CLIENT, False,
                              "window width on this machine"),
    "window.height": FieldSpec("window.height", DEVICE, CLIENT, False,
                               "window height on this machine"),
    "window.maximized": FieldSpec("window.maximized", DEVICE, CLIENT, False,
                                  "maximised on this machine"),
    "window.monitor": FieldSpec("window.monitor", DEVICE, CLIENT, False,
                                "target monitor id within local fingerprint"),
    "displays.fingerprint": FieldSpec("displays.fingerprint", DEVICE,
                                      CLIENT, False, "local display topology hash"),

    # ---- portable theme / background: per user, per group ---------------
    "theme.background.uri": FieldSpec("theme.background.uri", PROFILE,
                                      USER, True, "background image uri"),
    "theme.background.mode": FieldSpec("theme.background.mode", PROFILE,
                                       USER, True, "fill/fit/stretch/center"),
    "theme.accent": FieldSpec("theme.accent", PROFILE, USER, True,
                              "accent colour"),
    "theme.dark": FieldSpec("theme.dark", PROFILE, USER, True,
                            "dark mode toggle"),

    # ---- user session prefs: cross device, group independent ------------
    "ui.density": FieldSpec("ui.density", SESSION, USER, True,
                            "compact/normal/comfortable"),
    "ui.language": FieldSpec("ui.language", SESSION, USER, True,
                             "interface language"),
}

registry = _REGISTRY


def scope_of(path: str) -> str | None:
    spec = _REGISTRY.get(path)
    return spec.scope if spec else None


def is_synced(path: str) -> bool:
    spec = _REGISTRY.get(path)
    return bool(spec and spec.sync)
