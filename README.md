# ESP-NOW 遥控器数据发送工程

本工程运行在 ESP32-S3 上，用作遥控器数据发送端。ESP32-S3 通过 UART1 从外部遥控器或采集板接收 18 字节控制数据帧，完成串口缓存、寻帧和基础校验后，通过 ESP-NOW 协议将该帧发送给接收端 ESP32-S3，再由接收端继续转发或交给 H7 主控处理。

## 工程功能

当前工程实现的主要功能如下：

1. 初始化 NVS、UART1、WiFi STA 模式和 ESP-NOW。
2. 通过 UART1 周期性向底层遥控器硬件发送 `0x66` 心跳/拉取字节。
3. 从 UART1 RX 接收遥控器返回的原始数据流。
4. 在串口数据流中查找固定 18 字节遥控器数据帧。
5. 对数据帧做基础校验：长度为 18 字节，帧头为 `0xAA`，帧尾为 `0x55`。
6. 解析按键、左右摇杆、拨轮、左右开关字段，并通过日志打印关键控制量。
7. 将校验通过的完整 18 字节原始数据帧通过 ESP-NOW 发送给固定 MAC 的接收端。

## 数据流

```text
遥控器 / 采集板
    |
    | UART1, 115200 8N1
    v
ESP32-S3 发送端
    |
    | ESP-NOW, Channel 3
    v
ESP32-S3 接收端 / H7 通信模块
```

本工程本身不直接控制 H7，也不改变遥控器协议内容。它的角色是把 UART 收到的合法遥控器帧，通过 ESP-NOW 原样发送到无线接收端。

## 实际编译入口

当前工程实际编译的主程序为：

- `main/softap_example_main.c`

组件注册文件为：

- `main/CMakeLists.txt`

虽然文件名仍保留 `softap_example_main.c`，但当前代码已经不是普通 WiFi SoftAP 示例，而是 UART 接收 + ESP-NOW 转发程序。

## 遥控器数据帧格式

串口输入和 ESP-NOW 发送的有效载荷均为固定 18 字节：

| 偏移量 | 字段 | 长度 | 说明 |
| --- | --- | --- | --- |
| `0` | `header` | 1 字节 | 固定为 `0xAA` |
| `1~4` | `KEY[4]` | 4 字节 | 按键状态原始数据 |
| `5~6` | `rocker_l_` | 2 字节 | 左摇杆 X，数据为大端序 |
| `7~8` | `rocker_l1` | 2 字节 | 左摇杆 Y，数据为大端序 |
| `9~10` | `rocker_r_` | 2 字节 | 右摇杆 X，数据为大端序 |
| `11~12` | `rocker_r1` | 2 字节 | 右摇杆 Y，数据为大端序 |
| `13~14` | `dial` | 2 字节 | 拨轮数据，数据为大端序 |
| `15` | `switch_left` | 1 字节 | 左侧开关状态 |
| `16` | `switch_right` | 1 字节 | 右侧开关状态 |
| `17` | `footer` | 1 字节 | 固定为 `0x55` |

代码中的结构体定义：

```c
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
```

由于遥控器帧中的摇杆和拨轮字段采用大端序，代码中使用 `be16_to_i16()` 和 `parse_remote_frame()` 将这些字段解析成 ESP32 可直接打印的 `int16_t`。ESP-NOW 发送时仍发送原始 18 字节帧，不重新封包。

## 串口配置

ESP32-S3 通过 UART1 与外部遥控器/采集板连接：

| 配置项 | 当前值 |
| --- | --- |
| UART 外设 | `UART_NUM_1` |
| 波特率 | `115200` |
| 数据格式 | 8 数据位，无校验，1 停止位 |
| ESP32-S3 RX | GPIO4，接收遥控器数据 |
| ESP32-S3 TX | GPIO5，向遥控器发送心跳/拉取字节 |

串口初始化在 `uart_init_remote()` 中完成：

```c
#define UART_PORT      UART_NUM_1
#define UART_BAUD_RATE (115200)
#define UART_RX_PIN    (4)
#define UART_TX_PIN    (5)
```

程序启动后会创建 `uart_tx_task()`，每 100 ms 通过 UART1 发送一次 `0x66`：

