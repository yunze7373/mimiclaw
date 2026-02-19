#include "discovery/ssdp_service.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "mimi_config.h"

static const char *TAG = "ssdp";

#define SSDP_PORT 1900
#define SSDP_MULTICAST_ADDR "239.255.255.250"

static const char *ssdp_response_template =
    "HTTP/1.1 200 OK\r\n"
    "CACHE-CONTROL: max-age=1800\r\n"
    "EXT:\r\n"
    "LOCATION: http://%s:%d/description.xml\r\n"
    "SERVER: ESP32/1.0 UPnP/1.0 MimiClaw/1.0\r\n"
    "ST: urn:schemas-upnp-org:device:Basic:1\r\n"
    "USN: uuid:mimiclaw-esp32-s3::urn:schemas-upnp-org:device:Basic:1\r\n"
    "\r\n";

static void ssdp_task(void *pvParameters)
{
    int sock = -1;
    struct sockaddr_in saddr = { 0 };
    struct ip_mreq imreq = { 0 };

    /* Create UDP socket */
    sock = socket(PF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Failed to create socket");
        vTaskDelete(NULL);
        return;
    }

    /* Bind to 1900 */
    saddr.sin_family = PF_INET;
    saddr.sin_port = htons(SSDP_PORT);
    saddr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, (struct sockaddr *)&saddr, sizeof(struct sockaddr_in)) < 0) {
        ESP_LOGE(TAG, "Failed to bind socket");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    /* Join multicast group */
    imreq.imr_multiaddr.s_addr = inet_addr(SSDP_MULTICAST_ADDR);
    imreq.imr_interface.s_addr = htonl(INADDR_ANY);
    if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &imreq, sizeof(struct ip_mreq)) < 0) {
        ESP_LOGE(TAG, "Failed to join multicast group");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "SSDP listener started on port 1900");

    char rx_buffer[512];
    char tx_buffer[512];

    while (1) {
        struct sockaddr_in source_addr;
        socklen_t socklen = sizeof(source_addr);
        int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer) - 1, 0, (struct sockaddr *)&source_addr, &socklen);

        if (len < 0) {
            ESP_LOGE(TAG, "recvfrom failed");
            break;
        }
        rx_buffer[len] = 0;

        /* Check for M-SEARCH */
        if (strstr(rx_buffer, "M-SEARCH") && strstr(rx_buffer, "ssdp:discover")) {
            /* Check if looking for rootdevice or basic device */
            if (strstr(rx_buffer, "upnp:rootdevice") || strstr(rx_buffer, "ssdp:all") || strstr(rx_buffer, "urn:schemas-upnp-org:device:Basic:1")) {
                
                /* Get local IP */
                tcpip_adapter_ip_info_t ip_info;
                tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_STA, &ip_info);
                char ip_str[16];
                snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));

                /* Construct response */
                snprintf(tx_buffer, sizeof(tx_buffer), ssdp_response_template, ip_str, 80);

                /* Send back unicast */
                sendto(sock, tx_buffer, strlen(tx_buffer), 0, (struct sockaddr *)&source_addr, sizeof(source_addr));
                ESP_LOGI(TAG, "Sent SSDP response to %s via unicast", inet_ntoa(source_addr.sin_addr));
            }
        }
    }

    close(sock);
    vTaskDelete(NULL);
}

esp_err_t ssdp_service_init(void)
{
    /* Nothing special to init here, just return ready */
    return ESP_OK;
}

esp_err_t ssdp_service_start(void)
{
    xTaskCreate(ssdp_task, "ssdp_task", 4096, NULL, 5, NULL);
    return ESP_OK;
}
