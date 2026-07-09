#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "esp_timer.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "user_config.h"
#include "lv_demos.h"
#include "esp_lcd_axs15231b.h"
#include "i2c_bsp.h"
#include "driver/gpio.h"
#include "esp_io_expander_tca9554.h"

#include "lcd_bl_pwm_bsp.h"
#include "app.h"

static const char *TAG = "example";
static SemaphoreHandle_t lvgl_mux = NULL;

static uint16_t *lvgl_dma_buf = NULL; 
static SemaphoreHandle_t lvgl_flush_semap;
static esp_io_expander_handle_t io_expander = NULL;

// Rotated framebuffer + runtime rotation flag — declared UNCONDITIONALLY (landscape is a runtime toggle now,
// not the compile-time `Rotated` default). false = native portrait (172x640), true = landscape (640x172).
// Read by flush_cb + touch_cb, set by app_set_rotation(); all run in the LVGL task, so no locking needed.
uint16_t* rotat_ptr = NULL;
static volatile bool g_landscape = false;

// --- Standby / power ---------------------------------------------------------------------------
// "Standby" keeps WiFi + the WebSocket alive (so the deck stays pingable and wakes on events) and only
// puts the SCREEN to sleep: backlight off + panel display-off + flush_cb skips the SPI redraw. The idle
// timer + BOOT-button reader live in the power task (below). All hardware transitions run there under the
// LVGL lock, so any thread just posts a request flag (never touches the panel itself -> no SPI race, no
// re-entrant lock).
//
// "Lock" (BOOT long-press) is a deliberate pocket mode: the screen goes dark like standby, but touch AND
// incoming events are IGNORED — the ONLY way out is another long-press. This keeps stray pocket touches
// from driving the focused Claude session. Sounds still play (audio is independent of the display).
static volatile bool g_display_off   = false;     // true while the screen is asleep (read by flush_cb)
static volatile bool g_locked        = false;     // true while touch-LOCKED (screen dark, touch + events ignored)
static volatile bool s_wake_req      = false;     // any thread -> power task: wake the screen now
static volatile bool s_lock_toggle_req = false;   // BOOT long-press -> power task: toggle the lock
static volatile bool s_auto_standby  = true;      // idle timer enabled? (Settings toggle, live)
static volatile int64_t s_last_activity_us = 0;   // esp_timer time of the last touch / waking event
#define STANDBY_IDLE_MS   60000                    // auto screen-off after this much no-touch / no-event time
#define BOOT_LONGPRESS_MS 1200                     // BOOT held this long = lock toggle (else brightness cycle)
#define LOCK_FLASH_MS     3000                     // after locking, keep the "Locked" notice on-screen this long
static volatile int64_t s_lock_blank_at = 0;       // esp_timer deadline to blank the just-locked screen (0 = none)
// Backlight brightness cycle (BOOT short-press): duties from brightest to dim. PWM is active-low, so
// LCD_PWM_MODE_255 -> duty 0 -> brightest; larger MODE_n -> higher duty -> dimmer.
static const uint16_t s_bright_levels[] = { LCD_PWM_MODE_255, LCD_PWM_MODE_200, LCD_PWM_MODE_150, LCD_PWM_MODE_100 };
static int      s_bright_idx = 0;
static uint16_t s_cur_duty   = LCD_PWM_MODE_255;   // restored on wake
static bool     s_eat_touch  = false;              // swallow the touch gesture that woke the screen


#define LCD_BIT_PER_PIXEL (16)
   


static void example_lvgl_touch_cb(lv_indev_drv_t *drv, lv_indev_data_t *data);

static bool example_lvgl_lock(int timeout_ms);
static void example_lvgl_unlock(void);
static void example_lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map);
void example_lvgl_port_task(void *arg);
static void example_backlight_loop_task(void *arg);
static void example_lcd_pwm_off_early(void);
static void example_lcd_exio_init(void);
static void example_lcd_reset(void);
static void example_lcd_backlight_set(bool enable);


