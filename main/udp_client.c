#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/err.h"
#include "lwip/sys.h"

// =======================
// Configuración de red
// =======================
#define WIFI_SSID       "TU_SSID"
#define WIFI_PASSWORD   "TU_PASSWORD"
#define SERVER_IP       "192.168.0.207"   // IP de la Raspberry Pi
#define SERVER_PORT     1111

static const char *TAG = "UDP_CLIENT";

// =======================
// Declaración de funciones externas
// =======================
void wifi_init_sta(const char *ssid, const char *password);
esp_err_t nvs_init(void);

// =======================
// Funciones internas
// =======================
static void udp_client_task(void *pvParameters)
{
    char rx_buffer[128];
    char tx_buffer[128];
    struct sockaddr_in dest_addr;
    int sock;

    dest_addr.sin_addr.s_addr = inet_addr(SERVER_IP);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(SERVER_PORT);

    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Error creando socket UDP: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Socket UDP creado. Enviando datos al servidor %s:%d", SERVER_IP, SERVER_PORT);

    while (1) {
        snprintf(tx_buffer, sizeof(tx_buffer), "UDP: ESP32-S3 sigue activo");
        int err = sendto(sock, tx_buffer, strlen(tx_buffer), 0,
                         (struct sockaddr *)&dest_addr, sizeof(dest_addr));

        if (err < 0) {
            ESP_LOGE(TAG, "Error enviando mensaje UDP: errno %d", errno);
            break;
        }

        // Recibir respuesta
        struct sockaddr_in source_addr;
        socklen_t socklen = sizeof(source_addr);
        int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer) - 1, 0,
                           (struct sockaddr *)&source_addr, &socklen);

        if (len > 0) {
            rx_buffer[len] = 0; // Termina cadena
            ESP_LOGI(TAG, "Recibido: %s", rx_buffer);

            if (strcmp(rx_buffer, "MODE TCP") == 0) {
                ESP_LOGW(TAG, "Servidor solicitó cambiar a modo TCP");
                break;  // Finaliza la tarea UDP para que el main cambie a TCP
            }
        }

        vTaskDelay(pdMS_TO_TICKS(2000)); // Envía cada 2 s
    }

    if (sock != -1) {
        ESP_LOGI(TAG, "Cerrando socket UDP");
        shutdown(sock, 0);
        close(sock);
    }

    vTaskDelete(NULL);
}

// =======================
// Función principal UDP
// =======================
void udp_client_start(void)
{
    ESP_LOGI(TAG, "Inicializando NVS...");
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "Inicializando Wi-Fi en modo estación...");
    wifi_init_sta(WIFI_SSID, WIFI_PASSWORD);

    // Esperar un poco para conexión WiFi establecida
    vTaskDelay(pdMS_TO_TICKS(3000));

    ESP_LOGI(TAG, "Iniciando cliente UDP...");
    xTaskCreate(udp_client_task, "udp_client", 4096, NULL, 5, NULL);
}