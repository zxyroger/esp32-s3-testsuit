/**
 * @file  tf.h
 * @brief 极简板级测试框架（Test Framework）
 *
 * 设计目标：
 *   1. 加一个用例 = 写一个函数 + 一行 TF_REGISTER，不用改任何公共代码；
 *   2. 断言失败立即返回 FAIL，并把文件/行号/原因带进报告，不靠 printf 猜；
 *   3. 输出同时有人读的表格和机读的 JSON，方便主机侧脚本抓取；
 *   4. 不依赖 esp_console / Unity 等外部组件，只靠标准库 + esp_timer。
 *
 * 用例签名统一为 tf_result_t fn(void)，通过 TF_CHECK 系列宏做断言。
 * 断言上下文放在 g_tf 指向的栈对象里，跑完即销毁，用例之间无残留。
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- 结果与用例 ---------------- */

typedef enum {
    TF_PASS = 0,
    TF_FAIL,
    TF_SKIP,
} tf_result_t;

typedef tf_result_t (*tf_fn_t)(void);

typedef struct {
    const char *id;        /* 唯一标识，如 "axp2101.chip_id" */
    const char *category;  /* 分组，如 "pmic" / "bus" / "core" */
    const char *desc;      /* 一句话说明，便于 `list` 阅读 */
    tf_fn_t     fn;
    uint32_t    flags;
} tf_case_t;

#define TF_FLAG_NONE   0u
#define TF_FLAG_HW     (1u << 0)   /* 依赖硬件，缺件时应 SKIP 而不是 FAIL */
#define TF_FLAG_SLOW   (1u << 1)   /* 耗时较长，`run fast` 时跳过 */
/* 会切断网络（断开 STA / 关射频 / 把 STA 挂到自己的 SoftAP）。
 * `run` 默认跳过这一类：跑自检不应该把正在用的网页连接弄断。
 * 要跑必须显式点名（run wifi），跑完由应用层的恢复钩子把网络拉回来。 */
#define TF_FLAG_NONET  (1u << 2)

/* ---------------- 断言上下文 ---------------- */

typedef struct {
    int  checks;          /* 已执行断言数 */
    int  fails;
    char msg[192];        /* 失败原因 / 跳过说明 */
} tf_ctx_t;

extern tf_ctx_t *g_tf;    /* 由 runner 在调用用例前设置，用例内只读 */