static const axs15231b_lcd_init_cmd_t lcd_init_cmds[] = 
{
    {0x11, (uint8_t []){0x00}, 0, 100},
    {0x29, (uint8_t []){0x00}, 0, 100},
};

static bool example_notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    BaseType_t TaskWoken;
    xSemaphoreGiveFromISR(lvgl_flush_semap,&TaskWoken);
    return false;
}
static void example_increase_lvgl_tick(void *arg)
{
    lv_tick_inc(EXAMPLE_LVGL_TICK_PERIOD_MS);
}

static void example_lcd_pwm_off_early(void)
{
    gpio_config_t gpio_conf = {};
    gpio_conf.intr_type = GPIO_INTR_DISABLE;
    gpio_conf.mode = GPIO_MODE_OUTPUT;
    gpio_conf.pin_bit_mask = ((uint64_t)1 << EXAMPLE_PIN_NUM_BK_LIGHT);
    gpio_conf.pull_down_en = GPIO_PULLDOWN_ENABLE;
    gpio_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&gpio_conf));
    ESP_ERROR_CHECK(gpio_set_level(EXAMPLE_PIN_NUM_BK_LIGHT, 0));
}

static void example_lcd_exio_init(void)
{
    i2c_master_bus_handle_t tca9554_i2c_bus = NULL;
    ESP_ERROR_CHECK(i2c_master_get_bus_handle(0, &tca9554_i2c_bus));
    ESP_ERROR_CHECK(esp_io_expander_new_i2c_tca9554(tca9554_i2c_bus, ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000, &io_expander));
    ESP_ERROR_CHECK(esp_io_expander_set_dir(io_expander, EXAMPLE_EXIO_PIN_TOUCH_INT, IO_EXPANDER_INPUT));
    ESP_ERROR_CHECK(esp_io_expander_set_dir(io_expander, EXAMPLE_EXIO_PIN_BL_EN | EXAMPLE_EXIO_PIN_LCD_RST | EXAMPLE_EXIO_PIN_SYS_EN | EXAMPLE_EXIO_PIN_NS_MODE, IO_EXPANDER_OUTPUT));
    ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander, EXAMPLE_EXIO_PIN_BL_EN, 0));
    ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander, EXAMPLE_EXIO_PIN_LCD_RST, 1));
    ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander, EXAMPLE_EXIO_PIN_SYS_EN, 1));   // keep board powered
    ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander, EXAMPLE_EXIO_PIN_NS_MODE, 0));  // speaker amp off until a sound plays
}

static void example_lcd_reset(void)
{
    ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander, EXAMPLE_EXIO_PIN_LCD_RST, 1));
    vTaskDelay(pdMS_TO_TICKS(30));
    ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander, EXAMPLE_EXIO_PIN_LCD_RST, 0));
    vTaskDelay(pdMS_TO_TICKS(250));
    ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander, EXAMPLE_EXIO_PIN_LCD_RST, 1));
    vTaskDelay(pdMS_TO_TICKS(30));
}

static void example_lcd_backlight_set(bool enable)
{
    ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander, EXAMPLE_EXIO_PIN_BL_EN, enable ? 1 : 0));
}

