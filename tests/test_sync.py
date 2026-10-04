#!/usr/bin/env python3
"""状态同步系统端到端测试。

覆盖需求场景：
  - 断网期间本地移动窗口 + 后台换图，重连后各自保留（字段级合并）
  - 机器相关坐标不跨设备（双屏位置不强推到单屏机器）
  - 本地文件写一半断电 → 备份恢复 + 损坏隔离诊断副本
  - 双重损坏 → 兜底默认（source=fallback）绝不同步覆盖全组
  - 屏幕拔出 → 窗口钳制回可见区域（可见可操作）
  - 旧设备长时间离线回归 → 快照 resync
  - 服务器回退 → epoch/seq 检测 + resync，本机坐标不丢
  - 可验证迁移 + 未知新版字段不被旧客户端抹掉
  - 冲突原始记录与最终来源可查
  - 诊断副本上传可查
"""

import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import unittest
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TERMCTL = ROOT / "termctl"
sys.path.insert(0, str(ROOT))

from server.migrate import MigrationError, migrate, validate  # noqa: E402


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class Server:
    def __init__(self, db_path, allow_test_ops=True):
        self.db_path = str(db_path)
        self.port = free_port()
        cmd = [sys.executable, "-m", "server.server", "--db", self.db_path,
               "--port", str(self.port), "--host", "127.0.0.1"]
        if allow_test_ops:
            cmd.append("--allow-test-ops")
        self.proc = subprocess.Popen(
            cmd, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True)
        first = self.proc.stdout.readline().strip()
        if not first.startswith("PORT"):
            raise RuntimeError(f"server failed to start: {first}")
        self.port = int(first.split()[1])
        self.url = f"http://127.0.0.1:{self.port}"

    def stop(self):
        self.proc.terminate()
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()

    def get(self, path):
        import urllib.request
        with urllib.request.urlopen(self.url + path, timeout=5) as r:
            return json.loads(r.read().decode())

    def post(self, path, obj):
        import urllib.request
        req = urllib.request.Request(
            self.url + path, data=json.dumps(obj).encode(),
            headers={"Content-Type": "application/json"}, method="POST")
        with urllib.request.urlopen(req, timeout=5) as r:
            return json.loads(r.read().decode())

    def effective(self, device, user):
        return self.get(f"/api/admin/effective?device_id={device}&user={user}")["fields"]


def term(state_dir, device, user, server, *args, schema=None):
    cmd = [str(TERMCTL), "--dir", str(state_dir), "--device", device,
           "--user", user, "--server", server]
    if schema:
        cmd += ["--schema", str(schema)]
    cmd += list(args)
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
    return p


def term_json(state_dir, device, user, server, *args, schema=None, ok=True):
    p = term(state_dir, device, user, server, *args, schema=schema)
    if ok:
        assert p.returncode == 0, f"termctl {args} failed: {p.stderr}"
    return p, json.loads(p.stdout)


def write_state_file(path, payload, truncate=None):
    """按 TERMREC1 格式写状态文件（测试迁移/损坏恢复）。"""
    data = json.dumps(payload).encode()
    crc = zlib.crc32(data)
    content = f"TERMREC1 {len(data)} {crc:x}\n".encode() + data
    if truncate:
        content = content[:truncate]
    Path(path).write_bytes(content)


def read_state_doc(state_dir):
    raw = Path(state_dir, "state.json").read_bytes()
    header, _, payload = raw.partition(b"\n")
    magic, length, crc = header.split()
    payload = payload[:int(length)]
    assert zlib.crc32(payload) == int(crc, 16)
    return json.loads(payload)


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="statesync-"))
        self.server = Server(self.tmp / "test.db")

    def tearDown(self):
        self.server.stop()
        shutil.rmtree(self.tmp, ignore_errors=True)

    def devdir(self, name):
        d = self.tmp / name
        d.mkdir(exist_ok=True)
        return d


