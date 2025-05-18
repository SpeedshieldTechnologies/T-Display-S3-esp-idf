#include <stdio.h>
#include <sys/types.h>
#include <time.h>
#include "string.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "nvs_flash.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_err.h"
#include "esp_log.h"
#include "lvgl.h"
#include "lvgl_setup.h"
#include "hx711.h"


void display_timer_cb(lv_timer_t * display_timer);
lv_color_t convert_rgb888_colour(uint8_t R, uint8_t G, uint8_t B);


static const char *TAG = "main";
extern lv_obj_t *weight_label;
extern lv_obj_t *countdown_label;
extern lv_obj_t *state_label;
extern lv_obj_t *meter_weight, *meter_time;
extern lv_meter_indicator_t *indic_weight, *indic_acceptance_band;
lv_timer_t *display_timer;
float weight_kg;
float weight_tare_offset = 0.0;

//supplier of Sekisui 5225SXB has advised 0.5MPa for 15 seconds, equates to 30kg for our surface area. We will exceed this.
const int weight_lwr_band = 45.0;
const int weight_upr_band = 65.0;
const int weight_scale_max = 70.0;
//const uint16_t weight_ticks = 8;
const int weight_tare_limit = 3.0;
const int retare_time_seconds = 10;
const int weight_hold_time = 15;

lv_color_t idle_colour;
lv_color_t fail_colour;
lv_color_t wait_colour;
lv_color_t pass_colour;

typedef enum  {
    state_init,
    state_idle,
    state_out_of_range,
    state_in_range,
    state_completed
} weigh_state_t;

weigh_state_t state = state_idle;
time_t tare_time;
time_t countdown_time;

hx711_t hx711dev = {
    1, // dout
    2, // pd_sck
    HX711_GAIN_A_128
};

lv_color_t convert_rgb888_colour(uint8_t R, uint8_t G, uint8_t B)
{
    // convert 24 bit colour into the BBB RRRRRRR GGGGGG colour mapping that is used here.
    uint32_t colour;

    colour = (uint32_t)(R>>1)<<6;
    colour += (uint32_t)(G>>2);
    colour += (uint32_t)(B>>5)<<13;
    return lv_color_hex(colour);
}

void display_timer_cb(lv_timer_t * timer)
{
    char txt[30];
    time_t now, remaining;

    sprintf(txt, "%.1fkg\n",weight_kg);
    
    lv_label_set_text(weight_label, txt);
    lv_meter_set_indicator_start_value(meter_weight, indic_weight, 0);
    lv_meter_set_indicator_end_value(meter_weight, indic_weight, weight_kg);
    if (weight_kg >= weight_lwr_band && weight_kg < weight_upr_band)
        lv_obj_set_style_text_color(weight_label, lv_color_hex3(0x00F), LV_PART_MAIN | LV_STATE_DEFAULT); 
    else
        lv_obj_set_style_text_color(weight_label, lv_color_hex3(0x0F0), LV_PART_MAIN | LV_STATE_DEFAULT); 

    // run the state machine
    switch (state) {
        case state_init:
            lv_label_set_text(state_label, "Load parts");
            lv_obj_set_style_bg_color(lv_scr_act(), idle_colour, LV_PART_MAIN);
            state = state_idle;
            break;

        case state_idle:
            if (weight_kg >= weight_tare_limit) {
                lv_label_set_text(state_label, "Out of range");
                lv_obj_set_style_bg_color(lv_scr_act(),fail_colour,LV_PART_MAIN);
                state = state_out_of_range;
            }
            break;

        case state_out_of_range:
            if (weight_kg >= weight_lwr_band && weight_kg < weight_upr_band) {
                lv_label_set_text(state_label, "Please wait");
                lv_obj_set_style_bg_color(lv_scr_act(),wait_colour,LV_PART_MAIN);
                state = state_in_range;
                lv_obj_clear_flag(countdown_label, LV_OBJ_FLAG_HIDDEN);
                time(&now);
                countdown_time = now+15;
            } else if (weight_kg < weight_tare_limit)
                state = state_init;
            break;

        case state_in_range:
            // check how long we've been in range
            if (weight_kg < weight_lwr_band || weight_kg >= weight_upr_band) {
                lv_label_set_text(state_label, "out of range");
                lv_obj_set_style_bg_color(lv_scr_act(),fail_colour,LV_PART_MAIN);
                lv_obj_add_flag(countdown_label, LV_OBJ_FLAG_HIDDEN);
                state = state_out_of_range;
            } else {
                // display a countdown timer
               time(&now);
               remaining = countdown_time - now;
               if (remaining < 0) {
                    lv_label_set_text(state_label, "Remove parts");
                    lv_label_set_text_fmt(countdown_label, "[done]");
                    lv_obj_set_style_bg_color(lv_scr_act(),pass_colour,LV_PART_MAIN);    // this is a nice deep green.
                    state = state_completed;

               } else
                lv_label_set_text_fmt(countdown_label, "%llds", remaining);
            }
            break;

        case state_completed:
            if (weight_kg < weight_tare_limit) {
                state = state_init;
                lv_obj_add_flag(countdown_label, LV_OBJ_FLAG_HIDDEN);
            }
            break;
    }
}



