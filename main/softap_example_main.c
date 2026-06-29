/*  WiFi ESP-NOW Remote Example */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/param.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/uart.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "nvs_flash.h"

// 目标接收端的 MAC 地址 (目前用广播地址FF占位，请在这里填入飞控端ESP32-S3的真实MAC)
static const uint8_t dest_mac[6] = {0x1A, 0x2B, 0x3C, 0x4D, 0x5E, 0x6F};
#define ESPNOW_CHANNEL (3)

#define UART_ONLY_TEST (0)

#define UART_PORT      UART_NUM_1
#define UART_BAUD_RATE (115200)
#define UART_RX_PIN    (4)
#define UART_TX_PIN    (5)

#define FRAME_HEADER (0xAA)
#define FRAME_FOOTER (0x55)
#define REMOTE_FRAME_LEN (18)
#define REMOTE_ACK (0x55)
#define CONTROL_FRAME_LEN (3)
#define CONTROL_FRAME_BYTE1 (0xA1)
#define CONTROL_FRAME_BYTE2 (0xA2)

typedef struct __attribute__((packed)) {
    uint8_t header;
    uint8_t KEY[4];
    int16_t rocker_l_;
    int16_t rocker_l1;
    int16_t rocker_r_;
    int16_t rocker_r1;
    int16_t dial;
    uint8_t switch_left;
    uint8_t switch_right;
    uint8_t footer;
} remote_t;

static const char *TAG = "tx_espnow";

// 字节序转换函数
static inline int16_t be16_to_i16(const uint8_t *p)
{
    return (int16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

// 解析结构体（处理大端序）
static void parse_remote_frame(const uint8_t *buf, remote_t *frame)
{
    frame->header = buf[0];
    memcpy(frame->KEY, &buf[1], 4);
    frame->rocker_l_ = be16_to_i16(&buf[5]);
    frame->rocker_l1 = be16_to_i16(&buf[7]);
    frame->rocker_r_ = be16_to_i16(&buf[9]);
    frame->rocker_r1 = be16_to_i16(&buf[11]);
    frame->dial = be16_to_i16(&buf[13]);
    frame->switch_left = buf[15];
    frame->switch_right = buf[16];
    frame->footer = buf[17];
}

static void wifi_init_for_espnow(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    // ESP-NOW 不连路由器，配置为 RAM 存储以减少 Flash 磨损
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    // 锁定物理信道，必须与接收端保持一致
    ESP_ERROR_CHECK(esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE));
    ESP_LOGI(TAG, "WiFi hardware init done. Channel locked to %d", ESPNOW_CHANNEL);
}

// （可选）发送回调函数，可用来调试诊断包是否成功送到对方
static void espnow_send_cb(const wifi_tx_info_t *mac_addr, esp_now_send_status_t status)
{
    if (status != ESP_NOW_SEND_SUCCESS) {
        // ESP_LOGD(TAG, "ESP-NOW send failed");
    }
}

static void espnow_init_app(void)
{
    // 初始化 ESP-NOW 引擎
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_send_cb(espnow_send_cb));

    // 添加对端节点信息
    esp_now_peer_info_t peer_info = {0};
    memcpy(peer_info.peer_addr, dest_mac, 6);
    peer_info.channel = ESPNOW_CHANNEL;
    peer_info.encrypt = false;
    
    if (esp_now_add_peer(&peer_info) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add ESP-NOW peer");
        return;
    }
    ESP_LOGI(TAG, "ESP-NOW init done. Peer added.");
}