extern "C" void app_main(void)
{
    example_lcd_pwm_off_early();
    // landscape reader rotates into this buffer; allocate always (same pixel count as portrait: 172*640 = 640*172)
    rotat_ptr = (uint16_t*)heap_caps_malloc(EXAMPLE_LCD_H_RES * EXAMPLE_LCD_V_RES * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    assert(rotat_ptr);
    lvgl_flush_semap = xSemaphoreCreateBinary();
    i2c_master_Init();
    example_lcd_exio_init();
    static lv_disp_draw_buf_t disp_buf; // contains internal graphic buffer(s) called draw buffer(s)
    static lv_disp_drv_t disp_drv;      // contains callback functions

    ESP_LOGI(TAG, "Initialize QSPI bus");
    spi_bus_config_t buscfg = {};
        buscfg.data0_io_num = EXAMPLE_PIN_NUM_LCD_DATA0;
        buscfg.data1_io_num = EXAMPLE_PIN_NUM_LCD_DATA1;
        buscfg.sclk_io_num = EXAMPLE_PIN_NUM_LCD_PCLK;
        buscfg.data2_io_num = EXAMPLE_PIN_NUM_LCD_DATA2;
        buscfg.data3_io_num = EXAMPLE_PIN_NUM_LCD_DATA3;
        buscfg.max_transfer_sz = LVGL_DMA_BUFF_LEN;
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    ESP_LOGI(TAG, "Install panel IO");
	    esp_lcd_panel_io_handle_t panel_io = NULL;
        esp_lcd_panel_handle_t panel = NULL;
    
    esp_lcd_panel_io_spi_config_t io_config = {};
		io_config.cs_gpio_num = EXAMPLE_PIN_NUM_LCD_CS;                 
        io_config.dc_gpio_num = -1;          
        io_config.spi_mode = 3;              
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;    
        io_config.on_color_trans_done = example_notify_lvgl_flush_ready; 
        //io_config.user_ctx = &disp_drv,         
        io_config.lcd_cmd_bits = 32;         
        io_config.lcd_param_bits = 8;        
        io_config.flags.quad_mode = true;                         
	ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(LCD_HOST, &io_config, &panel_io));
    
	axs15231b_vendor_config_t vendor_config = {};
        vendor_config.flags.use_qspi_interface = 1;
        vendor_config.init_cmds = lcd_init_cmds;
        vendor_config.init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]);
    
    esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = -1;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = LCD_BIT_PER_PIXEL;
        panel_config.vendor_config = &vendor_config;
    
    ESP_LOGI(TAG, "Install panel driver");
    ESP_ERROR_CHECK(esp_lcd_new_panel_axs15231b(panel_io, &panel_config, &panel));

	example_lcd_reset();
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    lcd_bl_pwm_bsp_init(LCD_PWM_MODE_255);
    example_lcd_backlight_set(true);

  	lv_init();
  	lvgl_dma_buf = (uint16_t *)heap_caps_malloc(LVGL_DMA_BUFF_LEN , MALLOC_CAP_DMA);
  	assert(lvgl_dma_buf);
  	lv_color_t *buffer_1 = (lv_color_t *)heap_caps_malloc(LVGL_SPIRAM_BUFF_LEN , MALLOC_CAP_SPIRAM);
    lv_color_t *buffer_2 = (lv_color_t *)heap_caps_malloc(LVGL_SPIRAM_BUFF_LEN , MALLOC_CAP_SPIRAM);
  	assert(buffer_1);
    assert(buffer_2);
  	lv_disp_draw_buf_init(&disp_buf, buffer_1, buffer_2, EXAMPLE_LCD_H_RES * EXAMPLE_LCD_V_RES);

  	ESP_LOGI(TAG, "Register display driver to LVGL");
  	lv_disp_drv_init(&disp_drv);
  	disp_drv.hor_res = EXAMPLE_LCD_H_RES;
  	disp_drv.ver_res = EXAMPLE_LCD_V_RES;
  	disp_drv.flush_cb = example_lvgl_flush_cb;
  	disp_drv.draw_buf = &disp_buf;
  	disp_drv.full_refresh = 1;          //full_refresh must be 1
  	disp_drv.user_data = panel;
  	lv_disp_drv_register(&disp_drv);
	
  	ESP_LOGI(TAG, "Install LVGL tick timer");
  	esp_timer_create_args_t lvgl_tick_timer_args = {};
  	    lvgl_tick_timer_args.callback = &example_increase_lvgl_tick;
  	    lvgl_tick_timer_args.name = "lvgl_tick";
  	esp_timer_handle_t lvgl_tick_timer = NULL;
  	ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
  	ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer,EXAMPLE_LVGL_TICK_PERIOD_MS * 1000));

  	static lv_indev_drv_t indev_drv;    // Input device driver (Touch)
  	lv_indev_drv_init(&indev_drv);
  	indev_drv.type = LV_INDEV_TYPE_POINTER;
  	indev_drv.read_cb = example_lvgl_touch_cb;
  	lv_indev_drv_register(&indev_drv);

  	lvgl_mux = xSemaphoreCreateMutex();
  	assert(lvgl_mux);
  	xTaskCreatePinnedToCore(example_lvgl_port_task, "LVGL", 4000, NULL, 4, NULL,0); //运行于内核_0
  	xTaskCreatePinnedToCore(example_backlight_loop_task, "example_backlight_loop_task", 4 * 1024, NULL, 2, NULL,0); 
    battery_init();         // ADC1 for the battery gauge (independent of LVGL/WiFi; safe to init early)
    net_preload_config();   // load saved config so the Settings switches reflect real state at build time
    if (example_lvgl_lock(-1))
  	{
  	  	ui_init();
  	  	net_start();
  	  	audio_init();
  	  	example_lvgl_unlock();
  	}
    // apply saved standby/sound prefs (config already loaded by net_preload_config)
    bool auto_standby = true, sound_en = true;
    net_get_prefs(&auto_standby, &sound_en);
    app_set_auto_standby(auto_standby);
    audio_set_muted(!sound_en);
}
static void example_lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    (void)area;
    // Runtime rotation. Portrait: draw the buffer as-is. Landscape (LVGL renders a 640x172 logical frame
    // with sw_rotate=0 + ROT_90): rotate it into the 172x640 physical panel. Uses the FIXED panel dims
    // (LCD_NOROT_*), so the transform is correct regardless of the compile-time `Rotated` default and the
    // portrait path stays byte-identical to before.
    // Screen asleep: don't push pixels over QSPI (saves the redraw traffic + backlight is already off).
    // LVGL still ran its layout; we just ack the flush so it doesn't stall. A wake re-invalidates the screen.
    if (g_display_off) { lv_disp_flush_ready(drv); return; }
    uint16_t *map;
    if (g_landscape) {
        uint16_t *src = (uint16_t *)color_map;
        uint32_t index = 0;
        for (int j = 0; j < LCD_NOROT_VRES; j++)
            for (int i = 0; i < LCD_NOROT_HRES; i++)
                rotat_ptr[index++] = src[LCD_NOROT_VRES * (LCD_NOROT_HRES - 1 - i) + j];
        map = rotat_ptr;
    } else {
        map = (uint16_t *)color_map;
    }
    esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t) drv->user_data;
    const int flush_coun = (LVGL_SPIRAM_BUFF_LEN / LVGL_DMA_BUFF_LEN);
    const int offgap = (LCD_NOROT_VRES / flush_coun);
    const int dmalen = (LVGL_DMA_BUFF_LEN / 2);
    int offsetx1 = 0;
    int offsety1 = 0;
    int offsetx2 = LCD_NOROT_HRES;
    int offsety2 = offgap;

    xSemaphoreGive(lvgl_flush_semap);
    for(int i = 0; i<flush_coun; i++)
    {
        xSemaphoreTake(lvgl_flush_semap,portMAX_DELAY);
        memcpy(lvgl_dma_buf,map,LVGL_DMA_BUFF_LEN);
        esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, offsety1, offsetx2, offsety2, lvgl_dma_buf);
        offsety1 += offgap;
        offsety2 += offgap;
        map += dmalen;
    }
    xSemaphoreTake(lvgl_flush_semap,portMAX_DELAY);
    lv_disp_flush_ready(drv);
}
static bool example_lvgl_lock(int timeout_ms)
{
    const TickType_t timeout_ticks = (timeout_ms == -1) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTake(lvgl_mux, timeout_ticks) == pdTRUE;       
}

