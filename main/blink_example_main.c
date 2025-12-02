#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <stdlib.h>
#include <inttypes.h>
#include <math.h> 
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
#include "driver/i2c_master.h" // <--- USAMOS SOLO EL DRIVER NUEVO

// --- CONFIGURACIÓN DE RED ---
#define WIFI_SSID       "JVidal"
#define WIFI_PASS       "v1d1ls1lv1"
#define SERVER_IP       "192.168.4.183" 
#define TCP_PORT        1111
#define UDP_PORT        3333

// --- CONFIGURACIÓN I2C (COMPARTIDA) ---
#define I2C_MASTER_SCL_IO           GPIO_NUM_47
#define I2C_MASTER_SDA_IO           GPIO_NUM_48
#define I2C_MASTER_FREQ_HZ          100000 // 100kHz para estabilidad
#define I2C_PORT_NUM                I2C_NUM_0

// --- DIRECCIONES I2C ---
#define BME_ESP_SLAVE_ADDR          0x76

// --- DEFINES BME688 ---
#define CONCAT_BYTES(msb, lsb) (((uint16_t)msb << 8) | (uint16_t)lsb)

// Estados Globales
typedef enum { MODE_TCP = 0, MODE_UDP = 1 } connection_mode_t;
static connection_mode_t current_mode = MODE_TCP;
static int sockfd = -1;
static int udp_sock = -1;
static struct sockaddr_in udp_target_addr;
static SemaphoreHandle_t sock_mutex;
static volatile bool tcp_connected = false;
static volatile bool sending = false;
static uint32_t Fodr = 100;

// Tags
static const char *TAG1 = "SYSTEM";
static const char *TAG2 = "UDP_MODE";
static const char *TAG_BME = "BME688";

// --- MANEJADORES I2C NG ---
// Necesitamos estas variables globales para manejar el bus y el dispositivo BME
i2c_master_bus_handle_t bus_handle = NULL;
static i2c_master_dev_handle_t bme_dev_handle = NULL;

// Prototipos BMI
extern esp_err_t bmi_read(uint8_t *data_address, uint8_t *data_rd, size_t size);
extern esp_err_t bmi_write(uint8_t *data_address, uint8_t *data_wr, size_t size);
extern esp_err_t bmi_init(void);
extern void bmi_sensor_init(void);

// ============================================================================
// ======================= SECCIÓN BME688 (NG DRIVER) =========================
// ============================================================================

// Lectura usando Driver NG (Transmitir dirección -> Recibir datos)
esp_err_t bme_i2c_read(uint8_t *data_addres, uint8_t *data_rd, size_t size) {
    if (bme_dev_handle == NULL) return ESP_FAIL;
    // Transmitir registro (1 byte) y luego recibir 'size' bytes
    return i2c_master_transmit_receive(bme_dev_handle, data_addres, 1, data_rd, size, -1);
}

// Escritura usando Driver NG
esp_err_t bme_i2c_write(uint8_t *data_addres, uint8_t *data_wr, size_t size) {
    if (bme_dev_handle == NULL) return ESP_FAIL;
    
    // Concatenar registro + datos
    uint8_t *write_buf = (uint8_t *)malloc(size + 1);
    if (!write_buf) return ESP_ERR_NO_MEM;
    
    write_buf[0] = *data_addres;
    memcpy(&write_buf[1], data_wr, size);
    
    esp_err_t ret = i2c_master_transmit(bme_dev_handle, write_buf, size + 1, -1);
    free(write_buf);
    return ret;
}

// --- Helpers Matemáticos BME (Sin cambios lógicos) ---
uint8_t calc_gas_wait(uint16_t dur) {
    uint8_t factor = 0;
    uint8_t durval;
    if (dur >= 0xfc0) durval = 0xff;
    else {
        while (dur > 0x3F) { dur = dur >> 2; factor += 1; }
        durval = (uint8_t)(dur + (factor * 64));
    }
    return durval;
}