static esp_err_t uart_init_remote(void)
{
    const uart_config_t uart_config = {
        .baud_rate = UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(UART_PORT, 2048, 1024, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT, UART_TX_PIN, UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    return ESP_OK;
}

static void send_remote_ack(void)
{
    const uint8_t ack = REMOTE_ACK;
    int bytes_written = uart_write_bytes(UART_PORT, &ack, 1);
    if (bytes_written != 1) {
        ESP_LOGW(TAG, "UART ACK write failed: %d bytes written", bytes_written);
    }
}

static bool is_control_frame(const uint8_t *buf, size_t buf_len)
{
    return buf_len >= CONTROL_FRAME_LEN &&
           buf[0] == FRAME_HEADER &&
           buf[1] == CONTROL_FRAME_BYTE1 &&
           buf[2] == CONTROL_FRAME_BYTE2;
}

#if UART_ONLY_TEST
static void uart_only_loop(void)
{
    uint32_t frame_cnt = 0;
    uint8_t in[128];
    uint8_t buf[256];
    size_t buf_len = 0;
    remote_t frame;

    for (;;) {
        int n = uart_read_bytes(UART_PORT, in, sizeof(in), pdMS_TO_TICKS(200));
        if (n <= 0) continue;

        size_t copy_n = MIN((size_t)n, sizeof(buf) - buf_len);
        memcpy(&buf[buf_len], in, copy_n);
        buf_len += copy_n;

        while (buf_len >= CONTROL_FRAME_LEN) {
            size_t i = 0;
            while (i < buf_len && buf[i] != FRAME_HEADER) i++;

            if (i > 0) {
                memmove(buf, &buf[i], buf_len - i);
                buf_len -= i;
            }

            if (buf_len < CONTROL_FRAME_LEN) break;

            if (is_control_frame(buf, buf_len)) {
                ESP_LOGI(TAG, "control frame UART Received: AA A1 A2");
                memmove(buf, &buf[CONTROL_FRAME_LEN], buf_len - CONTROL_FRAME_LEN);
                buf_len -= CONTROL_FRAME_LEN;
                continue;
            }

            if (buf_len < REMOTE_FRAME_LEN) break;

            if (buf[0] != FRAME_HEADER || buf[REMOTE_FRAME_LEN - 1] != FRAME_FOOTER) {
                memmove(buf, &buf[1], buf_len - 1);
                buf_len -= 1;
                continue;
            }

            parse_remote_frame(buf, &frame);
            frame_cnt++;

            ESP_LOGI(TAG, "frame=%lu UART Received", (unsigned long)frame_cnt);
            send_remote_ack();

            memmove(buf, &buf[REMOTE_FRAME_LEN], buf_len - REMOTE_FRAME_LEN);
            buf_len -= REMOTE_FRAME_LEN;
        }
    }
}
#endif

static void forward_loop(void)
{
    uint32_t frame_cnt = 0;
    uint8_t in[128];
    uint8_t buf[256];
    size_t buf_len = 0;
    remote_t frame;

    ESP_LOGI(TAG, "ESP-NOW forward task loop started");

    for (;;) {
        // 从串口读取底层数据
        int n = uart_read_bytes(UART_PORT, in, sizeof(in), pdMS_TO_TICKS(200));
        if (n <= 0) continue;

        size_t copy_n = MIN((size_t)n, sizeof(buf) - buf_len);
        memcpy(&buf[buf_len], in, copy_n);
        buf_len += copy_n;

        // 核心拆包寻帧逻辑 (原有)
        while (buf_len >= CONTROL_FRAME_LEN) {
            size_t i = 0;
            while (i < buf_len && buf[i] != FRAME_HEADER) i++;

            if (i > 0) {
                memmove(buf, &buf[i], buf_len - i);
                buf_len -= i;
            }

            if (buf_len < CONTROL_FRAME_LEN) break;

            if (is_control_frame(buf, buf_len)) {
                ESP_LOGI(TAG, "ESP-NOW TX control: AA A1 A2");
                esp_err_t err = esp_now_send(dest_mac, buf, CONTROL_FRAME_LEN);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "esp_now_send(control) failed: error=%d", err);
                }

                memmove(buf, &buf[CONTROL_FRAME_LEN], buf_len - CONTROL_FRAME_LEN);
                buf_len -= CONTROL_FRAME_LEN;
                continue;
            }

            if (buf_len < REMOTE_FRAME_LEN) break;

            if (buf[0] != FRAME_HEADER || buf[REMOTE_FRAME_LEN - 1] != FRAME_FOOTER) {
                memmove(buf, &buf[1], buf_len - 1);
                buf_len -= 1;
                continue;
            }

            parse_remote_frame(buf, &frame);
            frame_cnt++;

            // 调试打印控制台
            ESP_LOGI(TAG,
                     "ESP-NOW TX msg: frame=%lu KEY=%02X%02X%02X%02X "
                     "L(X:%d, Y:%d) R(X:%d, Y:%d) dial=%d switch_left=%u switch_right=%u",
                     (unsigned long)frame_cnt,
                     frame.KEY[0], frame.KEY[1], frame.KEY[2], frame.KEY[3],
                     (int)frame.rocker_l_, (int)frame.rocker_l1,
                     (int)frame.rocker_r_, (int)frame.rocker_r1,
                     (int)frame.dial,
                     (unsigned int)frame.switch_left,
                     (unsigned int)frame.switch_right);

            // [关键修改] 使用 ESP-NOW 直接发生数据，取代 UDP
            esp_err_t err = esp_now_send(dest_mac, buf, REMOTE_FRAME_LEN);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "esp_now_send() failed: error=%d", err);
            } else {
                send_remote_ack();
            }

            // 处理完滑走本帧数据
            memmove(buf, &buf[REMOTE_FRAME_LEN], buf_len - REMOTE_FRAME_LEN);
            buf_len -= REMOTE_FRAME_LEN;
        }
    }
}

void app_main(void)
{
    // 必须要初始化 NVS，因为 WiFi 底层参数会用到
    ESP_ERROR_CHECK(nvs_flash_init());

    ESP_ERROR_CHECK(uart_init_remote());
    ESP_LOGI(TAG, "UART ready (TX=%d RX=%d baud=%d)", UART_TX_PIN, UART_RX_PIN, UART_BAUD_RATE);
    ESP_LOGI(TAG, "UART ACK mode enabled: reply 0x%02X after valid remote frames", REMOTE_ACK);
    
#if UART_ONLY_TEST
    uart_only_loop();
#else
    // 1. 初始化底层 WiFi，并强制锁定物理信道
    wifi_init_for_espnow();
    
    // 2. 初始化 ESP-NOW，注入目标接收器 MAC 地址
    espnow_init_app();

    // 3. 开始执行串口寻帧与 ESP-NOW 推送死循环
    forward_loop();
#endif
}
