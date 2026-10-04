# Visual Window App (C + SDL2 + noVNC) + 状态同步系统

这是一个真实的 **C 语言桌面 GUI 程序**（基于 SDL2），程序在 Linux 容器中启动窗口并将图片渲染为背景图。
通过 **Xvfb + x11vnc + noVNC**，你可以直接在浏览器看到并操作这个窗口。

本仓库在 GUI 程序之上实现了**窗口位置、尺寸与背景设置的状态同步**：

- **C 终端**保存本地恢复记录（原子写 + CRC 校验 + 备份 + 损坏隔离）
- **Web 管理端**编辑远程默认配置，可查看原始冲突与最终来源
- **后台**用 SQLite 维护设备配置版本与用户会话偏好
- 机器相关的显示器坐标（设备私有）与可跨设备同步的主题/背景（用户级）严格区分
- 字段级合并 + 版本基线 + 确定性冲突裁决（详见 `docs/design.md`）

## 特性

- C11 + SDL2 图形窗口（非 Web 页面）
- SDL2_image 加载背景图（`assets/background.png`）
- 窗口几何/背景实时持久化到本地恢复记录，断网期间变更入 journal，重连后字段级合并
- 屏幕拔出/分辨率变化时窗口自动钳制回可见区域
- 严格编译参数：`-Wall -Wextra -Werror`

## 技术架构

```text
Browser (http://localhost:6080)                Browser (http://localhost:8000)
        |                                              |
      noVNC (WebSocket)                          Web 管理端 (admin.html)
        |                                              |
      x11vnc (VNC)                             同步后台 server/ (Python + SQLite)
        |                                          ↑ 字段级合并 / 版本基线 / 冲突留档
      Xvfb (:99) + openbox                           | HTTP JSON
        |                                              |
   C SDL2 GUI App ── 本地恢复记录 state.json ──────────┘
   (窗口位置/尺寸=设备私有, 背景/主题=用户级同步)
```

## 目录结构

```text
├── docker-compose.yml          # app + sync-server 两个服务
├── assets/background.png
├── docker/                     # Dockerfile / entrypoint.sh
├── src/
│   ├── main.c window.c renderer.c      # SDL GUI（接入状态同步）
│   ├── termctl.c                       # 无头 C 终端 CLI（本机可构建/测试）
│   ├── cjson.c config.c state_file.c sync_client.c   # SDL 无关的同步核心
├── server/                     # 同步后台：server.py / store.py / merge.py / migrate.py
├── web/admin.html              # Web 管理端
├── tests/test_sync.py          # 端到端测试（断电/拔屏/长离线/服务器回退…）
└── docs/design.md              # 字段所有者、版本基线、冲突规则、合并策略对比
```

## 一键启动（完整系统）

```bash
docker compose up --build
```

- 窗口画面：`http://localhost:6080`
- 管理端：`http://localhost:8000`（远程默认配置 / 冲突记录 / 生效来源 / 诊断副本）

## 本机开发（无 SDL 环境）

```bash
make            # 构建 termctl（无 SDL 时自动跳过 GUI 应用）
make test       # 编译检查 + SDL 桩编译检查 + 运行全部端到端测试
python3 -m server.server --db /tmp/state.db --port 8000   # 单独起同步后台
./termctl --dir /tmp/dev1 --device dev-1 --user alice --server http://127.0.0.1:8000 sync
```

## termctl 命令

```text
init | show | get FIELD | set FIELD VALUE | move X Y | resize W H
monitors JSON | validate | sync | corrupt [--bak] | upload-diagnostics
```

## noVNC 说明

noVNC 仅用于远程显示容器中的真实图形窗口。
本项目本质仍是标准 C GUI 程序，不是 Web 应用，也不是 Canvas 模拟。

## 背景图

当前仓库默认提供了 `assets/background.png` 作为占位背景图。
如果你要替换成指定图片，直接覆盖同路径文件即可，无需改代码；
也可通过管理端把 `prefs.background` 远程默认指到新路径，全组设备同步生效。
