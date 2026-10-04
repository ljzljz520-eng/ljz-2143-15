/* 配置文档模型：字段注册表、可验证迁移、窗口可见性钳制。
 * 与 server/merge.py 的字段所有者规则保持同源：
 *   machine.* —— 设备私有（机器相关的显示器坐标，绝不跨设备应用）
 *   prefs.*   —— 用户级（可跨设备同步的主题/背景等会话偏好） */
#ifndef CONFIG_H
#define CONFIG_H

#include <stddef.h>
#include "cjson.h"

#define CFG_SCHEMA_VERSION 3

/* 内置兜底值（source=fallback，永不同步）。since 表示该字段引入的 schema 版本。 */
typedef struct {
    const char *field;
    const char *json_value; /* 以 JSON 文本表示的兜底值 */
    int since;
} CfgDefault;

extern const CfgDefault CFG_DEFAULTS[];
extern const int CFG_DEFAULTS_LEN;

/* machine.* 返回 1（设备私有），prefs.* 及其他返回 0（用户级）。 */
int cfg_owner_is_device(const char *field);

/* 生成指定 schema 版本的内置兜底文档（旧客户端不含新版字段）。 */
JVal *cfg_default_doc_for(int schema);

/* 生成当前最新版本的内置兜底文档。 */
JVal *cfg_default_doc(void);

/* 校验指定 schema 版本的文档结构。合法返回 1。 */
int cfg_validate_doc(const JVal *doc, int version);

/* 可验证迁移：把 doc 就地迁移到 target 版本，每步迁移后校验。
 * 未知字段原样保留；doc 比 target 新时不做任何改动（旧客户端不抹新字段）。
 * 成功返回 0；校验失败返回 -1 并填充 err。 */
int cfg_migrate_doc(JVal *doc, int target, char *err, size_t errlen);

/* 窗口可见性钳制：monitors 为 [{x,y,w,h},...]。
 * 窗口中心不在任何屏幕、或标题栏不可见时，搬回主屏并钳制尺寸，
 * 保证恢复后的窗口可见且可操作。有修改返回 1，note 记录诊断信息。 */
int cfg_clamp_window(JVal *doc, const JVal *monitors, char *note, size_t notelen);

#endif
