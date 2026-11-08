/**
 * @file  mqtt_console.h
 * @brief 把命令产生的 printf 输出顺手收一份，供 MQTT 通道回传
 *
 * 为什么不改成统一的 app_out(fmt,...)：app_cmds.c / tf.c 里有一百多处 printf，
 * 全部替换既容易漏又容易改错。这里用链接期的 -Wl,--wrap=printf 接管 printf，
 * 让它在照常输出到 USB 控制台的同时，把同样的内容抄进一个缓冲。
 *
 * 抄写只在 mqtt_console_begin() ~ mqtt_console_end() 之间进行（也就是一条
 * 远程命令执行的那几百毫秒），所以本地敲命令、后台日志都不会误抓。
 */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 开始收集（清空缓冲） */
void mqtt_console_begin(void);

/** 结束收集，返回本次收到的字节数 */
size_t mqtt_console_end(void);

/** 收集到的文本（begin 之后有效，以 0 结尾） */
const char *mqtt_console_text(void);

/** 缓冲容量，超出部分丢弃（尾部会补一行提示） */
size_t mqtt_console_capacity(void);

#ifdef __cplusplus
}
#endif