class TestFieldMerge(Base):
    def test_offline_window_move_and_backend_image_change_both_kept(self):
        """断网期间本地移动窗口 + 后台换图，重连后各保留合理结果。"""
        d1 = self.devdir("d1")
        term_json(d1, "dev-1", "alice", self.server.url, "init")
        term_json(d1, "dev-1", "alice", self.server.url, "sync")

        # 断网期间：本地移动窗口（离线操作，只入 journal）
        term_json(d1, "dev-1", "alice", "http://127.0.0.1:1", "move", "700", "450")
        # 同时后台换图（Web 管理端改远程默认）
        self.server.post("/api/admin/defaults", {
            "admin": "web-admin",
            "changes": [{"field": "prefs.background",
                         "value": "assets/bg-night.png", "base_version": 0}]})

        # 重连同步
        term_json(d1, "dev-1", "alice", self.server.url, "sync")
        _, doc = term_json(d1, "dev-1", "alice", self.server.url, "show")

        # 窗口移动（设备字段）与背景更换（管理端字段）都保留
        self.assertEqual(doc["machine"]["window"]["x"], 700)
        self.assertEqual(doc["machine"]["window"]["y"], 450)
        self.assertEqual(doc["prefs"]["background"], "assets/bg-night.png")

        eff = self.server.effective("dev-1", "alice")
        self.assertEqual(json.loads(eff["machine.window.x"]["value"]), 700)
        self.assertEqual(eff["machine.window.x"]["layer"], "device")
        self.assertEqual(json.loads(eff["prefs.background"]["value"]),
                         "assets/bg-night.png")

    def test_dual_screen_position_never_pushed_to_single_screen_machine(self):
        """双屏机器的坐标绝不强推到另一台单屏机器。"""
        d1, d2 = self.devdir("d1"), self.devdir("d2")
        dual = json.dumps([{"x": 0, "y": 0, "w": 1920, "h": 1080},
                           {"x": 1920, "y": 0, "w": 1920, "h": 1080}])
        term_json(d1, "dev-1", "alice", self.server.url, "monitors", dual)
        term_json(d1, "dev-1", "alice", self.server.url, "move", "2500", "300")
        term_json(d1, "dev-1", "alice", self.server.url, "set", "prefs.theme", "dark")
        term_json(d1, "dev-1", "alice", self.server.url, "sync")

        # 同用户的单屏机器同步：主题跨设备同步，坐标不跨设备
        term_json(d2, "dev-2", "alice", self.server.url, "monitors",
                  json.dumps([{"x": 0, "y": 0, "w": 1920, "h": 1080}]))
        term_json(d2, "dev-2", "alice", self.server.url, "sync")
        _, doc2 = term_json(d2, "dev-2", "alice", self.server.url, "show")
        self.assertEqual(doc2["prefs"]["theme"], "dark")          # 用户级同步 ✓
        self.assertEqual(doc2["machine"]["window"]["x"], 100)     # 坐标不跨设备 ✓

        eff2 = self.server.effective("dev-2", "alice")
        self.assertEqual(json.loads(eff2["machine.window.x"]["value"]), 100)


