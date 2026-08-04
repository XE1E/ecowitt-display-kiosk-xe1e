# Rendimiento del cambio de página

Cuánto tarda el display en cambiar de página, dónde se va ese tiempo y qué se
puede hacer al respecto. Todo lo de aquí está **medido en la placa**, no estimado.

## Cómo medirlo

El propio firmware cronometra cada carga y lo publica: fila **Cambio de página**
del bloque *Estado* del portal, o `GET /api/status`:

```
"ms": 264,      # solo el GET del JPEG
"bytes": 64447,
"dec": 148,     # decode del JPEG sobre el framebuffer
"pnt": 4,       # flush cache -> PSRAM por trozos
"load": 416     # total
```

Para saber si la culpa es del display o del servidor, el mismo GET desde una PC de
la misma red:

```bash
curl -o NUL -s -w "conexion=%{time_connect}s ttfb=%{time_starttransfer}s total=%{time_total}s bytes=%{size_download}\n" \
  "http://<servidor>:8080/api/display.jpg?page=1"
```

## Desglose (página 1, ~62 KB)

| | v1.2.2 | v1.2.4 (SIMD) | v1.3.0 (ventana TCP) |
|---|---|---|---|
| Red | 264-474 ms | 307-342 ms | **104-145 ms** |
| Decode | 240 ms | **148 ms** | 147 ms |
| Pintado (flush) | 4 ms | 4 ms | 4 ms |
| **Total** | 512-722 ms | 459-495 ms | **256-298 ms** |

Velocidad de bajada: **~180 KB/s → 427-596 KB/s**. El heap libre no se movió
(206-208 KB). En la consola (~142 KB), que es la página más pesada, el total pasa de
~740-1 020 ms a ~400-480 ms.

El pintado es despreciable desde que el JPEG se decodifica **directo sobre el
framebuffer** (antes se decodificaba a un buffer intermedio y se copiaba: 2.4 MB
de tráfico de bus por refresco, más 1.2 MB de PSRAM reservados).

## Palanca 1: el SIMD del ESP32-S3 en el decode (hecho)

JPEGDEC traía su ruta en C puro por dos frenos: `-DNO_SIMD` en `platformio.ini` y
el stub de `stubs/dsps_fft2r_platform.h`, que compilaba el assembly del S3 vacío.
Quitando el primero y poniendo el stub en 1, el assembly entra y enlaza sin
problema (+1.3 KB de flash). A/B por OTA con la misma página: **240 → 148 ms**.

## Palanca 2: la ventana TCP (hecho)

Bajar el JPEG se llevaba la mitad del tiempo, y no era ni el servidor ni el WiFi:

- La misma imagen baja en **46 ms desde una PC** de la misma red (primer byte en
  20 ms), o sea ~1 400 KB/s. El display saca **~180 KB/s**.
- Un TCP no puede tener más de una ventana de datos en vuelo, así que el techo es
  `ventana / RTT`. Con la ventana por defecto de lwIP (`CONFIG_LWIP_TCP_WND_DEFAULT
  = 5760`) y el RTT del display al VPS (~30 ms: ~15 ms de internet medidos con ping
  desde la PC, más el aire WiFi) sale **192 KB/s**. Que es exactamente lo medido.

Es decir: el limitante era la ventana, no el ancho de banda. En `custom_sdkconfig`:

```ini
CONFIG_LWIP_TCP_WND_DEFAULT=28720      ; 20 x MSS(1436); tope sin window scaling: 64 KB
CONFIG_LWIP_TCP_RECVMBOX_SIZE=24       ; una ventana entera en segmentos, con margen
CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=16
CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM=48
CONFIG_ESP_WIFI_RX_BA_WIN=16
```

La cola (`RECVMBOX_SIZE`) y los buffers del driver WiFi tienen que crecer con la
ventana: si la cola se queda corta, lwIP descarta segmentos ya recibidos y los pide
otra vez, que es peor que no haber ampliado nada. Cuesta ~12 KB fijos de RAM
interna. **Cambiar `custom_sdkconfig` obliga a recompilar el IDF** (`pio run -t
clean` primero; el relink completo tarda ~30 min).

Resultado medido: **180 KB/s → 427-596 KB/s**, o sea la red del cambio de página
pasó de 307-342 ms a 104-145 ms. Sigue por debajo del techo teórico (~950 KB/s)
porque ahora el limitante sí es el WiFi y el RTT variable, no la ventana.

## Palanca descartada: bajar la calidad del JPEG

El renderer sirve a `JPEG_QUALITY=92`, y el `docker-compose.yml` del servidor ya
advierte por qué: *"los degradados oscuros muestran ruido/puntitos en un panel
RGB565 si la calidad es baja"*. Se midió antes de tocarlo, simulando la reducción a
RGB565 del panel (5/6/5 bits) y comparando contra lo que se sirve hoy:

| Calidad | Consola | vs hoy | Píxeles del panel que cambian | …dos niveles o más |
|---|---|---|---|---|
| q92 (hoy) | 141 KB | — | — | — |
| q92 + Huffman óptimo | 140 KB | −1 % | **0 %** (sin pérdida) | 0 % |
| q88 | 122 KB | −13 % | 30 % | 9 % |
| q85 | 112 KB | −21 % | 32 % | 11 % |
| q80 | 95 KB | −32 % | 34 % | 13 % |
| q75 | 88 KB | −38 % | 36 % | 15 % |

El primer escalón ahorra 60-90 ms y ya altera el 15-30 % de los píxeles (según la
página); para ahorrar algo que se note hay que bajar a q80 o menos, justo donde
aparece el ruido documentado. La vía sin pérdida (reescribir con Huffman óptimo, sin
tocar un píxel) da solo −1 %. **No vale la pena:** la ventana TCP ataca el mismo
tiempo sin cambiar un solo píxel.

## Pendiente

- **Keep-alive HTTP (~30-60 ms).** Hoy cada petición abre una conexión TCP nueva;
  reutilizarla ahorra el handshake. Requiere que el `WiFiClient`/`HTTPClient` sobreviva
  entre peticiones (`http.setReuse(true)`) y manejar que el servidor cierre.
- **Brinco residual al conmutar de framebuffer.** Casi imperceptible desde que el
  swap espera el límite de frame (ver [`DISPLAY_ISSUES.md`](DISPLAY_ISSUES.md)). Si
  hiciera falta afinarlo más, el sospechoso es la latencia entre el ISR de fin de
  frame y que el netTask despierte a hacer el swap: subirle la prioridad a esa tarea,
  o hacer el swap dentro del propio callback.
