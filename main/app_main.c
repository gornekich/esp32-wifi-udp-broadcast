#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/_intsup.h>
#include "config.h"
#include "nerdy_wifi.h"
#include "nerdy_udp_client.h"
#include "nerdy_udp_server.h"
#include "nerdy_mac_address.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_wifi.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"


#define UPDATE_INTERVAL 5000
#define GPIO_PIN (GPIO_NUM_9)

static const char *TAG = "MAIN";

#define WEB_SERVER "example.com"
#define WEB_PORT "80"
#define WEB_PATH "/"

static const char *REQUEST = "GET " WEB_PATH " HTTP/1.0\r\n"
    "Host: "WEB_SERVER":"WEB_PORT"\r\n"
    "User-Agent: esp-idf/1.0 esp32\r\n"
    "\r\n";
    
#define WEB_SERVER_LOCAL "10.46.30.150"
#define WEB_PORT_LOCAL "8088"
#define WEB_PATH_LOCAL "/dnd?status=on&id=1"

static const char* REQUEST_LOCAL = "GET " WEB_PATH_LOCAL " HTTP/1.0\r\n"
    "Host: "WEB_SERVER_LOCAL":"WEB_PORT_LOCAL"\r\n"
    "User-Agent: esp-idf/1.0 esp32\r\n"
    "\r\n";

static QueueHandle_t gpio_evt_queue = NULL;

static void IRAM_ATTR gpio_isr_handler(void* arg)
{
    uint32_t gpio_num = (uint32_t) arg;
    xQueueSendFromISR(gpio_evt_queue, &gpio_num, NULL);
}

static void http_get(int id, bool state) {
    // const struct addrinfo hints = {
    //     .ai_family = AF_INET,
    //     .ai_socktype = SOCK_STREAM,
    // };
    // struct addrinfo *res;
    // struct in_addr *addr;
    int s, r;
    char recv_buf[64];

    // int err = getaddrinfo(WEB_SERVER, WEB_PORT, &hints, &res);

    // if(err != 0 || res == NULL) {
    //     ESP_LOGE(TAG, "DNS lookup failed err=%d res=%p", err, res);
    //     vTaskDelay(1000 / portTICK_PERIOD_MS);
    // }

    // /* Code to print the resolved IP.

    //     Note: inet_ntoa is non-reentrant, look at ipaddr_ntoa_r for "real" code */
    // addr = &((struct sockaddr_in *)res->ai_addr)->sin_addr;
    // ESP_LOGI(TAG, "DNS lookup succeeded. IP=%s", inet_ntoa(*addr));

    // s = socket(res->ai_family, res->ai_socktype, 0);
    // if(s < 0) {
    //     ESP_LOGE(TAG, "... Failed to allocate socket.");
    //     freeaddrinfo(res);
    //     vTaskDelay(1000 / portTICK_PERIOD_MS);
    // }
    // ESP_LOGI(TAG, "... allocated socket");

    // printf("res->ai_addr.sa_len = %d\nres->ai_addr.sa_family = %d\nres->ai_addr.sa_data = %s\nres->ai_addrlen = %ld\n",res->ai_addr->sa_len, res->ai_addr->sa_family, res->ai_addr->sa_data, res->ai_addrlen);
    // for(int i = 0; i < 14; i++) {
    //     printf("%x", res->ai_addr->sa_data[i]);
    // }
    // printf("\n");

    int ip_addr_size = udp_server_get_ip_addr_size();
    printf("Got %d addrs in the set\r\n", ip_addr_size);
    for(int i = 0; i < ip_addr_size; i++) {
        s = socket(AF_INET, SOCK_STREAM, 0);

        struct sockaddr_in dest_addr = {};
        dest_addr.sin_addr.s_addr = inet_addr(udp_server_get_ip_addr(i));
        dest_addr.sin_family = AF_INET;
        dest_addr.sin_port = htons(8088);
        printf("Connect to IP: %s\r\n", udp_server_get_ip_addr(i));

        if(connect(s, (const struct sockaddr *)&dest_addr, sizeof(struct sockaddr_in)) != 0) {
            ESP_LOGE(TAG, "... socket connect failed errno=%d", errno);
            close(s);
            // freeaddrinfo(res);
            vTaskDelay(4000 / portTICK_PERIOD_MS);
        }

        ESP_LOGI(TAG, "... connected");
        // freeaddrinfo(res);

        char request[128] = {};
        snprintf(request, sizeof(request), "GET  /dnd?status=%s&id=%d  HTTP/1.0\r\nHost: %s:8088\r\nUser-Agent: esp-idf/1.0 esp32\r\n""\r\n", state ? "on" : "off", id, udp_server_get_ip_addr(i));
        printf("Request: %s\r\n", request);

        if (write(s, request, strlen(request)) < 0) {
            ESP_LOGE(TAG, "... socket send failed");
            close(s);
            vTaskDelay(4000 / portTICK_PERIOD_MS);
        }
        ESP_LOGI(TAG, "... socket send success");

        struct timeval receiving_timeout;
        receiving_timeout.tv_sec = 5;
        receiving_timeout.tv_usec = 0;
        if (setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &receiving_timeout,
                sizeof(receiving_timeout)) < 0) {
            ESP_LOGE(TAG, "... failed to set socket receiving timeout");
            close(s);
            vTaskDelay(4000 / portTICK_PERIOD_MS);
        }
        ESP_LOGI(TAG, "... set socket receiving timeout success");

        /* Read HTTP response */
        do {
            bzero(recv_buf, sizeof(recv_buf));
            r = read(s, recv_buf, sizeof(recv_buf)-1);
            for(int i = 0; i < r; i++) {
                putchar(recv_buf[i]);
            }
        } while(r > 0);
        printf("\r\n");

        ESP_LOGI(TAG, "... done reading from socket. Last read return=%d errno=%d.", r, errno);
        close(s);
    }
}


