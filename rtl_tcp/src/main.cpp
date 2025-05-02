/*
 * rtl-sdr, turns your Realtek RTL2832 based DVB dongle into a SDR receiver
 * Copyright (C) 2012 by Steve Markgraf <steve@steve-m.de>
 * Copyright (C) 2012-2013 by Hoernchen <la@tfc-server.de>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"


#include "driver/gpio.h"
#include "driver/uart.h"

#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>

//#include "esp32-hal-cpu.h"

#include <fcntl.h>
#include <pthread.h>

#include <libusb.h>
#include "rtl-sdr.h"
#include "convenience.h"

#define LED 15
#define TAG "main"
#define LOG_LOCAL_LEVEL ESP_LOG_VERBOSE
#include "esp_log.h"

#define UART_NUM UART_NUM_0       
#define UART_TX_PIN GPIO_NUM_17
#define UART_RX_PIN GPIO_NUM_18


#if __has_include("password.h")
  #include "password.h"
#endif

#ifndef WIFI_SSID
	#define WIFI_SSID "SSID"
#endif
#ifndef WIFI_PASSWORD
    #define WIFI_PASSWORD "12345"
#endif


const char* ssid = WIFI_SSID;
const char* password = WIFI_PASSWORD;

#define closesocket close
#define SOCKADDR struct sockaddr
#define SOCKET int
#define SOCKET_ERROR -1

#define DEFAULT_PORT_STR "1234"
#define DEFAULT_SAMPLE_RATE_HZ 240000
#define DEFAULT_MAX_NUM_BUFFERS 100

static SOCKET s;

static pthread_t tcp_worker_thread;
static pthread_t command_thread;
static pthread_cond_t exit_cond;
static pthread_mutex_t exit_cond_lock;

static pthread_mutex_t ll_mutex;
static pthread_cond_t cond;

struct llist {
	char *data;
	size_t len;
	struct llist *next;
};

typedef struct { /* structure size must be multiple of 2 bytes */
	char magic[4];
	uint32_t tuner_type;
	uint32_t tuner_gain_count;
} dongle_info_t;

static rtlsdr_dev_t *dev = NULL;

static int enable_biastee = 0;
static int global_numq = 0;
static struct llist *ll_buffers = 0;
static int llbuf_num = DEFAULT_MAX_NUM_BUFFERS;

static volatile int do_exit = 0;

