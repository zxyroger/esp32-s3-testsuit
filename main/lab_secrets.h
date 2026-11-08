/**
 * @file  lab_secrets.h
 * @brief 私密配置的唯一入口 —— 其它文件都只 include 这个。
 *
 * 为什么多一层：secrets.h 不在版本库里。如果各处直接 include 它，clone 下来
 * 第一次编译就会因缺文件失败。这里用 __has_include 判断，没有就用占位符，
 * 保证「clone → 编译 → 烧写」这条路能走通，只是连不上 Wi-Fi、网页解不开图。
 *
 * 填真实值：cp main/secrets.h.example main/secrets.h 后编辑。
 */
#pragma once

#if defined(__has_include)
#  if __has_include("secrets.h")
#    include "secrets.h"
#    define LAB_SECRETS_PRESENT 1
#  endif
#endif
#ifndef LAB_SECRETS_PRESENT
#  define LAB_SECRETS_PRESENT 0    /* 用的是占位值，启动时会提示 */
#endif

/* ---- 兜底：没有 secrets.h 时用的占位值 ---- */
#ifndef SECRET_WIFI_SSID
#  define SECRET_WIFI_SSID   "your-2.4g-ssid"
#endif
#ifndef SECRET_WIFI_PASS
#  define SECRET_WIFI_PASS   "your-wifi-password"
#endif
#ifndef SECRET_AES_KEY_BYTES
#  define SECRET_AES_KEY_BYTES { 0 }   /* 全零：能编译，但和网页对不上，解不开 */
#endif
#ifndef SECRET_MQTT_PREFIX
#  define SECRET_MQTT_PREFIX "esp32cam/changeme00"
#endif
