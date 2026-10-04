# 窗口状态同步设计文档

把窗口位置、尺寸（机器相关）与背景、主题等设置（可跨设备）扩展为状态同步系统。
三方协同：**C 终端**（本地恢复记录）、**Web 管理端**（远程默认配置）、
**后台**（SQLite 维护设备配置版本与用户会话偏好）。

## 1. 配置模型与字段所有者

配置文档（当前 schema v3）：

```json
{
  "schema_version": 3,
  "machine": { "window": {"x":100,"y":100,"width":1024,"height":768,"monitor":"primary"} },
  "prefs":   { "theme":"light", "background":"assets/background.png", "opacity":1.0, "font_size":13 }
}
```

字段注册表（`server/merge.py` 与 `src/config.c` 同源规则）：

| 字段 | 所有者 | 同步范围 | 说明 |
|---|---|---|---|
| `machine.window.x/y/width/height/monitor` | **device** | 仅本设备 | 机器相关的显示器坐标；服务器按设备存档用于本机恢复，**绝不应用到其他设备**——双屏机器的坐标不会强推到单屏机器 |
| `prefs.theme` | user | 用户级跨设备 | 主题 |
| `prefs.background` | user（admin 兜底） | 用户级跨设备 | 背景图 |
| `prefs.opacity` | user（admin 兜底） | 用户级（schema≥3 引入） | 透明度 |
| `prefs.font_size` | user（admin 兜底） | 用户级 | 字号 |

未知字段（未来版本引入）按前缀路由：`machine.*` 归设备，其余归用户，
保证旧组件也不会丢字段。

### 分层与优先级

```
device 层（machine.* 唯一来源，scope=设备 id）
user   层（用户会话偏好，scope=用户 id，跨设备同步）
admin  层（Web 管理端维护的远程默认，scope=组）
builtin 层（内置兜底，source=fallback，永不同步）
```

生效值裁决：设备字段只认 device 层；偏好字段 user > admin > builtin。
**自动回退默认值（fallback）只是本地临时状态：客户端不把它写入 journal，
服务器硬拒收 `source=fallback` 的写入——一台机器的损坏回退不会同步覆盖全组设备。**

## 2. 版本基线

| 概念 | 位置 | 作用 |
|---|---|---|
| 字段 `version` | 每字段单调递增 | 推送时的乐观并发基线（`base_version`） |
| `change_seq` | 服务器全局自增 | 客户端增量拉取的水位线（`since_seq`） |
| `epoch` | 建库时生成的 UUID | 识别"服务器换了另一份历史"（换库/回退） |
| 客户端基线 | `(epoch, server_seq)` + 每字段 `version` | 上次同步点；离线变更记入本地 journal（带 `base_version`） |

## 3. 冲突规则

- **machine.\***：只有属主设备能写，天然无跨设备冲突；多屏坐标只在本机生效。
- **prefs.\***：推送携带 `base_version`：
  - `version == base_version`（或不旧于它）→ 快进接受；
  - `version > base_version` → 同层并发冲突：按 `(updated_at, source)` 确定性裁决
    （时间戳大者胜，并列时 source 字典序大者胜），**原始双方、base、胜方、原因
    全部写入 conflicts 表**，管理端页面可查（原始冲突及最终来源）；
  - 值与时间戳都不更新的重推（备份恢复后的 journal 重放）→ 幂等确认，不产生伪冲突。
- **admin 层**：与设备推送走同一套字段级合并，管理端与管理端/设备的并发同样留痕。
- 时钟假设：客户端时间戳为 Lamport 式参考值，生产环境应换 HLC；裁决的确定性
  由 `(ts, source)` 全序保证。

## 4. 整份覆盖 vs 字段级合并（对比与落实）

场景：断网期间本地把窗口移到新位置，同时后台换了背景图，重连后两边都应保留。

