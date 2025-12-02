#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <stdlib.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "bmi270.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"

// --- CREDENCIALES WIFI Y RED (¡VERIFICAR!) ---
#define WIFI_SSID       "JVidal"        // <--- TU SSID
#define WIFI_PASS       "v1d1ls1lv1"    // <--- TU PASS
#define SERVER_IP       "192.168.4.183" // <--- IP DE LA RASPBERRY PI (Según tu último mensaje)
#define TCP_PORT        1111            // Puerto TCP
#define UDP_PORT        3333            // Puerto UDP (Escucha y Envío)

// --- Defines BMI ---
#define I2C_MASTER_SCL_IO            GPIO_NUM_47
#define I2C_MASTER_SDA_IO            GPIO_NUM_48
#define I2C_MASTER_FREQ_HZ           10000
#define ESP_SLAVE_ADDR               0x68

typedef enum {
    MODE_TCP = 0,
    MODE_UDP = 1
} connection_mode_t;

// estado global protegido
static connection_mode_t current_mode = MODE_TCP;
static int sockfd = -1;             // Socket para TCP
static int udp_sock = -1;           // Socket para UDP
static struct sockaddr_in udp_target_addr; // Dirección de destino (Raspberry)

static SemaphoreHandle_t sock_mutex;
static volatile bool tcp_connected = false;
static volatile bool sending = false; // true=transmitir muestras

// sampling default
static uint32_t Fodr = 100;

// Prototipos Externos (BMI)
extern esp_err_t bmi_read(uint8_t *data_address, uint8_t *data_rd, size_t size);
extern esp_err_t bmi_write(uint8_t *data_address, uint8_t *data_wr, size_t size);
extern esp_err_t bmi_init(void);
extern void bmi_sensor_init(void);

static const char *TAG1 = "BMI_TCP";
static const char *TAG2 = "BMI_UDP";

// Prototipos Locales
static esp_err_t wifi_init_sta(void);
static void connection_handler_task(void *arg);
static void data_sender_task(void *arg);
static esp_err_t send_all(const char *buf, size_t len);
static void handle_command(const char *cmd);

// -----------------------------------------------------------------------------
// Implementación WiFi
static void wifi_reconnect_task(void *arg)
{
    ESP_LOGI(TAG1, "Reconectando WiFi...");
    esp_wifi_connect();
    vTaskDelete(NULL);
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        xTaskCreate(wifi_reconnect_task, "wifi_reconnect_task", 4096, NULL, 5, NULL);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG1, "WiFi desconectado, creando tarea de reconexión...");
        xSemaphoreTake(sock_mutex, portMAX_DELAY);
        if (sockfd >= 0) close(sockfd);
        sockfd = -1;
        tcp_connected = false;
        sending = false;
        xSemaphoreGive(sock_mutex);
        xTaskCreate(wifi_reconnect_task, "wifi_reconnect_task", 4096, NULL, 5, NULL);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG1, "Obtuvo IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

static esp_err_t wifi_init_sta(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t wifi_config = { 0 };
    strncpy((char*)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid)-1);
    strncpy((char*)wifi_config.sta.password, WIFI_PASS, sizeof(wifi_config.sta.password)-1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    return ESP_OK;
}

