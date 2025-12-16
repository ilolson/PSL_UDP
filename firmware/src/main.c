#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "pico/stdlib.h"
#include "pico/stdio_usb.h"
#include "pico/rand.h"

/* Define the CYW43 architecture header before pulling in the SDK headers */
#define PICO_CYW43_ARCH_HEADER pico/cyw43_arch/arch_threadsafe_background.h
#include "pico/cyw43_arch.h"

#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/watchdog.h"

#include "btstack.h"
#include "btstack_event.h"
#include "btstack_util.h"
#include "ble/att_db.h"
#include "ble/att_server.h"
#include "psl_motion_gatt.h"

#include "ws2812.pio.h"

#define LED_PIN 0
#define NUM_LEDS 300
#define PACKET_BUFFER 128
#define BLE_DEVICE_NAME "PSL Motion"
#define BLE_DEVICE_NAME_LEN (sizeof(BLE_DEVICE_NAME) - 1)
#define MAX_DEVICE_NAME_LEN (BLE_DEVICE_NAME_LEN + 5)
#define PSL_SHORT_NAME "PSL Mtn"

#define MIN_BRIGHTNESS_NORMALIZED 0.05f
#define MAX_BRIGHTNESS_NORMALIZED 1.0f
#define STARTUP_LOG_WAIT_MS 1000

static const uint8_t PSL_BLE_SERVICE_UUID[16] = {
    0x21, 0x43, 0x65, 0x87, 0xa9, 0xcb, 0xed, 0x0f,
    0x10, 0x32, 0x54, 0x76, 0x98, 0xba, 0xdc, 0xfe
};
static const uint16_t ble_command_value_handle =
    ATT_CHARACTERISTIC_0C1D2E3F_4051_6273_8495_A6B7C8D9EAFB_01_VALUE_HANDLE;

enum {
    PSL_SHORT_NAME_LEN = sizeof(PSL_SHORT_NAME) - 1,
    PSL_ADV_DATA_LEN = 3 + 2 + PSL_SHORT_NAME_LEN + 2 + sizeof(PSL_BLE_SERVICE_UUID),
    PSL_SCAN_DATA_LEN = 2 + MAX_DEVICE_NAME_LEN
};

static uint8_t adv_data[PSL_ADV_DATA_LEN];
static uint8_t scan_data[PSL_SCAN_DATA_LEN];
static uint8_t adv_data_len = 0;
static uint8_t scan_data_len = 0;
static char device_name[MAX_DEVICE_NAME_LEN + 1] = BLE_DEVICE_NAME;
static uint8_t device_name_len = BLE_DEVICE_NAME_LEN;

static bool advertising_active = false;
static btstack_packet_callback_registration_t btstack_event_cb;

static uint16_t segment_start = 0;
static uint16_t segment_end = NUM_LEDS - 1;

static void render_color_from_state(void);

static PIO led_pio = pio0;
static uint led_sm = 0;
static uint led_offset = 0;

static void copy_uuid_le(uint8_t *dest, const uint8_t *uuid) {
    for (size_t i = 0; i < sizeof(PSL_BLE_SERVICE_UUID); ++i) {
        dest[i] = uuid[sizeof(PSL_BLE_SERVICE_UUID) - 1 - i];
    }
}

static void wait_for_usb_logger(void) {
#if defined(PICO_STDIO_USB) && PICO_STDIO_USB
    absolute_time_t deadline = make_timeout_time_ms(STARTUP_LOG_WAIT_MS);
    while (!stdio_usb_connected() && absolute_time_diff_us(get_absolute_time(), deadline) > 0) {
        tight_loop_contents();
    }
#else
    sleep_ms(STARTUP_LOG_WAIT_MS);
#endif
}

static inline float clampf(float value, float min, float max) {
    if (value < min) {
        return min;
    }
    if (value > max) {
        return max;
    }
    return value;
}

