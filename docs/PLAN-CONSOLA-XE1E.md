# Plan — Consola: nueva cara de la estación (display kiosko)

> Última actualización: 2026-08-05. Vive en git (repo del firmware).
> Cruza dos repos: **firmware** `ecowitt-display-kiosk-xe1e` (esta) y
> **servidor** `ecowitt-weather-server-xe1e` (renderiza las páginas).

> **Estado (2026-08-04): la fase inmediata está TERMINADA** — 6ª pestaña,
> consola full-screen, toque para regresar y fuente DSEG en los números, todo
> hecho y desplegado en las dos mitades. Lo que sigue abierto es la **fase
> futura**: la consola como pantalla principal y las **zonas de toque por
> bloque** (decisión 4, aún sin definir).

## Idea

La **Consola** es una **cara diferente de la estación**: una réplica de la consola
física Ecowitt a pantalla completa (1024×600) con el dato vivo, estética LED sobre
negro. Es una **opción de despliegue** más para el mismo display. A futuro será la
pantalla **principal/home**, y desde ella el toque llevará a pantallas de detalle
(y de vuelta). Se desarrolla por fases para no romper lo que ya funciona.

## Estado actual (ya hecho)

- La página **`consola`** ya existe y está desplegada en el **servidor**:
  `GET /api/display.jpg?page=consola` → JPEG 1024×600, **pantalla completa SIN barra**
  de pestañas. Código: rama `page === 'consola'` en
  `dashboard/src/pages/KioskPage.tsx` (repo servidor); registrada en
  `renderer/app.py` (`VALID_PAGES`). Datos vivos vía `useStationData`+`useUnits`.
- El **display** (este firmware) muestra hoy **5 páginas numeradas** (1‑5) con una
  **barra de pestañas** inferior que dibuja el servidor; el firmware mapea el toque
  en la franja inferior (`y >= 490`) a la pestaña por la X (`x*NUM_PAGES/1024`).
  Ver `src/main.cpp` y `docs/ARQUITECTURA.md`.

## Fase inmediata — dejar la consola FUNCIONANDO (mínimos cambios) ✅ HECHA

Objetivo: llegar a la consola desde una **6ª pestaña**, y desde la consola
**regresar con un toque**. No se toca la navegación de las 5 páginas.

### Servidor (`ecowitt-weather-server-xe1e`) ✅
1. **6ª pestaña "Consola"** (🖥️) en la barra inferior de las páginas 1‑5
   (`KioskPage.tsx`, arreglo `TABS`). La consola en sí ya existe (`?page=consola`).
2. **Fuente 7‑segmentos (DSEG) SOLO en la consola** — en los **números** grandes
   (temp, humedad, presión, etc.); las **etiquetas** (OUTDOOR, HUMIDITY…) se quedan
   en la condensada actual (un 7‑seg sería ilegible para texto). Embeber DSEG como
   `@font-face` data-URI dentro de la rama `page==='consola'` (OFL, de keshikan/DSEG).
   El renderer (Chromium) la usará al capturar.

### Firmware (`ecowitt-display-kiosk-xe1e`) ✅
1. Barra táctil de **6 pestañas** (la 6ª → consola). La consola es una **página
   especial a pantalla completa** que baja `?page=consola` (no numérica).
   - Idea: id interno `PAGE_CONSOLA` (p. ej. 6); `net_fetch_display` mapea ese id a
     la URL `?page=consola`; arrays `g_fbPage`/`g_fetched` dimensionados para incluirla.
2. **Estando en la consola** (sin barra), **un toque en CUALQUIER parte → página 1**
   (la principal actual). En las páginas 1‑5 el toque sigue igual (barra de 6 tabs;
   la 6ª va a la consola).
3. Reflasheo por USB (COM del CH343/CH340).

### Contrato tab-bar (mantener sincronizado)
El nº de pestañas de la barra (servidor) DEBE coincidir con el mapeo del touch
(firmware). Al pasar de 5 a 6: `TABS` en `KioskPage` (6 entradas) ↔ mapeo del touch
a 6 en `main.cpp`. La 6ª es especial (va a `?page=consola`, no a `page=6`).

## Fase futura — la visión (PENDIENTE, es lo que queda)

