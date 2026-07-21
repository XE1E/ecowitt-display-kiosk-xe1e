/**
 * touch_input.h - Driver minimo del GT911 sobre Wire + deteccion de "tap".
 *
 * En la Waveshare ESP32-S3-Touch-LCD-7B el reset del GT911 va por el expansor
 * CH422G (IO1), no por un GPIO directo; y el pin INT selecciona la direccion
 * I2C al soltar el reset (INT bajo -> 0x5D). Por eso el init replica esa
 * secuencia usando io_extension (CH422G) + el pin INT.
 *
 * Sin LVGL ni esp_lcd_touch (que necesitaria el driver I2C nativo de IDF):
 * aqui todo va por Wire, igual que el BME280. Solo detectamos "un tap" (un
 * dedo, con debounce) para cambiar de pagina.
 */

#ifndef TOUCH_INPUT_H
#define TOUCH_INPUT_H

#include <Arduino.h>
#include <Wire.h>
#include "config.h"
#include "io_extension.h"

// GT911
#define GT911_ADDR        0x5D    // direccion por defecto (INT bajo al resetear)
#define GT911_REG_STATUS  0x814E  // estado + nº de puntos
#define GT911_REG_POINT1  0x8150  // primer punto (8 bytes)

static bool     _touch_was_down  = false;
static uint32_t _touch_last_tap  = 0;
static uint32_t _touch_last_seen = 0;   // ultimo millis con un dedo presente
static const uint32_t TOUCH_DEBOUNCE_MS = 150;
static const uint32_t TOUCH_RELEASE_MS  = 80;   // sin toque este tiempo => dedo arriba

// Lee `len` bytes desde el registro de 16 bits `reg` del GT911.
static bool _gt911_read(uint16_t reg, uint8_t *buf, size_t len)
{
    Wire.beginTransmission(GT911_ADDR);
    Wire.write((uint8_t)(reg >> 8));
    Wire.write((uint8_t)(reg & 0xFF));
    if (Wire.endTransmission(false) != 0) return false;
    size_t got = Wire.requestFrom((uint8_t)GT911_ADDR, (uint8_t)len);
    for (size_t i = 0; i < len && Wire.available(); i++) buf[i] = Wire.read();
    return got == len;
}

// Escribe un byte en el registro de 16 bits `reg`.
static void _gt911_write(uint16_t reg, uint8_t val)
{
    Wire.beginTransmission(GT911_ADDR);
    Wire.write((uint8_t)(reg >> 8));
    Wire.write((uint8_t)(reg & 0xFF));
    Wire.write(val);
    Wire.endTransmission();
}

// Lectura cruda del registro de estado del GT911 (debug): 0xFE si falla el I2C.
inline uint8_t touch_input_raw_status()
{
    uint8_t s = 0xFF;
    if (!_gt911_read(GT911_REG_STATUS, &s, 1)) return 0xFE;
    return s;
}

inline void touch_input_begin()
{
    IO_EXTENSION_Init();                       // CH422G (reset touch, backlight)

    // Secuencia de reset del GT911: INT bajo mientras se suelta el reset -> 0x5D.
    pinMode(TOUCH_INT, OUTPUT);
    IO_EXTENSION_Output(IO_EXTENSION_IO_1, 0);  // RST bajo (via CH422G)
    delay(100);
    digitalWrite(TOUCH_INT, LOW);               // INT bajo
    delay(100);
    IO_EXTENSION_Output(IO_EXTENSION_IO_1, 1);  // RST alto
    delay(200);
    pinMode(TOUCH_INT, INPUT);                  // liberar INT para operacion normal

    uint8_t id[4] = {0};
    if (_gt911_read(0x8140, id, 4)) {
        Serial.printf("[touch] GT911 ID: %c%c%c%c\n", id[0], id[1], id[2], id[3]);
    } else {
        Serial.println("[touch] GT911 no responde (revisar I2C/reset)");
    }
}

/**
 * Llamar cada iteracion del loop. Devuelve true UNA vez por tap nuevo (flanco
 * de toque, con debounce). Opcionalmente escribe la coordenada en *tx/*ty.
 */
inline bool touch_input_tapped(uint16_t *tx = nullptr, uint16_t *ty = nullptr)
{
    uint16_t x = 0, y = 0;
    bool present = false;   // hay un dedo AHORA (frame nuevo con >=1 punto)

    uint8_t status = 0;
    if (_gt911_read(GT911_REG_STATUS, &status, 1) && (status & 0x80)) {
        // bit7 = buffer listo; bits0-3 = nº de puntos.
        uint8_t points = status & 0x0F;
        if (points > 0) {
            uint8_t p[8];
            // Punto 1 desde 0x8150: X en p[0..1], Y en p[2..3] (little-endian).
            // (El track-id esta en 0x814F, un byte ANTES; no leerlo aqui.)
            if (_gt911_read(GT911_REG_POINT1, p, 8)) {
                x = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
                y = (uint16_t)p[2] | ((uint16_t)p[3] << 8);
                present = true;
            }
        }
        _gt911_write(GT911_REG_STATUS, 0);   // ack: el GT911 prepara el siguiente frame
    }

    uint32_t now = millis();
    if (present) _touch_last_seen = now;
    // "Dedo abajo" = hubo toque hace poco. Puentea los huecos en que el buffer
    // no esta "listo" (bit7=0) durante un toque sostenido, y cuenta como arriba
    // cuando el panel queda idle (0x00) al soltar -> resetea el flanco.
    bool down = (now - _touch_last_seen) <= TOUCH_RELEASE_MS;

    bool tapped = false;
    if (present && !_touch_was_down && (now - _touch_last_tap > TOUCH_DEBOUNCE_MS)) {
        _touch_last_tap = now;
        tapped = true;
        if (tx) *tx = x;
        if (ty) *ty = y;
    }
    _touch_was_down = down;
    return tapped;
}

#endif // TOUCH_INPUT_H
