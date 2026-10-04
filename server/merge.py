"""字段注册表与字段级合并引擎。

设计要点（详见 docs/design.md）：
- 每个字段有明确所有者：machine.* 归设备，prefs.* 归用户（admin 层提供远程默认）。
- 版本基线：每字段 version + 全局 change_seq + 服务器 epoch。
- 冲突规则：字段级合并（而非整份覆盖）。同层并发写按 (updated_at, source)
  确定性裁决，原始冲突双方与胜方全部落库，页面可查。
- source='fallback' 的自动回退默认值一律拒收，绝不污染全组设备。
"""

from .store import dumps_value, utcnow  # noqa: F401  (re-export for server)

CONFIG_SCHEMA = 3

# 字段注册表：field -> (owner, 内置兜底值)
# owner='device' 的机器相关显示器坐标只按设备存档，绝不应用到其他设备。
FIELD_REGISTRY = {
    "machine.window.x": ("device", 100),
    "machine.window.y": ("device", 100),
    "machine.window.width": ("device", 1024),
    "machine.window.height": ("device", 768),
    "machine.window.monitor": ("device", "primary"),
    "prefs.theme": ("user", "light"),
    "prefs.background": ("user", "assets/background.png"),
    "prefs.opacity": ("user", 1.0),
    "prefs.font_size": ("user", 13),
}

ADMIN_SCOPE = "default"


def field_owner(field):
    entry = FIELD_REGISTRY.get(field)
    if entry:
        return entry[0]
    # 未知字段（新版客户端引入）：按前缀路由，保证旧服务器也不会丢字段。
    return "device" if field.startswith("machine.") else "user"


def builtin_default(field):
    entry = FIELD_REGISTRY.get(field)
    return entry[1] if entry else None


def route(field, device_id, user):
    owner = field_owner(field)
    if owner == "device":
        return ("device", device_id)
    return ("user", user)


def _wins(incoming_ts, incoming_source, current_ts, current_source):
    """确定性裁决：时间戳大者胜；并列时 source 字典序大者胜。"""
    if incoming_ts != current_ts:
        return incoming_ts > current_ts
    return incoming_source > current_source


def apply_change(store, layer, scope, field, value, base_version, source, ts):
    """对单个字段应用一次写入，返回 (status, version)。

    status: accepted | conflict_won | conflict_lost | rejected_fallback
    """
    if source == "fallback":
        # 自动回退默认值绝不同步覆盖全组设备。
        return ("rejected_fallback", None)

    value_json = dumps_value(value)
    cur = store.get_field(layer, scope, field)

    if cur is not None and value_json == cur["value"] and ts <= cur["updated_at"]:
        # 幂等重放：备份恢复后 journal 中已确认的条目会被重推，
        # 值与时间戳都不更新，直接确认，不产生伪冲突。
        return ("accepted", cur["version"])

    if cur is None or base_version >= cur["version"]:
        # 首写或快进（base 与当前一致 / 客户端基线更新）：直接接受。
        version = store.put_field(layer, scope, field, value_json, source, ts)
        return ("accepted", version)

    # cur.version > base_version：同层并发写冲突。
    incoming_wins = _wins(ts, source, cur["updated_at"], cur["source"])
    winner = "incoming" if incoming_wins else "current"
    store.log_conflict(
        layer, scope, field,
        base_value=None if base_version == 0 else dumps_value(base_version),
        incoming_value=value_json,
        incoming_source=source,
        current_value=cur["value"],
        current_source=cur["source"],
        winner=winner,
        reason="concurrent_same_layer",
    )
    if incoming_wins:
        version = store.put_field(layer, scope, field, value_json, source, ts)
        return ("conflict_won", version)
    return ("conflict_lost", cur["version"])


def apply_push(store, device_id, user, changes):
    """应用客户端 journal 推送，逐字段合并，返回每字段结果列表。"""
    results = []
    for ch in changes:
        field = ch.get("field", "")
        value = ch.get("value")
        base_version = int(ch.get("base_version") or 0)
        source = ch.get("source") or f"device:{device_id}"
        ts = ch.get("ts") or utcnow()
        layer, scope = route(field, device_id, user)
        status, version = apply_change(
            store, layer, scope, field, value, base_version, source, ts
        )
        results.append({"field": field, "status": status, "version": version})
    return results


def apply_admin(store, changes, admin_id):
    """Web 管理端编辑远程默认配置（admin 层），同样走字段级合并。"""
    results = []
    for ch in changes:
        field = ch.get("field", "")
        if field_owner(field) != "user":
            results.append({"field": field, "status": "rejected_scope", "version": None})
            continue
        value = ch.get("value")
        base_version = int(ch.get("base_version") or 0)
        status, version = apply_change(
            store, "admin", ADMIN_SCOPE, field, value, base_version,
            f"admin:{admin_id}", utcnow(),
        )
        results.append({"field": field, "status": status, "version": version})
    return results


def effective_fields(store, device_id, user):
    """计算设备生效配置：device 层 > user 层 > admin 层 > 内置兜底。

    返回 {field: {value, version, source, layer}}；layer 即“最终来源”。
    """
    scopes = {
        "device": {r["field"]: r for r in store.list_fields("device", device_id)},
        "user": {r["field"]: r for r in store.list_fields("user", user)},
        "admin": {r["field"]: r for r in store.list_fields("admin", ADMIN_SCOPE)},
    }
    all_fields = set(FIELD_REGISTRY)
    for layer_fields in scopes.values():
        all_fields.update(layer_fields)

    out = {}
    for field in sorted(all_fields):
        owner = field_owner(field)
        picked = None
        layer = None
        if owner == "device":
            order = ("device", "admin", "user")
        else:
            order = ("user", "admin")
        for candidate in order:
            row = scopes[candidate].get(field)
            if row is not None:
                picked = row
                layer = candidate
                break
        if picked is not None:
            out[field] = {
                "value": picked["value"],
                "version": picked["version"],
                "source": picked["source"],
                "layer": layer,
                "updated_at": picked["updated_at"],
            }
        else:
            out[field] = {
                "value": dumps_value(builtin_default(field)),
                "version": 0,
                "source": "builtin",
                "layer": "builtin",
                "updated_at": None,
            }
    return out


def snapshot(store, device_id, user):
    """完整快照（长离线回归 / 服务器回退后的 resync 用）。"""
    return effective_fields(store, device_id, user)


def delta_since(store, device_id, user, since_seq):
    """since_seq 之后的增量变更。

    关键：必须下发"层裁决后的生效值"，而不是原始变更流水。
    例如 admin 层改了主题，但该用户的 user 层已有自己的偏好，
    对此设备而言生效值并未变化，不应把 admin 的原始值推给它。
    """
    scopes = [("device", device_id), ("user", user), ("admin", ADMIN_SCOPE)]
    rows = store.changes_since(since_seq, scopes)
    changed_fields = {row["field"] for row in rows}
    eff = effective_fields(store, device_id, user)
    return {f: eff[f] for f in sorted(changed_fields) if f in eff}
