idf_component_register(SRCS "main.c"
    REQUIRES bt esp_wifi nvs_flash driver esp_lcd fatfs spiffs protobuf-c mqtt json
             app_trace app_update esp_https_ota esp_http_client esp_http_server
             esp-tls esp_event esp_netif esp_timer esp_ringbuf esp_serial_slave_link
             ulp vfs espcoredump efuse perfmon openssl coap mdns wpa_supplicant
             wear_levelling freemodbus esp_adc_cal esp_hid tcp_transport
             esp_websocket_client libsodium asio unity esp_eth wifi_provisioning
             esp_local_ctrl sdmmc)