void rtlsdr_callback(unsigned char *buf, uint32_t len, void *ctx)
{
	//digitalWrite(LED,!digitalRead(LED));
	if(!do_exit) {
		//struct llist *rpt = (struct llist*)malloc(sizeof(struct llist));
		//rpt->data = (char*)malloc(len);
		struct llist *rpt = (struct llist*) heap_caps_malloc(sizeof(struct llist),MALLOC_CAP_SPIRAM);
		rpt->data = (char*)heap_caps_malloc(len,MALLOC_CAP_SPIRAM);
		memcpy(rpt->data, buf, len);
		rpt->len = len;
		rpt->next = NULL;

		pthread_mutex_lock(&ll_mutex);

		if (ll_buffers == NULL) {
			ll_buffers = rpt;
		} else {
			struct llist *cur = ll_buffers;
			int num_queued = 0;

			while (cur->next != NULL) {
				cur = cur->next;
				num_queued++;
			}

			if(llbuf_num && llbuf_num == num_queued-2){
				struct llist *curelem;

				free(ll_buffers->data);
				curelem = ll_buffers->next;
				free(ll_buffers);
				ll_buffers = curelem;
			}

			cur->next = rpt;

			if (num_queued > global_numq)
				printf("ll+, now %d\n", num_queued);
			else if (num_queued < global_numq)
				printf("ll-, now %d\n", num_queued);

			global_numq = num_queued;
		}
		pthread_cond_signal(&cond);
		pthread_mutex_unlock(&ll_mutex);
	}
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base, 	int32_t event_id, void* event_data) {
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


static void *tcp_worker(void *arg)
{
	struct llist *curelem,*prev;
	int bytesleft,bytessent, index;
	struct timeval tv= {1,0};
	struct timespec ts;
	struct timeval tp;
	fd_set writefds;
	int r = 0;

	while(1) {
		if(do_exit)
			pthread_exit(0);

		pthread_mutex_lock(&ll_mutex);
		gettimeofday(&tp, NULL);
		ts.tv_sec  = tp.tv_sec+5;
		ts.tv_nsec = tp.tv_usec * 1000;
		r = pthread_cond_timedwait(&cond, &ll_mutex, &ts);
		if(r == ETIMEDOUT) {
			pthread_mutex_unlock(&ll_mutex);
			printf("worker cond timeout\n");
			pthread_exit(NULL);
		}

		curelem = ll_buffers;
		ll_buffers = 0;
		pthread_mutex_unlock(&ll_mutex);

		while(curelem != 0) {
			bytesleft = curelem->len;
			index = 0;
			bytessent = 0;
			while(bytesleft > 0) {
				FD_ZERO(&writefds);
				FD_SET(s, &writefds);
				tv.tv_sec = 1;
				tv.tv_usec = 0;
				r = select(s+1, NULL, &writefds, NULL, &tv);
				if(r) {
					bytessent = send(s,  &curelem->data[index], bytesleft, 0);
					bytesleft -= bytessent;
					index += bytessent;
				}
				if(bytessent == SOCKET_ERROR || do_exit) {
						printf("worker socket bye\n");

						pthread_exit(NULL);
				}
			}
			prev = curelem;
			curelem = curelem->next;
			free(prev->data);
			free(prev);
		}
	}
}

static int set_gain_by_index(rtlsdr_dev_t *_dev, unsigned int index)
{
	int res = 0;
	int* gains;
	int count = rtlsdr_get_tuner_gains(_dev, NULL);

	if (count > 0 && (unsigned int)count > index) {
		gains = (int*) malloc(count* sizeof(int));
		count = rtlsdr_get_tuner_gains(_dev, gains);

		res = rtlsdr_set_tuner_gain(_dev, gains[index]);

		free(gains);
	}

	return res;
}

struct command{
	unsigned char cmd;
	unsigned int param;
}__attribute__((packed));


static void *command_worker(void *arg)
{
	int left, received = 0;
	fd_set readfds;
	struct command cmd={0, 0};
	struct timeval tv= {1, 0};
	int r = 0;
	uint32_t tmp;

	while(1) {
		left=sizeof(cmd);
		while(left >0) {
			FD_ZERO(&readfds);
			FD_SET(s, &readfds);
			tv.tv_sec = 1;
			tv.tv_usec = 0;
			r = select(s+1, &readfds, NULL, NULL, &tv);
			if(r) {
				received = recv(s, (char*)&cmd+(sizeof(cmd)-left), left, 0);
				left -= received;
			}
			if(received == SOCKET_ERROR || do_exit) {
				printf("comm recv bye\n");
				pthread_exit(NULL);
			}
		}
		switch(cmd.cmd) {
		case 0x01:
			printf("set freq %d\n", ntohl(cmd.param));
			rtlsdr_set_center_freq(dev,ntohl(cmd.param));
			break;
		case 0x02:
			printf("set sample rate %d\n", ntohl(cmd.param));
			rtlsdr_set_sample_rate(dev, ntohl(cmd.param));
			break;
		case 0x03:
			printf("set gain mode %d\n", ntohl(cmd.param));
			rtlsdr_set_tuner_gain_mode(dev, ntohl(cmd.param));
			break;
		case 0x04:
			printf("set gain %d\n", ntohl(cmd.param));
			rtlsdr_set_tuner_gain(dev, ntohl(cmd.param));
			break;
		case 0x05:
			printf("set freq correction %d\n", ntohl(cmd.param));
			rtlsdr_set_freq_correction(dev, ntohl(cmd.param));
			break;
		case 0x06:
			tmp = ntohl(cmd.param);
			printf("set if stage %d gain %d\n", tmp >> 16, (short)(tmp & 0xffff));
			rtlsdr_set_tuner_if_gain(dev, tmp >> 16, (short)(tmp & 0xffff));
			break;
		case 0x07:
			printf("set test mode %d\n", ntohl(cmd.param));
			rtlsdr_set_testmode(dev, ntohl(cmd.param));
			break;
		case 0x08:
			printf("set agc mode %d\n", ntohl(cmd.param));
			rtlsdr_set_agc_mode(dev, ntohl(cmd.param));
			break;
		case 0x09:
			printf("set direct sampling %d\n", ntohl(cmd.param));
			rtlsdr_set_direct_sampling(dev, ntohl(cmd.param));
			break;
		case 0x0a:
			printf("set offset tuning %d\n", ntohl(cmd.param));
			rtlsdr_set_offset_tuning(dev, ntohl(cmd.param));
			break;
		case 0x0b:
			printf("set rtl xtal %d\n", ntohl(cmd.param));
			rtlsdr_set_xtal_freq(dev, ntohl(cmd.param), 0);
			break;
		case 0x0c:
			printf("set tuner xtal %d\n", ntohl(cmd.param));
			rtlsdr_set_xtal_freq(dev, 0, ntohl(cmd.param));
			break;
		case 0x0d:
			printf("set tuner gain by index %d\n", ntohl(cmd.param));
			set_gain_by_index(dev, ntohl(cmd.param));
			break;
		case 0x0e:
			printf("set bias tee %d\n", ntohl(cmd.param));
			rtlsdr_set_bias_tee(dev, (int)ntohl(cmd.param));
			break;
		default:
			break;
		}
		cmd.cmd = 0xff;
	}
}


/*void printMemoryInfo() {
	Serial0.println("Memory Information:");
	// common info
	Serial0.printf("Total heap: %d bytes\n", ESP.getHeapSize());
	Serial0.printf("Free heap: %d bytes\n", ESP.getFreeHeap());
	Serial0.printf("Minimum free heap: %d bytes\n", ESP.getMinFreeHeap());
	// PSRAM info
	Serial0.printf("Total PSRAM: %d bytes\n", ESP.getPsramSize());
	Serial0.printf("Free PSRAM: %d bytes\n", ESP.getFreePsram());
  }
*/  

static void print_memory_info() {
    ESP_LOGI(TAG, "Memory Information:");
    ESP_LOGI(TAG, "Total heap: %d bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "Minimum free heap: %d bytes", esp_get_minimum_free_heap_size());
    ESP_LOGI(TAG, "Free PSRAM: %d bytes", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
	ESP_LOGI(TAG, "Free SRAM: %d bytes", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}


//int main(int argc, char **argv)
extern "C"  void app_main() {
	int r, opt, i;
	char *addr = "127.0.0.1";
	const char *port = DEFAULT_PORT_STR;
	int netPort = 1234;
	uint32_t frequency = 100000000, samp_rate = DEFAULT_SAMPLE_RATE_HZ;
	struct sockaddr_storage local, remote;
	struct addrinfo *ai;
	struct addrinfo *aiHead;
	struct addrinfo  hints = { 0 };
	char hostinfo[256];  //NI_MAXHOST
	char portinfo[256]; //NI_MAXSERV
	char remhostinfo[256];  //NI_MAXHOST
	char remportinfo[256];  //NI_MAXSERV
	int aiErr;
	uint32_t buf_num = 10;
	int dev_index = 0;
	int dev_given = 0;
	int gain = 0;
	int ppm_error = 0;
	int direct_sampling = 0;
	struct llist *curelem,*prev;
	pthread_attr_t attr;
	void *status;
	struct timeval tv = {1,0};
	struct linger ling = {1,0};
	SOCKET listensocket = 0;
	socklen_t rlen;
	fd_set readfds;
	u_long blockmode = 1;
	dongle_info_t dongle_info;

	uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0);
	uart_set_pin(UART_NUM_0, UART_TX_PIN, UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
 
	printf("xtrsdr v1.0.0 / rtl_tcp_wifi start\n");  
	esp_log_level_set("*", ESP_LOG_DEBUG);
	 // Print memory info
	//uint32_t cpu_freq = getCpuFrequencyMhz(); 
	//printf("CPU clock: %u MHz\n", cpu_freq);
	 print_memory_info();

	usbhost_begin();
	vTaskDelay(3000 / portTICK_PERIOD_MS); 

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
  
  	// init Wifi station
    wifi_init_sta();
	print_memory_info();


	
	//dev_index = verbose_device_search(optarg);
	//dev_given = 1;
	frequency = 102400000;
	gain = 100;
	samp_rate = 240000;


	//if (!dev_given) {
	//	dev_index = verbose_device_search("0");
	//}

	//if (dev_index < 0) {
	//    while(1) delay(100);
	//	}
	int device_count, device, offset;
	char vendor[256], product[256], serial[256];  
	 device_count = rtlsdr_get_device_count();    
	if (!device_count) {
		fprintf(stderr, "No supported devices found.\n");
		while (1) { 
			vTaskDelay(1000 / portTICK_PERIOD_MS); 
		}
	}

  	fprintf(stderr, "Found %d device(s):\n", device_count);
	for (int i = 0; i < device_count; i++) {
		rtlsdr_get_device_usb_strings(i, vendor, product, serial);
		fprintf(stderr, "  %d:  %s, %s, SN: %s\n", i, vendor, product, serial);
	}


	dev_index = 0;

	rtlsdr_open(&dev, (uint32_t)dev_index);
	if (NULL == dev) {
	fprintf(stderr, "Failed to open rtlsdr device #%d.\n", dev_index);
		//exit(1);
		while (1) { 
			vTaskDelay(1000 / portTICK_PERIOD_MS); 
		}
	}
	/* Set direct sampling */
        if (direct_sampling)
                verbose_direct_sampling(dev, 2);

	/* Set the tuner error */
	verbose_ppm_set(dev, ppm_error);

	/* Set the sample rate */
	r = rtlsdr_set_sample_rate(dev, samp_rate);
	if (r < 0)
		fprintf(stderr, "WARNING: Failed to set sample rate.\n");

	/* Set the frequency */
	r = rtlsdr_set_center_freq(dev, frequency);
	if (r < 0)
		fprintf(stderr, "WARNING: Failed to set center freq.\n");
	else
		fprintf(stderr, "Tuned to %i Hz.\n", frequency);

	if (0 == gain) {
		 /* Enable automatic gain */
		r = rtlsdr_set_tuner_gain_mode(dev, 0);
		if (r < 0)
			fprintf(stderr, "WARNING: Failed to enable automatic gain.\n");
	} else {
		/* Enable manual gain */
		r = rtlsdr_set_tuner_gain_mode(dev, 1);
		if (r < 0)
			fprintf(stderr, "WARNING: Failed to enable manual gain.\n");

		/* Set the tuner gain */
		r = rtlsdr_set_tuner_gain(dev, gain);
		if (r < 0)
			fprintf(stderr, "WARNING: Failed to set tuner gain.\n");
		else
			fprintf(stderr, "Tuner gain set to %f dB.\n", gain/10.0);
	}

	rtlsdr_set_bias_tee(dev, enable_biastee);
	if (enable_biastee)
		fprintf(stderr, "activated bias-T on GPIO PIN 0\n");

	/* Reset endpoint before we start reading from it (mandatory) */
	r = rtlsdr_reset_buffer(dev);
	if (r < 0)
		fprintf(stderr, "WARNING: Failed to reset buffers.\n");

	pthread_mutex_init(&exit_cond_lock, NULL);
	pthread_mutex_init(&ll_mutex, NULL);
	pthread_mutex_init(&exit_cond_lock, NULL);
	pthread_cond_init(&cond, NULL);
	pthread_cond_init(&exit_cond, NULL);

//	hints.ai_flags  = AI_PASSIVE; /* Server mode. */
//	hints.ai_family = PF_UNSPEC;  /* IPv4 or IPv6. */
//	hints.ai_socktype = SOCK_STREAM;
//	hints.ai_protocol = IPPROTO_TCP;

//	if ((aiErr = getaddrinfo(addr,
//				 port,
//				 &hints,
//				 &aiHead )) != 0)
//	{
//		fprintf(stderr, "local address %s ERROR - %s.\n", addr, aiErr);
//		return;
//	}
//	memcpy(&local, aiHead->ai_addr, aiHead->ai_addrlen);

//	for (ai = aiHead; ai != NULL; ai = ai->ai_next) {
//		aiErr = getnameinfo((struct sockaddr *)ai->ai_addr, ai->ai_addrlen,
				    //hostinfo, NI_MAXHOST,
//				    portinfo, NI_MAXSERV, NI_NUMERICSERV );  //| NI_NUMERICHOST
//		if (aiErr)
//			fprintf( stderr, "getnameinfo ERROR - %s.\n",hostinfo);

//		listensocket = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
//		if (listensocket < 0)
//			continue;

//		r = 1;
	
//		setsockopt(listensocket, SOL_SOCKET, SO_REUSEADDR, (char *)&r, sizeof(int));
		//setsockopt(listensocket, SOL_SOCKET, SO_LINGER, (char *)&ling, sizeof(ling));

		//if (bind(listensocket, (struct sockaddr *)&local, aiHead->ai_addrlen))
//			fprintf(stderr, "rtl_tcp bind error: %s", strerror(errno));
//		else
//			break;
//	}

///
int client_socket;
int ip_protocol;
int socket_id;
int bind_err;
int listen_error;
char addr_str[128];

	struct sockaddr_in destAddr;
	destAddr.sin_addr.s_addr = htonl(INADDR_ANY); //Change hostname to network byte order
	destAddr.sin_family = AF_INET;		//Define address family as Ipv4
	destAddr.sin_port = htons(netPort); 	//Define PORT
	int addr_family = AF_INET;				//Define address family as Ipv4
	ip_protocol = IPPROTO_TCP;			//Define protocol as TCP
	inet_ntoa_r(destAddr.sin_addr, addr_str, sizeof(addr_str) - 1);

	/* Create TCP socket*/
	listensocket = socket(addr_family, SOCK_STREAM, ip_protocol);
	if (listensocket < 0)
	{
		ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
	}
	ESP_LOGI(TAG, "Socket created");

	/* Bind a socket to a specific IP + port */
	bind_err = bind(listensocket, (struct sockaddr *)&destAddr, sizeof(destAddr));
	if (bind_err != 0)
	{
		ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
	}
	ESP_LOGI(TAG, "Socket binded");

///	

	r = fcntl(listensocket, F_GETFL, 0);
	r = fcntl(listensocket, F_SETFL, r | O_NONBLOCK);

	while(1) {
		printf("listening...\n");
		listen(listensocket,1);

		while(1) {
			FD_ZERO(&readfds);
			FD_SET(listensocket, &readfds);
			tv.tv_sec = 1;
			tv.tv_usec = 0;
			r = select(listensocket+1, &readfds, NULL, NULL, &tv);
			if(do_exit) {
				goto out;
			} else if(r) {
				rlen = sizeof(remote);
				s = accept(listensocket,(struct sockaddr *)&remote, &rlen);
				break;
			}
		}

		setsockopt(s, SOL_SOCKET, SO_LINGER, (char *)&ling, sizeof(ling));

/*		getnameinfo((struct sockaddr *)&remote, rlen,
			    remhostinfo, NI_MAXHOST,
			    remportinfo, NI_MAXSERV, NI_NUMERICSERV);*/
		printf("client accepted!"); // %s %s\n", remhostinfo, remportinfo);

		memset(&dongle_info, 0, sizeof(dongle_info));
		memcpy(&dongle_info.magic, "RTL0", 4);

		r = rtlsdr_get_tuner_type(dev);
		if (r >= 0)
			dongle_info.tuner_type = htonl(r);

		r = rtlsdr_get_tuner_gains(dev, NULL);
		if (r >= 0)
			dongle_info.tuner_gain_count = htonl(r);

		r = send(s, (const char *)&dongle_info, sizeof(dongle_info), 0);
		if (sizeof(dongle_info) != r)
			printf("failed to send dongle information\n");

		pthread_attr_init(&attr);
		pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
		r = pthread_create(&tcp_worker_thread, &attr, tcp_worker, NULL);
		r = pthread_create(&command_thread, &attr, command_worker, NULL);
		pthread_attr_destroy(&attr);

		r = rtlsdr_read_async(dev, rtlsdr_callback, NULL, buf_num, 0);

		pthread_join(tcp_worker_thread, &status);
		pthread_join(command_thread, &status);

		closesocket(s);

		printf("all threads dead..\n");
		curelem = ll_buffers;
		ll_buffers = 0;

		while(curelem != 0) {
			prev = curelem;
			curelem = curelem->next;
			free(prev->data);
			free(prev);
		}

		do_exit = 0;
		global_numq = 0;
	}

out:
	rtlsdr_close(dev);
	closesocket(listensocket);
	closesocket(s);

	printf("bye!\n");
	while (1) { 
		vTaskDelay(1000 / portTICK_PERIOD_MS); 
	}
}
