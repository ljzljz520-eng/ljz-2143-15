# 窗口/背景状态同步设计（状态同步扩展）

把原来写死在 `main.c` 里的 `WINDOW_WIDTH/WINDOW_HEIGHT/居中位置/背景路径`
扩展成可同步状态：

- **C 终端**：保存本地恢复记录（崩溃、断电、屏幕拔出后恢复）。
- **同步代理（agent）**：终端侧守护进程，负责断网队列、三向合并与重连回放。
- **后台服务**：SQLite 维护设备配置版本、用户会话偏好、冲突审计与诊断副本。
- **Web 管理端**：编辑远程默认配置（组默认），查看历史版本、原始冲突与最终来源。

核心原则：**机器相关的显示器坐标永不同步；可跨设备的主题/背景才同步。**
一台双屏机的窗口坐标绝不能强推到另一台单屏机。

---

## 1. 字段所有者（Field ownership）

每个叶子字段有三个属性：

| 属性 | 取值 | 含义 |
|---|---|---|
| `scope` | `device` / `profile` / `session` | 作用域 |
| `owner` | `client` / `user` / `admin` | 唯一写入者（强制时） |
| `sync` | true/false | 是否进入同步线 |

| 字段 | scope | 默认 owner | 同步 | 说明 |
|---|---|---|---|---|
| `window.x`,`window.y` | device | client | **否** | 显示器坐标，机器相关 |
| `window.width`,`window.height` | device | client | **否** | 尺寸跟随本机显示器 |
| `window.maximized` | device | client | 否 | |
| `window.monitor` | device | client | 否 | 目标显示器标识（本机指纹内） |
| `displays.fingerprint` | device | client | 否 | 屏幕拓扑哈希，拔屏检测依据 |
| `displays.monitors[]` | device | client | 否 | 每台显示器边界（仅诊断上报） |
| `theme.background.uri` | profile | user（可被 admin `enforced`） | 是 | 背景图（用户切换的"换图"） |
| `theme.background.mode` | profile | user/admin | 是 | fill/fit/stretch/center |
| `theme.accent` | profile | user/admin | 是 | 主题强调色 |
| `theme.dark` | profile | user/admin | 是 | 深色模式 |
| `ui.density`,`ui.language` | session | user | 是 | 用户级会话偏好（跨设备、不绑组） |
| `schema_version` | 元数据 | 迁移器 | 特殊 | 单调递增，见 §7 |

所有者规则：

1. `device.*` 只存在于本机状态文件，永不出网；代理拒绝打包、服务器拒绝接收。
2. `profile.*` 属于"用户在本组内的文档"。admin 可把默认配置中的叶子标记
   `enforced=true`，被强制的叶子由 admin 独占写入，用户改动会被拒绝并回报冲突。
3. `session.*` 属于用户账号，跨设备生效，与设备组无关。
4. 自动回退产生的默认值（损坏恢复、缺文件）打 `source=fallback` 标记，
   **不参与同步**，直到用户真实修改该叶子（source 变为 `user`）。

## 2. 版本基线（Version baseline）

- 服务器组配置、用户 profile、session 各有一个**单调递增整数版本**，永不回退
  （"服务器回退"是用旧内容创建一个新版本，见 §8）。
- 设备状态保存三段基线：
  `profile.base_version`、`session.base_version`、`server_cursor`。
- 同步请求携带 `(base_version, 本地改动)`，服务器据此做
  **base → server-head → client-edits 的三向合并（3-way merge）**。
- 服务器保留全部历史版本，支持"设备离线三个月后回归"时仍能找到其基线。
- 每个叶子带 `stamp`：`(修订时服务器版本或本地毫秒时钟, 修订者设备/用户)`，
  作为同叶子"最后写入获胜（LWW）"的决胜依据。

## 3. 冲突规则（Conflict rules）

合并单位是**叶子路径**（点分路径，如 `theme.background.uri`）。

对同一条叶子路径，记 B=基线、S=服务器当前、C=客户端：

1. 三方相等 → 不变。
2. 仅一方改动（B→S 或 B→C）→ 采用改动方。
3. 双方改动但结果相同 → 无冲突。
4. 双方改动且不同 → 冲突：
   - 任一侧对该叶子是 `enforced`（admin 强制）→ **强制值胜**，另一方记录为冲突。
   - 否则比较 `stamp`（先版本/时钟，修订者 ID 字典序兜底）→ LWW；
     离线本地时钟回拨时仍确定性收敛（设备 ID 兜底，不靠运气）。
5. 一侧删除/一侧修改：删除不是本产品支持的操作（叶子只改不删），天然规避。
6. 未知字段（见 §7）：对合并双方任一是未知 schema 的叶子，
   **原样透传保留，绝不判冲突、绝不丢弃**。

每次冲突产生一条审计：
`{path, base, server, client, resolution, resolved_to, winner, stamp}`，
原始三方值与最终来源都入库，Web 端可查。

## 4. 整份覆盖 vs 字段级合并

- **字段级合并（merge，默认同步路径）**：按 §3 逐叶子处理。
  断网期间 A 机移动窗口（device，不出网）、用户在主题里换强调色（profile 叶子），
  后台管理员换了背景图（另一个 profile 叶子）；重连后两个叶子都保留——
  背景用后台新图、强调色保留本地修改。
