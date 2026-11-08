# esp32-camera 2.1.7 —— 本工程裁剪版
#
# 原始组件来自 D:\esp\ai-esp32\firmware\managed_components\espressif__esp32-camera
# （同款板量产工程已实测出图）。裁剪只做两件事，不改任何驱动逻辑：
#
#   1) 去掉 conversions/（yuv.c / to_jpg.cpp / to_bmp.c / jpge.cpp）。
#      本工程让 OV2640 **硬件直出 JPEG**，从不做格式转换，因此这四份源码
#      一行都用不到；保留它们会把 esp_jpeg 依赖拉进来（800 KB 源文件、
#      几十 KB flash），而分区只剩 35%。
#   2) 只编译 sensors/ov2640.c。其余 15 款传感器的 .c 仍留在磁盘上，
#      靠 Kconfig 全部关掉（见 sdkconfig.defaults），既不占 flash，
#      又保留"以后换模组只改配置"的余地。
#
# 与上游的差异仅此而已 —— pin/xclk/dma/vsync 的驱动逻辑保持原样，
# 因为那正是本板验证过能出帧的那份代码。

set(idf_version "${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}")

set(priv_requires "")

set(srcs
  driver/esp_camera.c
  driver/esp_camera_af.c
  driver/cam_hal.c
  driver/sensor.c
  sensors/ov2640.c
  )

set(priv_include_dirs
  driver/private_include
  sensors/private_include
  target/private_include
  )

set(include_dirs
  driver/include
  )

if(IDF_TARGET STREQUAL "esp32s3")
  list(APPEND srcs
    target/esp32s3/ll_cam.c
    )
else()
  message(FATAL_ERROR "本工程裁剪版只保留 ESP32-S3 的 ll_cam 实现")
endif()

list(APPEND priv_requires freertos nvs_flash esp_mm esp_timer)

# SCCB：IDF >= 5.4 且未强制 legacy 时用新版 i2c_master（可直接挂到已有总线）
if (idf_version VERSION_GREATER_EQUAL "5.4" AND NOT CONFIG_SCCB_HARDWARE_I2C_DRIVER_LEGACY)
  list(APPEND srcs driver/sccb-ng.c)
else()
  list(APPEND srcs driver/sccb.c)
endif()

set(req driver)
if (idf_version VERSION_GREATER_EQUAL "6.0")
  list(APPEND priv_requires esp_driver_gpio esp_driver_spi esp_driver_i2c)
  list(APPEND req esp_driver_ledc esp_hal_clock)
endif()

idf_component_register(
  SRCS ${srcs}
  INCLUDE_DIRS ${include_dirs}
  PRIV_INCLUDE_DIRS ${priv_include_dirs}
  REQUIRES ${req}
  PRIV_REQUIRES ${priv_requires}
)
