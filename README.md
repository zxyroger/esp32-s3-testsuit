# ESP32-S3 板级测试台

一块 ESP32-S3 开发板的完整板级验证工程：**逐项自检硬件 + 摄像头 MQTT 远程推流 + 网页控制台**。

不是"点个灯"的例程。这里的每一项都用可观测的物理量去证明通路真的通了——
I2C 扫地址、ADC 交叉验证、音频数字回环、摄像头 SCCB 应答 + 帧率，
而不是"函数返回 ESP_OK 就算过"。

- **57 项**自检覆盖核心、I2C、PMIC、LCD、摄像头、Wi-Fi、音频
- 上电自动跑一遍，结果推到网页（RETAIN 消息，随时打开都能看到）
- 摄像头画面经 AES-256-GCM 加密后走公共 MQTT，网页端解密显示
- `health` 看门狗：摄像头无帧自动重初始化，MQTT 长时间无写出自动重连，连续失效软重启

---

## 硬件

| 部件 | 型号 | 接口 |
|---|---|---|
| 主控 | ESP32-S3-WROOM（16 MB Flash / 8 MB Octal PSRAM） | — |
| 摄像头 | OV2640 | DVP 8 位并口，SCCB 复用 I2C0 |
| 屏幕 | ILI9342C | SPI，320×240 |
| 音频 | ES8311 codec + NS4150B 功放 + 咪头 | I2S |
| 电源管理 | AXP2101 | I2C，地址 0x34 |

引脚定义全部集中在 `main/board_config.h`，换板只改这一个文件。
管脚依据同款板量产工程实测确定，与公开引脚图有若干差异（都在该文件注释里写明）。

---

## 目录结构

```
main/                  固件源码
  board_config.h       板级引脚与外设配置（换板改这里）
  lab_secrets.h        私密配置入口（含占位兜底）
  app_cmds.c           串口命令行
  tf.c / tf.h          测试框架（TF_ITEM / 分类 / JSON 上报）
  test_*.c             各分类测试用例
  cam_ov2640.c         摄像头驱动与初始化
  cam_jpeg.c           JPEG 取帧与发布
  cam_mqtt.c           MQTT 3.1.1 客户端（手写，TLS）
  sec_crypto.c         AES-256-GCM 加解密
  health.c             自愈看门狗
  axp2101.c            电源管理芯片驱动
  ...
components/
  esp32-camera/        摄像头传感器驱动
viewer/                网页控制台
  template.html        页面骨架
  app.js               业务逻辑
  mqtt-lite.js         手写 MQTT over WebSocket 客户端
  crypto.js            WebCrypto AES-GCM 解密
  build.py             把上面几份内联成单文件 index.html
docs/                  硬件笔记与测试项说明
*.py                   调试工具（帧率统计、命令通道、串口桥接、电池循环）
```

---

## 快速开始

### 1. 环境

- ESP-IDF **v6.0.2**（工程用了 PSA Crypto API，v5.x 的 `mbedtls/gcm.h` 在 6.0 已移除，旧版本编不过）
- 目标芯片 ESP32-S3，16 MB Flash

### 2. 配置私密信息（必须）

Wi-Fi 账密、加密密钥、MQTT 主题前缀**都不在仓库里**，需要自己填：

```bash
cp main/secrets.h.example    main/secrets.h
cp viewer/secrets.js.example viewer/secrets.js
```

然后编辑这两个文件：

| 文件 | 填什么 |
|---|---|
| `main/secrets.h` | `SECRET_WIFI_SSID` / `SECRET_WIFI_PASS`（**必须 2.4G**，S3 不支持 5G）<br>`SECRET_AES_KEY_BYTES` 32 字节密钥<br>`SECRET_MQTT_PREFIX` 主题前缀 |
| `viewer/secrets.js` | `KEY_HEX` 与前者的密钥一致（64 位 hex）<br>`BASE` 与前者的前缀一致 |

生成一对新密钥：

```bash
python -c "import os;print(os.urandom(32).hex())"          # 前端 KEY_HEX
python -c "import os;print(','.join('0x%02x'%b for b in os.urandom(32)))"  # 固件字节数组
```

两个文件都在 `.gitignore` 里。没填也能编译（走占位符），但连不上 Wi-Fi、网页解不开图像。

### 3. 编译烧写

```powershell
# D:\s3demo 下，需要先准备好 esp-idf 环境
.\flash.ps1 build          # 只编译
.\flash.ps1 flash COM3     # 编译 + 烧写
.\flash.ps1 monitor COM3   # 编译 + 烧写 + 串口监视
```

不用脚本的话，标准 IDF 流程：

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p COM3 flash monitor
```

### 4. 打开网页

```bash
cd viewer && python build.py      # 生成内联了密钥的单文件 index.html
```

`index.html` 直接双击打开即可（`file://` 也行），或者放到任意静态服务器上。
页面连的是公共 broker，和板子不在同一个网络也能看图。

---

## 使用

### 串口命令

```
help                      列出全部命令
list                      列出所有自检用例 ID
run                       跑全部（自动跳过会断网的 Wi-Fi 用例）
run <过滤词>              只跑 ID 里含该词的用例
cat <分类>                按分类跑：core / i2c / axp2101 / lcd / camera / wifi / audio / es8311
json                      打印上一次结果的 JSON（网页靠这条上报）

cam start | cam stop      启停摄像头推流
cam probe                 探测 SCCB 应答与传感器 ID
cam res vga | qvga        分辨率
cam 50hz [off]            工频防闪烁
cam gray [off]            灰度

wifi join [ssid pass]     加入路由器（不带参数用 secrets.h 里的）
wifi ip                   查看 IP
wifi knock                主动断开，观察自动重连

mqtt on | off             启停 MQTT 推流
sec                       查看密钥指纹
batt                      读一次电池状态
batt watch                持续刷新电压
health                    查看看门狗状态
health test               注入故障验证自愈
ddns ...                  动态域名（可选）
```

