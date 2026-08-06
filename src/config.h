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
// 1.4.0: navegacion por zonas (X-Kiosk-Nav). Cambia el modelo de pagina de numero a
// slug, asi que no es un parche: sube la minor.
// 1.4.1: el toque largo volvia a ser inalcanzable con el mapa de zonas; ahora la
// navegacion se resuelve al SOLTAR y el gesto funciona en toda la pantalla.
#define FW_VERSION "1.4.1"

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
// El firmware YA NO SABE que paginas existen: las nombra el servidor con slugs
// ("consola", "det-rain-7d", "stats-mes"...) y con cada imagen manda las zonas
// tocables en la cabecera X-Kiosk-Nav. Ver src/nav.h y, en el repo del servidor,
// docs/internal/PLAN-KIOSCO-NAVEGACION.md.
//
// Pagina con la que arranca el display. La consola es la cara principal de la
// estacion y ademas el INDICE: cada celda suya lleva al detalle de esa variable.
#define PAGE_HOME     "consola"

// Pestañas del RESPALDO. Solo se usa si una respuesta llega SIN mapa de zonas:
// entonces se vuelve al reparto por la X de la barra inferior, que es como se
// navegaba antes. Existe para no quedarse con un display muerto si un despliegue va
// a medias o si alguna pantalla no publica zonas; en marcha normal no interviene.
#define NUM_TABS      6

// Vuelta automatica a la home tras N minutos sin tocar la pantalla (0 = nunca).
// Un display de pared tiene que acabar siempre enseñando la consola: si alguien
// deja abierto un historico de hace tres meses, lo que se ve desde el sofa deja de
// ser el clima de ahora. Es el DEFAULT del primer arranque; el valor real vive en
// NVS y se edita desde el portal.
#define IDLE_HOME_MIN 5

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
