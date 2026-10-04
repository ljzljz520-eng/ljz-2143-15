#!/usr/bin/env python3
"""End-to-end walkthrough of the window/background state-sync design.

Run:  python3 demo.py
It starts the backend in-memory, simulates a dual-monitor machine (A) and a
single-monitor machine (B), walks through offline edits, admin background
swap, conflict, rollback and recovery, and prints what happened at every step.
"""
from __future__ import annotations

import json
import tempfile
import urllib.request

from agent.agent import SyncAgent
from synckit.geometry import Monitor
from synckit.server.app import create_server, serve_forever_in_thread
from synckit.store import StateStore


def hr(title):
    print(f"\n=== {title} " + "=" * (60 - len(title)))


def main():
    srv = create_server(":memory:", "127.0.0.1", 0)
    port = srv.server_address[1]
    serve_forever_in_thread(srv)
    url = f"http://127.0.0.1:{port}"

    def api(path, body=None):
        if body is None:
            with urllib.request.urlopen(url + path) as r:
                return json.loads(r.read())
        req = urllib.request.Request(
            url + path, data=json.dumps(body).encode(),
            headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(req) as r:
            return json.loads(r.read())

    dual = [Monitor("eDP", 0, 0, 1920, 1080, True),
            Monitor("HDMI", 1920, 0, 1920, 1080)]
    single = [Monitor("eDP", 0, 0, 1366, 768, True)]

    A = StateStore(tempfile.mkdtemp(), device_id="dev-A-dual")
    B = StateStore(tempfile.mkdtemp(), device_id="dev-B-single")
    agA, agB = SyncAgent(A, url), SyncAgent(B, url)

    hr("1. 启动：各自按本机显示器修复窗口并生成指纹")
    rA = A.update_displays(dual)[0]
    rB = B.update_displays(single)[0]
    print(f"A(双屏) 窗口: {rA}  fp={A.get('displays.fingerprint')}")
    print(f"B(单屏) 窗口: {rB}  fp={B.get('displays.fingerprint')}")

    hr("2. A 把窗口放到第二屏，并把主题强调色改成绿色")
    A.set_local("window.x", 2100); A.set_local("window.width", 1600)
    A.set_synced("theme.accent", "#00ff88")
    agA.sync_once()
    agB.sync_once()
    print(f"A 坐标 x={A.get('window.x')}（device，不出网）")
    print(f"B 收到主题 accent={B.get('theme.accent')}（profile，跨设备）")
    print(f"B 坐标 x={B.get('window.x')}（绝不是 A 的 2100）")

    hr("3. 断网：A 本地移动窗口 + 改强调色；后台发布新背景图")
    A.set_local("window.x", 2500)
    A.set_synced("theme.accent", "#00aaff")
    api("/api/admin/groups/default/publish",
        {"doc": {"theme.background.uri": "https://cdn/sunset.png",
                 "theme.background.mode": "fill",
                 "theme.accent": "#ff0000", "theme.dark": True},
         "mode": "merge", "enforced": [], "by": "admin",
         "note": "季节背景 + 红色强调"})
    r = agA.sync_once()
    print(f"重连后 A 坐标 x={A.get('window.x')}（本地移动保留）")
    print(f"        A 背景={A.get('theme.background.uri')}（后台新图保留）")
    ac = [c for c in r["conflicts"] if c["path"] == "theme.accent"]
    if ac:
        c = ac[0]
        print(f"        accent 冲突: base={c['base']!r} client={c['client']!r}"
              f" server={c['server']!r} -> {c['resolved_value']!r}"
              f" ({c['resolution']}, winner={c['winner']})")

    hr("4. B 单屏机同步：拿到背景，仍无 A 的坐标")
    agB.sync_once()
    print(f"B 背景={B.get('theme.background.uri')}  坐标 x={B.get('window.x')}")

    hr("5. 服务器回退：用旧内容发新版本（版本号继续递增）")
    versions = api("/api/admin/groups/default/versions")
    first = versions[0]["version"]
    new_v = api("/api/admin/groups/default/rollback",
                {"version": first, "by": "admin"})["version"]
    agA.sync_once()
    print(f"回退发布为 v{new_v}（不复用旧版本号）；"
          f"A 背景={A.get('theme.background.uri')}")

    hr("6. 冲突审计页面可见原始三方值与最终来源")
    for c in api("/api/admin/conflicts")[:3]:
        print(f"  {c['path']}: {c['base_json']!r} / client={c['client_json']!r}"
              f" / server={c['server_json']!r} -> {c['resolved_json']!r}"
              f" [{c['resolution']} by {c['winner']}]")

    hr("7. 损坏配置：隔离副本 + 自动回退值不广播")
    d = tempfile.mkdtemp()
    with open(f"{d}/state.json", "w") as f:
        f.write("{broken")
    C = StateStore(d, device_id="dev-C-recovered")
    agC = SyncAgent(C, url)
    ev = [e["kind"] for e in C.recovery_events]
    agC.sync_once()
    print(f"恢复事件: {ev}")
    print(f"待同步叶子（应为空，fallback 不广播）: {list(C.pending_changes())}")
    print(f"诊断副本: {sorted(__import__('glob').glob(d + '/diag/*'))}")
    diag = api("/api/admin/diagnostics")
    print(f"后台诊断记录设备: {[r['device_id'] for r in diag]}")

    print("\n演示完成。Web 管理端：", url)


if __name__ == "__main__":
    main()