static void example_lvgl_unlock(void)
{
    assert(lvgl_mux && "bsp_display_start must be called first");
    xSemaphoreGive(lvgl_mux);
}

void example_lvgl_port_task(void *arg)
{
    uint32_t task_delay_ms = EXAMPLE_LVGL_TASK_MAX_DELAY_MS;
    for(;;)
    {
        if (example_lvgl_lock(-1)) 
        {
            task_delay_ms = lv_timer_handler();
            //Release the mutex
            example_lvgl_unlock();
        }
        if (task_delay_ms > EXAMPLE_LVGL_TASK_MAX_DELAY_MS)
        {
            task_delay_ms = EXAMPLE_LVGL_TASK_MAX_DELAY_MS;
        } else if (task_delay_ms < EXAMPLE_LVGL_TASK_MIN_DELAY_MS)
        {
            task_delay_ms = EXAMPLE_LVGL_TASK_MIN_DELAY_MS;
        }
        vTaskDelay(pdMS_TO_TICKS(task_delay_ms));
    }
}

static void example_lvgl_touch_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    //static uint8_t read_touchpad_cmd[8] = {0xb5, 0xab, 0xa5, 0x5a, 0x0, 0x0, 0x0, 0x8};
    uint8_t read_touchpad_cmd[11] = {0xb5, 0xab, 0xa5, 0x5a, 0x0, 0x0, 0x0, 0x0e,0x0, 0x0, 0x0};
    uint8_t buff[32] = {0};
    memset(buff,0,32);
    ESP_ERROR_CHECK_WITHOUT_ABORT(i2c_master_write_read_dev(disp_touch_dev_handle,read_touchpad_cmd,11,buff,32));
    uint16_t pointX;
    uint16_t pointY;
    pointX = (((uint16_t)buff[2] & 0x0f) << 8) | (uint16_t)buff[3];
    pointY = (((uint16_t)buff[4] & 0x0f) << 8) | (uint16_t)buff[5];
    //ESP_LOGI("Touch","%d,%d",buff[0],buff[1]);
    if (buff[1]>0 && buff[1]<5)
    {
        // Locked: ignore touch entirely — no wake, no tap. Only a BOOT long-press unlocks.
        if (g_locked) { data->state = LV_INDEV_STATE_REL; return; }
        // A touch is a user interaction: keep the screen awake / wake it. The gesture that wakes the deck
        // is swallowed (reported as released) until the finger lifts, so it can't also land on a button.
        if (g_display_off) { app_note_user_activity(); s_eat_touch = true; data->state = LV_INDEV_STATE_REL; return; }
        app_note_user_activity();
        if (s_eat_touch) { data->state = LV_INDEV_STATE_REL; return; }
        data->state = LV_INDEV_STATE_PR;
        // Native touch coords: long axis (0..640) in pointX, short axis (0..172) in pointY. Map to LVGL
        // logical coords for the CURRENT rotation (fixed panel dims; matches the reference portrait/landscape).
        if (pointX > LCD_NOROT_VRES) pointX = LCD_NOROT_VRES;
        if (pointY > LCD_NOROT_HRES) pointY = LCD_NOROT_HRES;
        if (!g_landscape) {   // portrait
            data->point.x = pointY;
            data->point.y = (LCD_NOROT_VRES - pointX);
        } else {              // landscape (640x172)
            data->point.x = (LCD_NOROT_VRES - pointX);
            data->point.y = (LCD_NOROT_HRES - pointY);
        }
    }
    else
    {
        s_eat_touch = false;   // finger lifted: the next press is a real tap again
        data->state = LV_INDEV_STATE_REL;
    }
}

