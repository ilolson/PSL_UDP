#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
/* Define the CYW43 architecture header before pulling in the SDK headers */
#define PICO_CYW43_ARCH_HEADER pico/cyw43_arch/arch_threadsafe_background.h

#define LED_PIN 0
#define NUM_LEDS 16
#define PACKET_BUFFER 128
#define BLE_DEVICE_NAME "PSL Motion"
#define BLE_DEVICE_NAME_LEN (sizeof(BLE_DEVICE_NAME) - 1)

#define MIN_BRIGHTNESS_NORMALIZED 0.05f
#define MAX_BRIGHTNESS_NORMALIZED (100.0f / 255.0f)

static const uint8_t PSL_BLE_SERVICE_UUID[16] = {
    0x21, 0x43, 0x65, 0x87, 0xa9, 0xcb, 0xed, 0x0f,
    0x10, 0x32, 0x54, 0x76, 0x98, 0xba, 0xdc, 0xfe
};
static const uint8_t PSL_COMMAND_CHAR_UUID[16] = {
    0x0c, 0x1d, 0x2e, 0x3f, 0x40, 0x51, 0x62, 0x73,
    0x84, 0x95, 0xa6, 0xb7, 0xc8, 0xd9, 0xea, 0xfb
};
static uint16_t ble_command_value_handle = 0;

static uint16_t segment_start = 0;
static uint16_t segment_end = NUM_LEDS - 1;

static void render_color_from_state(void);

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"

#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/watchdog.h"

#include "btstack.h"
#include "ble/att_db.h"
#include "ble/att_db_util.h"
#include "ble/att_server.h"

#include "ws2812.pio.h"

static PIO led_pio = pio0;
static uint led_sm = 0;
static uint led_offset = 0;

static inline float clampf(float value, float min, float max) {
    if (value < min) {
        return min;
    }
    if (value > max) {
        return max;
    }
    return value;
}

static void hsv_to_rgb(float h, float s, float v, uint8_t *r, uint8_t *g, uint8_t *b) {
    h = fmodf(h, 360.0f);
    if (h < 0.0f) {
        h += 360.0f;
    }

    float c = v * s;
    float x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
    float m = v - c;
    float r1 = 0.0f;
    float g1 = 0.0f;
    float b1 = 0.0f;

    if (h < 60.0f) {
        r1 = c;
        g1 = x;
    } else if (h < 120.0f) {
        r1 = x;
        g1 = c;
    } else if (h < 180.0f) {
        g1 = c;
        b1 = x;
    } else if (h < 240.0f) {
        g1 = x;
        b1 = c;
    } else if (h < 300.0f) {
        r1 = x;
        b1 = c;
    } else {
        r1 = c;
        b1 = x;
    }

    *r = (uint8_t)clampf((r1 + m) * 255.0f, 0.0f, 255.0f);
    *g = (uint8_t)clampf((g1 + m) * 255.0f, 0.0f, 255.0f);
    *b = (uint8_t)clampf((b1 + m) * 255.0f, 0.0f, 255.0f);
}

static void ws2812_init(void) {
    led_offset = pio_add_program(led_pio, &ws2812_program);
    ws2812_program_init(led_pio, led_sm, led_offset, LED_PIN, 800000.0f, false);
}

static void ws2812_write_color(uint32_t grb) {
    for (uint i = 0; i < NUM_LEDS; ++i) {
        uint32_t color = (i >= segment_start && i <= segment_end) ? grb : 0;
        pio_sm_put_blocking(led_pio, led_sm, color << 8u);
    }
}

static void clamp_segment_bounds(void) {
    if (segment_start >= NUM_LEDS) {
        segment_start = NUM_LEDS - 1;
    }
    if (segment_end >= NUM_LEDS) {
        segment_end = NUM_LEDS - 1;
    }
    if (segment_start > segment_end) {
        segment_end = segment_start;
    }
}

static void set_segment_start(uint32_t start) {
    uint32_t index = start;
    if (index >= NUM_LEDS) {
        index = NUM_LEDS - 1;
    }
    segment_start = (uint16_t)index;
    clamp_segment_bounds();
    render_color_from_state();
}

