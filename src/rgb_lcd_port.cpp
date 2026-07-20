/*****************************************************************************
 * rgb_lcd_port.cpp
 *
 * Driver del panel RGB (Waveshare ESP32-S3-Touch-LCD-7B, 1024x600) basado en el
 * ejemplo original de Waveshare, PERO sin LVGL. En este proyecto el ESP32 es un
 * "display tonto": baja un JPEG del servidor, lo decodifica en el framebuffer de
 * atras y hace swap en vsync. Por eso aqui solo necesitamos:
 *   - init del panel con doble framebuffer (num_fbs=2) + bounce buffer
 *   - acceso a los dos framebuffers (waveshare_get_frame_buffer)
 *   - un semaforo que se libera en cada fin de frame (para sincronizar el swap)
 *
 * El callback de vsync del original notificaba a LVGL; aqui libera un semaforo.
 *****************************************************************************/

#include "rgb_lcd_port.h"
#include "freertos/semphr.h"

static const char *TAG = "rgb_lcd";

// Handle del panel RGB
static esp_lcd_panel_handle_t panel_handle = NULL;

// Semaforo que se libera al terminar cada frame (bounce frame finish). El bucle
// de render puede esperarlo para hacer el swap justo en el limite de frame y
// evitar tearing.
static SemaphoreHandle_t vsync_sem = NULL;

// Callback de fin de frame (contexto ISR). Da el semaforo y avisa si hay que
// hacer yield a una tarea de mayor prioridad que estuviera esperandolo.
IRAM_ATTR static bool rgb_lcd_on_frame_finish(
    esp_lcd_panel_handle_t panel,
    const esp_lcd_rgb_panel_event_data_t *edata,
    void *user_ctx)
{
    BaseType_t high_task_woken = pdFALSE;
    if (vsync_sem) {
        xSemaphoreGiveFromISR(vsync_sem, &high_task_woken);
    }
    return high_task_woken == pdTRUE;
}

esp_lcd_panel_handle_t waveshare_esp32_s3_rgb_lcd_init()
{
    ESP_LOGI(TAG, "Install RGB LCD panel driver");

    esp_lcd_rgb_panel_config_t panel_config = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = EXAMPLE_LCD_PIXEL_CLOCK_HZ,
            .h_res = EXAMPLE_LCD_H_RES,
            .v_res = EXAMPLE_LCD_V_RES,
            .hsync_pulse_width = 162,
            .hsync_back_porch = 152,
            .hsync_front_porch = 48,
            .vsync_pulse_width = 45,
            .vsync_back_porch = 13,
            .vsync_front_porch = 3,
            .flags = {
                .pclk_active_neg = 1,
            },
        },
        .data_width = EXAMPLE_RGB_DATA_WIDTH,
        .bits_per_pixel = EXAMPLE_RGB_BIT_PER_PIXEL,
        .num_fbs = EXAMPLE_LCD_RGB_BUFFER_NUMS,                  // 2 = doble buffer
        .bounce_buffer_size_px = EXAMPLE_RGB_BOUNCE_BUFFER_SIZE, // H_RES*10
        .sram_trans_align = 4,
        .psram_trans_align = 64,
        .hsync_gpio_num = EXAMPLE_LCD_IO_RGB_HSYNC,
        .vsync_gpio_num = EXAMPLE_LCD_IO_RGB_VSYNC,
        .de_gpio_num = EXAMPLE_LCD_IO_RGB_DE,
        .pclk_gpio_num = EXAMPLE_LCD_IO_RGB_PCLK,
        .disp_gpio_num = EXAMPLE_LCD_IO_RGB_DISP,
        .data_gpio_nums = {
            EXAMPLE_LCD_IO_RGB_DATA0,  EXAMPLE_LCD_IO_RGB_DATA1,
            EXAMPLE_LCD_IO_RGB_DATA2,  EXAMPLE_LCD_IO_RGB_DATA3,
            EXAMPLE_LCD_IO_RGB_DATA4,  EXAMPLE_LCD_IO_RGB_DATA5,
            EXAMPLE_LCD_IO_RGB_DATA6,  EXAMPLE_LCD_IO_RGB_DATA7,
            EXAMPLE_LCD_IO_RGB_DATA8,  EXAMPLE_LCD_IO_RGB_DATA9,
            EXAMPLE_LCD_IO_RGB_DATA10, EXAMPLE_LCD_IO_RGB_DATA11,
            EXAMPLE_LCD_IO_RGB_DATA12, EXAMPLE_LCD_IO_RGB_DATA13,
            EXAMPLE_LCD_IO_RGB_DATA14, EXAMPLE_LCD_IO_RGB_DATA15,
        },
        .flags = {
            .fb_in_psram = 1,   // framebuffers en PSRAM (8MB OPI)
        },
    };

    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&panel_config, &panel_handle));

    ESP_LOGI(TAG, "Initialize RGB LCD panel");
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));

    vsync_sem = xSemaphoreCreateBinary();

    esp_lcd_rgb_panel_event_callbacks_t cbs = {
#if EXAMPLE_RGB_BOUNCE_BUFFER_SIZE > 0
        .on_bounce_frame_finish = rgb_lcd_on_frame_finish,
#else
        .on_vsync = rgb_lcd_on_frame_finish,
#endif
    };
    ESP_ERROR_CHECK(esp_lcd_rgb_panel_register_event_callbacks(panel_handle, &cbs, NULL));

    return panel_handle;
}

// Espera hasta el proximo fin de frame (o timeout). Usar antes/despues del swap
// para alinear con el refresco del panel.
void waveshare_wait_vsync(uint32_t timeout_ms)
{
    if (vsync_sem) {
        xSemaphoreTake(vsync_sem, pdMS_TO_TICKS(timeout_ms));
    }
}

void wavesahre_rgb_lcd_display_window(int16_t Xstart, int16_t Ystart, int16_t Xend, int16_t Yend, uint8_t *Image)
{
    if (Xstart < 0) Xstart = 0;
    else if (Xend > EXAMPLE_LCD_H_RES) Xend = EXAMPLE_LCD_H_RES;
    if (Ystart < 0) Ystart = 0;
    else if (Yend > EXAMPLE_LCD_V_RES) Yend = EXAMPLE_LCD_V_RES;

    esp_lcd_panel_draw_bitmap(panel_handle, Xstart, Ystart, Xend, Yend, Image);
}

void wavesahre_rgb_lcd_display(uint8_t *Image)
{
    esp_lcd_panel_draw_bitmap(panel_handle, 0, 0, EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES, Image);
}

void waveshare_get_frame_buffer(void **buf1, void **buf2)
{
    ESP_ERROR_CHECK(esp_lcd_rgb_panel_get_frame_buffer(panel_handle, 2, buf1, buf2));
}

void wavesahre_rgb_lcd_bl_on()  {}
void wavesahre_rgb_lcd_bl_off() {}