// Put the screen to sleep / wake it. We blank with the BACKLIGHT ONLY (BL_EN + PWM duty) — the LED backlight
// is the dominant draw, and this is the proven path (same as brightness control). We deliberately do NOT send
// the panel a DISPOFF: this AXS15231B driver's disp_on_off() polarity left the panel dark on wake, and a panel
// that never gets DISPOFF always has a valid image to show the instant the backlight returns. The flush_cb
// skip still saves the SPI redraw traffic while off. MUST run under the LVGL lock (mutually exclusive with
// flush_cb) — the power task holds it around every call.
static void display_set_off(bool off)
{
    if (off == g_display_off) return;
    if (off) {
        setUpduty(LCD_PWM_MODE_0);                     // duty 255 -> backlight dark
        example_lcd_backlight_set(false);              // BL_EN low
        g_display_off = true;
        ESP_LOGI(TAG, "screen -> standby");
    } else {
        g_display_off = false;                         // let flush_cb draw again before we invalidate
        example_lcd_backlight_set(true);               // BL_EN high
        setUpduty(s_cur_duty);                         // restore last brightness
        lv_obj_invalidate(lv_scr_act());               // force a full repaint (in case UI changed while off)
        ESP_LOGI(TAG, "screen -> awake");
    }
}

// BOOT short-press while awake: step to the next backlight level (wraps).
static void cycle_brightness(void)
{
    s_bright_idx = (s_bright_idx + 1) % (int)(sizeof(s_bright_levels) / sizeof(s_bright_levels[0]));
    s_cur_duty = s_bright_levels[s_bright_idx];
    setUpduty(s_cur_duty);
}