```c
#define TX_DATA (0x66)
#define TX_INTERVAL_MS (100)
```

该字节用于保持或触发底层遥控器硬件输出数据。

## ESP-NOW 配置

当前无线链路配置如下：

| 配置项 | 当前值 |
| --- | --- |
| WiFi 模式 | `WIFI_MODE_STA` |
| ESP-NOW 信道 | Channel 3 |
| 加密 | 关闭 |
| 目标 MAC | `1a:2b:3c:4d:5e:6f` |

目标接收端 MAC 在 `main/softap_example_main.c` 中配置：

```c
static const uint8_t dest_mac[6] = {0x1A, 0x2B, 0x3C, 0x4D, 0x5E, 0x6F};
#define ESPNOW_CHANNEL (3)
```

发送端和接收端必须使用同一信道。接收端 MAC 如果发生变化，需要同步修改 `dest_mac`，否则 ESP-NOW 定向发送无法到达目标设备。

## 串口寻帧与转发逻辑

主转发逻辑在 `forward_loop()` 中：

1. 调用 `uart_read_bytes()` 从 UART1 读取数据流。
2. 将数据追加到本地缓存 `buf`。
3. 在缓存中查找 `0xAA` 帧头。
4. 等待缓存至少达到 18 字节。
5. 检查第 18 字节是否为 `0x55` 帧尾。
6. 调用 `parse_remote_frame()` 解析字段并打印调试日志。
7. 调用 `esp_now_send(dest_mac, buf, REMOTE_FRAME_LEN)` 发送原始 18 字节帧。
8. 从缓存中移除已处理帧，继续查找下一帧。

如果缓存中出现错位数据，程序会逐字节滑动，直到重新找到合法帧头和帧尾。

## 启动流程

`app_main()` 的主要流程为：

1. 初始化 NVS。
2. 初始化 UART1。
3. 创建 `uart_tx_task()`，周期性发送 `0x66`。
4. 初始化 WiFi STA，并锁定 Channel 3。
5. 初始化 ESP-NOW，并添加目标接收端 peer。
6. 进入 `forward_loop()`，持续执行串口接收和 ESP-NOW 发送。

如果将 `UART_ONLY_TEST` 改为 `1`，程序会只执行串口收帧测试，不初始化 ESP-NOW，也不会向接收端发送数据：

```c
#define UART_ONLY_TEST (0)
```

## 构建与下载

本工程是 ESP-IDF 工程，工程名为 `send`。常用命令：

```bash
idf.py build
idf.py -p PORT flash monitor
```

如果在 VS Code ESP-IDF 插件中使用，可以直接选择目标芯片和串口后执行 Build、Flash、Monitor。

## 正常日志示例

启动后可看到类似日志：

```text
UART ready (TX=5 RX=4 baud=115200)
UART TX task started, sending 0x66 every 100 ms
WiFi hardware init done. Channel locked to 3
ESP-NOW init done. Peer added.
ESP-NOW forward task loop started
```

收到合法遥控器帧后，会打印解析结果并发送 ESP-NOW：

```text
ESP-NOW TX msg: frame=1 KEY=00000000 L(X:0, Y:0) R(X:0, Y:0) switch_left=0 switch_right=0
```

## 关键源码位置

- `main/softap_example_main.c`：当前实际发送端主程序，包含 UART 接收、串口寻帧、ESP-NOW 初始化和发送逻辑。
- `main/CMakeLists.txt`：组件源文件和依赖注册。
- `PROTOCOL.md`：发送端与接收端通信协议和接收端开发建议。
- `DATA_PROTOCOL.md`：18 字节遥控器数据帧格式说明。

## 注意事项

1. UART 上游设备必须输出符合协议的 18 字节帧，否则不会触发 ESP-NOW 发送。
2. 帧头必须是 `0xAA`，帧尾必须是 `0x55`，当前没有额外 CRC 校验。
3. ESP-NOW 发送端和接收端必须锁定在 Channel 3。
4. `dest_mac` 必须填写接收端 ESP32-S3 的 STA MAC 地址。
5. 当前 ESP-NOW 未启用加密，适合调试和短链路控制验证。
6. 代码中 `uart_tx_task()` 会持续发送 `0x66`，如果后续遥控器硬件不需要拉取字节，需要同步调整该任务。
