/**
 * ap_screen.h - Pantalla de configuración WiFi (modo AP).
 *
 * Muestra instrucciones en el display cuando el ESP32 está en modo AP.
 * Usa fuente bitmap 5x7 simple embebida en RAM.
 */

#ifndef AP_SCREEN_H
#define AP_SCREEN_H

#include <Arduino.h>
#include "rgb_lcd_port.h"
#include "config.h"

// Fuente 5x7 simplificada (ASCII 32-90). Cada carácter son 5 bytes (columnas).
static const uint8_t FONT5X7[][5] = {
    {0x00,0x00,0x00,0x00,0x00}, // 32 espacio
    {0x00,0x00,0x5F,0x00,0x00}, // 33 !
    {0x00,0x07,0x00,0x07,0x00}, // 34 "
    {0x14,0x7F,0x14,0x7F,0x14}, // 35 #
    {0x24,0x2A,0x7F,0x2A,0x12}, // 36 $
    {0x23,0x13,0x08,0x64,0x62}, // 37 %
    {0x36,0x49,0x55,0x22,0x50}, // 38 &
    {0x00,0x05,0x03,0x00,0x00}, // 39 '
    {0x00,0x1C,0x22,0x41,0x00}, // 40 (
    {0x00,0x41,0x22,0x1C,0x00}, // 41 )
    {0x08,0x2A,0x1C,0x2A,0x08}, // 42 *
    {0x08,0x08,0x3E,0x08,0x08}, // 43 +
    {0x00,0x50,0x30,0x00,0x00}, // 44 ,
    {0x08,0x08,0x08,0x08,0x08}, // 45 -
    {0x00,0x60,0x60,0x00,0x00}, // 46 .
    {0x20,0x10,0x08,0x04,0x02}, // 47 /
    {0x3E,0x51,0x49,0x45,0x3E}, // 48 0
    {0x00,0x42,0x7F,0x40,0x00}, // 49 1
    {0x42,0x61,0x51,0x49,0x46}, // 50 2
    {0x21,0x41,0x45,0x4B,0x31}, // 51 3
    {0x18,0x14,0x12,0x7F,0x10}, // 52 4
    {0x27,0x45,0x45,0x45,0x39}, // 53 5
    {0x3C,0x4A,0x49,0x49,0x30}, // 54 6
    {0x01,0x71,0x09,0x05,0x03}, // 55 7
    {0x36,0x49,0x49,0x49,0x36}, // 56 8
    {0x06,0x49,0x49,0x29,0x1E}, // 57 9
    {0x00,0x36,0x36,0x00,0x00}, // 58 :
    {0x00,0x56,0x36,0x00,0x00}, // 59 ;
    {0x00,0x08,0x14,0x22,0x41}, // 60 <
    {0x14,0x14,0x14,0x14,0x14}, // 61 =
    {0x41,0x22,0x14,0x08,0x00}, // 62 >
    {0x02,0x01,0x51,0x09,0x06}, // 63 ?
    {0x32,0x49,0x79,0x41,0x3E}, // 64 @
    {0x7E,0x11,0x11,0x11,0x7E}, // 65 A
    {0x7F,0x49,0x49,0x49,0x36}, // 66 B
    {0x3E,0x41,0x41,0x41,0x22}, // 67 C
    {0x7F,0x41,0x41,0x22,0x1C}, // 68 D
    {0x7F,0x49,0x49,0x49,0x41}, // 69 E
    {0x7F,0x09,0x09,0x01,0x01}, // 70 F
    {0x3E,0x41,0x41,0x51,0x32}, // 71 G
    {0x7F,0x08,0x08,0x08,0x7F}, // 72 H
    {0x00,0x41,0x7F,0x41,0x00}, // 73 I
    {0x20,0x40,0x41,0x3F,0x01}, // 74 J
    {0x7F,0x08,0x14,0x22,0x41}, // 75 K
    {0x7F,0x40,0x40,0x40,0x40}, // 76 L
    {0x7F,0x02,0x04,0x02,0x7F}, // 77 M
    {0x7F,0x04,0x08,0x10,0x7F}, // 78 N
    {0x3E,0x41,0x41,0x41,0x3E}, // 79 O
    {0x7F,0x09,0x09,0x09,0x06}, // 80 P
    {0x3E,0x41,0x51,0x21,0x5E}, // 81 Q
    {0x7F,0x09,0x19,0x29,0x46}, // 82 R
    {0x46,0x49,0x49,0x49,0x31}, // 83 S
    {0x01,0x01,0x7F,0x01,0x01}, // 84 T
    {0x3F,0x40,0x40,0x40,0x3F}, // 85 U
    {0x1F,0x20,0x40,0x20,0x1F}, // 86 V
    {0x7F,0x20,0x18,0x20,0x7F}, // 87 W
    {0x63,0x14,0x08,0x14,0x63}, // 88 X
    {0x03,0x04,0x78,0x04,0x03}, // 89 Y
    {0x61,0x51,0x49,0x45,0x43}, // 90 Z
};