- ✅ **HECHO (2026-08-04):** la **consola es la principal/home**, el display arranca
  en ella (`PAGE_HOME` en `src/config.h`). Conserva su lugar como 6ª pestaña: el
  arranque no cambió el orden ni el número de pestañas de la barra.
- **Zonas de toque por bloque** de la rejilla 3×5 → pantalla de detalle. Mapeo
  **tentativo** (se ajustará; las zonas serán distintas a la barra de pestañas):
  - 🧭 **WIND** (compás) → rosa de vientos
  - 📊 **PRESSURE** → tendencia de presión
  - 🌧 **RAIN** → precipitación / histórico
  - 🌡 **OUTDOOR** → tendencia 24 h
  - 📡 **SENSOR CH1** → sensores / remota
- **Pulido visual** de la consola.
- **Fuente:** por ahora DSEG solo en la consola; a futuro se decidirá si se unifica
  la tipografía de TODO el display.

## Decisiones cerradas (2026-07-25)
1. 6ª pestaña "Consola" — OK.
2. Al tocar la consola → **regresa SIEMPRE a la página 1**.
3. Fuente **7‑segmentos (DSEG) solo en la consola** (en los números).
4. Zonas de toque por bloque — **pendiente de definir** ("lo iremos viendo").

## Layout de la consola (referencia, rejilla 3×5)

Estado real al **2026-08-05**. El bloque anterior de esta sección describía un
reparto que nunca llegó a desplegarse (`RAIN | icono | PRESSURE REL` en la fila 3,
sin ICA, sin luna aparte y sin la remota); se sustituye por lo que renderiza hoy
`dashboard/src/components/station/ConsoleReplica.tsx` en el repo del servidor.

```
 fr   │ columna 1              │ columna 2                │ columna 3
──────┼────────────────────────┼──────────────────────────┼────────────────────────
 1.23 │ EXT  temp + mín/máx    │ VIENTO — compás ovalado  │ VEL  velocidad + rumbo
 1.23 │ HUMEDAD  % + mín/máx   │   (fusionada, 2 filas)   │ LLUVIA evento/tasa/día
 1.18 │ PRES  + riel ±5 mb     │ condición (2/3) + LUNA   │ ROCÍO + SENSACIÓN
 1.00 │ INTERIOR  temp + hum   │ SOLAR + UV + ICA         │ REMOTA  temp + hum
 0.92 │ JARDÍN CH1  temp + hum │ reloj + fecha + título   │ PRESIÓN GW1100
```

Notas que no se ven en el diagrama:

- **Las filas 1 y 2 valen lo mismo a propósito.** EXT y HUMEDAD muestran la misma
  estructura (valor grande arriba, línea de MÍN/MÁX abajo) y con altos distintos el
  mín/máx de HUMEDAD quedaba pegado al valor. Repartirlas iguales no afecta a la
  celda del VIENTO, que abarca las dos y sólo depende de la suma.
- **Cifras grandes a 66 px** (EXT, HUMEDAD, VEL), unidad a 24 y el decimal a la
  mitad del entero (`decxs`). 66 es el techo medido: por encima, la tinta del DSEG
  invade la línea de mín/máx.
- **`PRES` abreviado** en la fila 3 —con la lectura subida, "PRESIÓN" chocaba con el
  número— mientras la fila 5 conserva `PRESIÓN GW1100`, que sí tiene sitio.
- **Riel de presión**: variación de 3 h sobre ±5 hPa, con el rango fijo en hPa y
  sólo los rótulos convertidos (en imperial son ±0.15 inHg). Mismos umbrales de
  color que la flecha de tendencia (±1 hPa) para que no se contradigan.
- **Marcadores de ubicación**: casa = sensor interior, casa con flecha = exterior.
  Antes el de exterior era un sol amarillo, que junto a la celda de condición se
  leía como estado del cielo.
- **REMOTA** prefiere el WN32 exterior y cae al integrado del GW1100; la etiqueta
  dice cuál de los dos se está viendo. **JARDÍN** mapea a CH1 y cae a la remota si
  no hay canal.

Datos vivos, unidades del dashboard (°C, km/h, mb, mm, W/m²).