uint8_t calc_res_heat(uint16_t temp) {
    uint8_t heatr_res;
    uint8_t amb_temp = 25;
    uint8_t reg_par_g1 = 0xED, par_g1;
    bme_i2c_read(&reg_par_g1, &par_g1, 1);
    uint8_t reg_par_g2_lsb = 0xEB, par_g2_lsb;
    bme_i2c_read(&reg_par_g2_lsb, &par_g2_lsb, 1);
    uint8_t reg_par_g2_msb = 0xEC, par_g2_msb;
    bme_i2c_read(&reg_par_g2_msb, &par_g2_msb, 1);
    uint16_t par_g2 = (int16_t)(CONCAT_BYTES(par_g2_msb, par_g2_lsb));
    uint8_t reg_par_g3 = 0xEE, par_g3;
    bme_i2c_read(&reg_par_g3, &par_g3, 1);
    uint8_t reg_res_heat_range = 0x02, tmp_res_heat_range, res_heat_range;
    uint8_t mask_res_heat_range = (0x3 << 4);
    uint8_t reg_res_heat_val = 0x00, res_heat_val;
    int32_t var1, var2, var3, var4, var5, heatr_res_x100;

    if (temp > 400) temp = 400;
    bme_i2c_read(&reg_res_heat_range, &tmp_res_heat_range, 1);
    bme_i2c_read(&reg_res_heat_val, &res_heat_val, 1);
    res_heat_range = (mask_res_heat_range & tmp_res_heat_range) >> 4;
    var1 = (((int32_t)amb_temp * par_g3) / 1000) * 256;
    var2 = (par_g1 + 784) * (((((par_g2 + 154009) * temp * 5) / 100) + 3276800) / 10);
    var3 = var1 + (var2 / 2);
    var4 = (var3 / (res_heat_range + 4));
    var5 = (131 * res_heat_val) + 65536;
    heatr_res_x100 = (int32_t)(((var4 / var5) - 250) * 34);
    heatr_res = (uint8_t)((heatr_res_x100 + 50) / 100);
    return heatr_res;
}

void bme_forced_mode(void) {
    uint8_t ctrl_hum = 0x72, ctrl_meas = 0x74, gas_wait_0 = 0x64;
    uint8_t res_heat_0 = 0x5A, ctrl_gas_1 = 0x71;
    uint8_t mask, prev;

    uint8_t osrs_h = 0b001;
    mask = 0b00000111;
    bme_i2c_read(&ctrl_hum, &prev, 1);
    osrs_h = (prev & ~mask) | osrs_h;

    uint8_t osrs_t = 0b01000000;
    uint8_t osrs_p = 0b00010100;
    uint8_t osrs_t_p = osrs_t | osrs_p;

    uint8_t gas_duration = calc_gas_wait(100);
    uint8_t heater_step = calc_res_heat(300);
    uint8_t nb_conv = 0b00000000;
    uint8_t run_gas = 0b00100000;
    uint8_t gas_conf = nb_conv | run_gas;

    bme_i2c_write(&gas_wait_0, &gas_duration, 1);
    bme_i2c_write(&res_heat_0, &heater_step, 1);
    bme_i2c_write(&ctrl_hum, &osrs_h, 1);
    bme_i2c_write(&ctrl_meas, &osrs_t_p, 1);
    bme_i2c_write(&ctrl_gas_1, &gas_conf, 1);

    uint8_t mode = 0b00000001;
    uint8_t tmp_pow_mode, pow_mode = 0;
    esp_err_t ret;

    do {
        ret = bme_i2c_read(&ctrl_meas, &tmp_pow_mode, 1);
        if (ret == ESP_OK) {
            pow_mode = (tmp_pow_mode & 0x03);
            if (pow_mode != 0) {
                tmp_pow_mode &= ~0x03;
                bme_i2c_write(&ctrl_meas, &tmp_pow_mode, 1);
            }
        }
    } while ((pow_mode != 0x0) && (ret == ESP_OK));

    tmp_pow_mode = (tmp_pow_mode & ~0x03) | mode;
    bme_i2c_write(&ctrl_meas, &tmp_pow_mode, 1);
}

