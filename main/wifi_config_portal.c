#include "wifi_config_portal.h"

#include <stdbool.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "web_server.h"

#define PORTAL_AP_SSID "PERRO_GUARDIAN_WIFI"
#define PORTAL_AP_PASSWORD "12345678"
#define PORTAL_AP_CHANNEL 6
#define PORTAL_AP_MAX_CONNECTIONS 4
#define PORTAL_NVS_NAMESPACE "portal"
#define PORTAL_NVS_FORCE_KEY "force_once"
#define PORTAL_DNS_PORT 53
#define PORTAL_DNS_TASK_STACK 3072
#define PORTAL_DNS_TASK_PRIORITY 3
#define PORTAL_AP_IP "192.168.4.1"
#define PORTAL_DHCPS_OFFER_DNS 0x02

static const char *TAG = "wifi_config_portal";
static TaskHandle_t s_dns_task;

static void captive_dns_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "No se pudo crear socket DNS");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(PORTAL_DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "No se pudo iniciar DNS cautivo");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "DNS cautivo activo en puerto %d", PORTAL_DNS_PORT);

    uint8_t query[256];
    uint8_t response[320];
    const uint32_t ap_ip = inet_addr(PORTAL_AP_IP);

    while (true) {
        struct sockaddr_in client_addr = {0};
        socklen_t client_len = sizeof(client_addr);
        int len = recvfrom(sock, query, sizeof(query), 0, (struct sockaddr *)&client_addr, &client_len);
        if (len < 12) {
            continue;
        }

        int q_end = 12;
        while (q_end < len && query[q_end] != 0) {
            q_end += query[q_end] + 1;
        }
        if (q_end + 5 > len || q_end >= (int)sizeof(response) - 16) {
            continue;
        }

        memcpy(response, query, q_end + 5);
        response[2] = 0x81; /* response, recursion desired/available */
        response[3] = 0x80;
        response[4] = 0x00; response[5] = 0x01; /* one question */
        response[6] = 0x00; response[7] = 0x01; /* one answer */
        response[8] = 0x00; response[9] = 0x00;
        response[10] = 0x00; response[11] = 0x00;

        int off = q_end + 5;
        response[off++] = 0xC0; response[off++] = 0x0C; /* name pointer */
        response[off++] = 0x00; response[off++] = 0x01; /* A */
        response[off++] = 0x00; response[off++] = 0x01; /* IN */
        response[off++] = 0x00; response[off++] = 0x00;
        response[off++] = 0x00; response[off++] = 0x3C; /* TTL 60s */
        response[off++] = 0x00; response[off++] = 0x04;
        memcpy(&response[off], &ap_ip, 4);
        off += 4;

        sendto(sock, response, off, 0, (struct sockaddr *)&client_addr, client_len);
    }
}

static esp_err_t start_captive_dns(void)
{
    if (s_dns_task != NULL) {
        return ESP_OK;
    }

    BaseType_t ok = xTaskCreate(
        captive_dns_task,
        "captive_dns",
        PORTAL_DNS_TASK_STACK,
        NULL,
        PORTAL_DNS_TASK_PRIORITY,
        &s_dns_task);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t wifi_config_portal_request_on_next_boot(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(PORTAL_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_u8(handle, PORTAL_NVS_FORCE_KEY, 1);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

bool wifi_config_portal_requested_on_next_boot(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(PORTAL_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return false;
    }

    uint8_t force_once = 0;
    err = nvs_get_u8(handle, PORTAL_NVS_FORCE_KEY, &force_once);
    if (err == ESP_OK && force_once == 1) {
        ESP_LOGW(TAG, "Portal WiFi solicitado desde NVS");
        nvs_erase_key(handle, PORTAL_NVS_FORCE_KEY);
        nvs_commit(handle);
        nvs_close(handle);
        return true;
    }

    nvs_close(handle);
    return false;
}

esp_err_t wifi_config_portal_run(void)
{
    ESP_LOGW(TAG, "Iniciando portal de configuracion WiFi");

    esp_wifi_stop();
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    if (ap_netif != NULL) {
        esp_netif_ip_info_t ip_info = {0};
        esp_netif_str_to_ip4(PORTAL_AP_IP, &ip_info.ip);
        esp_netif_str_to_ip4(PORTAL_AP_IP, &ip_info.gw);
        esp_netif_str_to_ip4("255.255.255.0", &ip_info.netmask);
        esp_netif_dhcps_stop(ap_netif);
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_set_ip_info(ap_netif, &ip_info));
        esp_netif_dns_info_t dns_info = {
            .ip = {
                .type = ESP_IPADDR_TYPE_V4,
                .u_addr = {
                    .ip4 = ip_info.ip,
                },
            },
        };
        uint8_t offer_dns = PORTAL_DHCPS_OFFER_DNS;
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_option(
            ap_netif,
            ESP_NETIF_OP_SET,
            ESP_NETIF_DOMAIN_NAME_SERVER,
            &offer_dns,
            sizeof(offer_dns)));
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_set_dns_info(ap_netif, ESP_NETIF_DNS_MAIN, &dns_info));
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_start(ap_netif));
    }

    wifi_config_t ap_config = {0};
    strlcpy((char *)ap_config.ap.ssid, PORTAL_AP_SSID, sizeof(ap_config.ap.ssid));
    strlcpy((char *)ap_config.ap.password, PORTAL_AP_PASSWORD, sizeof(ap_config.ap.password));
    ap_config.ap.ssid_len = strlen(PORTAL_AP_SSID);
    ap_config.ap.channel = PORTAL_AP_CHANNEL;
    ap_config.ap.max_connection = PORTAL_AP_MAX_CONNECTIONS;
    ap_config.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;

    /*
     * APSTA mantiene el portal visible como SoftAP y habilita la interfaz STA
     * para poder escanear redes cercanas desde la pagina web.
     */
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "esp_wifi_set_mode APSTA fallo");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_config), TAG, "esp_wifi_set_config AP fallo");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start AP fallo");

    ESP_RETURN_ON_ERROR(web_server_start(), TAG, "web_server_start fallo");
    ESP_RETURN_ON_ERROR(start_captive_dns(), TAG, "start_captive_dns fallo");

    ESP_LOGW(TAG, "Conectate a WiFi '%s' con password '%s'", PORTAL_AP_SSID, PORTAL_AP_PASSWORD);
    ESP_LOGW(TAG, "El celular deberia abrir el navegador automaticamente. URL manual: http://%s", PORTAL_AP_IP);

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}