class TestRecovery(Base):
    def test_power_loss_mid_write_recovers_from_backup(self):
        """本地文件写一半断电：CRC 检出 → 隔离诊断副本 → 备份恢复。"""
        d1 = self.devdir("d1")
        term_json(d1, "dev-1", "alice", self.server.url, "init")
        term_json(d1, "dev-1", "alice", self.server.url, "move", "400", "300")
        term_json(d1, "dev-1", "alice", self.server.url, "set", "prefs.theme", "red")

        term_json(d1, "dev-1", "alice", self.server.url, "corrupt")  # 截断模拟断电
        p, doc = term_json(d1, "dev-1", "alice", self.server.url, "show")

        self.assertIn("recovered from backup", p.stderr)
        # 备份是上一份完好记录（checkpoint 语义：最后一次写入丢失，
        # 之前已落盘的状态完整恢复）：窗口位置可恢复且可见
        self.assertEqual(doc["machine"]["window"]["x"], 400)
        self.assertEqual(doc["machine"]["window"]["y"], 300)
        # 损坏配置留了诊断副本
        corrupt = list(Path(d1).glob("*.corrupt-*"))
        self.assertTrue(corrupt, "诊断副本未生成")
        self.assertIn(b"TERMREC1", corrupt[0].read_bytes()[:20])
        # 恢复日志留痕
        self.assertTrue((Path(d1) / "recovery.log").exists())
        # 恢复后同步正常（幂等重放，不产生伪冲突）
        term_json(d1, "dev-1", "alice", self.server.url, "sync")
        conflicts = self.server.get("/api/admin/conflicts")["conflicts"]
        self.assertEqual(conflicts, [])

    def test_double_corruption_fallback_defaults_not_synced(self):
        """主备双损坏 → 兜底默认；自动回退默认值绝不同步覆盖全组设备。"""
        d1, d2 = self.devdir("d1"), self.devdir("d2")
        term_json(d1, "dev-1", "alice", self.server.url, "set", "prefs.theme", "dark")
        term_json(d1, "dev-1", "alice", self.server.url, "sync")
        term_json(d2, "dev-2", "alice", self.server.url, "sync")

        term_json(d1, "dev-1", "alice", self.server.url, "corrupt", "--bak")
        p, doc = term_json(d1, "dev-1", "alice", self.server.url, "show")
        self.assertIn("fallback defaults", p.stderr)
        self.assertEqual(doc["prefs"]["theme"], "light")  # 本地兜底

        # 同步后：服务器上的 dark 不被本地兜底 light 覆盖，反而拉回 dark
        term_json(d1, "dev-1", "alice", self.server.url, "sync")
        eff = self.server.effective("dev-1", "alice")
        self.assertEqual(json.loads(eff["prefs.theme"]["value"]), "dark")
        _, doc2 = term_json(d2, "dev-2", "alice", self.server.url, "sync")
        _, doc2 = term_json(d2, "dev-2", "alice", self.server.url, "show")
        self.assertEqual(doc2["prefs"]["theme"], "dark")  # 全组不受影响

        # 服务器侧硬拒绝 source=fallback 的写入
        r = self.server.post("/api/push", {
            "device_id": "dev-9", "user": "alice", "base_seq": 0,
            "changes": [{"field": "prefs.theme", "value": "light",
                         "base_version": 0, "source": "fallback"}]})
        self.assertEqual(r["results"][0]["status"], "rejected_fallback")
        self.assertEqual(json.loads(self.server.effective("dev-9", "alice")
                                    ["prefs.theme"]["value"]), "dark")

    def test_monitor_unplug_window_clamped_visible(self):
        """屏幕拔出：窗口钳制回主屏可见区域，保持可见可操作。"""
        d1 = self.devdir("d1")
        dual = json.dumps([{"x": 0, "y": 0, "w": 1920, "h": 1080},
                           {"x": 1920, "y": 0, "w": 1920, "h": 1080}])
        single = json.dumps([{"x": 0, "y": 0, "w": 1920, "h": 1080}])
        term_json(d1, "dev-1", "alice", self.server.url, "monitors", dual)
        term_json(d1, "dev-1", "alice", self.server.url, "move", "2500", "300")
        p, _ = term_json(d1, "dev-1", "alice", self.server.url, "monitors", single)
        self.assertIn("clamped", p.stderr)
        _, win = term_json(d1, "dev-1", "alice", self.server.url, "validate")
        self.assertGreaterEqual(win["x"], 0)
        self.assertLessEqual(win["x"] + win["width"], 1920)
        self.assertGreaterEqual(win["y"], 0)
        self.assertLessEqual(win["y"] + win["height"], 1080)
        self.assertGreaterEqual(win["width"], 320)   # 最小可操作尺寸
        self.assertGreaterEqual(win["height"], 200)

    def test_recovered_window_visible_and_operable_after_all_failures(self):
        """断电 + 拔屏叠加：恢复出的窗口必须可见可操作。"""
        d1 = self.devdir("d1")
        dual = json.dumps([{"x": 0, "y": 0, "w": 1920, "h": 1080},
                           {"x": 1920, "y": 0, "w": 1920, "h": 1080}])
        single = json.dumps([{"x": 0, "y": 0, "w": 1366, "h": 768}])
        term_json(d1, "dev-1", "alice", self.server.url, "monitors", dual)
        term_json(d1, "dev-1", "alice", self.server.url, "move", "2600", "400")
        term_json(d1, "dev-1", "alice", self.server.url, "resize", "1800", "900")
        term_json(d1, "dev-1", "alice", self.server.url, "corrupt")
        term_json(d1, "dev-1", "alice", self.server.url, "monitors", single)
        _, win = term_json(d1, "dev-1", "alice", self.server.url, "validate")
        self.assertGreaterEqual(win["x"], 0)
        self.assertLessEqual(win["x"] + win["width"], 1366)
        self.assertGreaterEqual(win["y"], 0)
        self.assertLessEqual(win["y"], 768 - 20)  # 标题栏在屏幕内
        self.assertGreaterEqual(win["width"], 320)
        self.assertGreaterEqual(win["height"], 200)

    def test_window_restored_from_server_after_total_local_loss(self):
        """本地状态全丢：本机坐标从服务器按设备存档恢复。"""
        d1 = self.devdir("d1")
        term_json(d1, "dev-1", "alice", self.server.url, "move", "700", "500")
        term_json(d1, "dev-1", "alice", self.server.url, "sync")
        shutil.rmtree(d1)
        d1.mkdir()
        term_json(d1, "dev-1", "alice", self.server.url, "sync")
        _, doc = term_json(d1, "dev-1", "alice", self.server.url, "show")
        self.assertEqual(doc["machine"]["window"]["x"], 700)
        self.assertEqual(doc["machine"]["window"]["y"], 500)


