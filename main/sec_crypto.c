/**
 * @file  sec_crypto.c
 * @brief AES-256-GCM 负载加密，线格式见 sec_crypto.h。
 *
 * ESP-IDF 6.0 带的是 Mbed TLS 4.x，旧的 `mbedtls/gcm.h` 已经移除，统一走
 * PSA Crypto API（`psa_aead_encrypt/decrypt`）。PSA 的 AEAD 输出天然就是
 * 「密文 ‖ 16 字节 tag」，正好和浏览器 WebCrypto 的 AES-GCM 输出一致，
 * 两边不用做任何拼接转换。
 *
 * ESP32-S3 有 AES 硬件加速，8 KB 一帧的加解密在微秒级，不影响推流帧率。
 */
#include "sec_crypto.h"
#include "lab_secrets.h"

#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"
#include "psa/crypto.h"

static const char *TAG = "sec";

/* 默认密钥（32 字节）来自 main/secrets.h，必须与 viewer/secrets.js 里的 KEY_HEX 一致。
 * 这是"共享密钥"模型：拿到网页的人就拿得到它，所以它挡的是公共 broker 上的
 * 陌生人偷看，而不是挡拿到分享链接的人。要更强就换成口令派生，见文档。
 * secrets.h 缺失时会退化成全零（能编译，但网页解不开）。 */
static const uint8_t SEC_DEFAULT_KEY[32] = SECRET_AES_KEY_BYTES;

static uint8_t      s_key[32];
static psa_key_id_t s_key_id;
static bool         s_ready;
static bool         s_default_key = true;   /* 没被 NVS 覆盖时才跑"对齐 PC"的已知答案测试 */

static bool key_load(void)
{
    memcpy(s_key, SEC_DEFAULT_KEY, sizeof(s_key));

    nvs_handle_t h;
    if (nvs_open("sec", NVS_READONLY, &h) == ESP_OK) {
        uint8_t tmp[32];
        size_t len = sizeof(tmp);
        if (nvs_get_blob(h, "key", tmp, &len) == ESP_OK && len == sizeof(tmp)) {
            memcpy(s_key, tmp, sizeof(s_key));
            s_default_key = false;
            ESP_LOGI(TAG, "使用 NVS 中的密钥");
        }
        nvs_close(h);
    }

    psa_status_t st = psa_crypto_init();
    if (st != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_crypto_init 失败 %d", (int)st);
        return false;
    }

    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&attr, PSA_ALG_GCM);
    psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attr, 256);

    st = psa_import_key(&attr, s_key, sizeof(s_key), &s_key_id);
    psa_reset_key_attributes(&attr);
    if (st != PSA_SUCCESS) {
        ESP_LOGE(TAG, "导入密钥失败 %d", (int)st);
        return false;
    }
    return true;
}

