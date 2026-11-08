/**
 * @file  mqtt_console.c
 * @brief printf 的旁路抄写（链接期 wrap，不需要改任何调用点）
 *
 * 原理：CMake 里加 -Wl,--wrap=printf 之后，全工程对 printf 的调用都会跳到
 * __wrap_printf。我们在这里先调 vprintf 保证控制台输出一切照旧，再按需
 * 把同一份内容格式化进捕获缓冲。
 *
 * 注意：ESP_LOGx 走的是 esp_log_write → vprintf，不经过 printf，所以后台
 * 日志不会被抄进来 —— 这正是想要的（否则摄像头日志会把命令回显淹掉）。
 */
#include "mqtt_console.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define CAP_SIZE 4096
#define FMT_MAX  320

static volatile bool s_on;
static size_t        s_len;
static char          s_buf[CAP_SIZE + 1];

static void cap_put(const char *s, int n)
{
    if (n <= 0 || s_len >= CAP_SIZE) {
        return;
    }
    if ((size_t)n > CAP_SIZE - s_len) {
        n = (int)(CAP_SIZE - s_len);
    }
    memcpy(s_buf + s_len, s, (size_t)n);
    s_len += (size_t)n;
    s_buf[s_len] = '\0';
}

void mqtt_console_begin(void)
{
    s_len = 0;
    s_buf[0] = '\0';
    s_on = true;
}

size_t mqtt_console_end(void)
{
    s_on = false;
    if (s_len >= CAP_SIZE) {
        /* 缓冲写满：补一句，否则页面看到的输出像是被截断了却没有任何提示 */
        static const char tail[] = "\n[输出过长，已截断]\n";
        size_t room = sizeof(s_buf) - 1 - s_len;
        if (room > 0) {
            size_t n = sizeof(tail) - 1;
            if (n > room) {
                n = room;
            }
            memcpy(s_buf + s_len, tail, n);
            s_len += n;
            s_buf[s_len] = '\0';
        }
    }
    return s_len;
}

const char *mqtt_console_text(void)
{
    return s_buf;
}

size_t mqtt_console_capacity(void)
{
    return CAP_SIZE;
}

/* 链接期 --wrap=printf 的落点 */
int __wrap_printf(const char *fmt, ...)
{
    va_list ap;

    /* 1. 控制台照旧 */
    va_start(ap, fmt);
    int n = vprintf(fmt, ap);
    va_end(ap);

    /* 2. 需要时再格式化一份进缓冲 */
    if (s_on && n > 0) {
        char tmp[FMT_MAX];
        va_start(ap, fmt);
        int m = vsnprintf(tmp, sizeof(tmp), fmt, ap);
        va_end(ap);
        if (m > 0) {
            cap_put(tmp, m < (int)sizeof(tmp) ? m : (int)sizeof(tmp) - 1);
        }
    }
    return n;
}

/* 只 wrap printf 是不够的：GCC 会把 printf("字面量\n") 优化成 puts()、
 * 把 printf("%c", c) 优化成 putchar()，help 那种整段常量文本全走 puts，
 * 结果采集到的是空字符串（实测踩过）。三个入口都得接管。 */
int __real_puts(const char *s);
int __real_putchar(int c);

int __wrap_puts(const char *s)
{
    int n = __real_puts(s);
    if (s_on && s != NULL) {
        cap_put(s, (int)strlen(s));
        cap_put("\n", 1);
    }
    return n;
}

int __wrap_putchar(int c)
{
    int r = __real_putchar(c);
    if (s_on) {
        char ch = (char)c;
        cap_put(&ch, 1);
    }
    return r;
}
