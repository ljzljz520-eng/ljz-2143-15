/* 本地恢复记录：C 终端的窗口/偏好状态持久化。
 *
 * 文件格式：第一行 "TERMREC1 <payload_len> <crc32_hex>"，随后是 JSON 负载。
 * 写入：tmp 文件 + fsync + 旧文件转 .bak + rename + fsync 目录（防写一半断电）。
 * 读取：校验 magic/长度/CRC；损坏 → 隔离为 *.corrupt-<ts> 诊断副本 → 尝试 .bak
 *       → 再失败 → 内置兜底默认（全部字段标记 source=fallback，绝不同步）。 */
#ifndef STATE_FILE_H
#define STATE_FILE_H

#include <stddef.h>
#include "cjson.h"

#define STATE_FILE_NAME "state.json"
#define STATE_BACKUP_NAME "state.json.bak"
#define STATE_RECOVERY_LOG "recovery.log"

typedef struct {
    char device_id[128];
    char user_id[128];
    char epoch[64];        /* 上次同步的服务器 epoch（回退检测） */
    long server_seq;       /* 上次同步的全局变更序号（版本基线） */
    int schema_version;    /* 本客户端支持的文档 schema */
    JVal *doc;             /* 配置文档（未知字段原样保留） */
    JVal *meta;            /* obj: field -> {version, source, ts} */
    JVal *journal;         /* array: 离线期间待推送的本地变更 */
    JVal *fallback_fields; /* array of string: 处于自动回退的字段 */
    JVal *monitors;        /* array of {x,y,w,h}: 本机显示器拓扑 */
    int recovered;         /* 0=正常 1=从备份恢复 2=兜底默认 */
    char note[256];        /* 最近一次恢复/钳制的诊断信息 */
} AppState;

void state_init(AppState *st, const char *device_id, const char *user_id, int schema);
void state_free(AppState *st);

/* 加载：0=正常 1=备份恢复 2=兜底默认（err 填充诊断信息）。 */
int state_load(AppState *st, const char *dir, char *err, size_t errlen);

/* 原子保存。成功返回 0。 */
int state_save(AppState *st, const char *dir, char *err, size_t errlen);

/* 本地修改字段：更新 doc、追加 journal（含 base_version）、清除 fallback 标记。 */
void state_local_set(AppState *st, const char *field, JVal *value);

int state_journal_has(const AppState *st, const char *field);
int state_is_fallback(const AppState *st, const char *field);
void state_clear_fallback(AppState *st, const char *field);

/* 追加恢复/钳制诊断日志（本地留痕）。 */
void state_log_recovery(const char *dir, const char *fmt, ...);

/* 当前时间的 ISO8601 UTC 字符串（毫秒精度，与服务器格式一致可字典序比较）。 */
void state_now_iso(char *buf, size_t len);

#endif
