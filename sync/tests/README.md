# 测试与需求场景索引

运行：`make test`（仓库根目录）= C 逻辑测试 + 44 个 Python 测试。

## C 终端（tests/../tests/test_sync_logic.c，ASan/UBSan 干净）

| 场景 | 用例 |
|---|---|
| 配置写到一半断电（journal/tmp/rename 三阶段注入） | `test_power_loss_stages` |
| 损坏 state 隔离诊断副本 + fallback 不广播 | `test_corrupt_state_quarantine` |
| state 缺失后 WAL/journal 重放恢复 | `test_journal_replay` |
| 屏幕拔出（双屏→单屏）后窗口移动到主屏、≥320x240 可见 | `test_geometry` |
| 双屏坐标绝不进同步载荷 | `test_dirty_and_device_isolation` |
| 旧客户端保存时保留未知新版字段 | `test_json_unknown_preserved` |
| 可验证迁移（up/down 往返、幂等、前向兼容） | `test_migrations` |

## Python 同步核心（sync/tests/）

| 需求 | 用例 |
|---|---|
| 断网移动窗口（device）+ 后台换图（profile）重连后各保留 | `test_offline_window_move_and_admin_background_both_survive` |
| 主题跨设备、显示器坐标不跨设备 | `test_theme_crosses_devices_but_coords_do_not` |
| 整份覆盖 vs 字段级合并对照 | `test_strategies_and_agent.py` 全部 |
| 旧设备长期离线（5 个版本）后回归收敛 | `test_long_offline_device_returns_and_converges` |
| 服务器回退=新版本，离线本地编辑不被静默抹掉 | `test_server_rollback_*` |
| enforced 管理员强制叶子拒绝客户端覆盖 | `test_enforced_background_rejects_client_override` |
| 未知新版字段按设备透传，不泄漏给其他设备 | `test_unknown_new_schema_round_trips_per_device` |
| 损坏恢复 fallback 默认值不同步覆盖全组 | `test_fallback_defaults_are_not_broadcast_to_group` |
| 页面/API 可查看原始 base/client/server 与最终来源 | `test_audit_page_shows_raw_conflict_and_final_source` |
| 断网队列重连回放 | `test_offline_changes_are_queued_then_flushed` |
| 诊断副本上报 | `test_corrupt_telemetry_is_reported_but_fallback_not_pushed` |
| 半写三阶段（Python 镜像存储） | `test_store_recovery.py` 全部 10 例 |
| 后台跨重启持久化 | `demo.py` + CLI（`agent.py --once`） |

所有冲突决议都在数据库 `profile_audit` 保留
`base_json/client_json/server_json/resolved_json/winner/resolution`，
Web「冲突与最终来源」页直接展示。
