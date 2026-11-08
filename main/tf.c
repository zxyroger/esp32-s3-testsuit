/**
 * @file  tf.c
 * @brief 测试框架实现：注册表、运行器、报告、命令行
 */
#include "tf.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "tf";

/* ---------------- 注册表 ---------------- */

static tf_case_t s_cases[TF_MAX_CASES];
static int        s_case_count = 0;

/* 最近一次运行的结果，供报告与 JSON 复用 */
typedef struct {
    const tf_case_t *c;
    tf_result_t      result;
    int              checks;
    int              fails;
    uint64_t         us;
    char             msg[192];
} tf_record_t;

static tf_record_t s_records[TF_MAX_CASES];
static int         s_record_count = 0;

/* 恢复联网：本层不认识 WiFi，跑完会断网的用例后交给应用层收拾 */
static tf_net_restore_t s_net_restore = NULL;
static bool             s_net_broken  = false;  /* 本次运行执行过 NONET 用例 */
static int              s_net_skipped = 0;      /* 本次运行因 NONET 被跳过的项数 */

/* 本次运行的范围：全量还是只跑了一部分。
 * 网页要靠这个区分"这是一份完整自检"还是"只跑了 axp2101"，
 * 否则部分结果会把没跑的用例显示成跳过，看起来像全量。 */
static bool             s_last_full = false;
static char             s_last_scope[40] = "";

void tf_set_net_restore_hook(tf_net_restore_t fn)
{
    s_net_restore = fn;
}

tf_ctx_t *g_tf = NULL;

void tf_register(const tf_case_t *c)
{
    if (c == NULL || c->id == NULL || c->fn == NULL) {
        return;
    }
    if (s_case_count >= TF_MAX_CASES) {
        ESP_LOGE(TAG, "用例数超过上限 %d，%s 未注册", TF_MAX_CASES, c->id);
        return;
    }
    for (int i = 0; i < s_case_count; i++) {
        if (strcmp(s_cases[i].id, c->id) == 0) {
            ESP_LOGE(TAG, "用例 id 重复: %s", c->id);
            return;
        }
    }
    s_cases[s_case_count++] = *c;
}

void tf_register_group(const tf_case_t *cases, size_t count)
{
    if (cases == NULL) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        tf_register(&cases[i]);
    }
}

void tf_fail(const char *file, int line, const char *fmt, ...)
{
    if (g_tf == NULL) {
        return;
    }
    /* 只保留第一条失败原因——后续断言往往只是同一次故障的连带结果 */
    if (g_tf->fails == 0) {
        int n = snprintf(g_tf->msg, sizeof(g_tf->msg), "%s:%d ", file, line);
        if (n > 0 && n < (int)sizeof(g_tf->msg)) {
            va_list ap;
            va_start(ap, fmt);
            vsnprintf(g_tf->msg + n, sizeof(g_tf->msg) - n, fmt, ap);
            va_end(ap);
        }
    }
    g_tf->fails++;
}

void tf_info(const char *fmt, ...)
{
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    printf("        | %s\n", buf);
}

/* ---------------- 运行器 ---------------- */

static const char *result_str(tf_result_t r)
{
    switch (r) {
    case TF_PASS: return "PASS";
    case TF_FAIL: return "FAIL";
    case TF_SKIP: return "SKIP";
    default:      return "????";
    }
}

static tf_result_t run_one(const tf_case_t *c, tf_record_t *rec)
{
    tf_ctx_t ctx = { .checks = 0, .fails = 0, .msg = { 0 } };
    g_tf = &ctx;

    /* 记下来，跑完整个批次后由应用层把网络拉回来 —— 不能等到用例内部
     * 自己恢复：stop 那个用例正是把射频关掉的那个，之后它已经没有网络了。 */
    if ((c->flags & TF_FLAG_NONET) != 0) {
        s_net_broken = true;
    }

    int64_t t0 = esp_timer_get_time();
    tf_result_t r = c->fn();
    uint64_t us = (uint64_t)(esp_timer_get_time() - t0);

    g_tf = NULL;

    /* 用例返回 PASS 但断言已经失败过 —— 说明写了 return TF_PASS 的裸路径，
     * 以失败为准，避免"假通过"污染报告。 */
    if (r == TF_PASS && ctx.fails > 0) {
        r = TF_FAIL;
    }

    rec->c      = c;
    rec->result = r;
    rec->checks = ctx.checks;
    rec->fails  = ctx.fails;
    rec->us     = us;
    memcpy(rec->msg, ctx.msg, sizeof(rec->msg));
    return r;
}

