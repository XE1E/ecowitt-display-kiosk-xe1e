/**
 * config.h - Configuracion de hardware del display kiosco.
 *
 * Solo pines y constantes del hardware. Las credenciales (WiFi, URL del
 * servidor, etc.) van en my_config.h (copiar desde my_config.h.template).
 *
 * Hardware: Waveshare ESP32-S3-Touch-LCD-7B
 *   - LCD RGB 1024x600 16-bit
 *   - Touch GT911 (I2C)
 *   - Expansor CH422G (I2C) para reset/backlight
 *   - BME280 local (I2C)
 */

#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

// Version del firmware (se muestra en el bloque "Estado" del portal web).
#define FW_VERSION "1.2.3"

// ============================================================================
// I2C (bus compartido: CH422G + GT911 + BME280)
// IMPORTANTE: pines 8/9, NO 19/20.
// ============================================================================
#define I2C_SDA        8
#define I2C_SCL        9
#define I2C_FREQ       400000

// Touch GT911 (usa el bus I2C compartido; RST via CH422G, no GPIO)
#define TOUCH_INT      4

// BME280 (bus I2C compartido). Nombre propio para no chocar con el macro
// BME280_ADDRESS que define la libreria Adafruit.
#define BME280_I2C_ADDR 0x76
// Altitud del sitio en metros, para corregir la presion a nivel del mar.
// CDMX ~2240 m. Solo es el DEFAULT del primer arranque: la altitud real se edita
// desde el portal web y vive en NVS. En 0 se reporta la presion absoluta.
#define BME280_ALTITUDE 2240

// ============================================================================
// Resolucion del display (debe coincidir con rgb_lcd_port.h y el renderer)
// ============================================================================
#define SCREEN_WIDTH   1024
#define SCREEN_HEIGHT  600

// ============================================================================
// Páginas del kiosco
// ============================================================================
// 5 páginas numeradas (con barra de pestañas que dibuja el servidor) + la
// CONSOLA, que es full-screen SIN barra (id interno PAGE_CONSOLA, URL
// ?page=consola). La barra muestra NUM_TABS pestañas: las 5 páginas + "Consola".
#define NUM_TABS      6     // pestañas visibles en la barra (5 + Consola)
#define PAGE_CONSOLA  6     // id interno de la consola (full-screen)
#define MAX_PAGE_ID   6     // ids de página válidos: 1..MAX_PAGE_ID

// ============================================================================
// Estructura del sensor local (la usa bme280_sensor.h)
// ============================================================================
struct LocalSensorData {
    float temperature;
    float humidity;
    float pressure;
    bool  valid;
    unsigned long last_read;
};

#endif // CONFIG_H
