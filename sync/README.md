# sync/ — 窗口位置/尺寸/背景的状态同步子系统

设计全文见 [`../docs/DESIGN.md`](../docs/DESIGN.md)。

```
synckit/                  终端代理与后台共用的纯逻辑库（无第三方依赖）
  schema.py               字段所有者：device / profile / session + 是否同步
  merge.py                叶子级三向合并、整份覆盖、强制锁、LWW 冲突
  store.py                与 C 终端同构的崩溃安全本地状态（WAL+原子替换）
  geometry.py             本机显示器拓扑指纹与拔屏修复
  migrations.py           可验证迁移链 + 未知新版字段透传
  server/db.py            SQLite：组配置版本/profile/session/设备/审计/诊断
  server/engine.py        权威三向合并编排
  server/app.py           HTTP：/api/sync + 管理端 JSON API + 静态页
  server/static/index.html Web 管理端
agent/agent.py            终端同步代理：断网队列、重连回放、未知字段保留
tests/                    端到端与单元测试（44+ 用例）
../src/                   C 终端：jsonutil / geometry / config / migrate / 主程序
```

## 快速运行

```bash
# 后台 + Web 管理端
python3 synckit/server/app.py --db data/state-sync.db --port 8080
# 打开 http://127.0.0.1:8080

# 终端代理（与 C 客户端共用状态目录）
python3 agent/agent.py --state-dir ~/.local/state/visual-window \
  --server http://127.0.0.1:8080 --device dev-laptop --once

# 端到端走查（离线移动窗口 + 后台换图 + 冲突 + 回退 + 损坏恢复）
python3 demo.py

# 测试
cd .. && make test        # C 逻辑测试 + Python 全部测试
```

## 作用域铁律

| scope | 字段 | 是否出网 |
|---|---|---|
| device | `window.x/y/width/height/maximized/monitor`, `displays.fingerprint` | **永不** |
| profile | `theme.background.uri/mode`, `theme.accent`, `theme.dark` | 是（组内） |
| session | `ui.density`, `ui.language` | 是（跨设备、与组无关） |

双屏机的 `window.x=2100` 永远不会出现在单屏机的状态里；它只在本机
按显示器拓扑做 `geometry_repair`，拔屏后移动到主显示器并保持 ≥320x240
完全可见可操作。

## 合并策略对照（tests/test_strategies_and_agent.py）

- **字段级 merge（同步默认）**：A 改 accent、管理员改 background.uri，
  重连后两个叶子都保留；同一叶子双方都改才产生 LWW 冲突并审计。
- **整份 overwrite（仅 Web 显式发布）**：以管理员整份文档为准，被覆盖的
  用户叶子逐叶子写入审计；只影响 profile，绝不触碰 device。

## 恢复与前向兼容

- 半写：WAL(`journal.jsonl`) 追加 fsync → `state.json.tmp` fsync → rename；
  遗留 `.tmp` 一律不采纳；损坏 `state.json` 移入 `diag/quarantine-*` 留档。
- 自动回退默认值打 `source=fallback`，**不上行同步**，直到用户真实修改。
- 旧客户端读到更高 `schema_version`：整树保留未知节点，保存时原样回写；
  未知叶子在服务端按设备透传，不泄漏给其他设备。