static void report_record(int idx, const tf_record_t *rec)
{
    /* 用例名左对齐 22 列，便于肉眼扫 */
    printf("[TF] %2d %-4s %-22s %3d checks %6llu us",
           idx + 1, result_str(rec->result), rec->c->id,
           rec->checks, (unsigned long long)rec->us);
    if (rec->result != TF_PASS && rec->msg[0] != '\0') {
        printf("  <- %s", rec->msg);
    }
    printf("\n");
}

static void print_summary(void)
{
    int pass = 0, fail = 0, skip = 0;
    uint64_t total_us = 0;
    for (int i = 0; i < s_record_count; i++) {
        switch (s_records[i].result) {
        case TF_PASS: pass++; break;
        case TF_FAIL: fail++; break;
        default:      skip++; break;
        }
        total_us += s_records[i].us;
    }
    printf("[TF] ------------------------------------------------------------\n");
    printf("[TF] 结果: %d 通过 / %d 失败 / %d 跳过，共 %d 项，耗时 %llu us\n",
           pass, fail, skip, s_record_count, (unsigned long long)total_us);
    printf("[TF] %s\n", fail == 0 ? "总体判定: PASS" : "总体判定: FAIL");
}

/* 一批用例跑完后，如果其中有会断网的，就把网络拉回来。
 * 放在批次的末尾（而不是用例内部）是有意的：wifi.stop 之后射频已经关了，
 * 用例自己没法再联网；只有在所有用例都结束、射频状态稳定后重连才可靠。 */
static void maybe_restore_net(void)
{
    if (!s_net_broken) {
        return;
    }
    s_net_broken = false;
    if (s_net_restore == NULL) {
        return;
    }
    printf("[TF] 本批执行了会断网的用例，正在恢复联网…\n");
    fflush(stdout);
    s_net_restore();
}

void tf_run_all(void)
{
    s_record_count = 0;
    s_net_skipped  = 0;
    s_net_broken   = false;
    s_last_full    = true;
    snprintf(s_last_scope, sizeof(s_last_scope), "run");
    printf("\n[TF] ============== 开始执行全部用例（%d 项） ==============\n",
           s_case_count);
    for (int i = 0; i < s_case_count && s_record_count < TF_MAX_CASES; i++) {
        /* 会断网的用例默认不跑：自检的目的是查问题，不该顺手把
         * 正在用的网页/MQTT 连接弄断。要跑请指名 run wifi。 */
        if ((s_cases[i].flags & TF_FLAG_NONET) != 0) {
            /* 仍然记一条 SKIP，而不是"整条消失"：
             * 一是报告里能看出这一类存在、只是没跑；
             * 二是网页按 id 匹配结果，缺项会让上一次的旧状态残留在页面上。 */
            tf_record_t *rec = &s_records[s_record_count];
            rec->c      = &s_cases[i];
            rec->result = TF_SKIP;
            rec->checks = 0;
            rec->fails  = 0;
            rec->us     = 0;
            snprintf(rec->msg, sizeof(rec->msg),
                     "会切断网络：run 默认不执行（要跑请用 run wifi）");
            printf("[TF] -- 跳过 %-22s 会切断网络（要跑请用 run wifi）\n",
                   s_cases[i].id);
            s_net_skipped++;
            s_record_count++;
            continue;
        }
        tf_result_t r = run_one(&s_cases[i], &s_records[s_record_count]);
        report_record(s_record_count, &s_records[s_record_count]);
        s_record_count++;
        (void)r;
    }
    print_summary();
    if (s_net_skipped > 0) {
        printf("[TF] 其中 %d 项为「会断网」而未执行：要跑请用 run wifi（跑完自动恢复联网）\n",
               s_net_skipped);
    }
    tf_print_json();
}

static bool id_contains(const char *id, const char *substr)
{
    return strstr(id, substr) != NULL;
}

