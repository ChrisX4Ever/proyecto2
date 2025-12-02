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

// --- tu código BMI / defines (copiados/adaptados) ---
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
static struct sockaddr_in udp_target_addr; // Dirección de destino para UDP

static SemaphoreHandle_t sock_mutex;
static volatile bool tcp_connected = false;
static volatile bool sending = false; // true=transmitir muestras, false=conectado pero sin transmitir

// sampling default (Fodr) (se usa para configurar BMI)
static uint32_t Fodr = 100; // valor inicial

// Prototipos (asumo tus funciones bmi_read/bmi_write/bmi_init tal como definiste)
extern esp_err_t bmi_read(uint8_t *data_address, uint8_t *data_rd, size_t size);
extern esp_err_t bmi_write(uint8_t *data_address, uint8_t *data_wr, size_t size);
extern esp_err_t bmi_init(void);
extern void bmi_sensor_init(void); // Asumo que esta llama a bmi_init() y setup

// --- Configuración WiFi / Server ---
//#define WIFI_SSID       "Sala de Estudios DIE"
#define WIFI_SSID       "JVidal"
//#define WIFI_SSID       "LAB.SISTEMAS DE COMUNICACIONES"
//#define WIFI_PASS       "SE.die2025"
//#define WIFI_PASS       "Comunicaciones"
#define WIFI_PASS       "v1d1ls1lv1"
//#define SERVER_IP       "192.168.50.81"   // IP de la Raspberry Pi (ajusta)
#define SERVER_IP       "192.168.4.183"   // IP de la Raspberry Pi (ajusta)
//#define SERVER_IP       "192.168.0.207"   // IP de la Raspberry Pi (ajusta)
#define TCP_PORT        1111              // Puerto TCP
#define UDP_PORT        3333              // Puerto UDP

static const char *TAG1 = "BMI_TCP";
static const char *TAG2 = "BMI_UDP";

// Prototipos (actualizados)
static esp_err_t wifi_init_sta(void);
static void connection_handler_task(void *arg); // Antes tcp_client_task
static void sensor_task(void *arg);
static esp_err_t send_all(const char *buf, size_t len);
static void handle_command(const char *cmd);


// -----------------------------------------------------------------------------
// Implementación WiFi (modo station básico)
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
        // Asegurar que el estado TCP se limpie
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

    ESP_LOGI(TAG1, "WiFi inicializado en modo STA");
    return ESP_OK;
}