void weight_task(void *pvParameter)
{
    int32_t data;
    static bool first_run = true;
    time_t now;


    while(1) {
        hx711_read_average(&hx711dev, 4, &data);
        weight_kg = 4.64689e-5 *data + 1.17795;
        if (first_run) {
            first_run = false;
            time(&tare_time);
            if (weight_kg < weight_tare_limit)
                weight_tare_offset = weight_kg;
        } else {
            if (weight_kg < weight_tare_limit) {
                // re-tare the unit
                // removing this for now, as I am concerned that leaving parts on the tray is less weight than the retare limit, so may cause problems.
                // time(&now);
                // if (difftime(now, tare_time) >=0) {
                //     weight_tare_offset = weight_kg;
                //     time(&tare_time);
                // }
            }
        }
        weight_kg -= weight_tare_offset;
        // UI interactions like displaying weight are handled in an lv_timer callback
        vTaskDelay(50 / portTICK_PERIOD_MS);
    }
}


void app_main(void)
{
    esp_err_t ret;
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    /* Print chip information */
    esp_chip_info_t chip_info;
    uint32_t flash_size;
    esp_chip_info(&chip_info);
    printf("This is %s chip with %d CPU core(s), WiFi%s%s, ",
           CONFIG_IDF_TARGET,
           chip_info.cores,
           (chip_info.features & CHIP_FEATURE_BT) ? "/BT" : "",
           (chip_info.features & CHIP_FEATURE_BLE) ? "/BLE" : "");

    unsigned major_rev = chip_info.revision / 100;
    unsigned minor_rev = chip_info.revision % 100;
    printf("silicon revision v%d.%d, ", major_rev, minor_rev);
    if(esp_flash_get_size(NULL, &flash_size) != ESP_OK) {
        printf("Get flash size failed");
        return;
    }

    printf("%" PRIu32 "MB %s flash\n", flash_size / (uint32_t)(1024 * 1024),
           (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "embedded" : "external");

    printf("Minimum free heap size: %" PRIu32 " bytes\n", esp_get_minimum_free_heap_size());

    bool ready;
    printf("initialising HX711\n");
    hx711_init(&hx711dev);
    hx711_wait(&hx711dev, 1000);
    hx711_is_ready(&hx711dev, &ready);
    if (ready)
        printf("HX711 is ready\n");
    else
        printf("failed to initalise HX711\n");

    
    lvgl_setup();
    bsp_display_lock(0);

    //https://vuetifyjs.com/en/styles/colors/#material-colors
    //0x001000 is a nice deep red (have to convert back to rgb888)
    //0x000008 is a nice deep green (have to convert back to rgb888)
    idle_colour = convert_rgb888_colour(0xFF, 0xe0, 0xb2);  // orange-lighten-4
    fail_colour = convert_rgb888_colour(0xb7, 0x1c, 0x1c);  // red-darken-4
    wait_colour = convert_rgb888_colour(0xe6, 0x51, 0x00);  // orange-darken-3
    pass_colour = convert_rgb888_colour(0x1b, 0x5e, 0x20);  // green-darken-4

    build_display();
    state = state_init;
    display_timer = lv_timer_create(display_timer_cb, 50, NULL);
   
    bsp_display_unlock();


    xTaskCreate(&weight_task, "weight_task", 4096,NULL,1,NULL );

}