| 策略 | 该场景结果 | 评价 |
|---|---|---|
| 整份覆盖（LWW 文档） | 后写方整篇覆盖：要么丢窗口移动，要么丢背景更换 | 字段所有者不同却被捆绑裁决，不可接受 |
| 整份 + 文档版本检查（乐观锁） | 后写方 409，需客户端重试/人工合并 | 安全但离线场景体验差，且仍需字段级逻辑兜底 |
| **字段级合并（本系统）** | `machine.window.*`（设备所有）与 `prefs.background`（管理端/用户所有）互不影响，**两者都保留** | 按字段所有者独立裁决，离线两端互不阻塞 |

落实：服务器 `merge.apply_change` 逐字段裁决；客户端按字段维护
`meta.version` 与 journal；拉取只下发**层裁决后的生效值**（避免 admin 层
原始变更误盖 user 层生效值）。

## 5. 离线与重连协议

```
push:  {device_id, user, base_seq, epoch, changes:[{field,value,base_version,ts,source}]}
pull:  ?device_id&user&since_seq&epoch  →  增量 changed_fields | 快照 snapshot
```

- 推送前的 `base_seq` 作为随后拉取的水位线，推送窗口期内其他方的变更不会漏。
- 触发 resync（服务器回退 `since_seq > max_seq`、epoch 不匹配、长离线超出
  变更日志保留窗）→ 快照**强制**刷新用户层/默认层（服务器历史可能倒退，
  版本单调性假设失效），设备字段本地为准，并把本机机器字段重新建档推回服务器。
- 本地有待推送 journal 的字段在合并时一律受保护，推送时由服务器裁决。

## 6. 可验证迁移与未知字段保留

- 迁移链 `v1→v2→v3`（`server/migrate.py` 与 `src/config.c` 双端同构）：
  每步 `up()` 之后立即 `validate()`，校验失败即视为损坏（隔离留诊断副本），
  绝不带病继续——**可验证迁移**。
- 迁移只改已知键，从不按白名单重建文档；旧客户端保存时写回整份原始文档，
  只按路径改自己认识的字段——**未知新版字段不会被抹掉**
  （测试：schema 2 的旧客户端保存后，v3 的 `prefs.opacity` 与注入的
  `future_widget` 都原样保留）。

## 7. 故障场景矩阵（全部有自动化测试）

| 场景 | 机制 | 测试 |
|---|---|---|
| 本地文件写一半断电 | tmp+fsync+rename 原子写；magic+长度+CRC32 校验；损坏→隔离 `*.corrupt-<ts>` 诊断副本→回退 `.bak`（checkpoint 语义：最后一次未完成的写入丢失，之前状态完整） | `test_power_loss_mid_write_recovers_from_backup` |
| 主备双损坏 | 内置兜底默认，全字段标 `source=fallback`，客户端不入 journal、服务器拒收 | `test_double_corruption_fallback_defaults_not_synced` |
| 屏幕拔出 | 按当前显示器拓扑钳制：中心/标题栏不在任何屏→搬回主屏；尺寸钳到 `[320×200, 主屏]`；原值写入 `recovery.log` | `test_monitor_unplug_window_clamped_visible` |
| 断电+拔屏叠加 | 恢复出的窗口必须在屏幕内、标题栏可见、尺寸可操作 | `test_recovered_window_visible_and_operable_after_all_failures` |
| 旧设备长时间离线 | `since_seq` 超出保留窗 → 快照 resync；本地离线变更照推 | `test_long_offline_device_snapshot_resync` |
| 服务器回退 | `client_seq > max_seq` → resync；用户层以回退后服务器为准，本机坐标不丢并重新建档 | `test_server_rollback_detected_and_recovers` |
| 服务器换库 | `epoch` 不匹配 → resync + 机器字段重新建档 | `test_server_epoch_change_forces_resync` |
| 本地状态全丢 | 本机坐标从服务器设备层存档恢复 | `test_window_restored_from_server_after_total_local_loss` |
| 损坏配置留档 | 本地隔离副本 + 上传服务器，管理端可查内容 | `test_admin_page_and_diagnostics` |

## 8. 可观测性

- 管理端页面（`GET /`）：远程默认编辑、生效配置/**最终来源**查询、
  **原始冲突**（base/传入/当前/胜方/原因）、设备清单、诊断副本查看。
- 客户端 `recovery.log`：隔离、备份恢复、窗口钳制全部留痕。
