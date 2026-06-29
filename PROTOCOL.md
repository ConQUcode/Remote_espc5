# ESP-NOW 遥控器与接收端开发指南及协议规范

本文档旨在为本项目的“接收端（如飞控、小车底盘等）”开发提供通信协议及网络配置的参考标准。

## 1. 系统架构概述

*   **发送端（本工程）**：ESP32-S3，作为遥控发卡器/透传模块。通过串口（UART）读取外部摇杆、按键的物理数据，打包后通过 ESP-NOW 协议发送。
*   **接收端（待开发）**：建议同样使用 ESP32 系列芯片（如 ESP32-S3）。接收 ESP-NOW 数据帧，并将其还原为控制指令用于驱动电机或飞控。
*   **物理链路**：2.4GHz Wi-Fi (ESP-NOW 协议，无连接，超低延迟)。

## 2. 射频链路配置 (ESP-NOW)

为了确保发送端与接收端能够成功建立通信，**接收端的代码必须满足以下条件**：

### 2.1 物理信道 (Channel) 必须一致
发送端默认将 WiFi 信道锁定在 **Channel 3**。
接收端在初始化 WiFi 时，也必须锁定在同一信道：
```c
// 接收端锁定信道的示例代码
esp_wifi_set_channel(3, WIFI_SECOND_CHAN_NONE);
```

### 2.2 MAC 地址配对
*   **接收端需知**：必须通过代码读取并记录接收端 ESP32 的出厂 MAC 地址（例如 `1A:2B:3C:4D:5E:6F`）。
*   **发送端修改**：将获取到的接收端 MAC 地址填入发送端代码 `softap_example_main.c` 中的 `dest_mac` 数组中，替换默认的 `0xFF` 广播地址。

---

## 3. 通信数据帧协议 (UART & RF Payload)

发送端与接收端之间传输的主要有效载荷（Payload）长度为 **18 字节**，格式严格对齐。当前发送端也会透传 3 字节控制帧 `AA A1 A2`，接收端需要按 payload 长度区分处理。

### 3.1 帧结构定义
每帧数据以单字节 `0xAA` 开头，以单字节 `0x55` 结尾。

| 偏移量(Byte) | 字段名          | 数据类型 | 说明 |
| :--- | :--- | :--- | :--- |
| 0 | `header` | `uint8_t` | 帧头，固定为 `0xAA` |
| 1~4 | `KEY[4]` | `uint8_t[4]` | 4字节的按键状态位势 |
| 5~6 | `rocker_l_` | `int16_t` | 左摇杆 X 轴 (大端序) |
| 7~8 | `rocker_l1` | `int16_t` | 左摇杆 Y 轴 (大端序) |
| 9~10 | `rocker_r_` | `int16_t` | 右摇杆 X 轴 (大端序) |
| 11~12 | `rocker_r1` | `int16_t` | 右摇杆 Y 轴 (大端序) |
| 13~14 | `dial` | `int16_t` | 拨轮数据 (大端序) |
| 15 | `switch_left` | `uint8_t` | 左侧拨动开关状态 |
| 16 | `switch_right`| `uint8_t` | 右侧拨动开关状态 |
| 17 | `footer` | `uint8_t` | 帧尾，固定为 `0x55` |

### 3.2 字节序转换注意
原生采集的 16位 (int16_t) 摇杆数据为**大端序 (Big-Endian)**。当在 ESP32 (小端序架构) 上解析时，接收端也需要进行转换才能得到正确的整型数值：
```c
// 接收端解析大端序的参考函数
static inline int16_t be16_to_i16(const uint8_t *p) {
    return (int16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}
```

### 3.3 控制帧

STM32 端可能通过同一 UART 链路输出 3 字节控制帧：

```text
AA A1 A2
```

本发送端会识别该控制帧，并通过 ESP-NOW 原样发送 3 字节 payload。该帧不是 18 字节遥控器主数据帧，接收端不能只用 `data_len == 18` 一个分支，否则会丢弃控制命令。

### 3.4 UART ACK 说明

STM32 遥控器端发送 18 字节主数据帧后会等待 1 字节 ACK。本发送端在收到合法主数据帧并成功提交 ESP-NOW 发送后，会通过 UART1 TX 回发：

```text
0x55
```

这个 ACK 只存在于 STM32 与本发送端之间，不会作为 ESP-NOW payload 发给接收端。

---

## 4. 接收端开发建议步骤

1.  **新建工程**：基于 ESP-IDF 创建一个空白工程。
2.  **初始化硬件**：初始化 NVS，初始化 WiFi 为 `WIFI_MODE_STA` 或 `WIFI_MODE_APSTA`，并**锁定信道为 3**。
3.  **初始化 ESP-NOW**：调用 `esp_now_init()`。
4.  **注册接收回调函数**：
    由于我们现在是 ESP-NOW 接收端，需要调用 `esp_now_register_recv_cb(recv_cb)`。
    ```c
    void recv_cb(const uint8_t *mac_addr, const uint8_t *data, int data_len) {
        if (data_len == 18 && data[0] == 0xAA && data[17] == 0x55) {
            // 校验通过，解析摇杆和按键数据...
        } else if (data_len == 3 &&
                   data[0] == 0xAA &&
                   data[1] == 0xA1 &&
                   data[2] == 0xA2) {
            // 控制/重连命令...
        }
    }
    ```
5.  **电机 / 飞控驱动**：根据解析出的摇杆坐标（如 PWM 占空比映射），控制后续的电机或舵机。