class TestResync(Base):
    def test_long_offline_device_snapshot_resync(self):
        """旧设备长时间离线后回归：变更日志已裁剪 → 快照 resync。"""
        d1, d2 = self.devdir("d1"), self.devdir("d2")
        # dev-1 先产生一笔同步，拿到非零的版本基线（否则 since_seq=0 按新设备处理）
        term_json(d1, "dev-1", "alice", self.server.url, "set", "prefs.font_size", "14")
        term_json(d1, "dev-1", "alice", self.server.url, "sync")
        seq_after_d1 = self.server.get("/api/health")["server_seq"]
        self.assertGreater(seq_after_d1, 0)

        # dev-1 离线期间，世界继续变化
        term_json(d2, "dev-2", "alice", self.server.url, "set", "prefs.theme", "ocean")
        term_json(d2, "dev-2", "alice", self.server.url, "sync")
        self.server.post("/api/admin/defaults", {
            "admin": "web-admin",
            "changes": [{"field": "prefs.background", "value": "assets/bg-new.png",
                         "base_version": 0}]})
        # 裁剪变更日志，模拟长离线超出保留窗
        self.server.post("/api/test/prune", {"before_seq": seq_after_d1 + 2})

        # dev-1 离线期间本地移动过窗口，回归后同步
        term_json(d1, "dev-1", "alice", "http://127.0.0.1:1", "move", "600", "400")
        p, out = term_json(d1, "dev-1", "alice", self.server.url, "sync")
        self.assertIn("resync", p.stderr)
        _, doc = term_json(d1, "dev-1", "alice", self.server.url, "show")
        self.assertEqual(doc["machine"]["window"]["x"], 600)      # 本地移动保留
        self.assertEqual(doc["prefs"]["theme"], "ocean")          # 错过的变更补齐
        self.assertEqual(doc["prefs"]["background"], "assets/bg-new.png")
        eff = self.server.effective("dev-1", "alice")
        self.assertEqual(json.loads(eff["machine.window.x"]["value"]), 600)

    def test_server_rollback_detected_and_recovers(self):
        """服务器回退：客户端基线比服务器新 → resync；本机坐标与离线变更不丢。"""
        d1 = self.devdir("d1")
        term_json(d1, "dev-1", "alice", self.server.url, "set", "prefs.theme", "red")
        term_json(d1, "dev-1", "alice", self.server.url, "sync")

        # 备份当前库（含 red），再制造新状态后回退
        self.server.stop()
        shutil.copy(self.server.db_path, self.server.db_path + ".bak")

        self.server = Server(self.server.db_path)
        term_json(d1, "dev-1", "alice", self.server.url, "set", "prefs.theme", "blue")
        term_json(d1, "dev-1", "alice", self.server.url, "sync")
        self.assertEqual(json.loads(self.server.effective("dev-1", "alice")
                                    ["prefs.theme"]["value"]), "blue")

        # 服务器回退到 red 时代的库
        self.server.stop()
        for suffix in ("", "-wal", "-shm"):
            try:
                os.remove(self.server.db_path + suffix)
            except FileNotFoundError:
                pass
        shutil.copy(self.server.db_path + ".bak", self.server.db_path)
        self.server = Server(self.server.db_path)

        # dev-1 在服务器回退期间又离线移动了窗口
        term_json(d1, "dev-1", "alice", "http://127.0.0.1:1", "move", "111", "222")
        p, _ = term_json(d1, "dev-1", "alice", self.server.url, "sync")
        self.assertIn("resync", p.stderr)

        _, doc = term_json(d1, "dev-1", "alice", self.server.url, "show")
        self.assertEqual(doc["prefs"]["theme"], "red")   # 回退后的服务器事实
        self.assertEqual(doc["machine"]["window"]["x"], 111)  # 本机坐标不丢
        eff = self.server.effective("dev-1", "alice")
        self.assertEqual(json.loads(eff["machine.window.x"]["value"]), 111)
        self.assertEqual(json.loads(eff["machine.window.y"]["value"]), 222)

    def test_server_epoch_change_forces_resync(self):
        """服务器换库（epoch 变化）→ 快照 resync，本机坐标重新建档。"""
        d1 = self.devdir("d1")
        term_json(d1, "dev-1", "alice", self.server.url, "move", "333", "444")
        term_json(d1, "dev-1", "alice", self.server.url, "sync")
        self.server.stop()
        for suffix in ("", "-wal", "-shm"):
            try:
                os.remove(self.server.db_path + suffix)
            except FileNotFoundError:
                pass
        self.server = Server(self.server.db_path)  # 全新库，新 epoch

        p, _ = term_json(d1, "dev-1", "alice", self.server.url, "sync")
        self.assertIn("resync", p.stderr)
        _, doc = term_json(d1, "dev-1", "alice", self.server.url, "show")
        self.assertEqual(doc["machine"]["window"]["x"], 333)  # 本机坐标保留
        eff = self.server.effective("dev-1", "alice")
        self.assertEqual(json.loads(eff["machine.window.x"]["value"]), 333)