int tf_run_filter(const char *substr)
{
    s_record_count = 0;
    s_net_broken   = false;
    s_last_full    = false;
    snprintf(s_last_scope, sizeof(s_last_scope), "run %.30s", substr);
    for (int i = 0; i < s_case_count; i++) {
        if (!id_contains(s_cases[i].id, substr)) {
            continue;
        }
        run_one(&s_cases[i], &s_records[s_record_count]);
        report_record(s_record_count, &s_records[s_record_count]);
        s_record_count++;
    }
    if (s_record_count == 0) {
        printf("[TF] 没有匹配 \"%s\" 的用例\n", substr);
        return 0;
    }
    print_summary();
    tf_print_json();
    maybe_restore_net();
    return s_record_count;
}

int tf_run_category(const char *category)
{
    s_record_count = 0;
    s_net_broken   = false;
    s_last_full    = false;
    snprintf(s_last_scope, sizeof(s_last_scope), "cat %.30s", category);
    for (int i = 0; i < s_case_count; i++) {
        if (s_cases[i].category == NULL ||
            strcmp(s_cases[i].category, category) != 0) {
            continue;
        }
        run_one(&s_cases[i], &s_records[s_record_count]);
        report_record(s_record_count, &s_records[s_record_count]);
        s_record_count++;
    }
    if (s_record_count == 0) {
        printf("[TF] 没有分组 \"%s\"\n", category);
        return 0;
    }
    print_summary();
    tf_print_json();
    maybe_restore_net();
    return s_record_count;
}

void tf_list(void)
{
    printf("\n[TF] 已注册用例（%d 项）\n", s_case_count);
    for (int i = 0; i < s_case_count; i++) {
        printf("  %-24s [%-6s] %s\n", s_cases[i].id, s_cases[i].category,
               s_cases[i].desc);
    }
    printf("\n");
}

static void cache_results(void);

void tf_print_json(void)
{
    int pass = 0, fail = 0, skip = 0;
    for (int i = 0; i < s_record_count; i++) {
        switch (s_records[i].result) {
        case TF_PASS: pass++; break;
        case TF_FAIL: fail++; break;
        default:      skip++; break;
        }
    }
    /* 单行 JSON，方便主机侧按行匹配 ^TFJSON */
    printf("[TF] TFJSON {\"total\":%d,\"pass\":%d,\"fail\":%d,\"skip\":%d,\"cases\":[",
           s_record_count, pass, fail, skip);
    for (int i = 0; i < s_record_count; i++) {
        const tf_record_t *r = &s_records[i];
        printf("%s{\"id\":\"%s\",\"result\":\"%s\",\"checks\":%d,\"us\":%llu",
               i ? "," : "", r->c->id, result_str(r->result), r->checks,
               (unsigned long long)r->us);
        if (r->msg[0] != '\0') {
            /* 转义双引号，避免破坏 JSON */
            printf(",\"msg\":\"");
            for (const char *p = r->msg; *p; p++) {
                if (*p == '"' || *p == '\\') {
                    printf("\\%c", *p);
                } else if (*p == '\n') {
                    printf("\\n");
                } else {
                    printf("%c", *p);
                }
            }
            printf("\"");
        }
        printf("}");
    }
    printf("]}\n");
    cache_results();
}

/* ---------------- 结果缓存：给"开机自动上报"用 ----------------
 *
 * 为什么要有缓存：自检在启动时跑完（此时 MQTT 还没连上），而网页要在
 * 连上之后自动看到这份结果。所以结果先落到静态缓冲区，谁需要谁来取，
 * 不必为了一次发布把整个测试再跑一遍。
 *
 * 和串口那份 JSON 的区别：这里省掉每项的 checks/us（网页不显示），
 * 只保留 id/result，失败项才带 msg —— 52 项大约 2.5 KB，一条报文发完。
 */
static char     s_json_cache[6144];
static bool     s_json_ready = false;
static uint32_t s_json_gen   = 0;

static void json_append(char *buf, size_t cap, int *off, const char *fmt, ...)
{
    if (*off < 0 || (size_t)*off >= cap) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + *off, cap - (size_t)*off, fmt, ap);
    va_end(ap);
    if (n < 0) {
        *off = -1;
        return;
    }
    if ((size_t)n >= cap - (size_t)*off) {
        /* 放不下就标记溢出，调用方据此截断（宁可少几条也不要半条） */
        *off = (int)cap;
        return;
    }
    *off += n;
}