// -----------------------------------------------------------------------------
// Tarea de Conexión y Manejo de Comandos (TCP y UDP Receiver)
static void connection_handler_task(void *arg)
{
    // 1. Inicializar socket UDP (una sola vez al inicio)
    udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (udp_sock < 0) {
        ESP_LOGE(TAG2, "No se pudo crear socket UDP: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    // 2. Configurar dirección de ESCUCHA (Bind) para recibir comandos UDP
    struct sockaddr_in listen_addr;
    memset(&listen_addr, 0, sizeof(listen_addr));
    listen_addr.sin_family = AF_INET;
    listen_addr.sin_addr.s_addr = htonl(INADDR_ANY); // Escuchar en todas las interfaces
    listen_addr.sin_port = htons(UDP_PORT);          // Puerto 3333

    if (bind(udp_sock, (struct sockaddr *)&listen_addr, sizeof(listen_addr)) < 0) {
        ESP_LOGE(TAG2, "Error en bind UDP (Puerto %d ocupado?): errno %d", UDP_PORT, errno);
    } else {
        ESP_LOGI(TAG2, "UDP Bind exitoso en puerto %d. Listo para recibir comandos.", UDP_PORT);
    }

    // 3. Configurar dirección de DESTINO inicial (Backup)
    memset(&udp_target_addr, 0, sizeof(udp_target_addr));
    udp_target_addr.sin_family = AF_INET;
    udp_target_addr.sin_port = htons(UDP_PORT);
    udp_target_addr.sin_addr.s_addr = inet_addr(SERVER_IP); // Usa la definida, pero se actualizará sola


    while (1) {
        // ======================= MODO TCP =======================
        if (current_mode == MODE_TCP) {
            struct sockaddr_in dest_addr;
            dest_addr.sin_addr.s_addr = inet_addr(SERVER_IP);
            dest_addr.sin_family = AF_INET;
            dest_addr.sin_port = htons(TCP_PORT);

            int s = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
            if (s < 0) {
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }

            ESP_LOGI(TAG1, "Conectando TCP a %s:%d...", SERVER_IP, TCP_PORT);
            if (connect(s, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) != 0) {
                ESP_LOGE(TAG1, "Fallo TCP. Reintentando en 1s...");
                close(s);
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }

            xSemaphoreTake(sock_mutex, portMAX_DELAY);
            sockfd = s;
            tcp_connected = true;
            xSemaphoreGive(sock_mutex);

            ESP_LOGI(TAG1, "TCP Conectado. Esperando comandos...");

            char rxbuf[128];
            // Bucle de recepción TCP
            while (current_mode == MODE_TCP) {
                struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
                setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

                ssize_t r = recv(s, rxbuf, sizeof(rxbuf) - 1, 0);
                if (r > 0) {
                    rxbuf[r] = 0;
                    ESP_LOGI(TAG1, "CMD TCP: %s", rxbuf);
                    char *line, *saveptr=NULL;
                    line = strtok_r(rxbuf, "\r\n", &saveptr);
                    while (line) {
                        handle_command(line);
                        line = strtok_r(NULL, "\r\n", &saveptr);
                    }
                } else if (r == 0) {
                    ESP_LOGW(TAG1, "Conexión TCP cerrada por el servidor.");
                    break; 
                } else if (errno != EWOULDBLOCK && errno != EAGAIN) {
                    ESP_LOGE(TAG1, "Error recv TCP: %d", errno);
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(10));
            }

            // Salida de TCP
            xSemaphoreTake(sock_mutex, portMAX_DELAY);
            if (sockfd >= 0) close(sockfd);
            sockfd = -1;
            tcp_connected = false;
            sending = false;
            xSemaphoreGive(sock_mutex);
        }
        
        // ======================= MODO UDP =======================
        else {
            // Solo imprimimos una vez al entrar
            static bool msg_printed = false;
            if (!msg_printed) {
                ESP_LOGI(TAG2, "Modo UDP activo. Esperando START en puerto %d...", UDP_PORT);
                msg_printed = true;
            }
            
            char rxbuf[128];
            struct sockaddr_in source_addr; // Para guardar quién nos envió el comando
            socklen_t socklen = sizeof(source_addr);

            // Bucle de recepción UDP
            while (current_mode == MODE_UDP) {
                int len = recvfrom(udp_sock, rxbuf, sizeof(rxbuf) - 1, 0, 
                                   (struct sockaddr *)&source_addr, &socklen);

                if (len > 0) {
                    rxbuf[len] = 0;
                    ESP_LOGI(TAG2, "CMD UDP Recibido: %s", rxbuf);

                    // --- [CORRECCIÓN CLAVE] ACTUALIZAR DESTINO DINÁMICAMENTE ---
                    // Copiamos la IP de quien nos envió el comando (Raspberry Pi)
                    // a la estructura que usa data_sender_task para enviar los datos.
                    udp_target_addr.sin_addr.s_addr = source_addr.sin_addr.s_addr;
                    udp_target_addr.sin_port = htons(UDP_PORT); // Aseguramos puerto 3333
                    
                    // Imprimimos confirmación de actualización de IP (Debug)
                    ESP_LOGI(TAG2, "Objetivo actualizado a la IP del emisor (Raspberry).");
                    // -----------------------------------------------------------
                    
                    char *line, *saveptr=NULL;
                    line = strtok_r(rxbuf, "\r\n", &saveptr);
                    while (line) {
                        handle_command(line);
                        line = strtok_r(NULL, "\r\n", &saveptr);
                    }
                }
                
                vTaskDelay(pdMS_TO_TICKS(10)); 
            }
            msg_printed = false; // Resetear flag para la próxima vez
        }
    }
}

// -----------------------------------------------------------------------------
// Enviar todo (Unificado)
static esp_err_t send_all(const char *buf, size_t len)
{
    if (current_mode == MODE_TCP) {
        if (!tcp_connected || sockfd < 0) return ESP_FAIL;
        xSemaphoreTake(sock_mutex, portMAX_DELAY);
        ssize_t r = send(sockfd, buf, len, 0);
        xSemaphoreGive(sock_mutex);
        return (r > 0 ? ESP_OK : ESP_FAIL);
    }
    else if (current_mode == MODE_UDP) {
        if (udp_sock < 0) return ESP_FAIL;
        ssize_t r = sendto(udp_sock, buf, len, 0,
                           (struct sockaddr*)&udp_target_addr,
                           sizeof(udp_target_addr));
        return (r > 0 ? ESP_OK : ESP_FAIL);
    }
    return ESP_FAIL;
}

// -----------------------------------------------------------------------------
// Manejo de Comandos
static void handle_command(const char *cmd)
{
    if (cmd == NULL) return;

    if (strncmp(cmd, "SRATE ", 6) == 0) {
        int val = atoi(cmd + 6);
        Fodr = val;
        send_all("SRATE_OK\n", strlen("SRATE_OK\n"));
    } else if (strcmp(cmd, "START") == 0) {
        // En UDP, aceptamos START si el socket está creado
        bool ready = (current_mode == MODE_TCP && tcp_connected) || (current_mode == MODE_UDP && udp_sock >= 0);
        if (ready) {
            sending = true;
            ESP_LOGI(TAG1, "--> START aceptado. Modo: %s", (current_mode==MODE_TCP)?"TCP":"UDP");
            send_all("STARTED\n", strlen("STARTED\n"));
        }
    } else if (strcmp(cmd, "STOP") == 0) {
        sending = false;
        ESP_LOGI(TAG1, "--> STOP aceptado.");
        send_all("STOPPED\n", strlen("STOPPED\n"));
    } else if (strcmp(cmd, "TCP") == 0) {
        if (current_mode != MODE_TCP) {
            ESP_LOGW(TAG1, "Switch a TCP");
            current_mode = MODE_TCP;
            sending = false;
            // connection_handler saldrá del loop UDP y reconectará TCP
        }
    } else if (strcmp(cmd, "UDP") == 0) {
        if (current_mode != MODE_UDP) {
            ESP_LOGW(TAG1, "Switch a UDP");
            current_mode = MODE_UDP;
            sending = false; // Esperar nuevo START
            send_all("SWITCHED_TO_UDP\n", strlen("SWITCHED_TO_UDP\n"));
            // connection_handler saldrá del loop TCP y entrará al UDP
        }
    }
}

// -----------------------------------------------------------------------------
// Tarea de Lectura y Envío de Datos (Consolidada)
void data_sender_task(void *pvParameter)
{
    // Variables de escala (declaradas fuera del loop)
    const float acc_scale_ms2 = 78.4532f / 32768.0f;
    const float acc_scale_g = 8.0f / 32768.0f;
    const float gyr_scale = 34.90659f / 32768.0f;
    
    while (1) {
        // 1. Verificar estado de envío
        bool can_send = false;
        if (current_mode == MODE_TCP) can_send = tcp_connected;
        else if (current_mode == MODE_UDP) can_send = (udp_sock >= 0);
        
        if (!sending || !can_send) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // 2. Leer datos BMI
        uint8_t reg_data = 0x0C;
        uint8_t data_data8[12];
        esp_err_t r = bmi_read(&reg_data, data_data8, 12);
        
        if (r == ESP_OK) {
            // 3. Procesar variables (Declaradas aquí para evitar error de compilación)
            int16_t acc_x = (int16_t)((data_data8[1] << 8) | data_data8[0]);
            int16_t acc_y = (int16_t)((data_data8[3] << 8) | data_data8[2]);
            int16_t acc_z = (int16_t)((data_data8[5] << 8) | data_data8[4]);
            int16_t gyr_x = (int16_t)((data_data8[7] << 8) | data_data8[6]);
            int16_t gyr_y = (int16_t)((data_data8[9] << 8) | data_data8[8]);
            int16_t gyr_z = (int16_t)((data_data8[11] << 8) | data_data8[10]);

            int64_t t_ms = esp_timer_get_time() / 1000;

            // 4. Formatear y Enviar
            char outbuf[256];
            // Sintaxis corregida: '%' antes de PRId64 y lista de argumentos limpia
            int n = snprintf(outbuf, sizeof(outbuf), 
                "{\"ts_ms\":%" PRId64 ",\"acc_m_s2\":[%.5f,%.5f,%.5f],\"acc_g\":[%.5f,%.5f,%.5f],\"gyr_rad_s\":[%.5f,%.5f,%.5f],\"fs_hz\":%u}\n",
                t_ms,
                acc_x * acc_scale_ms2, acc_y * acc_scale_ms2, acc_z * acc_scale_ms2,
                acc_x * acc_scale_g, acc_y * acc_scale_g, acc_z * acc_scale_g,
                gyr_x * gyr_scale, gyr_y * gyr_scale, gyr_z * gyr_scale,
                (unsigned)Fodr
            );

            if (n > 0) {
                if (send_all(outbuf, n) != ESP_OK) {
                    if (current_mode == MODE_TCP) {
                        // Forzar reconexión si falla TCP
                        xSemaphoreTake(sock_mutex, portMAX_DELAY);
                        if (sockfd >= 0) close(sockfd);
                        sockfd = -1;
                        tcp_connected = false;
                        xSemaphoreGive(sock_mutex);
                        sending = false;
                    }
                }
            }
        }
        
        // Control de frecuencia
        uint32_t wait_ms = (Fodr > 0) ? (1000 / Fodr) : 10;
        vTaskDelay(pdMS_TO_TICKS(wait_ms));
    }
}

// -----------------------------------------------------------------------------
void app_main(void)
{
    ESP_LOGI(TAG1, "Iniciando...");
    sock_mutex = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(wifi_init_sta());
    
    // Inicia I2C y Sensor
    bmi_sensor_init(); 

    current_mode = MODE_TCP;

    // Crear Tareas (Solo 2: conexión y envío)
    xTaskCreate(connection_handler_task, "conn_handler", 8 * 1024, NULL, 5, NULL);
    xTaskCreate(data_sender_task, "data_sender", 8 * 1024, NULL, 6, NULL);

    ESP_LOGI(TAG1, "Sistema iniciado.");
}