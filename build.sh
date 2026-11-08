#!/usr/bin/env bash
# ESP-IDF 6.0.2 构建脚本（bash 版，给 WorkBuddy 的 Bash 工具用）
#
# 为什么不用 flash.ps1：PowerShell 那条链路把 idf.py 的 stdout 接进了管道，
# 某个编译进程一旦被管道回压就会整轮卡死（实测 nan 分钟 CPU 时间为 0、
# 无新产物）。这里统一把输出写进 build.log，进程不再依赖管道，稳定得多。
#
# 用法：
#   ./build.sh            编译
#   ./build.sh clean      清理后编译
set -o pipefail

export IDF_PATH="D:/esp/v6.0.2/v6.0.2/esp-idf"
export IDF_TOOLS_PATH="C:/Espressif/tools"
export IDF_PYTHON_ENV_PATH="C:/Espressif/tools/python/v6.0.2/venv"
export ESP_IDF_VERSION="6.0.2"
export ESP_ROM_ELF_DIR="$(ls -d C:/Espressif/tools/esp-rom-elfs/*/ 2>/dev/null | head -1)"

export PATH="/c/Espressif/tools/python/v6.0.2/venv/Scripts:/c/Espressif/tools/git/bin:/c/Espressif/tools/cmake/3.30.2/bin:/c/Espressif/tools/ninja/1.12.1:/c/Espressif/tools/xtensa-esp-elf/esp-15.2.0_20251204/xtensa-esp-elf/bin:/c/Espressif/tools/esp32ulp-elf/2.38.1-esp-20220906/esp32ulp-elf-binutils/bin:$PATH"

# C 盘紧张，编译中间文件放 D 盘
export TMPDIR=/d/s3demo_tmp TEMP=D:/s3demo_tmp TMP=D:/s3demo_tmp
mkdir -p /d/s3demo_tmp

cd /d/s3demo || exit 1
PY="/c/Espressif/tools/python/v6.0.2/venv/Scripts/python.exe"
IDFPY="D:/esp/v6.0.2/v6.0.2/esp-idf/tools/idf.py"

if [ "$1" = "clean" ]; then
    "$PY" "$IDFPY" fullclean >>build.log 2>&1
fi

"$PY" "$IDFPY" -j 4 build
