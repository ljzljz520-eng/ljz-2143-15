"""Display topology and the "window must stay visible/operable" rules.

Everything here operates on device-scoped data only. It is the mechanism that
prevents a dual-monitor machine's coordinates being forced onto a single
monitor machine: coordinates are repaired against *local* monitor bounds and
are never sent over the wire.
"""
from __future__ import annotations

from dataclasses import dataclass

MIN_W = 320
MIN_H = 240


@dataclass(frozen=True)
class Monitor:
    id: str
    x: int
    y: int
    w: int
    h: int
    primary: bool = False

    @property
    def right(self) -> int:
        return self.x + self.w

    @property
    def bottom(self) -> int:
        return self.y + self.h


def fingerprint(monitors: list[Monitor]) -> str:
    parts = sorted(f"{m.w}x{m.h}@{m.x},{m.y}" for m in monitors)
    return "|".join(parts)


def _intersection(w: tuple[int, int, int, int], m: Monitor) -> int:
    x, y, width, height = w
    ix = max(0, min(x + width, m.right) - max(x, m.x))
    iy = max(0, min(y + height, m.bottom) - max(y, m.y))
    return ix * iy


def _primary(monitors: list[Monitor]) -> Monitor:
    for m in monitors:
        if m.primary:
            return m
    return monitors[0]


def clamp_window(win: tuple[int, int, int, int], m: Monitor) -> tuple[int, int, int, int]:
    """Shrink (if needed) and move a window so it is fully inside monitor m."""
    x, y, width, height = win
    width = max(MIN_W, min(width, m.w))
    height = max(MIN_H, min(height, m.h))
    x = max(m.x, min(x, m.right - width))
    y = max(m.y, min(y, m.bottom - height))
    return x, y, width, height


def geometry_repair(
    win: tuple[int, int, int, int],
    monitors: list[Monitor],
) -> tuple[tuple[int, int, int, int], str]:
    """Return repaired window rect and a human-readable reason.

    Guarantees for any non-empty monitor list:
      * the result intersects (and is fully inside the union of) monitors;
      * at least 320x240 is visible, title bar reachable, edges grabbable.
    """
    if not monitors:
        # No display reported: keep object alive at a safe virtual position so
        # that when a display returns the window can be mapped immediately.
        return (0, 0, max(MIN_W, win[2]), max(MIN_H, win[3])), "no-monitor"

    x, y, width, height = win
    if x < 0 or y < 0:  # SDL_WINDOWPOS_CENTERED / UNDEFINED sentinels
        m = _primary(monitors)
        width = max(MIN_W, min(width, m.w))
        height = max(MIN_H, min(height, m.h))
        cx, cy = m.x + (m.w - width) // 2, m.y + (m.h - height) // 2
        return (cx, cy, width, height), "centered-default"

    best, best_area = None, 0
    for m in monitors:
        area = _intersection(win, m)
        if area > best_area:
            best, best_area = m, area

    if best is not None and best_area > 0:
        repaired = clamp_window(win, best)
        if repaired != win:
            return repaired, f"clamped-to-monitor:{best.id}"
        return win, f"unchanged-on-monitor:{best.id}"

    # Window lived on a display that is now unplugged.
    m = _primary(monitors)
    width = max(MIN_W, min(width, m.w))
    height = max(MIN_H, min(height, m.h))
    cx, cy = m.x + (m.w - width) // 2, m.y + (m.h - height) // 2
    return (cx, cy, width, height), f"moved-to-primary:{m.id}"


def is_visible_and_operable(
    win: tuple[int, int, int, int], monitors: list[Monitor]
) -> bool:
    x, y, width, height = win
    if width < MIN_W or height < MIN_H:
        return False
    for m in monitors:
        if _intersection(win, m) == width * height:
            return True
    return False