static void set_segment_end(uint32_t end) {
    uint32_t index = end;
    if (index >= NUM_LEDS) {
        index = NUM_LEDS - 1;
    }
    segment_end = (uint16_t)index;
    clamp_segment_bounds();
    render_color_from_state();
}

static float current_hue = 210.0f;
static float current_saturation = 0.5f;
static float current_brightness = 0.5f;
static float hue_offset = 0.0f;
static float brightness_offset = 0.0f;

static void render_color_from_state(void) {
    float adjusted_hue = fmodf(current_hue + hue_offset, 360.0f);
    if (adjusted_hue < 0.0f) {
        adjusted_hue += 360.0f;
    }
    float adjusted_brightness = clampf(
        current_brightness + brightness_offset,
        MIN_BRIGHTNESS_NORMALIZED,
        MAX_BRIGHTNESS_NORMALIZED
    );

    uint8_t r, g, b;
    hsv_to_rgb(adjusted_hue, current_saturation, adjusted_brightness, &r, &g, &b);
    uint32_t color = ((uint32_t)g << 16) | ((uint32_t)r << 8) | b;
    ws2812_write_color(color);
}

static void set_hue(float degrees) {
    float normalized = fmodf(degrees, 360.0f);
    if (normalized < 0.0f) {
        normalized += 360.0f;
    }
    current_hue = normalized;
    hue_offset = 0.0f;
    render_color_from_state();
}

static void set_brightness(float percent) {
    float normalized = percent / 100.0f;
    current_brightness = clampf(
        normalized,
        MIN_BRIGHTNESS_NORMALIZED,
        MAX_BRIGHTNESS_NORMALIZED
    );
    brightness_offset = 0.0f;
    render_color_from_state();
}

static void reset_system(void) {
    watchdog_reboot(0, 0, 0);
}

static void render_motion_color(float pitch, float roll, float yaw) {
    float norm_roll = clampf((roll + 3.14159f) / (2.0f * 3.14159f), 0.0f, 1.0f);
    float norm_yaw = clampf((yaw + 3.14159f) / (2.0f * 3.14159f), 0.0f, 1.0f);
    float norm_pitch = clampf((pitch + (3.14159f / 2.0f)) / 3.14159f, 0.0f, 1.0f);
    float hue = fmodf(norm_yaw * 360.0f + norm_roll * 120.0f, 360.0f);
    float saturation = clampf(0.35f + norm_roll * 0.65f, 0.2f, 1.0f);
    float brightness = clampf(
        0.2f + norm_pitch * 0.8f,
        MIN_BRIGHTNESS_NORMALIZED,
        MAX_BRIGHTNESS_NORMALIZED
    );

    current_hue = hue;
    current_saturation = saturation;
    current_brightness = brightness;
    render_color_from_state();
}

static void adjust_hue(float delta) {
    hue_offset = fmodf(hue_offset + delta, 360.0f);
    if (hue_offset < 0.0f) {
        hue_offset += 360.0f;
    }
    render_color_from_state();
}

static void adjust_brightness(float delta) {
    float desired = clampf(
        current_brightness + brightness_offset + delta,
        MIN_BRIGHTNESS_NORMALIZED,
        MAX_BRIGHTNESS_NORMALIZED
    );
    brightness_offset = desired - current_brightness;
    render_color_from_state();
}

static void handle_motion_packet(const char *packet, size_t len) {
    char buffer[PACKET_BUFFER];
    size_t copy_len = len < sizeof(buffer) - 1 ? len : sizeof(buffer) - 1;
    memcpy(buffer, packet, copy_len);
    buffer[copy_len] = '\0';

    float pitch = 0.0f;
    float roll = 0.0f;
    float yaw = 0.0f;
    float delta = 0.0f;
    if (strncmp(buffer, "RESET", 5) == 0) {
        reset_system();
        return;
    }
    if (sscanf(buffer, "H_SET,%f", &delta) == 1) {
        set_hue(delta);
        return;
    }
    if (sscanf(buffer, "B_SET,%f", &delta) == 1) {
        set_brightness(delta);
        return;
    }
    if (sscanf(buffer, "H,%f", &delta) == 1) {
        adjust_hue(delta);
        return;
    }
    if (sscanf(buffer, "B,%f", &delta) == 1) {
        adjust_brightness(delta);
        return;
    }
    unsigned long segment_idx = 0;
    if (sscanf(buffer, "SEG_START,%lu", &segment_idx) == 1) {
        set_segment_start(segment_idx > 0 ? segment_idx - 1 : 0);
        return;
    }
    if (sscanf(buffer, "SEG_END,%lu", &segment_idx) == 1) {
        set_segment_end(segment_idx > 0 ? segment_idx - 1 : 0);
        return;
    }
    if (sscanf(buffer, "%f,%f,%f", &pitch, &roll, &yaw) == 3) {
        render_motion_color(pitch, roll, yaw);
    }
}

