/**
 * nav.h - navegacion por ZONAS del kiosco.
 *
 * El firmware ya no sabe que paginas existen. Con cada JPEG llega la cabecera
 * `X-Kiosk-Nav` con los rectangulos tocables de ESA pantalla y a donde lleva cada
 * uno; aqui se parsea, se guarda y se resuelve un toque.
 *
 * Antes el numero de pestañas estaba en config.h y tenia que coincidir con el array
 * TABS del dashboard: cada pantalla nueva del servidor obligaba a recompilar y
 * reflashear, y si los dos lados se desincronizaban los toques caian en la pestaña
 * equivocada. Ver PLAN-KIOSCO-NAVEGACION.md en el repo del servidor.
 *
 * Formato (plano a proposito, para parsearlo sin un JSON en el ESP32):
 *
 *   v=1;back=consola;ttl=1800;z=0,536,171,64,det-rain-24h;z=171,536,171,64,det-rain-7d
 *
 *   back  destino de un toque FUERA de toda zona, si la pila esta vacia
 *   ttl   segundos que el servidor considera valida la imagen (informativo)
 *   z     x,y,ancho,alto,slug
 */
#ifndef NAV_H
#define NAV_H

#include <Arduino.h>
#include <string.h>
#include <stdlib.h>

#define NAV_MAX_ZONAS   20    // el servidor no manda mas (MAX_ZONAS en nav-zones.tsx)
#define NAV_SLUG_MAX    24    // "det-press-24h" son 13; 24 deja aire de sobra
#define NAV_HDR_MAX    900    // 20 zonas de ~30 bytes, mas los campos sueltos
#define NAV_STACK_MAX    8    // profundidad de la pila de "atras"

struct NavZona {
    uint16_t x, y, w, h;
    char to[NAV_SLUG_MAX];
};

struct NavMapa {
    NavZona zonas[NAV_MAX_ZONAS];
    uint8_t n;
    char back[NAV_SLUG_MAX];
};

/** Vacia un mapa: sin zonas y sin destino de retroceso. */
inline void nav_clear(NavMapa &m)
{
    m.n = 0;
    m.back[0] = '\0';
}

/**
 * Parsea la cabecera en `m`. Devuelve false (y deja el mapa vacio) si la cadena no
 * trae ninguna zona utilizable, que es la señal para caer al modo antiguo.
 */
inline bool nav_parse(const char *hdr, NavMapa &m)
{
    nav_clear(m);
    if (!hdr || !*hdr) return false;

    // Se trabaja sobre una copia: strtok escribe en el buffer y la cabecera puede
    // venir de un String que aun se usa fuera.
    char buf[NAV_HDR_MAX];
    strlcpy(buf, hdr, sizeof(buf));

    char *save = nullptr;
    for (char *tok = strtok_r(buf, ";", &save); tok; tok = strtok_r(nullptr, ";", &save)) {
        if (!strncmp(tok, "back=", 5)) {
            strlcpy(m.back, tok + 5, sizeof(m.back));
        } else if (!strncmp(tok, "z=", 2) && m.n < NAV_MAX_ZONAS) {
            // x,y,w,h,slug — el slug es lo ultimo y puede llevar guiones, asi que se
            // toma tal cual desde la quinta coma en vez de trocearlo mas.
            int x, y, w, h, len = 0;
            if (sscanf(tok + 2, "%d,%d,%d,%d,%n", &x, &y, &w, &h, &len) == 4 && len > 0) {
                const char *slug = tok + 2 + len;
                if (*slug && w > 0 && h > 0) {
                    NavZona &z = m.zonas[m.n];
                    z.x = (uint16_t)x; z.y = (uint16_t)y;
                    z.w = (uint16_t)w; z.h = (uint16_t)h;
                    strlcpy(z.to, slug, sizeof(z.to));
                    m.n++;
                }
            }
        }
        // v= y ttl= se ignoran: el firmware no decide caducidad, la imagen la sirve
        // el renderer ya cacheada.
    }
    return m.n > 0;
}

/**
 * Slug al que lleva tocar en (x,y), o nullptr si el toque no cae en ninguna zona.
 *
 * Se recorren en ORDEN y gana la primera que contenga el punto. El servidor las
 * manda de menor a mayor area justamente para esto: asi un boton dentro de un bloque
 * navegable mas grande gana al bloque.
 */
inline const char *nav_hit(const NavMapa &m, uint16_t x, uint16_t y)
{
    for (uint8_t i = 0; i < m.n; i++) {
        const NavZona &z = m.zonas[i];
        if (x >= z.x && x < z.x + z.w && y >= z.y && y < z.y + z.h) return z.to;
    }
    return nullptr;
}

/**
 * Hash FNV-1a de un slug, para la contabilidad de los framebuffers.
 *
 * Esos dos huecos guardaban un numero de pagina; ahora las paginas son cadenas y
 * compararlas en cada swap seria comparar strings en el camino critico. Se reservan
 * 0 y -1, que ya significan "sin pagina" y "spinner", y cualquier hash que caiga ahi
 * se desplaza: una colision solo haria presentar la pagina equivocada de las DOS que
 * hay en cache, y aun asi no puede pasar con esos dos valores.
 */
inline int32_t nav_hash(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; ++s) { h ^= (uint8_t)*s; h *= 16777619u; }
    int32_t v = (int32_t)h;
    if (v == 0 || v == -1) v = 2;
    return v;
}

/** Pila de "atras": recuerda por donde se llego hasta aqui. */
struct NavPila {
    char items[NAV_STACK_MAX][NAV_SLUG_MAX];
    uint8_t n;
};

inline void nav_push(NavPila &p, const char *slug)
{
    if (!slug || !*slug) return;
    if (p.n >= NAV_STACK_MAX) {
        // Llena: se tira la MAS ANTIGUA. Perder el fondo de la pila solo significa
        // que un "atras" de mas acabara usando el `back` del mapa; perder la mas
        // reciente romperia el retroceso inmediato, que es el que se usa siempre.
        memmove(p.items[0], p.items[1], (NAV_STACK_MAX - 1) * NAV_SLUG_MAX);
        p.n = NAV_STACK_MAX - 1;
    }
    strlcpy(p.items[p.n], slug, NAV_SLUG_MAX);
    p.n++;
}

/** Saca el ultimo, o nullptr si esta vacia. */
inline const char *nav_pop(NavPila &p)
{
    if (!p.n) return nullptr;
    p.n--;
    return p.items[p.n];
}

#endif // NAV_H