// Poll the BOOT button (GPIO0, active-low). Short press = cycle brightness (or just wake if asleep);
// long press = toggle standby. Also runs the idle-timeout auto screen-off and services wake/sleep
// requests posted by other threads. This one task owns every screen power transition.
static void example_backlight_loop_task(void *arg)
{
    gpio_config_t io = {};
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    io.pin_bit_mask = (1ULL << EXAMPLE_PIN_NUM_BOOT0);
    gpio_config(&io);

    bool pressed = false, fired_long = false;
    int64_t press_us = 0;
    s_last_activity_us = esp_timer_get_time();
    int64_t next_bat_us = 0;   // read the battery immediately on the first pass, then every 10s

    for (;;)
    {
        // --- battery gauge (cheap ADC read every ~10s; keeps updating while asleep, flush is skipped) ---
        {
            int64_t t = esp_timer_get_time();
            if (t >= next_bat_us) {
                float v; int pct; bool chg;
                bool ok = battery_read(&v, &pct, &chg) && ui_set_battery(pct, chg, true);
                next_bat_us = t + (ok ? 10 * 1000 * 1000 : 500 * 1000);  // retry soon until ADC+UI are both up
            }
        }

        // --- BOOT button (debounced by the 30ms poll cadence) ---
        bool down = (gpio_get_level(EXAMPLE_PIN_NUM_BOOT0) == 0);
        int64_t now = esp_timer_get_time();
        if (down && !pressed) {                        // edge: press begins
            pressed = true; fired_long = false; press_us = now;
        } else if (down && pressed) {                  // held
            if (!fired_long && (now - press_us) >= BOOT_LONGPRESS_MS * 1000) {
                fired_long = true;                     // long press: toggle lock (fires once mid-hold)
                s_lock_toggle_req = true;
                s_last_activity_us = now;
            }
        } else if (!down && pressed) {                 // edge: release
            pressed = false;
            if (!fired_long) {                         // short press
                if (g_locked) { /* locked: ignore — only a long-press unlocks */ }
                else if (g_display_off) s_wake_req = true;                 // asleep -> just wake
                else { cycle_brightness(); s_last_activity_us = now; }     // awake -> next brightness
            }
        }

        // --- service wake/sleep requests + idle auto-standby (all panel work under the LVGL lock) ---
        // Consume the flags atomically: a concurrent app_wake_for_event() set right here (e.g. an incoming
        // question) must never be silently dropped, or the screen would stay dark with an ask pending.
        bool want_wake  = __atomic_exchange_n(&s_wake_req, false, __ATOMIC_SEQ_CST);
        bool want_lock  = __atomic_exchange_n(&s_lock_toggle_req, false, __ATOMIC_SEQ_CST);
        bool want_sleep = false;                       // set only by the auto-standby idle timer below

        // Lock toggle (BOOT long-press). Locking shows the "Locked" notice over the current screen and arms a
        // deadline to blank it (non-blocking, so the button keeps polling). Unlocking drops the notice and
        // wakes below. ui_show_lock_notice takes the LVGL lock itself, so it's called WITHOUT the lock held.
        if (want_lock) {
            if (!g_locked) {                               // -> lock
                g_locked = true;
                ui_show_lock_notice(true);                 // "Locked" flash over the current (still-lit) screen
                s_lock_blank_at = now + (int64_t)LOCK_FLASH_MS * 1000;   // keep it visible, then blank
                want_wake = false;
            } else {                                       // -> unlock
                g_locked = false;                          // stop dropping touch / ignoring events immediately
                s_lock_blank_at = 0;
                ui_show_lock_notice(false);                // drop the notice
                want_wake = true;                          // wake the (now unlocked) screen below
            }
        }

        if (g_locked) {
            // Locked: screen is dark EXCEPT (a) for LOCK_FLASH_MS right after locking, and (b) while BOOT is
            // held — so pressing to unlock lights the screen immediately (feedback while the long-press
            // completes). Touch + events stay ignored throughout. Level-driven: recompute the wanted state
            // each tick and only transition on a mismatch.
            if (s_lock_blank_at && now >= s_lock_blank_at) s_lock_blank_at = 0;
            bool lit = pressed || (s_lock_blank_at != 0);
            if (lit == g_display_off) {                    // current state disagrees with the wanted one
                if (ui_lock(1000)) { display_set_off(!lit); ui_unlock(); }
            }
        } else {
            // Unlocked: normal wake-on-request + idle auto-standby.
            if (!want_wake && s_auto_standby && !g_display_off &&
                !ui_has_pending_ask() && (now - s_last_activity_us) >= (int64_t)STANDBY_IDLE_MS * 1000) {
                want_sleep = true;
            }
            if (want_wake || want_sleep) {
                if (ui_lock(1000)) {
                    if (want_wake) { display_set_off(false); s_last_activity_us = now; }
                    else           { display_set_off(true); }
                    ui_unlock();
                } else if (want_wake) {
                    s_wake_req = true;                     // couldn't lock now; retry next tick
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

// --- activity / standby hooks (callable from any task; only post flags, never touch the panel) ---
extern "C" void app_note_user_activity(void) {
    if (g_locked) return;                              // locked: ignore all activity until unlocked
    s_last_activity_us = esp_timer_get_time();
    if (g_display_off) s_wake_req = true;
}
extern "C" void app_wake_for_event(void) {
    if (g_locked) return;                              // locked: events don't wake the screen
    s_last_activity_us = esp_timer_get_time();
    if (g_display_off) s_wake_req = true;
}
extern "C" void app_set_auto_standby(bool enabled) {
    s_auto_standby = enabled;
    s_last_activity_us = esp_timer_get_time();         // fresh grace period when (re)enabling
}

// Bridge between net/ui modules and the LVGL mutex owned by this translation unit.
extern "C" bool ui_lock(int timeout_ms) { return example_lvgl_lock(timeout_ms); }

// Toggle the display between native portrait and landscape (for the reader). Call under the LVGL lock.
// NOTE: we deliberately do NOT use lv_disp_set_rotation() — that also makes LVGL rotate the touch input
// (lv_indev.c), which would double-transform on top of our own touch_cb rotation. Instead we swap only the
// logical resolution (rotated stays NONE), so LVGL lays out landscape while our flush_cb + touch_cb own the
// rotation consistently. Buffer is fine either way: 172*640 == 640*172 pixels.
extern "C" void app_set_rotation(bool landscape) {
    g_landscape = landscape;
    lv_disp_t *disp = lv_disp_get_default();
    if (!disp) return;
    disp->driver->hor_res = landscape ? LCD_NOROT_VRES : LCD_NOROT_HRES;   // 640 or 172
    disp->driver->ver_res = landscape ? LCD_NOROT_HRES : LCD_NOROT_VRES;   // 172 or 640
    lv_disp_drv_update(disp, disp->driver);
}
extern "C" void ui_unlock(void) { example_lvgl_unlock(); }
extern "C" void app_io_set_pa(bool on) {
    if (io_expander) esp_io_expander_set_level(io_expander, EXAMPLE_EXIO_PIN_NS_MODE, on ? 1 : 0);
}