static void configure_random_address(void) {
    bd_addr_t addr;
    for (size_t i = 0; i < sizeof(addr); ++i) {
        addr[i] = (uint8_t)(get_rand_32() & 0xFF);
    }
    addr[5] = (addr[5] & 0x3F) | 0xC0;
    gap_random_address_set(addr);
    printf("Using random static addr %02x:%02x:%02x:%02x:%02x:%02x\n",
           addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
}

static void update_device_name_suffix(void) {
    uint32_t suffix = get_rand_32() & 0xFFFFu;
    int written = snprintf(device_name, sizeof(device_name), "%s-%04X", BLE_DEVICE_NAME, (unsigned int)suffix);
    if (written < 0) {
        strncpy(device_name, BLE_DEVICE_NAME, sizeof(device_name) - 1);
        device_name[sizeof(device_name) - 1] = '\0';
    }
    device_name_len = (uint8_t)strlen(device_name);
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

static float current_hue = 25.0f;
static float current_saturation = 1.0f;
static float current_brightness = 125.0f / 255.0f;
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
        return;
    }
    printf("Unrecognized BLE packet: '%s'\n", buffer);
}

static void log_att_data_packet(const uint8_t *packet, uint16_t size) {
    if (!packet || size == 0) {
        return;
    }
    const uint8_t opcode = packet[0];
    printf("ATT data opcode=0x%02x len=%u", opcode, size);
    switch (opcode) {
    case ATT_EXCHANGE_MTU_REQUEST:
        if (size >= 3) {
            const uint16_t client_rx_mtu = little_endian_read_16(packet, 1);
            printf(" MTU_REQ client=%u", client_rx_mtu);
        }
        break;
    case ATT_EXCHANGE_MTU_RESPONSE:
        if (size >= 3) {
            const uint16_t server_rx_mtu = little_endian_read_16(packet, 1);
            printf(" MTU_RSP server=%u", server_rx_mtu);
        }
        break;
    case ATT_READ_BY_GROUP_TYPE_REQUEST:
    case ATT_READ_BY_TYPE_REQUEST:
        if (size >= 5) {
            const uint16_t start = little_endian_read_16(packet, 1);
            const uint16_t end = little_endian_read_16(packet, 3);
            printf(" range=0x%04x-0x%04x", start, end);
        }
        break;
    case ATT_READ_REQUEST:
    case ATT_READ_BLOB_REQUEST:
    case ATT_READ_MULTIPLE_REQUEST:
    case ATT_READ_MULTIPLE_VARIABLE_REQ:
        if (size >= 3) {
            const uint16_t handle = little_endian_read_16(packet, 1);
            printf(" read_handle=0x%04x", handle);
        }
        break;
    case ATT_WRITE_REQUEST:
    case ATT_WRITE_COMMAND:
    case ATT_SIGNED_WRITE_COMMAND:
        if (size >= 3) {
            const uint16_t handle = little_endian_read_16(packet, 1);
            printf(" write_handle=0x%04x payload=%u", handle, size - 3);
        }
        break;
    default:
        break;
    }
    printf(" payload:");
    for (uint16_t i = 0; i < size; ++i) {
        printf(" %02x", packet[i]);
    }
    printf("\n");
}

static void att_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;
    if (packet_type == ATT_DATA_PACKET) {
        log_att_data_packet(packet, size);
    } else if (packet_type == HCI_EVENT_PACKET) {
        const uint8_t event_type = hci_event_packet_get_type(packet);
        if (event_type == ATT_EVENT_CONNECTED) {
            printf("ATT server connected handle=0x%04x\n", att_event_connected_get_handle(packet));
        } else if (event_type == ATT_EVENT_DISCONNECTED) {
            printf("ATT server disconnected handle=0x%04x\n",
                   att_event_disconnected_get_handle(packet));
        }
    }
}

static int ble_command_write_callback(hci_con_handle_t con_handle, uint16_t attribute_handle,
                                      uint16_t transaction_mode, uint16_t offset, uint8_t *buffer, uint16_t buffer_size) {
    (void)con_handle;
    (void)offset;

    if (attribute_handle != ble_command_value_handle) {
        printf("Write to unexpected handle 0x%04x (%u bytes)\n", attribute_handle, buffer_size);
        return ATT_ERROR_ATTRIBUTE_NOT_FOUND;
    }
    if (transaction_mode != ATT_TRANSACTION_MODE_NONE || !buffer || buffer_size == 0) {
        return 0;
    }

    char payload[PACKET_BUFFER];
    size_t copy_len = buffer_size < sizeof(payload) - 1 ? buffer_size : sizeof(payload) - 1;
    memcpy(payload, buffer, copy_len);
    payload[copy_len] = '\0';

    printf("BLE write (%u bytes): %s\n", buffer_size, payload);

    handle_motion_packet(payload, copy_len);
    return 0;
}

