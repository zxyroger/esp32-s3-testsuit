/**
 * @file  app_cmds.h
 * @brief 串口应用命令注册：cam / wifi
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** 把 cam / wifi 命令挂进串口命令行；main.c 启动时调用一次 */
void app_cmds_register(void);

/** 开机自动联网并开始出图（失败只打印原因，不阻塞命令行） */
void app_cmds_autostart(void);

#ifdef __cplusplus
}
#endif