float bme_temp_celsius(uint32_t temp_adc) {
    uint8_t addr_par_t1_lsb = 0xE9, addr_par_t1_msb = 0xEA;
    uint8_t addr_par_t2_lsb = 0x8A, addr_par_t2_msb = 0x8B;
    uint8_t addr_par_t3_lsb = 0x8C;
    uint8_t par[5];
    bme_i2c_read(&addr_par_t1_lsb, par, 1);
    bme_i2c_read(&addr_par_t1_msb, par + 1, 1);
    bme_i2c_read(&addr_par_t2_lsb, par + 2, 1);
    bme_i2c_read(&addr_par_t2_msb, par + 3, 1);
    bme_i2c_read(&addr_par_t3_lsb, par + 4, 1);

    uint16_t par_t1 = (par[1] << 8) | par[0];
    uint16_t par_t2 = (par[3] << 8) | par[2];
    uint16_t par_t3 = par[4];

    int64_t var1, var2, var3;
    int t_fine, calc_temp;

    var1 = ((int32_t)temp_adc >> 3) - ((int32_t)par_t1 << 1);
    var2 = (var1 * (int32_t)par_t2) >> 11;
    var3 = ((var1 >> 1) * (var1 >> 1)) >> 12;
    var3 = ((var3) * ((int32_t)par_t3 << 4)) >> 14;
    t_fine = (int32_t)(var2 + var3);
    calc_temp = (((t_fine * 5) + 128) >> 8);
    return (float)calc_temp / 100.0f;
}

// ============================================================================
// ========================= FUNCIONES DE RED =================================
// ============================================================================

static esp_err_t wifi_init_sta(void);
static void connection_handler_task(void *arg);
static void data_sender_task(void *arg);
static esp_err_t send_all(const char *buf, size_t len);
static void handle_command(const char *cmd);