/** 记录一次失败（一般不直接调用，用 TF_CHECK 宏） */
void tf_fail(const char *file, int line, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/** 打印一条信息性日志（不计入断言） */
void tf_info(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/** 用例内打印实测数据，统一缩进便于在报告里对齐 */
#define TF_INFO(fmt, ...) tf_info(fmt, ##__VA_ARGS__)

/* 断言：条件不成立 → 记录原因并立即让用例返回 TF_FAIL */
#define TF_CHECK(cond, fmt, ...)                                              \
    do {                                                                      \
        g_tf->checks++;                                                       \
        if (!(cond)) {                                                        \
            tf_fail(__FILE__, __LINE__, fmt, ##__VA_ARGS__);                  \
            return TF_FAIL;                                                   \
        }                                                                     \
    } while (0)

/* 断言：esp_err_t 必须为 ESP_OK，失败时自动带上 esp_err_to_name() */
#define TF_CHECK_ERR(expr, what)                                              \
    do {                                                                      \
        esp_err_t _tf_e = (expr);                                             \
        g_tf->checks++;                                                       \
        if (_tf_e != ESP_OK) {                                                \
            tf_fail(__FILE__, __LINE__, "%s 失败: %s", (what),                \
                    esp_err_to_name(_tf_e));                                  \
            return TF_FAIL;                                                   \
        }                                                                     \
    } while (0)

/* 断言：浮点/整数落在区间内，失败时打印实测值 —— 硬件测试最常用的形态 */
#define TF_CHECK_RANGE(val, lo, hi, what)                                     \
    do {                                                                      \
        double _tf_v = (double)(val);                                         \
        g_tf->checks++;                                                       \
        if (_tf_v < (double)(lo) || _tf_v > (double)(hi)) {                   \
            tf_fail(__FILE__, __LINE__, "%s = %.2f 超出 [%.2f, %.2f]",        \
                    (what), _tf_v, (double)(lo), (double)(hi));               \
            return TF_FAIL;                                                   \
        }                                                                     \
    } while (0)

/* 主动跳过（缺硬件、缺前置条件） */
#define TF_SKIP(fmt, ...)                                                     \
    do {                                                                      \
        snprintf(g_tf->msg, sizeof(g_tf->msg), fmt, ##__VA_ARGS__);            \
        return TF_SKIP;                                                        \
    } while (0)

/* 直接判失败，不占断言计数。
 * 用于"走到这里就说明板子有问题"的分支：这类判断不是断言（没有期望值可比），
 * 但结论是确定的失败。千万别用 TF_SKIP 表达它 —— 跳过会被统计成"没测"，
 * 让真实故障从报告里消失。 */
#define TF_FAIL_MSG(fmt, ...)                                                 \
    do {                                                                      \
        snprintf(g_tf->msg, sizeof(g_tf->msg), fmt, ##__VA_ARGS__);            \
        return TF_FAIL;                                                        \
    } while (0)

/* ---------------- 注册 ---------------- */

#define TF_MAX_CASES 64

/** 由分组寄存器调用；重复 id 会被拒绝并打日志 */
void tf_register(const tf_case_t *c);

/** 批量注册一整个分组 */
void tf_register_group(const tf_case_t *cases, size_t count);

/**
 * 用例分组声明。
 *
 * 为什么不用 __attribute__((constructor)) 自动注册：
 * xtensa-esp-elf 把构造器放进旧式 .ctors 段，而 ESP-IDF 链接脚本只收集
 * .init_array，结果是注册函数被 --gc-sections 当死代码删掉（实测用例表恒为 0 项）；
 * 该工具链也不支持 -fuse-init-array 开关。改用显式分组表，行为完全确定。
 *
 * 用法（放在 .c 文件末尾）：
 *   TF_GROUP_BEGIN(axp2101)
 *       TF_ITEM(axp2101, present, "说明", t_present, TF_FLAG_HW),
 *   TF_GROUP_END(axp2101)
 * 然后在 main.c 的 TF_GROUPS 列表里加一个组名即可。
 */
#define TF_GROUP_BEGIN(group)                                                 \
    static const tf_case_t tf_cases_##group[] = {

/* 注意：宏体不含结尾逗号，由调用处自己加 —— 两边都加会展开成 "},," */
#define TF_ITEM(cat, name, desc, fn, flags)                                   \
        { #cat "." #name, #cat, (desc), (fn), (flags) }

#define TF_GROUP_END(group)                                                   \
    };                                                                        \
    const tf_case_t *tf_group_##group(size_t *count)                          \
    {                                                                         \
        if (count != NULL) {                                                  \
            *count = sizeof(tf_cases_##group) / sizeof(tf_cases_##group[0]);  \
        }                                                                     \
        return tf_cases_##group;                                              \
    }

/* ---------------- 运行与报告 ---------------- */

/** 跑全部用例，打印表格 + 汇总 + JSON */
void tf_run_all(void);

/** 按 id 子串过滤运行；返回实际执行数，0 表示没匹配到 */
int tf_run_filter(const char *substr);

/** 只跑某个分组（category 精确匹配） */
int tf_run_category(const char *category);

/** 列出所有已注册用例 */
void tf_list(void);

/** 打印最近一次运行的 JSON 汇总（供主机脚本解析） */
void tf_print_json(void);

/**
 * 最近一次运行的结果 JSON（静态缓冲区，随时可取，不需要重跑）。
 *
 * 开机自检跑完时 MQTT 还没连上，所以结果先缓存在这里；
 * 联网后由应用层发给 <prefix>/test，网页打开就能看到本次启动的自检结论。
 *
 * @return 以 NUL 结尾的 JSON；从未跑过任何用例时返回 NULL
 */
const char *tf_results_json(void);

/** 结果版本号：每次运行结束 +1，用来判断是否需要重发 */
uint32_t tf_results_gen(void);

/** 启动串口命令行：list / run / run <id> / cat <category> / json / help */
void tf_shell_start(void);

/**
 * 执行一行命令，与串口命令行完全同一套分发。
 *
 * 给远程命令通道（MQTT）用：网页发来的命令在本机执行，效果和本地敲一样。
 * 内部会复制一份到自己的缓冲区，调用方的字符串可以随即释放。
 */
void tf_exec_line(const char *cmd_line);

/**
 * 应用自定义命令钩子。
 *
 * 框架只认测试相关命令；`cam start`、`wifi join` 这类应用命令不该塞进 tf.c。
 * 应用层在启动时用 tf_shell_set_hook() 注册一个回调即可。
 *
 * 为什么不用 __attribute__((weak))：main 组件是一个静态库，弱符号已经有定义
 * （框架里的空实现），链接器就不会再去拉应用层那个 .o —— 结果是钩子静默失效，
 * 编译还完全成功。显式注册能避免这个坑。
 *
 * @return true 表示命令已被处理，框架不再报"未知命令"
 */
typedef bool (*tf_cmd_hook_t)(const char *cmd);

/** 注册应用命令钩子；传 NULL 取消 */
void tf_shell_set_hook(tf_cmd_hook_t hook);

/**
 * 恢复联网回调。
 *
 * tf.c 不该认识 WiFi：跑完 wifi 组（里面有把射频关掉的用例）之后，
 * "把网络重新拉起来"这件事交给应用层做。应用层在启动时注册一次即可。
 */
typedef void (*tf_net_restore_t)(void);

/** 注册恢复联网回调；传 NULL 取消 */
void tf_set_net_restore_hook(tf_net_restore_t fn);

#ifdef __cplusplus
}
#endif