// Minúsculas (a-z = 97-122)
static const uint8_t FONT5X7_LOWER[][5] = {
    {0x20,0x54,0x54,0x54,0x78}, // a
    {0x7F,0x48,0x44,0x44,0x38}, // b
    {0x38,0x44,0x44,0x44,0x20}, // c
    {0x38,0x44,0x44,0x48,0x7F}, // d
    {0x38,0x54,0x54,0x54,0x18}, // e
    {0x08,0x7E,0x09,0x01,0x02}, // f
    {0x08,0x14,0x54,0x54,0x3C}, // g
    {0x7F,0x08,0x04,0x04,0x78}, // h
    {0x00,0x44,0x7D,0x40,0x00}, // i
    {0x20,0x40,0x44,0x3D,0x00}, // j
    {0x00,0x7F,0x10,0x28,0x44}, // k
    {0x00,0x41,0x7F,0x40,0x00}, // l
    {0x7C,0x04,0x18,0x04,0x78}, // m
    {0x7C,0x08,0x04,0x04,0x78}, // n
    {0x38,0x44,0x44,0x44,0x38}, // o
    {0x7C,0x14,0x14,0x14,0x08}, // p
    {0x08,0x14,0x14,0x18,0x7C}, // q
    {0x7C,0x08,0x04,0x04,0x08}, // r
    {0x48,0x54,0x54,0x54,0x20}, // s
    {0x04,0x3F,0x44,0x40,0x20}, // t
    {0x3C,0x40,0x40,0x20,0x7C}, // u
    {0x1C,0x20,0x40,0x20,0x1C}, // v
    {0x3C,0x40,0x30,0x40,0x3C}, // w
    {0x44,0x28,0x10,0x28,0x44}, // x
    {0x0C,0x50,0x50,0x50,0x3C}, // y
    {0x44,0x64,0x54,0x4C,0x44}, // z
};

// Colores RGB565
static inline uint16_t _rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

// Obtener glifo de un carácter
static const uint8_t* _get_glyph(char c) {
    if (c >= 32 && c <= 90) return FONT5X7[c - 32];
    if (c >= 'a' && c <= 'z') return FONT5X7_LOWER[c - 'a'];
    return FONT5X7[0]; // espacio
}

// Dibuja un carácter escalado
static void _draw_char(uint16_t *fb, int x, int y, char c, uint16_t fg, uint16_t bg, int scale)
{
    const uint8_t *glyph = _get_glyph(c);

    for (int col = 0; col < 5; col++) {
        uint8_t line = glyph[col];
        for (int row = 0; row < 7; row++) {
            uint16_t color = (line & (1 << row)) ? fg : bg;
            for (int sy = 0; sy < scale; sy++) {
                for (int sx = 0; sx < scale; sx++) {
                    int px = x + col * scale + sx;
                    int py = y + row * scale + sy;
                    if (px >= 0 && px < SCREEN_WIDTH && py >= 0 && py < SCREEN_HEIGHT) {
                        fb[py * SCREEN_WIDTH + px] = color;
                    }
                }
            }
        }
    }
}

// Dibuja una cadena de texto
static void _draw_text(uint16_t *fb, int x, int y, const char *text, uint16_t fg, uint16_t bg, int scale)
{
    int cx = x;
    while (*text) {
        _draw_char(fb, cx, y, *text, fg, bg, scale);
        cx += 6 * scale; // 5 + 1 espacio
        text++;
    }
}

// Ancho de texto en píxeles
static int _text_width(const char *text, int scale) {
    return strlen(text) * 6 * scale - scale;
}

// Centrar texto horizontalmente
static int _center_x(const char *text, int scale) {
    return (SCREEN_WIDTH - _text_width(text, scale)) / 2;
}

// Llena la pantalla: pinta la primera fila y replica con memcpy. Pixel a pixel
// son 614400 escrituras sueltas a la PSRAM; asi es una fila y 599 copias.
static void _fill_screen(uint16_t *fb, uint16_t color) {
    for (int x = 0; x < SCREEN_WIDTH; x++) fb[x] = color;
    for (int y = 1; y < SCREEN_HEIGHT; y++)
        memcpy(fb + (size_t)y * SCREEN_WIDTH, fb, (size_t)SCREEN_WIDTH * 2);
}

