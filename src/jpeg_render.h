/**
 * jpeg_render.h - Decodifica un JPEG (en RAM) directamente sobre un framebuffer
 * RGB565 de 1024x600 usando JPEGDEC (bitbank2).
 *
 * El servidor sirve la imagen ya a 1024x600, asi que decodificamos 1:1 sin
 * escalar. JPEGDEC entrega bloques MCU via callback; cada bloque se copia fila
 * a fila al framebuffer destino en su posicion (x,y).
 */

#ifndef JPEG_RENDER_H
#define JPEG_RENDER_H

#include <Arduino.h>
#include <JPEGDEC.h>
#include "config.h"

static JPEGDEC _jpeg;
static uint16_t *_jpeg_fb = nullptr;   // framebuffer destino (RGB565), 1024x600

// Callback de dibujo: copia el bloque decodificado al framebuffer.
static int _jpeg_draw_cb(JPEGDRAW *pDraw)
{
    if (!_jpeg_fb) return 0;

    int x = pDraw->x;
    int y = pDraw->y;
    int w = pDraw->iWidth;
    int h = pDraw->iHeight;

    // Recorte defensivo por si el bloque se sale del framebuffer.
    if (x >= SCREEN_WIDTH || y >= SCREEN_HEIGHT) return 1;
    if (x + w > SCREEN_WIDTH)  w = SCREEN_WIDTH - x;
    if (y + h > SCREEN_HEIGHT) h = SCREEN_HEIGHT - y;

    const uint16_t *src = (const uint16_t *)pDraw->pPixels;
    for (int row = 0; row < h; row++) {
        uint16_t *dst = _jpeg_fb + (size_t)(y + row) * SCREEN_WIDTH + x;
        memcpy(dst, src + row * pDraw->iWidth, (size_t)w * sizeof(uint16_t));
    }
    return 1; // continuar
}

/**
 * Decodifica el JPEG `data`/`len` sobre el framebuffer `fb` (RGB565, 1024x600).
 * @return true si decodifico OK.
 */
inline bool jpeg_decode_to_fb(const uint8_t *data, size_t len, uint16_t *fb)
{
    _jpeg_fb = fb;
    if (!_jpeg.openRAM((uint8_t *)data, (int)len, _jpeg_draw_cb)) {
        Serial.printf("[jpeg] openRAM fallo (err %d)\n", _jpeg.getLastError());
        return false;
    }
    // El panel RGB guarda RGB565 little-endian en memoria (ESP32-S3 LE).
    // Si los colores salen con rojo/azul invertidos, cambiar a RGB565_BIG_ENDIAN.
    _jpeg.setPixelType(RGB565_LITTLE_ENDIAN);

    bool ok = _jpeg.decode(0, 0, 0) == 1;
    if (!ok) Serial.printf("[jpeg] decode fallo (err %d)\n", _jpeg.getLastError());
    _jpeg.close();
    _jpeg_fb = nullptr;
    return ok;
}

#endif // JPEG_RENDER_H