// -----------------------------------------------------------------------------
// Tarea de Conexión y Manejo de Comandos (TCP y UDP Init)
static void connection_handler_task(void *arg)
{
    // Inicializar socket UDP desde el principio
    udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (udp_sock < 0) {
        ESP_LOGE(TAG2, "No se pudo crear socket UDP al iniciar: errno %d", errno);
        vTaskDelete(NULL); // Fallo crítico
        return;
    }
    memset(&udp_target_addr, 0, sizeof(udp_target_addr));
    udp_target_addr.sin_family = AF_INET;
    udp_target_addr.sin_port = htons(UDP_PORT);
    udp_target_addr.sin_addr.s_addr = inet_addr(SERVER_IP);
    ESP_LOGI(TAG2, "Socket UDP listo y apuntando a Raspberry: %s:%d", SERVER_IP, UDP_PORT);


    while (1) {
        if (current_mode == MODE_TCP) {
            // --- Lógica de Conexión y Recepción TCP ---
            struct sockaddr_in dest_addr;
            dest_addr.sin_addr.s_addr = inet_addr(SERVER_IP);
            dest_addr.sin_family = AF_INET;
            dest_addr.sin_port = htons(TCP_PORT);

            int s = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
            if (s < 0) {
                ESP_LOGE(TAG1, "Error al crear socket TCP: errno %d", errno);
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }

            ESP_LOGI(TAG1, "Intentando conectar TCP a %s:%d...", SERVER_IP, TCP_PORT);
            if (connect(s, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) != 0) {
                ESP_LOGE(TAG1, "Fallo al conectar TCP: errno %d", errno);
                close(s);
                vTaskDelay(pdMS_TO_TICKS(5000));
                continue;
            }

            // Conexión exitosa, actualizar globales
            xSemaphoreTake(sock_mutex, portMAX_DELAY);
            sockfd = s;
            tcp_connected = true;
            xSemaphoreGive(sock_mutex);

            ESP_LOGI(TAG1, "TCP conectado. Esperando comandos...");

            // --- Bucle de Recepción de Comandos TCP ---
            char rxbuf[128];
            while (current_mode == MODE_TCP) {
                // Timeout de 1 segundo para no bloquear indefinidamente
                struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
                setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

                ssize_t r = recv(s, rxbuf, sizeof(rxbuf) - 1, 0);

                if (r > 0) {
                    rxbuf[r] = '\0';
                    // --- LÍNEA DE DEBUG ---
                    ESP_LOGI(TAG1, "Dato recibido RAW: '%s'", rxbuf); 
                    // -----------------------------------
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
                    ESP_LOGE(TAG1, "Error en recv TCP: errno %d", errno);
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(10)); // Pequeño delay para ceder tiempo
            }

            // Cierre de socket y limpieza de estado
            ESP_LOGW(TAG1, "Saliendo de modo TCP…");
            xSemaphoreTake(sock_mutex, portMAX_DELAY);
            if (sockfd >= 0) close(sockfd);
            sockfd = -1;
            tcp_connected = false;
            sending = false; // Parar el envío de datos al perder conexión
            xSemaphoreGive(sock_mutex);
        } else {
            // Modo UDP: esta tarea solo espera
            ESP_LOGI(TAG2, "Modo UDP activo. Enviando datos (si 'sending' es true).");
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}

// -----------------------------------------------------------------------------
// Enviar todo (Unificado para TCP/UDP)
static esp_err_t send_all(const char *buf, size_t len)
{
    if (current_mode == MODE_TCP) {
        if (!tcp_connected) return ESP_FAIL;
        xSemaphoreTake(sock_mutex, portMAX_DELAY);
        if (sockfd < 0) {
            xSemaphoreGive(sock_mutex);
            return ESP_FAIL;
        }
        ssize_t r = send(sockfd, buf, len, 0);
        xSemaphoreGive(sock_mutex);
        return (r > 0 ? ESP_OK : ESP_FAIL);
    }

    else if (current_mode == MODE_UDP) {
        if (udp_sock < 0) return ESP_FAIL;
        // La dirección de destino ya está configurada en udp_target_addr
        ssize_t r = sendto(udp_sock, buf, len, 0,
                           (struct sockaddr*)&udp_target_addr,
                           sizeof(udp_target_addr));
        return (r > 0 ? ESP_OK : ESP_FAIL);
    }

    return ESP_FAIL;
}

// -----------------------------------------------------------------------------
// Manejo de comandos recibidos desde Raspberry Pi (Solo vía TCP)
static void handle_command(const char *cmd)
{
    if (cmd == NULL) return;

    if (strncmp(cmd, "SRATE ", 6) == 0) {
        int val = atoi(cmd + 6);
        if (val == 100 || val == 400 || val == 1600) {
            Fodr = val;
            ESP_LOGI(TAG1, "Frequencia muestreo cambiada a %d Hz", Fodr);
            // Aquí iría tu lógica para reconfigurar el registro acc_conf del BMI
            send_all("SRATE_OK\n", strlen("SRATE_OK\n"));
        } else {
            send_all("SRATE_ERR\n", strlen("SRATE_ERR\n"));
        }
    } else if (strcmp(cmd, "START") == 0) {
        // Solo inicia si el canal actual está activo
        bool can_start = (current_mode == MODE_TCP && tcp_connected) || (current_mode == MODE_UDP && udp_sock >= 0);
        if (can_start) {
            sending = true;
            ESP_LOGI(TAG1, "START recibido: comenzando transmisión de datos en modo %s", 
                     (current_mode == MODE_TCP ? "TCP" : "UDP"));
            send_all("STARTED\n", strlen("STARTED\n"));
        } else {
            send_all("START_ERR_NOT_READY\n", strlen("START_ERR_NOT_READY\n"));
        }
    } else if (strcmp(cmd, "STOP") == 0) {
        sending = false;
        ESP_LOGI(TAG1, "STOP recibido: deteniendo transmisión de datos");
        send_all("STOPPED\n", strlen("STOPPED\n"));
    } else if (strcmp(cmd, "PING") == 0) {
        send_all("PONG\n", strlen("PONG\n"));
    }
    else if (strcmp(cmd, "TCP") == 0) {
        if (current_mode != MODE_TCP) {
            ESP_LOGW(TAG1, "Cambiando a modo TCP...");
            current_mode = MODE_TCP;
            sending = false;  // detén transmisión hasta que TCP se conecte y reciba nuevo START
        }
        send_all("SWITCHED_TO_TCP\n", strlen("SWITCHED_TO_TCP\n"));
    }
    else if (strcmp(cmd, "UDP") == 0) {
        if (current_mode != MODE_UDP) {
            ESP_LOGW(TAG1, "Cambiando a modo UDP...");
            current_mode = MODE_UDP;
            sending = false;
            // No cerramos el socket TCP aquí, lo hace connection_handler_task al romper el loop recv
        }
        send_all("SWITCHED_TO_UDP\n", strlen("SWITCHED_TO_UDP\n"));
    } else {
    ESP_LOGI(TAG1, "Comando desconocido: %s", cmd);
    send_all("UNK_CMD\n", strlen("UNK_CMD\n"));
    }
}

// -----------------------------------------------------------------------------
// Tarea de envío de datos (data_sender_task) - COMPLETO Y CORREGIDO
// -----------------------------------------------------------------------------
void data_sender_task(void *pvParameter)
{
    // Variables de escala (declaradas una sola vez fuera del loop)
    const float acc_scale_ms2 = 78.4532f / 32768.0f; // m/s2 per bit
    const float acc_scale_g = 8.0f / 32768.0f;       // g per bit
    const float gyr_scale = 34.90659f / 32768.0f;    // rad/s per bit
    
    while (1) {
        
        // 1. Verificar si hay un canal activo y si se debe enviar
        bool can_send = false; // <--- Declaración de can_send
        if (current_mode == MODE_TCP) {
            can_send = tcp_connected;
        } else if (current_mode == MODE_UDP) {
            can_send = (udp_sock >= 0);
        }
        
        if (!sending || !can_send) {
            // No estamos enviando o no hay canal disponible, esperar
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        // 2. Leer datos del BMI
        uint8_t reg_data = 0x0C; // Dirección de registro de datos crudos
        uint8_t data_data8[12];
        esp_err_t r = bmi_read(&reg_data, data_data8, 12);
        if (r != ESP_OK) {
            ESP_LOGW(TAG1, "Error leyendo BMI: %s", esp_err_to_name(r));
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // 3. Procesar datos
        int16_t acc_x = (int16_t)((data_data8[1] << 8) | data_data8[0]);
        int16_t acc_y = (int16_t)((data_data8[3] << 8) | data_data8[2]);
        int16_t acc_z = (int16_t)((data_data8[5] << 8) | data_data8[4]);
        int16_t gyr_x = (int16_t)((data_data8[7] << 8) | data_data8[6]);
        int16_t gyr_y = (int16_t)((data_data8[9] << 8) | data_data8[8]);
        int16_t gyr_z = (int16_t)((data_data8[11] << 8) | data_data8[10]);

        // timestamp ms
        int64_t t_us = esp_timer_get_time(); // microsegundos
        int64_t t_ms = t_us / 1000;          // <--- Declaración de t_ms

        // 4. Generar JSON y enviar
        char outbuf[256]; // <--- Declaración de outbuf
        
        // snprintf corregido (uso de PRId64, formato de flotantes y enteros)
        int n = snprintf(outbuf, sizeof(outbuf), 
            "{\"ts_ms\":%" PRId64 ",\"acc_m_s2\":[%.5f,%.5f,%.5f],\"acc_g\":[%.5f,%.5f,%.5f],\"gyr_rad_s\":[%.5f,%.5f,%.5f],\"fs_hz\":%u}\n",
            t_ms,
            acc_x * acc_scale_ms2, acc_y * acc_scale_ms2, acc_z * acc_scale_ms2,
            acc_x * acc_scale_g, acc_y * acc_scale_g, acc_z * acc_scale_g,
            gyr_x * gyr_scale, gyr_y * gyr_scale, gyr_z * gyr_scale,
            (unsigned)Fodr
        );

        if (n > 0 && send_all(outbuf, n) != ESP_OK) {
            ESP_LOGW(TAG1, "Falló envío de muestra en modo %s.",
                     (current_mode == MODE_TCP ? "TCP" : "UDP"));
            
            // Si falla el envío TCP, forzar la desconexión para reintento
            if (current_mode == MODE_TCP) {
                xSemaphoreTake(sock_mutex, portMAX_DELAY);
                if (sockfd >= 0) close(sockfd);
                sockfd = -1;
                tcp_connected = false;
                xSemaphoreGive(sock_mutex);
                sending = false; // detener hasta reconexión y nuevo START
            }
        } else {
            ESP_LOGI(TAG1, "Muestra enviada ts=%" PRId64 " via %s", t_ms, 
                     (current_mode == MODE_TCP ? "TCP" : "UDP"));
        }

        // 5. Esperar intervalo según Fodr
        if (Fodr > 0) {
            uint32_t wait_ms = 1000 / Fodr;
            if (wait_ms == 0) wait_ms = 1; // mínimo 1ms
            vTaskDelay(pdMS_TO_TICKS(wait_ms));
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

// -----------------------------------------------------------------------------
// Tarea de sensor: lee, arma JSON y envía
static void sensor_task(void *arg)
{
    while (1) {
        
        // 1. Verificar si hay un canal activo y si se debe enviar
        bool can_send = false;
        if (current_mode == MODE_TCP) {
            can_send = tcp_connected;
        } else if (current_mode == MODE_UDP) {
            can_send = (udp_sock >= 0);
        }
        
        if (!sending || !can_send) {
            // No estamos enviando o no hay canal disponible, esperar
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        // 2. Leer datos del BMI
        uint8_t reg_data = 0x0C; // Dirección de registro de datos crudos
        uint8_t data_data8[12];
        esp_err_t r = bmi_read(&reg_data, data_data8, 12);
        if (r != ESP_OK) {
            ESP_LOGW(TAG1, "Error leyendo BMI: %s", esp_err_to_name(r));
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // 3. Procesar datos
        int16_t acc_x = (int16_t)((data_data8[1] << 8) | data_data8[0]);
        int16_t acc_y = (int16_t)((data_data8[3] << 8) | data_data8[2]);
        int16_t acc_z = (int16_t)((data_data8[5] << 8) | data_data8[4]);
        int16_t gyr_x = (int16_t)((data_data8[7] << 8) | data_data8[6]);
        int16_t gyr_y = (int16_t)((data_data8[9] << 8) | data_data8[8]);
        int16_t gyr_z = (int16_t)((data_data8[11] << 8) | data_data8[10]);

        // Escalas (las mismas que usabas)
        float acc_scale_ms2 = 78.4532f / 32768.0f; // m/s2 per bit
        float acc_scale_g = 8.0f / 32768.0f;       // g per bit (si tu cfg era +/-8g)
        float gyr_scale = 34.90659f / 32768.0f;    // rad/s per bit

        // timestamp ms
        int64_t t_us = esp_timer_get_time(); // microsegundos
        int64_t t_ms = t_us / 1000;

        // 4. Generar JSON y enviar
        char outbuf[256];
        int n = snprintf(outbuf, sizeof(outbuf), 
            // CORRECCIÓN: El '%' debe ir ANTES de la macro PRId64 para asegurar la sintaxis
            // y corregir el desajuste de argumentos.
            "{\"ts_ms\":%" PRId64 ",\"acc_m_s2\":[%.5f,%.5f,%.5f],\"acc_g\":[%.5f,%.5f,%.5f],\"gyr_rad_s\":[%.5f,%.5f,%.5f],\"fs_hz\":%u}\n",
            // ----------------------------------------------------------------------------------------------------------------------------------
            
            // ARGUMENTO 1 (para PRId64)
            t_ms, 
            
            // ARGUMENTO 2 (para el primer %.5f)
            acc_x * acc_scale_ms2, 
            
            // El resto de los argumentos siguen en orden
            acc_y * acc_scale_ms2, acc_z * acc_scale_ms2,
            acc_x * acc_scale_g, acc_y * acc_scale_g, acc_z * acc_scale_g,
            gyr_x * gyr_scale, gyr_y * gyr_scale, gyr_z * gyr_scale,
            (unsigned)Fodr // ARGUMENTO 10 (para %u)
        );
        
        if (n > 0 && send_all(outbuf, n) != ESP_OK) {
            ESP_LOGW(TAG1, "Falló envío de muestra en modo %s.",
                     (current_mode == MODE_TCP ? "TCP" : "UDP"));
            
            // Si falla el envío TCP, forzar la desconexión para reintento
            if (current_mode == MODE_TCP) {
                xSemaphoreTake(sock_mutex, portMAX_DELAY);
                if (sockfd >= 0) close(sockfd);
                sockfd = -1;
                tcp_connected = false;
                xSemaphoreGive(sock_mutex);
                sending = false; // detener hasta reconexión y nuevo START
            }
        } else {
            ESP_LOGI(TAG1, "Muestra enviada ts=%" PRId64 " via %s", t_ms, 
                     (current_mode == MODE_TCP ? "TCP" : "UDP"));
        }

        // 5. Esperar intervalo según Fodr
        if (Fodr > 0) {
            uint32_t wait_ms = 1000 / Fodr;
            if (wait_ms == 0) wait_ms = 1; // mínimo 1ms
            vTaskDelay(pdMS_TO_TICKS(wait_ms));
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

// -----------------------------------------------------------------------------
// app_main: inicializa WiFi, BMI, crea tareas
void app_main(void)
{
    ESP_LOGI(TAG1, "Iniciando...");

    // Mutex para proteger sockfd y tcp_connected
    sock_mutex = xSemaphoreCreateMutex();

    // Inicializar WiFi
    ESP_ERROR_CHECK(wifi_init_sta());

    // Inicializar BMI
    bmi_sensor_init(); // Asumo tu función de inicialización del BMI270

    current_mode = MODE_TCP;

    // Crear tareas
    xTaskCreate(connection_handler_task, "conn_handler_task", 8 * 1024, NULL, 5, NULL);
    xTaskCreate(sensor_task, "sensor_task", 8 * 1024, NULL, 6, NULL);
    xTaskCreate(data_sender_task, "data_sender", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG1, "Tareas creadas.");
}