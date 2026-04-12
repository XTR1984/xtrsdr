#include <Arduino.h>
#include "esp32-hal-cpu.h"
#define TAG "main"
#define LOG_LOCAL_LEVEL ESP_LOG_VERBOSE
#include "esp_log.h"
#include "lwipopts.h"
//#include "sdkconfig.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>
#include <WiFi.h>

// https://docs.espressif.com/projects/esp-idf/en/v5.2.2/esp32s2/api-guides/wifi.html#how-to-improve-wi-fi-performance

#if __has_include("password.h")
  #include "password.h"
#endif
#ifndef WIFI_PASSWORD
    #define WIFI_PASSWORD "12345"
#endif

const char* ssid = "X";
const char* password = WIFI_PASSWORD;
const int tcpPort = 1234;

int bufi = 0;

#define BUFFERN 4
#define BUFFERL 32000

#define LED 15

SemaphoreHandle_t queueSemaphore; 
SemaphoreHandle_t bufferMutex[BUFFERN]; 

const int buttonPin = 0;         // input pin for pushbutton
int previousButtonState = HIGH;  // for checking the state of a pushButton
int counter = 0;                 // button push counter
int cb_counter=-1;
int do_exit = 0;
int bytes_to_read = 0;


int client_socket;
int ip_protocol;
int socket_id;
int bind_err;
int listen_error;

char * buffers[BUFFERN];

static void sendDataTask(void *pvParameters)
{
    char addr_str[128];		// char array to store client IP
    int bytes_received;		// immediate bytes received
    int addr_family;		// Ipv4 address protocol variable

    while (1)
    {
        struct sockaddr_in destAddr;
        destAddr.sin_addr.s_addr = htonl(INADDR_ANY); //Change hostname to network byte order
        destAddr.sin_family = AF_INET;		//Define address family as Ipv4
        destAddr.sin_port = htons(tcpPort); 	//Define PORT
        addr_family = AF_INET;				//Define address family as Ipv4
        ip_protocol = IPPROTO_TCP;			//Define protocol as TCP
        inet_ntoa_r(destAddr.sin_addr, addr_str, sizeof(addr_str) - 1);

        /* Create TCP socket*/
        socket_id = socket(addr_family, SOCK_STREAM, ip_protocol);
        if (socket_id < 0)
        {
            ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
            break;
        }
        ESP_LOGI(TAG, "Socket created");

        /* Bind a socket to a specific IP + port */
        bind_err = bind(socket_id, (struct sockaddr *)&destAddr, sizeof(destAddr));
        if (bind_err != 0)
        {
            ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
            break;
        }
        ESP_LOGI(TAG, "Socket binded");

        /* Begin listening for clients on socket */
        listen_error = listen(socket_id, 3);
        if (listen_error != 0)
        {
            ESP_LOGE(TAG, "Error occured during listen: errno %d", errno);
            break;
        }
        ESP_LOGI(TAG, "Socket listening");

        	struct sockaddr_in sourceAddr; // Large enough for IPv4
        	socklen_t addrLen = sizeof(sourceAddr);
        	/* Accept connection to incoming client */
        	client_socket = accept(socket_id, (struct sockaddr *)&sourceAddr, &addrLen);
          int nodelay = 1;

          int send_buf_size = 64 * 1024;  
          setsockopt(client_socket, SOL_SOCKET, SO_SNDBUF, &send_buf_size, sizeof(send_buf_size));
          setsockopt(client_socket, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(int));
        	if (client_socket < 0)
        	{
        		ESP_LOGE(TAG, "Unable to accept connection: errno %d", errno);
        		break;
        	}
        	ESP_LOGI(TAG, "Socket accepted");

        while (1) {
            digitalWrite(LED,!digitalRead(LED));
            ESP_LOGD(TAG, "Start send %d", bufi);
            int sent = send(client_socket,buffers[bufi] ,BUFFERL, 0);
            //int sent = send(client_socket,"hello" ,5, 0);
            if (sent < 0) {
               ESP_LOGE(TAG, "Send failed");
               break;
            }
                  
                
            ESP_LOGD(TAG, "End send %d", bufi);
            bufi++;
            if (bufi>=BUFFERN) bufi =0;

//            }    
            //delay(1);
        }

        close(client_socket);
        close(socket_id);
        ESP_LOGI(TAG, "Client disconnected");

    }
    vTaskDelete(NULL);

}

