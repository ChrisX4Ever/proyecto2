#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "bmi270.h"
#include "udp_client.h"

#define WIFI_SSID       "TU_SSID"
#define WIFI_PASSWORD   "TU_PASSWORD"
#define SERVER_IP       "192.168.0.207"  // IP de tu Raspberry Pi
#define SERVER_PORT     1111

static const char *TAG = "TCP_MAIN";

// Prototipo de funciones
void wifi_init_sta(const char *ssid, const char *password);
esp_err_t nvs_init(void);

static void tcp_client_task(void *pvParameters)
{
    char rx_buffer[128];
    char tx_buffer[256];
    struct sockaddr_in dest_addr;
    int sock;

    dest_addr.sin_addr.s_addr = inet_addr(SERVER_IP);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(SERVER_PORT);

    while (1)
    {
        // Crear socket TCP
        sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if (sock < 0) {
            ESP_LOGE(TAG, "Error creando socket TCP: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        ESP_LOGI(TAG, "Conectando con servidor TCP %s:%d ...", SERVER_IP, SERVER_PORT);
        if (connect(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) != 0) {
            ESP_LOGE(TAG, "Error de conexión: errno %d", errno);
            close(sock);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        ESP_LOGI(TAG, "Conectado al servidor TCP.");

        // Inicializa sensor BMI270
        bmi_sensor_init();

        while (1)
        {
            // Leer datos del acelerómetro
            bmi_data_t data = bmi_read_accel_gyro();

            snprintf(tx_buffer, sizeof(tx_buffer),
                     "ACC: %.3f,%.3f,%.3f | GYR: %.3f,%.3f,%.3f\n",
                     data.ax, data.ay, data.az,
                     data.gx, data.gy, data.gz);

            int err = send(sock, tx_buffer, strlen(tx_buffer), 0);
            if (err < 0) {
                ESP_LOGE(TAG, "Error enviando datos TCP: errno %d", errno);
                break;
            }

            // Escuchar comandos desde la Raspberry Pi
            int len = recv(sock, rx_buffer, sizeof(rx_buffer) - 1, MSG_DONTWAIT);
            if (len > 0) {
                rx_buffer[len] = 0;
                ESP_LOGI(TAG, "Comando recibido: %s", rx_buffer);

                if (strcmp(rx_buffer, "MODE UDP") == 0) {
                    ESP_LOGW(TAG, "Cambio solicitado a modo UDP.");

                    shutdown(sock, 0);
                    close(sock);

                    // Inicia cliente UDP
                    udp_client_start();
                    vTaskDelete(NULL);
                }
            }

            vTaskDelay(pdMS_TO_TICKS(100)); // ~10 Hz
        }

        if (sock != -1) {
            ESP_LOGI(TAG, "Cerrando socket TCP");
            shutdown(sock, 0);
            close(sock);
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void)
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

    // Esperar a conexión Wi-Fi
    vTaskDelay(pdMS_TO_TICKS(3000));

    // Iniciar el cliente TCP principal
    xTaskCreate(tcp_client_task, "tcp_client", 8192, NULL, 5, NULL);
}