static void prepare_ble_advertising_payload(void) {
    memset(adv_data, 0, sizeof(adv_data));
    size_t adv_index = 0;
    adv_data[adv_index++] = 2;
    adv_data[adv_index++] = BLUETOOTH_DATA_TYPE_FLAGS;
    adv_data[adv_index++] = 0x06;

    adv_data[adv_index++] = 17;
    adv_data[adv_index++] = BLUETOOTH_DATA_TYPE_COMPLETE_LIST_OF_128_BIT_SERVICE_CLASS_UUIDS;
    copy_uuid_le(&adv_data[adv_index], PSL_BLE_SERVICE_UUID);
    adv_index += sizeof(PSL_BLE_SERVICE_UUID);
    adv_data_len = (uint8_t)adv_index;

    memset(scan_data, 0, sizeof(scan_data));
    size_t scan_len = 0;
    scan_data[scan_len++] = (uint8_t)(1 + device_name_len);
    scan_data[scan_len++] = BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME;
    memcpy(&scan_data[scan_len], device_name, device_name_len);
    scan_len += device_name_len;
    scan_data_len = (uint8_t)scan_len;
}

static void start_advertising(void) {
    if (advertising_active) {
        return;
    }
    bd_addr_t null_addr;
    memset(null_addr, 0, sizeof(null_addr));
    gap_advertisements_set_params(0x0030, 0x0030, 0x00, 0x01, null_addr, 0x07, 0x00);
    gap_advertisements_set_data(adv_data_len, adv_data);
    gap_scan_response_set_data(scan_data_len, scan_data);
    gap_advertisements_enable(1);
    advertising_active = true;
    printf("Advertising %s (%u adv bytes, %u scan bytes)\n",
           BLE_DEVICE_NAME, adv_data_len, scan_data_len);
}

static void stop_advertising(void) {
    if (!advertising_active) {
        return;
    }
    gap_advertisements_enable(0);
    advertising_active = false;
}

static void btstack_event_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;
    if (packet_type != HCI_EVENT_PACKET) {
        return;
    }
    const uint8_t event_type = hci_event_packet_get_type(packet);
    switch (event_type) {
    case BTSTACK_EVENT_STATE: {
        const uint8_t state = btstack_event_state_get_state(packet);
        printf("BTstack state %u\n", state);
        if (state == HCI_STATE_WORKING) {
            printf("BTstack ready, enabling advertising\n");
            configure_random_address();
            start_advertising();
        }
        break;
    }
    case HCI_EVENT_LE_META: {
        const uint8_t subevent = hci_event_le_meta_get_subevent_code(packet);
        if (subevent == HCI_SUBEVENT_LE_CONNECTION_COMPLETE) {
            printf("LE connected handle=0x%04x status=%u\n",
                   hci_subevent_le_connection_complete_get_connection_handle(packet),
                   hci_subevent_le_connection_complete_get_status(packet));
        }
        break;
    }
    case HCI_EVENT_DISCONNECTION_COMPLETE:
        printf("LE disconnected handle=0x%04x reason=0x%02x\n",
               hci_event_disconnection_complete_get_connection_handle(packet),
               hci_event_disconnection_complete_get_reason(packet));
        stop_advertising();
        start_advertising();
        break;
    default:
        break;
    }
}

static void init_ble_service(void) {
    l2cap_init();
    sm_init();

    update_device_name_suffix();
    att_server_init(profile_data, NULL, ble_command_write_callback);
    att_server_register_packet_handler(att_packet_handler);

    prepare_ble_advertising_payload();

    memset(&btstack_event_cb, 0, sizeof(btstack_event_cb));
    btstack_event_cb.callback = &btstack_event_handler;
    hci_add_event_handler(&btstack_event_cb);
    printf("ATT handles: custom svc %04x-%04x cmd=%04x\n",
           ATT_SERVICE_21436587_A9CB_ED0F_1032_547698BADCFE_START_HANDLE,
           ATT_SERVICE_21436587_A9CB_ED0F_1032_547698BADCFE_END_HANDLE,
           ble_command_value_handle);

    hci_power_control(HCI_POWER_ON);
    printf("BLE %s service ready\n", BLE_DEVICE_NAME);
}

int main(void) {
    stdio_init_all();
    wait_for_usb_logger();
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
