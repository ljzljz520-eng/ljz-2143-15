/* 迷你 JSON 库：仅支持本系统需要的子集（对象/数组/字符串/数字/布尔/null）。
 * 客户端配置文档的读写、未知字段原样保留都依赖它。 */
#ifndef CJSON_H
#define CJSON_H

#include <stddef.h>

typedef enum {
    J_NULL, J_FALSE, J_TRUE, J_NUM, J_STR, J_ARR, J_OBJ
} JType;

typedef struct JVal {
    JType type;
    double num;            /* J_NUM */
    char *str;             /* J_STR（拥有所有权） */
    struct JVal **items;   /* J_ARR */
    int len;               /* J_ARR 长度 */
    int cap;               /* J_ARR 容量 */
    char **keys;           /* J_OBJ 键 */
    struct JVal **vals;    /* J_OBJ 值 */
    int olen;              /* J_OBJ 长度 */
    int ocap;              /* J_OBJ 容量 */
} JVal;

JVal *j_new(JType type);
JVal *j_num(double v);
JVal *j_str(const char *s);
JVal *j_bool(int b);
JVal *j_null(void);
void j_free(JVal *v);
JVal *j_copy(const JVal *v);

/* 解析失败返回 NULL，err_out 指向静态错误描述。 */
JVal *j_parse(const char *text, const char **err_out);
/* 序列化为紧凑 JSON，调用方负责 free。 */
char *j_write(const JVal *v);

JVal *j_obj_get(const JVal *obj, const char *key);
void j_obj_set(JVal *obj, const char *key, JVal *val); /* 接管 val 所有权 */
void j_obj_del(JVal *obj, const char *key);
void j_arr_push(JVal *arr, JVal *val);                 /* 接管 val 所有权 */

/* 点路径访问，如 "machine.window.x"；路径不存在返回 NULL。 */
JVal *j_get_path(JVal *root, const char *path);
/* 点路径写入，自动创建中间对象；接管 val 所有权。成功返回 0。 */
int j_set_path(JVal *root, const char *path, JVal *val);

double j_as_num(const JVal *v, double dflt);
const char *j_as_str(const JVal *v, const char *dflt);

#endif
