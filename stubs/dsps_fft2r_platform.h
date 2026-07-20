/*
 * Stub de dsps_fft2r_platform.h para ESP32-S3.
 *
 * El SIMD de JPEGDEC (s3_simd_*.S) incluye este header de esp-dsp, PERO
 * esp-dsp no provee este header para la variante esp32s3 en arduino-esp32 3.x.
 * Antes del relink la macro ARDUINO_ESP32S3_DEV no estaba definida y el .S
 * quedaba vacio; tras habilitar CONFIG_LCD_RGB_RESTART_IN_VSYNC (relink) esa
 * macro queda definida y el .S intenta incluir un header inexistente.
 *
 * Con este stub, `dsps_fft2r_sc16_aes3_enabled` = 0 hace que el cuerpo del .S
 * se compile vacio. Junto con -DNO_SIMD (ver platformio.ini), JPEGDEC decodifica
 * por su ruta en C puro (mas que suficiente: refrescamos ~1 vez por minuto).
 */
#pragma once
#define dsps_fft2r_sc16_aes3_enabled 0