class TestMigrationAndCompat(Base):
    def test_python_migrations_verifiable(self):
        """迁移可验证：逐级校验；未知字段保留；坏文档报错。"""
        v1 = {"schema_version": 1, "theme": "dark", "bg": "a.png",
              "win": {"x": 1, "y": 2, "w": 800, "h": 600},
              "future_feature": {"nested": True}}
        doc, applied = migrate(v1)
        self.assertEqual(applied, ["1->2", "2->3"])
        self.assertTrue(validate(doc, 3))
        self.assertEqual(doc["machine"]["window"]["width"], 800)
        self.assertEqual(doc["prefs"]["background"], "a.png")
        self.assertEqual(doc["prefs"]["opacity"], 1.0)
        self.assertEqual(doc["future_feature"], {"nested": True})  # 未知字段保留
        with self.assertRaises(MigrationError):
            migrate({"schema_version": 1, "theme": "dark"})  # 缺 win，校验失败

    def test_c_client_migrates_v1_state_file(self):
        """C 终端加载 v1 状态文件：可验证迁移 + 未知字段保留。"""
        d1 = self.devdir("d1")
        payload = {
            "device_id": "dev-1", "user_id": "alice", "epoch": "", "server_seq": 0,
            "schema_version": 3,
            "doc": {"schema_version": 1, "theme": "dark", "bg": "old.png",
                    "win": {"x": 10, "y": 20, "w": 800, "h": 600},
                    "future_thing": {"a": 1}},
            "meta": {}, "journal": [], "fallback_fields": [], "monitors": [],
        }
        write_state_file(Path(d1, "state.json"), payload)
        _, doc = term_json(d1, "dev-1", "alice", self.server.url, "show")
        self.assertEqual(doc["schema_version"], 3)
        self.assertEqual(doc["machine"]["window"]["x"], 10)
        self.assertEqual(doc["machine"]["window"]["width"], 800)
        self.assertEqual(doc["prefs"]["theme"], "dark")
        self.assertEqual(doc["prefs"]["background"], "old.png")
        self.assertEqual(doc["prefs"]["opacity"], 1.0)
        self.assertEqual(doc["future_thing"], {"a": 1})  # 未知字段保留

    def test_unknown_new_fields_survive_old_client_save(self):
        """旧客户端（schema 2）保存时不得抹掉新版字段（如 prefs.opacity）。"""
        self.server.post("/api/admin/defaults", {
            "admin": "web-admin",
            "changes": [{"field": "prefs.opacity", "value": 0.8, "base_version": 0}]})
        d1 = self.devdir("d1")
        # 旧客户端同步到含 opacity 的快照，然后改主题再保存
        term_json(d1, "dev-1", "alice", self.server.url, "sync", schema=2)
        term_json(d1, "dev-1", "alice", self.server.url, "set",
                  "prefs.theme", "dark", schema=2)
        term_json(d1, "dev-1", "alice", self.server.url, "sync", schema=2)

        eff = self.server.effective("dev-1", "alice")
        self.assertEqual(json.loads(eff["prefs.opacity"]["value"]), 0.8)  # 未被抹掉
        self.assertEqual(json.loads(eff["prefs.theme"]["value"]), "dark")
        state = read_state_doc(d1)
        self.assertEqual(state["doc"]["prefs"]["opacity"], 0.8)  # 本地也保留

        # 直接注入未知字段，验证保存后仍在
        state["doc"]["future_widget"] = {"enabled": True, "level": 7}
        write_state_file(Path(d1, "state.json"), state)
        term_json(d1, "dev-1", "alice", self.server.url, "set",
                  "prefs.font_size", "15", schema=2)
        _, doc = term_json(d1, "dev-1", "alice", self.server.url, "show", schema=2)
        self.assertEqual(doc["future_widget"], {"enabled": True, "level": 7})
        self.assertEqual(doc["prefs"]["font_size"], 15)


