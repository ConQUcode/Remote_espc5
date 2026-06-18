idf_component_register(
    SRCS "softap_example_main.c"  # 确保这是你 main 文件夹下的文件名
    INCLUDE_DIRS "."
    REQUIRES 
        esp_wifi        # 无线功能依赖
        esp_event       # 事件循环依赖
        nvs_flash       # 存储依赖（Wi-Fi 通常需要）
        driver          # 包含串口 (UART)、GPIO 等传统驱动
        esp_driver_uart # 建议在 v5.0+ / v6.0 中显式加上具体的串口驱动
)