static void gpio_task(void* arg) {
    int id = 0;
    bool state = false;
    while(true) {
    uint32_t io_num;
        if (xQueueReceive(gpio_evt_queue, &io_num, portMAX_DELAY)) {
            ESP_LOGI(TAG, "Button pressed");
            id++;
            state = !state;
            http_get(id, state);
        }
    }
}

static void gpio_init() {
    gpio_config_t io_conf = {};
    //interrupt of rising edge
    io_conf.intr_type = GPIO_INTR_NEGEDGE;
    //bit mask of the pins, use GPIO4/5 here
    io_conf.pin_bit_mask = 1 << GPIO_PIN;
    //set as input mode
    io_conf.mode = GPIO_MODE_INPUT;
    //enable pull-up mode
    io_conf.pull_up_en = 1;
    gpio_config(&io_conf);

    //change gpio interrupt type for one pin
    gpio_set_intr_type(GPIO_PIN, GPIO_INTR_NEGEDGE);

    //create a queue to handle gpio event from isr
    gpio_evt_queue = xQueueCreate(10, sizeof(uint32_t));
    //start gpio task
    xTaskCreate(gpio_task, "gpio_task_example", 2048, NULL, 10, NULL);

    //install gpio isr service
    gpio_install_isr_service(ESP_INTR_FLAG_LEVEL3);
    //hook isr handler for specific gpio pin
    gpio_isr_handler_add(GPIO_PIN, gpio_isr_handler, (void*) GPIO_PIN);
}

/**
 * UDP message sending immitation. The messages will be sent in an infinite loop with UPDATE_INTERVAL interval.
*/
void immitate_udp_message_sending() {
    int message_number = 1;
    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(UPDATE_INTERVAL));
        if (nerdy_wifi_ip_broadcast != NULL) 
        {
            char *message;
            // Build a message, increment message_number 
            asprintf(&message, "{\"mac_address\": \"%s\" , \"message_number\": \"%d\"}\n", nerdy_get_mac_address(), message_number++);
            // Send message
            nerdy_udp_client_send_message(nerdy_wifi_ip_broadcast, UDP_PORT_CLIENT, message);
            // Release message from memory
            free(message);
            // Log to the console
            ESP_LOGI(TAG, "Message is sent to %s %d", nerdy_wifi_ip_broadcast, UDP_PORT_CLIENT);
        }
    }
}

void app_main(void)
{
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    ESP_ERROR_CHECK(ret);

    // Connect ESP32 to a Wi-Fi network. 
    // Change Access Point Name and Password in the config. 
    // Read the README.md or nerdy_wifi/README.md for details!!!
    esp_log_level_set("wifi", ESP_LOG_ERROR);
    nerdy_wifi_connect();

    gpio_init();

    /**
     * We don't have sensors so we will send a mock message
    */
    // immitate_udp_message_sending();

    /** Start UDP server (listen to messages)
    * For testing use 
    * socat - UDP-DATAGRAM:255.255.255.255:32411,broadcast 
    * Don't forget to change 32411 to your port, if you change config.
    **/
    nerdy_udp_server_start(UDP_PORT_SERVER);
}