class TestConflictsAndAdmin(Base):
    def test_concurrent_writes_conflict_logged_and_viewable(self):
        """同层并发写：冲突原始双方与胜方落库，页面/API 可查。"""
        d1, d2 = self.devdir("d1"), self.devdir("d2")
        term_json(d1, "dev-1", "alice", self.server.url, "sync")
        term_json(d2, "dev-2", "alice", self.server.url, "sync")
        # 双方基于同一基线离线改主题；red 是更晚的修改（LWW 应胜）
        term_json(d2, "dev-2", "alice", "http://127.0.0.1:1", "set", "prefs.theme", "blue")
        time.sleep(0.05)
        term_json(d1, "dev-1", "alice", "http://127.0.0.1:1", "set", "prefs.theme", "red")
        term_json(d2, "dev-2", "alice", self.server.url, "sync")  # blue 先到服务器
        term_json(d1, "dev-1", "alice", self.server.url, "sync")  # red 并发冲突但更新

        conflicts = self.server.get("/api/admin/conflicts")["conflicts"]
        self.assertEqual(len(conflicts), 1)
        c = conflicts[0]
        self.assertEqual(c["field"], "prefs.theme")
        self.assertEqual(json.loads(c["current_value"]), "blue")   # 原始双方留档
        self.assertEqual(json.loads(c["incoming_value"]), "red")
        self.assertEqual(c["winner"], "incoming")                  # red 时间戳更新
        self.assertEqual(c["reason"], "concurrent_same_layer")
        eff = self.server.effective("dev-1", "alice")
        self.assertEqual(json.loads(eff["prefs.theme"]["value"]), "red")
        self.assertEqual(eff["prefs.theme"]["layer"], "user")      # 最终来源可查

    def test_admin_defaults_layering(self):
        """分层：user > admin > builtin；admin 默认只兜不压。"""
        self.server.post("/api/admin/defaults", {
            "admin": "web-admin",
            "changes": [{"field": "prefs.theme", "value": "sepia", "base_version": 0}]})
        d1, d2 = self.devdir("d1"), self.devdir("d2")
        term_json(d1, "dev-1", "alice", self.server.url, "sync")
        _, doc = term_json(d1, "dev-1", "alice", self.server.url, "show")
        self.assertEqual(doc["prefs"]["theme"], "sepia")  # 无用户偏好时用 admin 默认

        term_json(d1, "dev-1", "alice", self.server.url, "set", "prefs.theme", "dark")
        term_json(d1, "dev-1", "alice", self.server.url, "sync")
        self.server.post("/api/admin/defaults", {
            "admin": "web-admin",
            "changes": [{"field": "prefs.theme", "value": "ocean", "base_version": 1}]})
        term_json(d1, "dev-1", "alice", self.server.url, "sync")
        _, doc = term_json(d1, "dev-1", "alice", self.server.url, "show")
        self.assertEqual(doc["prefs"]["theme"], "dark")  # 用户偏好压过 admin 默认

        term_json(d2, "dev-2", "bob", self.server.url, "sync")
        _, doc2 = term_json(d2, "dev-2", "bob", self.server.url, "show")
        self.assertEqual(doc2["prefs"]["theme"], "ocean")  # 新用户拿到新默认

    def test_admin_page_and_diagnostics(self):
        """管理页面可用；损坏配置诊断副本上传可查。"""
        import urllib.request
        with urllib.request.urlopen(self.server.url + "/", timeout=5) as r:
            html = r.read().decode()
        self.assertIn("冲突记录", html)
        self.assertIn("最终来源", html)
        self.assertIn("诊断副本", html)

        d1 = self.devdir("d1")
        term_json(d1, "dev-1", "alice", self.server.url, "init")
        term_json(d1, "dev-1", "alice", self.server.url, "corrupt")
        term_json(d1, "dev-1", "alice", self.server.url, "show")  # 触发隔离
        _, out = term_json(d1, "dev-1", "alice", self.server.url, "upload-diagnostics")
        self.assertGreaterEqual(out["uploaded"], 1)
        diags = self.server.get("/api/admin/diagnostics")["diagnostics"]
        self.assertEqual(diags[0]["device_id"], "dev-1")
        self.assertEqual(diags[0]["kind"], "corrupt")
        content = self.server.get(f"/api/admin/diagnostics/{diags[0]['id']}")
        self.assertIn("TERMREC1", content["content"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
