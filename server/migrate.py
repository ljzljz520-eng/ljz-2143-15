"""可验证的配置文档迁移。

每条迁移由 up() 与 validate() 组成；migrate() 逐级应用并在每一级之后
立即校验（可验证迁移）。所有迁移只修改已知键，绝不按白名单重建文档，
因此未知的新版字段在迁移与旧客户端保存时都不会被抹掉。
"""

import copy
import json

CONFIG_SCHEMA = 3


class MigrationError(Exception):
    pass


def _up_1_2(doc):
    """v1 扁平结构 -> v2 分层结构。

    v1: {"theme": ..., "bg": ..., "win": {"x","y","w","h"}}
    v2: {"machine": {"window": {...}}, "prefs": {"theme","background"}}
    """
    doc = copy.deepcopy(doc)
    win = doc.pop("win", None)
    if isinstance(win, dict):
        machine = doc.setdefault("machine", {})
        window = machine.setdefault("window", {})
        if "x" in win:
            window["x"] = win["x"]
        if "y" in win:
            window["y"] = win["y"]
        if "w" in win:
            window["width"] = win["w"]
        if "h" in win:
            window["height"] = win["h"]
    if "bg" in doc:
        doc.setdefault("prefs", {})["background"] = doc.pop("bg")
    if "theme" in doc:
        doc.setdefault("prefs", {})["theme"] = doc.pop("theme")
    doc["schema_version"] = 2
    return doc


def _up_2_3(doc):
    """v2 -> v3：引入 prefs.opacity（默认 1.0）。未知键原样保留。"""
    doc = copy.deepcopy(doc)
    doc.setdefault("prefs", {}).setdefault("opacity", 1.0)
    doc["schema_version"] = 3
    return doc


def _is_num(v):
    return isinstance(v, (int, float)) and not isinstance(v, bool)


def _validate_1(doc):
    return (
        isinstance(doc, dict)
        and isinstance(doc.get("win"), dict)
        and all(_is_num(doc["win"].get(k)) for k in ("x", "y", "w", "h"))
        and isinstance(doc.get("theme"), str)
    )


def _validate_2(doc):
    try:
        w = doc["machine"]["window"]
        return (
            all(_is_num(w[k]) for k in ("x", "y", "width", "height"))
            and isinstance(doc["prefs"]["theme"], str)
            and isinstance(doc["prefs"]["background"], str)
        )
    except (KeyError, TypeError):
        return False


def _validate_3(doc):
    return _validate_2(doc) and _is_num(doc["prefs"].get("opacity"))


MIGRATIONS = {
    1: (_up_1_2, _validate_2),
    2: (_up_2_3, _validate_3),
}

VALIDATORS = {1: _validate_1, 2: _validate_2, 3: _validate_3}


def validate(doc, version):
    validator = VALIDATORS.get(version)
    if validator is None:
        raise MigrationError(f"unknown schema version: {version}")
    return validator(doc)


def migrate(doc, target=CONFIG_SCHEMA):
    """把 doc 迁移到 target 版本，返回 (新文档, 已应用的迁移列表)。

    每一步迁移后都会校验；校验失败抛 MigrationError，调用方必须能把
    原始文档留作诊断副本而不是静默继续使用。
    """
    version = doc.get("schema_version", 1) if isinstance(doc, dict) else 0
    if not isinstance(version, int) or version < 1:
        raise MigrationError(f"bad schema_version: {version!r}")
    if version > target:
        # 新版文档、旧客户端：原样保留（含未知字段），不做降级。
        return doc, []
    applied = []
    while version < target:
        step = MIGRATIONS.get(version)
        if step is None:
            raise MigrationError(f"no migration from schema {version}")
        up, validator = step
        doc = up(doc)
        version += 1
        if not validator(doc):
            raise MigrationError(f"validation failed after migrating to schema {version}")
        applied.append(f"{version - 1}->{version}")
    if not validate(doc, version):
        raise MigrationError(f"validation failed at schema {version}")
    return doc, applied


def dumps_stable(doc):
    return json.dumps(doc, sort_keys=True, ensure_ascii=False)