### 网页

三个页面：**画面**（实时视频 + 帧率）、**测试**（自检结果，按分类展开）、**控制台**（发命令 + 串口输出）。

测试页的数据来自固件上电后自动发布的一条 RETAIN 消息，所以任何时刻打开都能看到最近一次结果，
不用手动触发。想重跑就点页面上的按钮。

### MQTT 主题

前缀由 `SECRET_MQTT_PREFIX` 决定，下面用 `<前缀>` 表示：

| 话题 | 方向 | 内容 |
|---|---|---|
| `<前缀>/jpg` | 板 → 页 | JPEG 帧 |
| `<前缀>/meta` | 板 → 页 | 帧元数据 |
| `<前缀>/serial/tx` | 板 → 页 | 串口输出 |
| `<前缀>/cmd` | 页 → 板 | 命令 |
| `<前缀>/batt` | 板 → 页 | 电池采样 |
| `<前缀>/chal` | 板 → 页 | 防重放挑战码 |
| `<前缀>/test` | 板 → 页 | 自检结果（RETAIN） |

---

## 加密

公共 broker 上跑的是密文。线格式（固件与网页完全一致）：

```
[0] 版本 0x01   [1] 保留 0x00   [2..13] 12 字节 IV   [14..] 密文 ‖ 16 字节 tag
```

AAD 用话题名本身，所以密文被搬到别的话题上会认证失败。

**这是共享密钥模型**：密钥同时存在于固件和网页里，拿到网页的人就拿得到密钥。
它挡的是公共 broker 上的陌生人偷看，**不挡**拿到分享链接的人。要更强的模型得换成
口令派生或非对称密钥交换。

调试时想临时换密钥：板子 `sec key <hex>` → 重启 → 页面用 `?k=<同一串>` 打开一次（会记住）。

---

## 测试框架

`tf.c` 是一个轻量自检框架，用例用宏注册：

```c
/* 参数：分类, 名称, 描述, 函数, 标志 */
TF_ITEM(i2c, scan, "扫描总线并列出所有应答地址", test_i2c_scan, TF_FLAG_HW);
```

当前分布（共 57 项）：`core` 4、`i2c` 2、`axp2101` 14、`lcd` 8、`camera` 8、`wifi` 8、`audio` 7、`es8311` 6。

标志位：

| 标志 | 含义 |
|---|---|
| `TF_FLAG_HW` | 需要真实硬件 |
| `TF_FLAG_SLOW` | 耗时较长，默认批次里跳过 |
| `TF_FLAG_NONET` | 会切断网络（Wi-Fi 那 5 项），`run` 默认跳过并记 SKIP |

设计取舍：**跳过的用例也记一条 SKIP**，不静默消失——否则汇总里"57 项"和实际跑的对不上，
会让人误以为全过了。

### SKIP 和 FAIL 的边界

这条边界写错，故障就会从报告里消失：

- **SKIP = 测不了**。缺前置条件（I2C 无应答，后面的寄存器读不到）、缺工装（板子没引出
  MISO，读不到屏 ID）、按策略主动跳过（`TF_FLAG_NONET`）。结论是"本次没有证据"。
- **FAIL = 测得了，结论是坏的**。信号过弱、寄存器值不对、回环全 0。这类判断往往没有
  "期望值 vs 实测值"的断言形态，用 `TF_FAIL_MSG()` 直接判失败——别图省事写 `TF_SKIP`。

`audio.capture_level` 原来就踩了这个坑：麦克风 RMS=1（全直流码，PGA 从 42 dB 拉到 0 dB
读数一点不变），被判成 SKIP，于是汇总里一直是"0 失败"。改成 FAIL 后问题立刻显形。

`run` 结束后如果发现有 `NONET` 用例被跳过，框架会回调应用层恢复联网（重新 init Wi-Fi +
`wifi join` + 重连 MQTT），所以跑测试不会把网页搞断。

---

## 自愈

`health.c` 每秒巡检：

| 现象 | 动作 |
|---|---|
| 摄像头 5 秒无新帧 | 停止再重新初始化；连续 3 次救不回 → `esp_restart()` |
| MQTT 60 秒没有成功写出 | 主动重连；连续 2 次无效 → `esp_restart()` |

判定用"最后一次真正写出去的时刻"而不是连接状态标志——socket 还开着但对端已经不理你，
这种情况只有写出成功才算活。

---

## 已知限制

- **只支持 2.4 GHz**。5G SSID 永远连不上，这是芯片限制。
- 公共 broker `broker.emqx.io` 免注册，任何人都能连。安全性靠上面的加密，主题前缀不是密码。
- `cam 50hz off` 目前不会恢复厂商推荐的 BD50/BD50MAX 默认值（切太多次容易出错，宁可不动）。
- 音频的 DAC → 功放 → 喇叭这一段没有电气回环，只能验证到数字链路（I2S 数据完整）。
  模拟输出要示波器或耳朵确认。

---

## 参考

`docs/` 下有 AXP2101 的寄存器笔记与 14 项电源测试的判据推导。
数据手册请从厂商官网获取（不随仓库分发）。

摄像头驱动的管脚与初始化序列参考了同款板量产工程，差异处均在 `board_config.h` 注释里标注。