static int ble_command_write_callback(hci_con_handle_t con_handle, uint16_t attribute_handle,
                                      uint16_t transaction_mode, uint16_t offset, uint8_t *buffer, uint16_t buffer_size) {
    (void)con_handle;
    (void)offset;

    if (attribute_handle != ble_command_value_handle) {
        return ATT_ERROR_ATTRIBUTE_NOT_FOUND;
    }
    if (transaction_mode != ATT_TRANSACTION_MODE_NONE || !buffer || buffer_size == 0) {
        return 0;
    }

    char payload[PACKET_BUFFER];
    size_t copy_len = buffer_size < sizeof(payload) - 1 ? buffer_size : sizeof(payload) - 1;
    memcpy(payload, buffer, copy_len);
    payload[copy_len] = '\0';

    handle_motion_packet(payload, copy_len);
    return 0;
}

static void init_ble_service(void) {
    l2cap_init();
    sm_init();

    att_db_util_init();
    att_db_util_add_service_uuid128(PSL_BLE_SERVICE_UUID);
    ble_command_value_handle = att_db_util_add_characteristic_uuid128(
        PSL_COMMAND_CHAR_UUID,
        ATT_PROPERTY_WRITE | ATT_PROPERTY_WRITE_WITHOUT_RESPONSE,
        ATT_SECURITY_NONE,
        ATT_SECURITY_NONE,
        NULL,
        0);
    att_server_init(att_db_util_get_address(), NULL, ble_command_write_callback);

    uint8_t adv_data[21];
    size_t adv_index = 0;
    adv_data[adv_index++] = 2;
    adv_data[adv_index++] = BLUETOOTH_DATA_TYPE_FLAGS;
    adv_data[adv_index++] = 0x06;
    adv_data[adv_index++] = 17;
    adv_data[adv_index++] = BLUETOOTH_DATA_TYPE_COMPLETE_LIST_OF_128_BIT_SERVICE_CLASS_UUIDS;
    memcpy(&adv_data[adv_index], PSL_BLE_SERVICE_UUID, sizeof(PSL_BLE_SERVICE_UUID));
    adv_index += sizeof(PSL_BLE_SERVICE_UUID);

    uint8_t scan_data[2 + BLE_DEVICE_NAME_LEN];
    size_t scan_len = 0;
    scan_data[scan_len++] = (uint8_t)(1 + BLE_DEVICE_NAME_LEN);
    scan_data[scan_len++] = BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME;
    memcpy(&scan_data[scan_len], BLE_DEVICE_NAME, BLE_DEVICE_NAME_LEN);
    scan_len += BLE_DEVICE_NAME_LEN;

    bd_addr_t null_addr;
    memset(null_addr, 0, sizeof(null_addr));
    gap_advertisements_set_params(0x0030, 0x0030, 0, 0, null_addr, 0x07, 0x00);
    const uint8_t adv_len = (uint8_t)adv_index;
    gap_advertisements_set_data(adv_len, adv_data);
    gap_scan_response_set_data((uint8_t)scan_len, scan_data);
    gap_advertisements_enable(1);

    hci_power_control(HCI_POWER_ON);
    printf("BLE %s service ready\n", BLE_DEVICE_NAME);
}

int main(void) {
    stdio_init_all();
    printf("Starting PSL BLE motion controller\n");

    if (cyw43_arch_init()) {
        printf("cyw43 init failed\n");
        return 1;
    }

    ws2812_init();
    render_color_from_state();

    init_ble_service();

    btstack_run_loop_execute();

    cyw43_arch_deinit();
    return 0;
}
