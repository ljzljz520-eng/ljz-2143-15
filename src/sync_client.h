/* 同步客户端：与状态同步后台交换字段级变更。
 *
 * 流程：推送 journal（带版本基线）→ 处理服务器裁决结果 → 拉取增量/快照。
 * 服务器回退 / epoch 变更 / 长离线超出保留窗时，服务器会要求 resync，
 * 客户端用快照刷新用户层与默认层，本机 machine.* 字段始终保留本地值。 */
#ifndef SYNC_CLIENT_H
#define SYNC_CLIENT_H

#include <stddef.h>
#include "state_file.h"

/* 同步结果码 */
#define SYNC_OK 0        /* 成功 */
#define SYNC_OFFLINE 1   /* 服务器不可达（journal 保留，下次重试） */
#define SYNC_PROTOCOL 2  /* 协议错误 */

/* 执行一轮同步；log 填充人类可读的摘要。 */
int sync_now(AppState *st, const char *server_url, char *log, size_t loglen);

/* 上传目录中的 *.corrupt-* 诊断副本到服务器。成功上传的数量，-1 表示不可达。 */
int sync_upload_diagnostics(const AppState *st, const char *dir,
                            const char *server_url, char *log, size_t loglen);

/* 供测试与复用：最小 HTTP 请求（http://host[:port]/path）。
 * 成功返回 0 且 *resp_out 为 malloc 的响应体；失败返回非 0。 */
int http_request(const char *method, const char *url, const char *body,
                 char **resp_out, char *err, size_t errlen);

#endif
