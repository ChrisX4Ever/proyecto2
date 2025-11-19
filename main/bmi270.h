#ifndef BMI270_H
#define BMI270_H

#include "esp_err.h"

// Estructura de datos combinada
typedef struct {
    float ax;  // acelerómetro X
    float ay;  // acelerómetro Y
    float az;  // acelerómetro Z
    float gx;  // giroscopio X
    float gy;  // giroscopio Y
    float gz;  // giroscopio Z
} bmi_data_t;

// Inicializa completamente el sensor BMI270
void bmi_sensor_init(void);

// Lectura de solo acelerómetro
bmi_data_t bmi_read_accel(void);

// Lectura combinada acelerómetro + giroscopio
bmi_data_t bmi_read_accel_gyro(void);

#endif // BMI270_H