void printMemoryInfo() {
  Serial0.println("\nMemory Information:");
  // common info
  Serial0.printf("Total heap: %d bytes\n", ESP.getHeapSize());
  Serial0.printf("Free heap: %d bytes\n", ESP.getFreeHeap());
  Serial0.printf("Minimum free heap: %d bytes\n", ESP.getMinFreeHeap());
  // PSRAM info
  Serial0.printf("Total PSRAM: %d bytes\n", ESP.getPsramSize());
  Serial0.printf("Free PSRAM: %d bytes\n", ESP.getFreePsram());
}




void setup() {
  pinMode(LED, OUTPUT);
  Serial0.begin(230400,SERIAL_8N1,18,17);
  //while(!Serial0) delay(100);
  delay(10);
  uint32_t cpu_freq = getCpuFrequencyMhz();  // Получаем частоту CPU в Гц
  printf("CPU clock: %u MHz\n", cpu_freq);

  printMemoryInfo();

  fprintf(stderr, "tcpmeter start\n");  
  esp_log_level_set("*", ESP_LOG_DEBUG);
  
  #define WIFI_STATIC_TX_BUFFER_NUM 64
  // Инициализация Wi-Fi
  WiFi.useStaticBuffers(true);
  WiFi.begin(ssid, password);
  WiFi.setTxPower(WIFI_POWER_7dBm);
  
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial0.println("");
  Serial0.println("WiFi connected");
  Serial0.println("IP address: ");
  Serial0.println(WiFi.localIP());
  Serial.print("RSSI Wi-Fi: ");
  Serial.print(WiFi.RSSI());        // Уровень сигнала (RSSI)
  Serial.println(" dBm");

  
// init buffers
  ESP_LOGD(TAG,"init buffers");
  for (int i=0;i<BUFFERN;i++){
      //buffers[i] =  (char*)  heap_caps_malloc(BUFFERL, MALLOC_CAP_SPIRAM);
      buffers[i] =  (char*)  heap_caps_malloc(BUFFERL, MALLOC_CAP_INTERNAL);
      if (!buffers[i]){
         ESP_LOGE(TAG, "Cannot alloc buffers");
         while(1) delay(100);
      }
      
  }
  ESP_LOGD(TAG,"free PSram after alloc buffers %d",  ESP.getFreePsram());
  queueSemaphore = xSemaphoreCreateCounting(BUFFERN, 0); 
  for (int i =0;i<BUFFERN;i++)
    bufferMutex[i] = xSemaphoreCreateMutex();
  TaskHandle_t taskHandle = NULL;
  BaseType_t taskStatus;
  
  if(WiFi.isConnected()){
      BaseType_t taskStatus = xTaskCreate(
      sendDataTask,
      "sendertask",
      10000,
      NULL,
      3,
      &taskHandle
      );

      if (taskStatus != pdPASS) {
      ESP_LOGE("","Error creating senderTask!");
        while(1) delay(100);
      }
  }    
   else {
      ESP_LOGE("","Wifi not connected!");
  }



  digitalWrite(LED,HIGH);

  

}


void loop() {
  // read the pushbutton:
  int buttonState = digitalRead(buttonPin);
  // if the button state has changed,
  if ((buttonState != previousButtonState)
      // and it's currently pressed:
      && (buttonState == LOW)) {
    // increment the button counter
    counter++;
    // type out a message
  }
  // save the current button state for comparison next time:
  previousButtonState = buttonState;
 
  delay(100);
}
