#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "driver/gpio.h"
#include "driver/uart.h"

#define TAG "main"
#define LOG_LOCAL_LEVEL ESP_LOG_VERBOSE

// Configuration
#if __has_include("password.h")
  #include "password.h"
#endif

#ifndef WIFI_SSID
	#define WIFI_SSID "SSID"
#endif
#ifndef WIFI_PASSWORD
    #define WIFI_PASSWORD "12345"
#endif


#define TCP_PORT 1234

#define BUFFERN 10
#define BUFFERL 32000

// Global variables

#define UART_NUM UART_NUM_0       // Используем UART0
#define UART_TX_PIN GPIO_NUM_17
#define UART_RX_PIN GPIO_NUM_18
#define BUF_SIZE (1024)
#define RD_BUF_SIZE (BUF_SIZE)


const gpio_num_t LED_GPIO = GPIO_NUM_15;  // вместо #define LED_GPIO 15
// Для кнопки
const gpio_num_t BUTTON_GPIO = GPIO_NUM_0; // вместо #define BUTTON_GPIO 0

static int bufi = 0;
static int client_socket = -1;
static char *buffers[BUFFERN];
static SemaphoreHandle_t bufferMutex[BUFFERN];

static void wifi_event_handler(void* arg, esp_event_base_t event_base, 
                             int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
        ESP_LOGI(TAG, "Retrying to connect to the AP");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

static void wifi_init_sta() {
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));


wifi_config_t wifi_config = {
  .sta = {
      .ssid = WIFI_SSID,
      .password = WIFI_PASSWORD,
      .threshold = {
          .authmode = WIFI_AUTH_WPA2_PSK,
      },
      .pmf_cfg = {
          .capable = true,
          .required = false
      },
      .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
      .failure_retry_cnt = 3
  },
};
  
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(44));

    ESP_LOGI(TAG, "wifi_init_sta finished.");
}

static void print_memory_info() {
    ESP_LOGI(TAG, "Memory Information:");
    ESP_LOGI(TAG, "Total heap: %d bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "Minimum free heap: %d bytes", esp_get_minimum_free_heap_size());
    ESP_LOGI(TAG, "Free PSRAM: %d bytes", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

void init_uart() {
  uart_config_t uart_config = {
      .baud_rate = 230400,
      .data_bits = UART_DATA_8_BITS,
      .parity = UART_PARITY_DISABLE,
      .stop_bits = UART_STOP_BITS_1,
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
      .rx_flow_ctrl_thresh = 122,
      .source_clk = UART_SCLK_DEFAULT,
  };
  
  // Настройка UART
  ESP_ERROR_CHECK(uart_param_config(UART_NUM, &uart_config));
  
  // Установка пинов
  ESP_ERROR_CHECK(uart_set_pin(UART_NUM, UART_TX_PIN, UART_RX_PIN, 
                             UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
  
  // Установка буферов
  ESP_ERROR_CHECK(uart_driver_install(UART_NUM, BUF_SIZE, RD_BUF_SIZE, 
                                    0, NULL, 0));
}

static void send_data_task(void *pvParameters) {
    char addr_str[128];
    struct sockaddr_in destAddr = {
      .sin_family = AF_INET,
      .sin_port = htons(TCP_PORT),
      .sin_addr = {
          .s_addr = htonl(INADDR_ANY)
      },
      .sin_zero = {0}
  };
    ESP_LOGI(TAG, "send task started");
    while (1) {
        // Create TCP socket
        int socket_id = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socket_id < 0) {
            ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            continue;
        }

        // Bind socket
        int bind_err = bind(socket_id, (struct sockaddr *)&destAddr, sizeof(destAddr));
        if (bind_err != 0) {
            ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
            close(socket_id);
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            continue;
        }

        // Listen for connections
        if (listen(socket_id, 3) != 0) {
            ESP_LOGE(TAG, "Error during listen: errno %d", errno);
            close(socket_id);
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            continue;
        }
        ESP_LOGI(TAG, "Listening...");
        // Accept connection
        struct sockaddr_in sourceAddr;
        socklen_t addrLen = sizeof(sourceAddr);
        client_socket = accept(socket_id, (struct sockaddr *)&sourceAddr, &addrLen);
        if (client_socket < 0) {
            ESP_LOGE(TAG, "Unable to accept connection: errno %d", errno);
            close(socket_id);
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            continue;
        }

        ESP_LOGI(TAG, "Client connected!");
        // Set TCP_NODELAY
        int send_buf_size = 64 * 1024;  
        setsockopt(client_socket, SOL_SOCKET, SO_SNDBUF, &send_buf_size, sizeof(send_buf_size));
   
        int nodelay = 1;
        setsockopt(client_socket, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(int));

        // Communication loop
        while (1) {
            gpio_set_level(LED_GPIO, !gpio_get_level(LED_GPIO));
            
            //if (xSemaphoreTake(bufferMutex[bufi], portMAX_DELAY)) {
                int sent = send(client_socket, buffers[bufi], BUFFERL, 0);
            //    xSemaphoreGive(bufferMutex[bufi]);
                
                if (sent < 0) {
                    ESP_LOGE(TAG, "Send failed");
                    break;
                }
                
                bufi = (bufi + 1) % BUFFERN;
            //}
        }

        close(client_socket);
        close(socket_id);
        client_socket = -1;
        ESP_LOGI(TAG, "Client disconnected!");
    }

    vTaskDelete(NULL);
}


extern "C"  void app_main() {

    esp_log_level_set("*", ESP_LOG_INFO);  
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

   // Инициализация UART
   //init_uart();
   uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0);
   uart_set_pin(UART_NUM_0, UART_TX_PIN, UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

   // Теперь можно использовать printf/puts и т.д.
   printf("UART initialized!\n");
    // Print memory info
    print_memory_info();


    // Initialize GPIO
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io_conf);
    
    io_conf.pin_bit_mask = (1ULL << BUTTON_GPIO);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);

    // Initialize WiFi
    wifi_init_sta();
    print_memory_info();


    // Initialize buffers
    for (int i = 0; i < BUFFERN; i++) {
        //buffers[i] = (char*) heap_caps_malloc(BUFFERL, MALLOC_CAP_INTERNAL);
        buffers[i] = (char*) heap_caps_malloc(BUFFERL, MALLOC_CAP_SPIRAM);
        if (!buffers[i]) {
            ESP_LOGE(TAG, "Cannot alloc buffers");
            while(1) vTaskDelay(100 / portTICK_PERIOD_MS);
        }
        bufferMutex[i] = xSemaphoreCreateMutex();
    }

    print_memory_info();

    // Create sender task
    ESP_LOGI(TAG, "Starting send task");
    BaseType_t xReturned = xTaskCreate(send_data_task, "send_data_task", 4096, NULL, 3, NULL);
    if (xReturned !=pdPASS){
        ESP_LOGI(TAG, "send task not created");
    }

    // Main loop - button handling
    int previous_button_state = 1;
    int counter = 0;
    
    while (1) {
        int button_state = gpio_get_level(BUTTON_GPIO);
        if (button_state != previous_button_state && button_state == 0) {
            counter++;
            ESP_LOGI(TAG, "Button pressed %d times", counter);
        }
        previous_button_state = button_state;
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
}