- **整份覆盖（overwrite，仅 Web 管理端显式发布时可用）**：以管理员整份文档为准，
  但仍受三条护栏约束：
  1. 只影响 `profile` 作用域，绝不触碰任何 `device` 叶子；
  2. `enforced` 之外的叶子，若用户在基线之后修改过，逐叶子列入冲突预览，
     管理员确认后才覆盖（审计可查）；
  3. 覆盖发布生成新版本，旧版本保留，可"回退"（实为发新版本）。
- 两种策略在 `tests/` 中用同一组离线场景对照实现与断言（见
  `test_merge_strategies`：merge 保留两侧、overwrite 仅在显式确认后覆盖）。

## 5. C 终端：本地状态文件与恢复记录

状态目录默认 `~/.local/state/visual-window/`（可 `VW_STATE_DIR` 覆盖）：

```
state.json                 # 完整状态（device + profile + session + 基线 + stamp）
state.json.tmp             # 原子替换临时文件
journal.jsonl              # 预写日志：改前先追加，落盘后再重写 state.json
restore.jsonl              # 本地恢复记录（每次启动一条，追加，只增不改）
diag/quarantine-*.json     # 损坏配置诊断副本（永不自动删除）
```

写入协议（防"写到一半断电"）：

1. `journal.jsonl` 追加一条变更事件并 flush/fsync；
2. 写 `state.json.tmp`（完整 JSON）→ fsync → `rename(state.json.tmp, state.json.tmp)`；
3. 成功后写入 checkpoint（journal 截断点）。

启动恢复（`config_recover`）：

- `state.json` 缺失但 `.tmp` 存在 → tmp 是被丢弃的半成品，不采用；重放 journal。
- `state.json` 解析失败 → **原文件移动到 `diag/quarantine-<时间>.json`**，
  不覆盖、不删除；写 `restore.jsonl`；用安全默认值启动（叶子标 fallback）。
- journal 中 checkpoint 之后的事件按序重放，任一条损坏即停止重放并诊断留档，
  已提交的部分生效。
- 任何路径下窗口最终都必须落在可见区域（§6）。

## 6. 显示器拓扑与拔屏修复（仅 device 作用域）

- 终端枚举显示器边界，生成指纹
  `fingerprint = join(sort("w,h@x,y"), "|")`，写入 `displays.fingerprint`。
- 启动或收到 SDL 显示变化事件时 `geometry_repair(window, monitors)`：
  - 与目标窗口有交集的显示器中选交集最大者；
  - 窗口裁剪进该显示器工作区（保留标题栏可拖、边宽可拉，至少 320x240 可见）；
  - 无交集（显示器被拔出）→ 移动到主显示器并整体可见，尺寸按工作区收缩；
  - 坐标为负（`SDL_WINDOWPOS_CENTERED` 等）→ 按当前主显示器居中。
- 修复只改 device 叶子；profile/session 不受拔屏影响。
- 断言（测试强制）：修复后窗口矩形与某台显示器相交、完全在并集内、可交互。

## 7. 配置升级：可验证迁移 + 前向兼容

- `schema_version` 为整数。升级由**有序迁移链**完成：
  v1→v2、v2→v3…… 每步是纯函数，`migrate(doc)` 必须满足
  - 幂等：`migrate(migrate(x)) == migrate(x)`；
  - 可逆验证：测试提供每步的 down/样例校验（hashes/断言）；
  - 产出版本号严格 +1。
- 状态文件版本 **高于** 客户端已知版本时：
  - 旧客户端**整树保留未知节点**（解析器不按 schema 裁剪），照常启动；
  - 保存时未知节点原样回写——旧客户端编辑已知字段不会抹掉新版字段；
  - 同步时代理只上报已知叶子，未知叶子在服务端按设备原样透传（per-device
    passthrough），新版本客户端回来后重新可见。
- 绝不把 schema_version 写低；不认识的版本不就地迁移、不重置。

## 8. 后台版本维护、服务器回退

- 表：`groups_versions`（组默认全量版本，含 enforced 标记）、
  `user_profiles`（每用户/组文档+版本+基线）、`user_sessions`、
  `devices`（基线版本、最后指纹、诊断计数）、
  `sync_audit`（原始冲突 + 最终来源）、`corrupt_reports`（诊断副本元数据）。
- "回退"= 取 vN 内容创建 **v(N+1)**，版本号继续单调；
  设备若基于 v(N+2) 离线、随后收到这个"回退新版本"，三向合并仍成立，
  用户在 v(N+2) 之后的本地修改不会被静默抹掉（走 §3 冲突与审计）。
- 自动回退默认值不带入版本线，不广播、不会同步覆盖全组设备。

## 9. 同步协议（HTTP/JSON，agent ↔ server）

`POST /api/sync`
```json
{
  "device_id": "...", "user_id": "...", "group_id": "...",
  "profile": {"base_version": 7, "changes": {"path": {"value":..., "stamp":...}}},
  "session": {"base_version": 3, "changes": {...}},
  "known_schema": 3,
  "displays": {"fingerprint": "...", "monitors": [...], "diagnostic_only": true}
}
```
响应：合并后的完整 profile/session 文档（含未知字段 passthrough 包）、
新版本号、被强制拒绝的叶子、冲突审计列表。`device` 字段从不出现在请求/响应正文。

## 10. 端到端场景索引（对应 tests/）

断电半写恢复、拔屏修复、双屏→单屏隔离、长期离线三向合并、
断网移动窗口+后台换图、整份覆盖 vs 字段合并、服务器回退、
可验证迁移、旧客户端不抹新字段、损坏诊断副本、fallback 不全组广播、
Web 冲突来源可查——见 `tests/README.md`。
