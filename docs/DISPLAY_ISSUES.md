# Problemas de Display y Soluciones

## Problema: Desplazamiento Vertical + Rayitas

### Síntomas
- La imagen aparece desplazada hacia arriba (parte inferior de la imagen visible arriba)
- Rayitas/artefactos en los primeros ~60 pixels del lado izquierdo
- Ocurre durante cambios de página y auto-refresh
- Entre las 2 últimas páginas visitadas no hay problema (swap de caché)

### Causa Raíz
`CONFIG_LCD_RGB_RESTART_IN_VSYNC=y` reinicia el DMA del panel RGB en cada vsync.
Este reinicio desincroniza el barrido cuando hay un swap de framebuffer,
causando que el panel muestre datos desde una posición incorrecta.

### Solución
Desactivar el restart en vsync en `platformio.ini`:

```ini
custom_sdkconfig =
    CONFIG_LCD_RGB_RESTART_IN_VSYNC=n
```

Con `=n` el DMA corre continuo y el swap de framebuffer es limpio.

**Nota:** Requiere recompilar el IDF (`pio run -t clean` antes de compilar).

---

## Problema: Brinco Visual al Cargar Páginas

### Síntomas
- Al ir a una página no cacheada, se ve un "brinco" de la página anterior a la nueva
- Transición abrupta y poco profesional

### Causa
El ESP32 tiene 2 framebuffers = caché para 2 páginas. Ir a una tercera requiere:
1. Descargar JPEG del servidor (~200-500ms)
2. Decodificar JPEG (~100ms)
3. Copiar al framebuffer (~50ms)

Durante este tiempo, la pantalla sigue mostrando la página anterior.

### Solución
Oscurecer la pantalla inmediatamente al detectar el touch (antes de cargar):

1. Detectar touch en `loop()` (core 1)
2. Verificar si la página destino está en caché
3. Si no está, llamar a `blackout_now()` que:
   - Limpia el FB de atrás con negro
   - Hace swap inmediato a negro
4. Luego despertar a `netTask` (core 0) para cargar la página

El oscurecimiento es instantáneo porque se hace desde el touch handler,
no hay que esperar a que netTask despierte.

---

## Configuración del Bounce Buffer

### Parámetro
`EXAMPLE_RGB_BOUNCE_BUFFER_SIZE` en `rgb_lcd_port.h`

### Comportamiento
- Bounce buffer más grande = más datos "en vuelo" = mayor desplazamiento potencial
- Bounce buffer más pequeño = transiciones más lentas

### Valor Óptimo
```cpp
#define EXAMPLE_RGB_BOUNCE_BUFFER_SIZE  (EXAMPLE_LCD_H_RES * 3)  // 3 líneas
```

Balance entre velocidad de transición y estabilidad visual.

---

## Otras Configuraciones Importantes

### XIP desde PSRAM
```ini
CONFIG_SPIRAM_XIP_FROM_PSRAM=y
```
Ejecuta código desde PSRAM para evitar contención del bus Flash/PSRAM.
Sin esto, la CPU bloquea el bus al buscar instrucciones y el DMA pierde sincronía.

### Doble Framebuffer
```cpp
#define EXAMPLE_LCD_RGB_BUFFER_NUMS  (2)
```
Permite swap instantáneo entre las 2 últimas páginas visitadas.
El driver soporta hasta 3 framebuffers pero 2 es suficiente.

---

## Resumen de Archivos Modificados

| Archivo | Cambio |
|---------|--------|
| `platformio.ini` | `CONFIG_LCD_RGB_RESTART_IN_VSYNC=n` |
| `src/rgb_lcd_port.h` | Bounce buffer de 10 a 3 líneas |
| `src/main.cpp` | Oscurecimiento instantáneo desde touch |