static void wifi_reconnect_task(void *arg) {
    ESP_LOGI(TAG1, "Reconectando WiFi...");
    esp_wifi_connect();
    vTaskDelete(NULL);
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        xTaskCreate(wifi_reconnect_task, "wifi_reconnect_task", 4096, NULL, 5, NULL);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
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

static esp_err_t wifi_init_sta(void) {
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

static void connection_handler_task(void *arg) {
    udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (udp_sock < 0) { vTaskDelete(NULL); return; }

    struct sockaddr_in listen_addr;
    memset(&listen_addr, 0, sizeof(listen_addr));
    listen_addr.sin_family = AF_INET;
    listen_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    listen_addr.sin_port = htons(UDP_PORT);
    bind(udp_sock, (struct sockaddr *)&listen_addr, sizeof(listen_addr));

    memset(&udp_target_addr, 0, sizeof(udp_target_addr));
    udp_target_addr.sin_family = AF_INET;
    udp_target_addr.sin_port = htons(UDP_PORT);
    udp_target_addr.sin_addr.s_addr = inet_addr(SERVER_IP);

    while (1) {
        if (current_mode == MODE_TCP) {
            struct sockaddr_in dest_addr;
            dest_addr.sin_addr.s_addr = inet_addr(SERVER_IP);
            dest_addr.sin_family = AF_INET;
            dest_addr.sin_port = htons(TCP_PORT);
            int s = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
            if (s < 0) { vTaskDelay(pdMS_TO_TICKS(2000)); continue; }
            if (connect(s, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) != 0) {
                close(s); vTaskDelay(pdMS_TO_TICKS(2000)); continue;
            }
            xSemaphoreTake(sock_mutex, portMAX_DELAY);
            sockfd = s; tcp_connected = true;
            xSemaphoreGive(sock_mutex);
            ESP_LOGI(TAG1, "TCP Conectado.");

            char rxbuf[128];
            while (current_mode == MODE_TCP) {
                struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
                setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                ssize_t r = recv(s, rxbuf, sizeof(rxbuf) - 1, 0);
                if (r > 0) {
                    rxbuf[r] = 0;
                    char *line, *saveptr=NULL;
                    line = strtok_r(rxbuf, "\r\n", &saveptr);
                    while (line) { handle_command(line); line = strtok_r(NULL, "\r\n", &saveptr); }
                } else if (r == 0) break;
                else if (errno != EWOULDBLOCK) break;
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            xSemaphoreTake(sock_mutex, portMAX_DELAY);
            if (sockfd >= 0) close(sockfd);
            sockfd = -1; tcp_connected = false; sending = false;
            xSemaphoreGive(sock_mutex);
        } else {
            static bool printed = false;
            if (!printed) { ESP_LOGI(TAG2, "Modo UDP Activo."); printed = true; }
            char rxbuf[128];
            struct sockaddr_in source_addr;
            socklen_t socklen = sizeof(source_addr);
            while (current_mode == MODE_UDP) {
                int len = recvfrom(udp_sock, rxbuf, sizeof(rxbuf) - 1, 0, (struct sockaddr *)&source_addr, &socklen);
                if (len > 0) {
                    rxbuf[len] = 0;
                    udp_target_addr.sin_addr.s_addr = source_addr.sin_addr.s_addr;
                    udp_target_addr.sin_port = htons(UDP_PORT);
                    char *line, *saveptr=NULL;
                    line = strtok_r(rxbuf, "\r\n", &saveptr);
                    while (line) { handle_command(line); line = strtok_r(NULL, "\r\n", &saveptr); }
                }
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            printed = false;
        }
    }
}

void data_sender_task(void *pvParameter)
{
    const float acc_scale_ms2 = 78.4532f / 32768.0f;
    const float acc_scale_g = 8.0f / 32768.0f;
    const float gyr_scale = 34.90659f / 32768.0f;
    
    // Configuración BME688
    uint8_t forced_temp_addr[] = {0x22, 0x23, 0x24};
    int bme_counter = 0;
    float last_temp = 0.0;

    // Inicializar BME en Forced Mode
    if (bme_dev_handle) bme_forced_mode();

    while (1) {
        bool can_send = false;
        if (current_mode == MODE_TCP) can_send = tcp_connected;
        else if (current_mode == MODE_UDP) can_send = (udp_sock >= 0);
        
        if (!sending || !can_send) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        uint8_t reg_data = 0x0C;
        uint8_t data_data8[12];
        esp_err_t r = bmi_read(&reg_data, data_data8, 12);
        
        bme_counter++;
        if (bme_counter >= 20 && bme_dev_handle) { 
            uint8_t tmp;
            uint32_t temp_adc = 0;
            bme_i2c_read(&forced_temp_addr[0], &tmp, 1); temp_adc |= tmp << 12;
            bme_i2c_read(&forced_temp_addr[1], &tmp, 1); temp_adc |= tmp << 4;
            bme_i2c_read(&forced_temp_addr[2], &tmp, 1); temp_adc |= (tmp & 0xf0) >> 4;
            last_temp = bme_temp_celsius(temp_adc); 
            bme_forced_mode(); 
            bme_counter = 0;
        }

        if (r == ESP_OK) {
            int16_t acc_x = (int16_t)((data_data8[1] << 8) | data_data8[0]);
            int16_t acc_y = (int16_t)((data_data8[3] << 8) | data_data8[2]);
            int16_t acc_z = (int16_t)((data_data8[5] << 8) | data_data8[4]);
            int16_t gyr_x = (int16_t)((data_data8[7] << 8) | data_data8[6]);
            int16_t gyr_y = (int16_t)((data_data8[9] << 8) | data_data8[8]);
            int16_t gyr_z = (int16_t)((data_data8[11] << 8) | data_data8[10]);

            int64_t t_ms = esp_timer_get_time() / 1000;
            char outbuf[300]; 

            int n = snprintf(outbuf, sizeof(outbuf), 
                "{\"ts_ms\":%" PRId64 ",\"acc_m_s2\":[%.5f,%.5f,%.5f],\"acc_g\":[%.5f,%.5f,%.5f],\"gyr_rad_s\":[%.5f,%.5f,%.5f],\"fs_hz\":%u,\"temp_c\":%.2f}\n",
                t_ms,
                acc_x * acc_scale_ms2, acc_y * acc_scale_ms2, acc_z * acc_scale_ms2,
                acc_x * acc_scale_g, acc_y * acc_scale_g, acc_z * acc_scale_g,
                gyr_x * gyr_scale, gyr_y * gyr_scale, gyr_z * gyr_scale,
                (unsigned)Fodr,
                last_temp
            );

            if (n > 0) {
                if (send_all(outbuf, n) != ESP_OK) {
                    if (current_mode == MODE_TCP) {
                        xSemaphoreTake(sock_mutex, portMAX_DELAY);
                        if (sockfd >= 0) close(sockfd);
                        sockfd = -1; tcp_connected = false;
                        xSemaphoreGive(sock_mutex); sending = false;
                    }
                }
            }
        }
        uint32_t wait_ms = (Fodr > 0) ? (1000 / Fodr) : 10;
        vTaskDelay(pdMS_TO_TICKS(wait_ms));
    }
}

static esp_err_t send_all(const char *buf, size_t len) {
    if (current_mode == MODE_TCP) {
        if (!tcp_connected || sockfd < 0) return ESP_FAIL;
        xSemaphoreTake(sock_mutex, portMAX_DELAY);
        ssize_t r = send(sockfd, buf, len, 0);
        xSemaphoreGive(sock_mutex);
        return (r > 0 ? ESP_OK : ESP_FAIL);
    } else if (current_mode == MODE_UDP) {
        if (udp_sock < 0) return ESP_FAIL;
        sendto(udp_sock, buf, len, 0, (struct sockaddr*)&udp_target_addr, sizeof(udp_target_addr));
        return ESP_OK;
    }
    return ESP_FAIL;
}

static void handle_command(const char *cmd) {
    if (cmd == NULL) return;
    if (strncmp(cmd, "SRATE ", 6) == 0) {
        Fodr = atoi(cmd + 6);
        send_all("SRATE_OK\n", 9);
    } else if (strcmp(cmd, "START") == 0) {
        sending = true;
        send_all("STARTED\n", 8);
    } else if (strcmp(cmd, "STOP") == 0) {
        sending = false;
        send_all("STOPPED\n", 8);
    } else if (strcmp(cmd, "TCP") == 0) {
        if (current_mode != MODE_TCP) { current_mode = MODE_TCP; sending = false; }
    } else if (strcmp(cmd, "UDP") == 0) {
        if (current_mode != MODE_UDP) { current_mode = MODE_UDP; sending = false; send_all("SWITCHED_TO_UDP\n", 16); }
    }
}

// Función para inicializar el Bus I2C NG MANUALMENTE
// Esto es necesario para añadir el BME688
static void i2c_bus_init(void) {
    i2c_master_bus_config_t i2c_mst_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_PORT_NUM,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    // Creamos el Bus Master
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_mst_config, &bus_handle));

    // Añadimos el dispositivo BME688
    i2c_device_config_t bme_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BME_ESP_SLAVE_ADDR,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus_handle, &bme_cfg, &bme_dev_handle));
    
    // Setup inicial del BME
    uint8_t reg_id = 0xd0, tmp;
    bme_i2c_read(&reg_id, &tmp, 1);
    ESP_LOGI(TAG_BME, "Init completo. ID BME: %02x", tmp);
    
    uint8_t reg_soft = 0xE0, val_soft = 0xB6;
    bme_i2c_write(&reg_soft, &val_soft, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
}

void app_main(void) {
    ESP_LOGI(TAG1, "Iniciando...");
    sock_mutex = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(wifi_init_sta());
    
    // 1. Inicializamos el Bus I2C NG y el BME688
    i2c_bus_init();

    // 2. Inicializamos el BMI270
    // OJO: Si bmi_sensor_init() falla aquí, es porque intenta crear el bus de nuevo.
    // Si eso pasa, comentamos la línea de abajo y confiamos en que el BMI se configure con el bus existente
    // (si la librería lo permite) o desactivamos la creación del bus dentro de bmi_sensor_init.
    bmi_sensor_init();  

    current_mode = MODE_TCP;
    xTaskCreate(connection_handler_task, "conn_handler", 8192, NULL, 5, NULL);
    xTaskCreate(data_sender_task, "data_sender", 8192, NULL, 6, NULL);
    ESP_LOGI(TAG1, "Sistema iniciado.");
}