static void cache_results(void)
{
    int pass = 0, fail = 0, skip = 0;
    uint64_t us = 0;
    for (int i = 0; i < s_record_count; i++) {
        switch (s_records[i].result) {
        case TF_PASS: pass++; break;
        case TF_FAIL: fail++; break;
        default:      skip++; break;
        }
        us += s_records[i].us;
    }

    /* scope 来自命令行输入，只留白名单字符，避免把 JSON 写坏 */
    char scope[40] = { 0 };
    for (int i = 0; i < (int)sizeof(s_last_scope) - 1 && s_last_scope[i]; i++) {
        char ch = s_last_scope[i];
        bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                  (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' ||
                  ch == '-' || ch == ' ';
        scope[i] = ok ? ch : '?';
    }

    int off = 0;
    json_append(s_json_cache, sizeof(s_json_cache), &off,
                "{\"up\":%u,\"total\":%d,\"pass\":%d,\"fail\":%d,\"skip\":%d,"
                "\"us\":%llu,\"full\":%d,\"scope\":\"%s\",\"cases\":[",
                (unsigned)(esp_timer_get_time() / 1000000), s_record_count,
                pass, fail, skip, (unsigned long long)us,
                s_last_full ? 1 : 0, scope);

    int emitted = 0;
    for (int i = 0; i < s_record_count && off > 0 && (size_t)off < sizeof(s_json_cache); i++) {
        const tf_record_t *r = &s_records[i];
        /* 带上 category：网页靠它把"固件新增、页面清单里还没有的用例"
         * 归到正确的分组里，否则这些项会整条消失、计数也对不上。 */
        json_append(s_json_cache, sizeof(s_json_cache), &off,
                    "%s{\"id\":\"%s\",\"cat\":\"%s\",\"result\":\"%s\"",
                    emitted ? "," : "", r->c->id,
                    r->c->category ? r->c->category : "", result_str(r->result));
        if (off <= 0 || (size_t)off >= sizeof(s_json_cache)) {
            break;
        }
        if (r->msg[0] != '\0') {
            json_append(s_json_cache, sizeof(s_json_cache), &off, ",\"msg\":\"");
            for (const char *p = r->msg; *p && off > 0 && (size_t)off < sizeof(s_json_cache); p++) {
                if (*p == '"' || *p == '\\') {
                    json_append(s_json_cache, sizeof(s_json_cache), &off, "\\%c", *p);
                } else if (*p == '\n') {
                    json_append(s_json_cache, sizeof(s_json_cache), &off, "\\n");
                } else {
                    json_append(s_json_cache, sizeof(s_json_cache), &off, "%c", *p);
                }
            }
            json_append(s_json_cache, sizeof(s_json_cache), &off, "\"");
        }
        json_append(s_json_cache, sizeof(s_json_cache), &off, "}");
        emitted++;
    }
    /* 缓冲不够时补上截断标记，网页能看出这份结果不完整 */
    json_append(s_json_cache, sizeof(s_json_cache), &off, "],\"cut\":%d}",
                emitted < s_record_count ? 1 : 0);
    if (off > 0 && (size_t)off <= sizeof(s_json_cache)) {
        s_json_ready = true;
        s_json_gen++;
    }
}

const char *tf_results_json(void)
{
    return s_json_ready ? s_json_cache : NULL;
}

uint32_t tf_results_gen(void)
{
    return s_json_gen;
}

/* ---------------- 串口命令行 ---------------- */

/* 应用层注册的命令钩子；未注册时退回"未知命令" */
static tf_cmd_hook_t s_cmd_hook = NULL;

void tf_shell_set_hook(tf_cmd_hook_t hook)
{
    s_cmd_hook = hook;
}

static void shell_help(void)
{
    printf("\n[TF] 命令:\n"
           "  list              列出所有用例\n"
           "  run               跑全部（自动跳过会断网的用例）\n"
           "  run <关键字>      按 id 子串跑，如 run axp2101\n"
           "  cat <分组>        按分组跑，如 cat pmic\n"
           "  json              重发最近一次结果 JSON\n"
           "  reboot            重启\n"
           "  help              显示本帮助\n"
           "  cam start|stop    摄像头出图 + 网页服务\n"
           "  cam               查看画面状态\n"
           "  batt              电池电压与充电状态\n"
           "  batt watch [秒]   连续采样电池电压（观察插拔 USB / 充电趋势）\n"
           "  batt probe        判定 BAT 脚是否与 VSYS 同一节点（电池是否真在回路）\n"
           "  sec               查看端到端加密状态（AES-256-GCM）\n"
           "  sec key <hex>     换 32 字节密钥（64 位 hex，重启生效）\n"
           "  health            自愈看门狗状态（摄像头卡死 / MQTT 假死）\n"
           "  health test       手动跑一次摄像头自愈，验证救得回来\n"
           "  cam res vga|qvga  切 640x480 / 320x240（画质 vs 带宽）\n"
           "  cam q <0..63>     JPEG 画质档，0 最清晰、63 最省流量\n"
           "  cam bar           内建彩条（判断数据通路）\n"
           "  cam probe         停流后量 PCLK/VSYNC/HREF/D0 引脚跳变（排查用）\n"
           "  wifi join         加入板级默认路由器并启动网页\n"
           "  wifi knock        踢掉当前连接，验证掉线能否自动重连\n"
           "  cam dn 0..4       降噪等级（0 关，暗光横纹重时调大）\n"
           "  cam gain 2..128   增益上限（调小画面更干净，但会更暗）\n"
           "  wifi ip           查看当前 IP\n\n");
}

/**
 * 执行一行命令（串口命令行与 MQTT 远程命令共用同一套分发）。
 *
 * 抽出来是因为 mqtt_cmd.c 需要在**自己的任务里**执行同一条命令：
 * 复制一份 if 链必然会两边慢慢走偏（加了新命令只改一处），所以只留这一份。
 */
void tf_exec_line(const char *cmd_line)
{
    char buf[192];
    size_t n;

    if (cmd_line == NULL) {
        return;
    }
    snprintf(buf, sizeof(buf), "%s", cmd_line);

    /* 去掉行尾换行 */
    n = strlen(buf);
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) {
        buf[--n] = '\0';
    }
    /* 去掉前导空格 */
    char *cmd = buf;
    while (*cmd == ' ') {
        cmd++;
    }
    if (*cmd == '\0') {
        return;
    }

    if (strcmp(cmd, "list") == 0) {
        tf_list();
    } else if (strcmp(cmd, "run") == 0) {
        tf_run_all();
    } else if (strncmp(cmd, "run ", 4) == 0) {
        tf_run_filter(cmd + 4);
    } else if (strncmp(cmd, "cat ", 4) == 0) {
        tf_run_category(cmd + 4);
    } else if (strcmp(cmd, "json") == 0) {
        tf_print_json();
    } else if (strcmp(cmd, "reboot") == 0) {
        printf("[TF] 重启…\n");
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
    } else if (strcmp(cmd, "help") == 0) {
        shell_help();
    } else if (s_cmd_hook != NULL && s_cmd_hook(cmd)) {
        /* 应用层命令已处理，这里什么都不做 */
    } else {
        printf("[TF] 未知命令: %s（输入 help 查看）\n", cmd);
    }
}

static void shell_task(void *arg)
{
    (void)arg;
    static char line[128];
    bool need_prompt = true;
    for (;;) {
        /* 只在"进入空闲"时打一次提示符。
         * 无输入时 fgets 会立即返回失败，若每轮都打提示符，屏幕上会刷出一长串 "tf> "。 */
        if (need_prompt) {
            printf("tf> ");
            fflush(stdout);
            need_prompt = false;
        }
        if (fgets(line, sizeof(line), stdin) == NULL) {
            /* 控制台未接输入时不要空转烧 CPU */
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        need_prompt = true;      /* 处理完这条命令后重新显示提示符 */
        tf_exec_line(line);
    }
}

void tf_shell_start(void)
{
    /* 栈 4096：里面只有 snprintf/fgets 与几层调用，够用
     * （printf 走 vfs 会吃掉几百字节，留一倍余量） */
    BaseType_t ok = xTaskCreate(shell_task, "tf_shell", 4096, NULL, 4, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "命令行任务创建失败");
        return;
    }
    shell_help();
}