// Dibuja rectángulo
static void _fill_rect(uint16_t *fb, int x, int y, int w, int h, uint16_t color) {
    for (int row = y; row < y + h && row < SCREEN_HEIGHT; row++) {
        for (int col = x; col < x + w && col < SCREEN_WIDTH; col++) {
            if (row >= 0 && col >= 0) {
                fb[row * SCREEN_WIDTH + col] = color;
            }
        }
    }
}

/**
 * Muestra la pantalla de configuración WiFi en modo AP.
 */
inline void ap_screen_show(uint16_t *fb, const char *ap_name, const char *ip)
{
    uint16_t bg = _rgb565(10, 30, 80);       // Azul oscuro
    uint16_t fg = _rgb565(255, 255, 255);    // Blanco
    uint16_t accent = _rgb565(255, 220, 0);  // Amarillo
    uint16_t gray = _rgb565(120, 140, 160);  // Gris

    _fill_screen(fb, bg);

    int y = 60;
    int scale = 4;

    // Título
    const char *title = "CONFIGURACION WIFI";
    _draw_text(fb, _center_x(title, scale), y, title, accent, bg, scale);

    y += 80;
    scale = 3;

    // Paso 1
    _draw_text(fb, 80, y, "1. CONECTA A LA RED:", fg, bg, scale);
    y += 50;
    _fill_rect(fb, 120, y - 5, _text_width(ap_name, scale) + 20, scale * 7 + 10, _rgb565(40, 60, 100));
    _draw_text(fb, 130, y, ap_name, accent, _rgb565(40, 60, 100), scale);

    y += 70;

    // Paso 2
    _draw_text(fb, 80, y, "2. ABRE EN TU NAVEGADOR:", fg, bg, scale);
    y += 50;
    _fill_rect(fb, 120, y - 5, _text_width(ip, scale) + 20, scale * 7 + 10, _rgb565(40, 60, 100));
    _draw_text(fb, 130, y, ip, accent, _rgb565(40, 60, 100), scale);

    y += 70;

    // Paso 3
    _draw_text(fb, 80, y, "3. CONFIGURA TU RED WIFI", fg, bg, scale);

    // Pie
    scale = 2;
    const char *footer = "ECOWITT DISPLAY KIOSK";
    _draw_text(fb, _center_x(footer, scale), SCREEN_HEIGHT - 50, footer, gray, bg, scale);

    // Mostrar
    waveshare_fb_flush(fb, SCREEN_WIDTH * SCREEN_HEIGHT * 2);
    waveshare_swap_fb(fb);
}

/**
 * Pantalla de informacion (toque largo fuera de la barra de pestañas).
 *
 * Es la unica forma de averiguar la IP del display sin entrar al router ni
 * abrir el gabinete, y sin la IP no se puede llegar a la pagina de
 * configuracion estando ya conectado a la red.
 */
inline void info_screen_show(uint16_t *fb, const char *ssid, const char *ip, int rssi)
{
    uint16_t bg     = _rgb565(10, 30, 80);
    uint16_t fg     = _rgb565(255, 255, 255);
    uint16_t accent = _rgb565(255, 220, 0);
    uint16_t gray   = _rgb565(120, 140, 160);
    uint16_t box    = _rgb565(40, 60, 100);

    _fill_screen(fb, bg);

    int y = 55;
    const char *title = "CONFIGURACION";
    _draw_text(fb, _center_x(title, 4), y, title, accent, bg, 4);

    y += 85;
    _draw_text(fb, 80, y, "ABRE EN TU NAVEGADOR:", fg, bg, 3);

    // URL de la pagina de configuracion, destacada.
    y += 55;
    char url[64];
    snprintf(url, sizeof(url), "http://%s/", ip);
    _fill_rect(fb, 100, y - 8, _text_width(url, 4) + 24, 4 * 7 + 16, box);
    _draw_text(fb, 112, y, url, accent, box, 4);

    y += 90;
    char line[80];
    snprintf(line, sizeof(line), "Red: %s", ssid);
    _draw_text(fb, 80, y, line, fg, bg, 3);

    y += 45;
    snprintf(line, sizeof(line), "Senal: %d dBm", rssi);
    _draw_text(fb, 80, y, line, fg, bg, 3);

    y += 45;
    snprintf(line, sizeof(line), "Firmware: %s", FW_VERSION);
    _draw_text(fb, 80, y, line, gray, bg, 3);

    const char *footer = "TOCA LA PANTALLA PARA VOLVER";
    _draw_text(fb, _center_x(footer, 2), SCREEN_HEIGHT - 45, footer, gray, bg, 2);

    waveshare_fb_flush(fb, SCREEN_WIDTH * SCREEN_HEIGHT * 2);
    waveshare_wait_vsync(50);
    waveshare_swap_fb(fb);
}

#endif // AP_SCREEN_H