esp_err_t sec_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }
    if (!key_load()) {
        return ESP_FAIL;
    }
    s_ready = true;

    /* 自检：自己加密 5 字节再解开。加密配置有问题时这里就会暴露，
     * 而不是等到页面上"解密失败"再回头查。 */
    uint8_t buf[64];
    const char *probe = "hello";
    size_t n = sec_seal("selftest", (const uint8_t *)probe, 5, buf, sizeof(buf));
    uint8_t back[32];
    int m = (n > 0) ? sec_open("selftest", buf, n, back, sizeof(back)) : -1;
    if (m == 5 && memcmp(back, probe, 5) == 0) {
        ESP_LOGI(TAG, "已就绪：AES-256-GCM，每包开销 %d 字节，自检通过", SEC_OVERHEAD);
    } else {
        ESP_LOGE(TAG, "自检失败：n=%d m=%d", (int)n, m);
    }
    ESP_LOGI(TAG, "密钥指纹 %02x%02x%02x%02x（与页面 KEY_HEX 前 4 字节应一致）",
             s_key[0], s_key[1], s_key[2], s_key[3]);

    /* 已知答案测试：固定 IV=全 0、明文 "0123456789abcdef"、AAD "vect"。
     * 和 PC/浏览器算出来的结果必须逐字节一致，用来确认线格式没跑偏。 */
    {
        uint8_t iv0[12] = {0};
        uint8_t kat[64];
        size_t klen = 0;
        if (psa_aead_encrypt(s_key_id, PSA_ALG_GCM, iv0, 12,
                             (const uint8_t *)"vect", 4,
                             (const uint8_t *)"0123456789abcdef", 16,
                             kat, sizeof(kat), &klen) == PSA_SUCCESS) {
            char hex[2 * 64 + 1];
            for (size_t i = 0; i < klen && i < 48; i++) {
                snprintf(&hex[i * 2], 3, "%02x", kat[i]);
            }
            hex[klen * 2] = '\0';
            ESP_LOGI(TAG, "KAT %u: %s", (unsigned)klen, hex);

            /* 反向验证：把 PC/浏览器算出来的同一组密文拿来解密。
             * 用默认密钥时必须能解开；换成 NVS 自定义密钥后这组值自然对不上，跳过。 */
            if (s_default_key) {
            static const uint8_t pc_kat[32] = {
                0x0e, 0x13, 0xeb, 0x2e, 0x2c, 0xf3, 0xfc, 0xb3,
                0x57, 0x70, 0x01, 0x1d, 0x4c, 0x30, 0x49, 0xc2,
                0xd2, 0xde, 0xd7, 0xf3, 0x91, 0x7a, 0xfa, 0xa5,
                0xc1, 0x97, 0x59, 0x9d, 0xa8, 0xd2, 0xa0, 0x6f,
            };
            uint8_t env[2 + 12 + 32];
            env[0] = 0x01; env[1] = 0x00;
            memset(env + 2, 0, 12);
            memcpy(env + 14, pc_kat, sizeof(pc_kat));
            uint8_t out[32];
            int r = sec_open("vect", env, sizeof(env), out, sizeof(out));
            if (r == 16) {
                ESP_LOGI(TAG, "与 PC/浏览器互通：是");
            } else {
                ESP_LOGE(TAG, "与 PC/浏览器不通（r=%d），检查线格式或密钥", r);
            }
            }
        } else {
            ESP_LOGE(TAG, "KAT 计算失败");
        }
    }
    return ESP_OK;
}

bool sec_ready(void)
{
    return s_ready;
}

size_t sec_seal(const char *aad, const uint8_t *pt, size_t pt_len, uint8_t *out, size_t out_cap)
{
    if (!s_ready || out == NULL || out_cap < pt_len + SEC_OVERHEAD) {
        return 0;
    }
    const uint8_t *a = (const uint8_t *)(aad ? aad : "");
    size_t aad_len = aad ? strlen(aad) : 0;

    uint8_t *iv = out + 2;
    uint8_t *ct = out + 14;          /* 密文 ‖ tag */
    out[0] = 0x01;
    out[1] = 0x00;
    esp_fill_random(iv, 12);

    size_t ct_len = 0;
    psa_status_t st = psa_aead_encrypt(s_key_id, PSA_ALG_GCM,
                                       iv, 12,
                                       a, aad_len,
                                       pt, pt_len,
                                       ct, out_cap - 14, &ct_len);
    if (st != PSA_SUCCESS) {
        ESP_LOGW(TAG, "加密失败 %d", (int)st);
        return 0;
    }
    return 14 + ct_len;
}

void sec_key_fp(uint8_t out[4])
{
    memcpy(out, s_key, 4);
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

esp_err_t sec_set_key_hex(const char *hex)
{
    if (hex == NULL || strlen(hex) != 64) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t k[32];
    for (int i = 0; i < 32; i++) {
        int hi = hex_val(hex[i * 2]);
        int lo = hex_val(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return ESP_ERR_INVALID_ARG;
        }
        k[i] = (uint8_t)((hi << 4) | lo);
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open("sec", NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, "key", k, sizeof(k));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

int sec_open(const char *aad, const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap)
{
    if (!s_ready || in == NULL || in_len < SEC_OVERHEAD || in[0] != 0x01) {
        return -1;
    }
    const uint8_t *a = (const uint8_t *)(aad ? aad : "");
    size_t aad_len = aad ? strlen(aad) : 0;

    const uint8_t *iv = in + 2;
    const uint8_t *ct = in + 14;     /* 密文 ‖ tag */
    size_t ct_len = in_len - 14;

    size_t pt_len = 0;
    psa_status_t st = psa_aead_decrypt(s_key_id, PSA_ALG_GCM,
                                       iv, 12,
                                       a, aad_len,
                                       ct, ct_len,
                                       out, out_cap, &pt_len);
    if (st != PSA_SUCCESS) {
        return -1;
    }
    return (int)pt_len;
}
