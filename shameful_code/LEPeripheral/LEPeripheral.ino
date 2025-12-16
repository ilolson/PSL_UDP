#include <BTstackLib.h>
#include <Adafruit_NeoPixel.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#if defined(ARDUINO_ARCH_RP2040)
#include "hardware/watchdog.h"
#endif

namespace {

constexpr uint16_t kLedCount = 300;
constexpr uint8_t kPacketBuffer = 128;
constexpr uint8_t kBluetoothDataTypeFlags = 0x01;
constexpr uint8_t kBluetoothDataTypeComplete128BitUUIDs = 0x07;
constexpr uint8_t kBluetoothDataTypeCompleteLocalName = 0x09;
constexpr float kMinBrightness = 0.05f;
constexpr float kMaxBrightness = 1.0f;
constexpr float kDefaultHue = 25.0f;
constexpr float kDefaultSaturation = 1.0f;
constexpr float kDefaultBrightness = 125.0f / 255.0f;
constexpr uint16_t kCommandCharacteristicId = 1;

#ifndef PSL_LED_PIN
#define PSL_LED_PIN 0
#endif

const char kBleDeviceName[] = "PSL Motion";
constexpr size_t kDeviceNameMaxLen = sizeof(kBleDeviceName) + 4;

const uint8_t kServiceUuid[16] = {
    0x21, 0x43, 0x65, 0x87, 0xa9, 0xcb, 0xed, 0x0f,
    0x10, 0x32, 0x54, 0x76, 0x98, 0xba, 0xdc, 0xfe
};

Adafruit_NeoPixel strip(kLedCount, PSL_LED_PIN, NEO_GRB + NEO_KHZ800);

uint16_t segment_start = 0;
uint16_t segment_end = kLedCount - 1;
float current_hue = kDefaultHue;
float current_saturation = kDefaultSaturation;
float current_brightness = kDefaultBrightness;
float hue_offset = 0.0f;
float brightness_offset = 0.0f;

char device_name[kDeviceNameMaxLen + 1] = {};
size_t device_name_len = 0;
uint8_t adv_data[31] = {};
uint8_t scan_data[31] = {};
uint8_t adv_data_len = 0;
uint8_t scan_data_len = 0;
bool advertising_active = false;

float clampf(float value, float min_value, float max_value) {
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
}

void hsv_to_rgb(float h, float s, float v, uint8_t *r, uint8_t *g, uint8_t *b) {
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

    *r = static_cast<uint8_t>(clampf((r1 + m) * 255.0f, 0.0f, 255.0f));
    *g = static_cast<uint8_t>(clampf((g1 + m) * 255.0f, 0.0f, 255.0f));
    *b = static_cast<uint8_t>(clampf((b1 + m) * 255.0f, 0.0f, 255.0f));
}

void write_strip_color(uint32_t color) {
    for (uint16_t i = 0; i < kLedCount; ++i) {
        if (i >= segment_start && i <= segment_end) {
            strip.setPixelColor(i, color);
        } else {
            strip.setPixelColor(i, 0);
        }
    }
    strip.show();
}

void render_color_from_state() {
    float adjusted_hue = fmodf(current_hue + hue_offset, 360.0f);
    if (adjusted_hue < 0.0f) {
        adjusted_hue += 360.0f;
    }
    float adjusted_brightness = clampf(
        current_brightness + brightness_offset,
        kMinBrightness,
        kMaxBrightness
    );

    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    hsv_to_rgb(adjusted_hue, current_saturation, adjusted_brightness, &r, &g, &b);
    uint32_t grb = strip.Color(r, g, b);
    write_strip_color(grb);
}

void clamp_segment_bounds() {
    if (segment_start >= kLedCount) {
        segment_start = kLedCount - 1;
    }
    if (segment_end >= kLedCount) {
        segment_end = kLedCount - 1;
    }
    if (segment_start > segment_end) {
        segment_end = segment_start;
    }
}

void set_segment_start(uint32_t start) {
    uint32_t index = start;
    if (index >= kLedCount) {
        index = kLedCount - 1;
    }
    segment_start = static_cast<uint16_t>(index);
    clamp_segment_bounds();
    render_color_from_state();
}

void set_segment_end(uint32_t end) {
    uint32_t index = end;
    if (index >= kLedCount) {
        index = kLedCount - 1;
    }
    segment_end = static_cast<uint16_t>(index);
    clamp_segment_bounds();
    render_color_from_state();
}

void set_hue(float degrees) {
    float normalized = fmodf(degrees, 360.0f);
    if (normalized < 0.0f) {
        normalized += 360.0f;
    }
    current_hue = normalized;
    hue_offset = 0.0f;
    render_color_from_state();
}

void adjust_hue(float delta) {
    hue_offset = fmodf(hue_offset + delta, 360.0f);
    if (hue_offset < 0.0f) {
        hue_offset += 360.0f;
    }
    render_color_from_state();
}

void set_brightness(float percent) {
    float normalized = percent / 100.0f;
    current_brightness = clampf(normalized, kMinBrightness, kMaxBrightness);
    brightness_offset = 0.0f;
    render_color_from_state();
}

void adjust_brightness(float delta) {
    float desired = clampf(
        current_brightness + brightness_offset + delta,
        kMinBrightness,
        kMaxBrightness
    );
    brightness_offset = desired - current_brightness;
    render_color_from_state();
}

void render_motion_color(float pitch, float roll, float yaw) {
    float norm_roll = clampf((roll + 3.14159f) / (2.0f * 3.14159f), 0.0f, 1.0f);
    float norm_yaw = clampf((yaw + 3.14159f) / (2.0f * 3.14159f), 0.0f, 1.0f);
    float norm_pitch = clampf((pitch + (3.14159f / 2.0f)) / 3.14159f, 0.0f, 1.0f);
    float hue = fmodf(norm_yaw * 360.0f + norm_roll * 120.0f, 360.0f);
    float saturation = clampf(0.35f + norm_roll * 0.65f, 0.2f, 1.0f);
    float brightness = clampf(
        0.2f + norm_pitch * 0.8f,
        kMinBrightness,
        kMaxBrightness
    );

    current_hue = hue;
    current_saturation = saturation;
    current_brightness = brightness;
    hue_offset = 0.0f;
    brightness_offset = 0.0f;
    render_color_from_state();
}

void reset_system() {
#if defined(ARDUINO_ARCH_RP2040)
    watchdog_reboot(0, 0, 0);
#elif defined(NVIC_SystemReset)
    NVIC_SystemReset();
#else
    Serial.println("Reset requested but unsupported on this platform.");
#endif
}

void handle_motion_packet(const char *packet, size_t len) {
    char buffer[kPacketBuffer] = {};
    size_t copy_len = len < sizeof(buffer) - 1 ? len : sizeof(buffer) - 1;
    memcpy(buffer, packet, copy_len);
    buffer[copy_len] = '\0';

    float pitch = 0.0f;
    float roll = 0.0f;
    float yaw = 0.0f;
    float delta = 0.0f;
    if (strncmp(buffer, "RESET", 5) == 0) {
        Serial.println("Reset command received.");
        delay(50);
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
    Serial.print("Unrecognized BLE packet: ");
    Serial.println(buffer);
}

void update_device_name_suffix() {
    uint16_t suffix = static_cast<uint16_t>(random(0x10000));
    int written = snprintf(device_name, sizeof(device_name), "%s-%04X", kBleDeviceName, suffix);
    if (written <= 0) {
        strncpy(device_name, kBleDeviceName, sizeof(device_name) - 1);
        device_name[sizeof(device_name) - 1] = '\0';
        device_name_len = strlen(device_name);
        return;
    }
    device_name_len = static_cast<size_t>(written);
}

void copy_uuid_le(uint8_t *dest, const uint8_t *uuid) {
    for (size_t i = 0; i < 16; ++i) {
        dest[i] = uuid[15 - i];
    }
}

void prepare_advertising_payload() {
    adv_data_len = 0;
    adv_data[adv_data_len++] = 2;
    adv_data[adv_data_len++] = kBluetoothDataTypeFlags;
    adv_data[adv_data_len++] = 0x06;

    adv_data[adv_data_len++] = static_cast<uint8_t>(1 + sizeof(kServiceUuid));
    adv_data[adv_data_len++] = kBluetoothDataTypeComplete128BitUUIDs;
    copy_uuid_le(&adv_data[adv_data_len], kServiceUuid);
    adv_data_len += sizeof(kServiceUuid);

    scan_data_len = 0;
    uint8_t name_len = static_cast<uint8_t>(min(device_name_len, sizeof(scan_data) - 2));
    scan_data[scan_data_len++] = static_cast<uint8_t>(1 + name_len);
    scan_data[scan_data_len++] = kBluetoothDataTypeCompleteLocalName;
    memcpy(&scan_data[scan_data_len], device_name, name_len);
    scan_data_len += name_len;
}

void start_advertising() {
    prepare_advertising_payload();
    BTstack.setAdvData(adv_data_len, adv_data);
    BTstack.setScanData(scan_data_len, scan_data);
    BTstack.startAdvertising();
    advertising_active = true;
    Serial.print("Advertising as ");
    Serial.println(device_name);
}

void deviceConnectedCallback(BLEStatus status, BLEDevice *device) {
    (void)device;
    if (status == BLE_STATUS_OK) {
        Serial.println("Device connected.");
    } else {
        Serial.print("Connect status ");
        Serial.println(status);
    }
}

void deviceDisconnectedCallback(BLEDevice *device) {
    (void)device;
    Serial.println("Device disconnected.");
    advertising_active = false;
    start_advertising();
}

int gattWriteCallback(uint16_t characteristic_id, uint8_t *buffer, uint16_t size) {
    if (characteristic_id != kCommandCharacteristicId) {
        Serial.print("Unexpected characteristic write id ");
        Serial.println(characteristic_id);
        return ATT_ERROR_ATTRIBUTE_NOT_FOUND;
    }
    if (!buffer || size == 0) {
        return 0;
    }
    size_t copy_len = size < kPacketBuffer - 1 ? size : kPacketBuffer - 1;
    char payload[kPacketBuffer] = {};
    memcpy(payload, buffer, copy_len);
    payload[copy_len] = '\0';
    Serial.print("BLE write: ");
    Serial.println(payload);
    handle_motion_packet(payload, copy_len);
    return 0;
}

void init_ble() {
    update_device_name_suffix();
    BTstack.setBLEDeviceConnectedCallback(deviceConnectedCallback);
    BTstack.setBLEDeviceDisconnectedCallback(deviceDisconnectedCallback);
    BTstack.setGATTCharacteristicWrite(gattWriteCallback);

    BTstack.addGATTService(new UUID("21436587-A9CB-ED0F-1032-547698BADCFE"));
    BTstack.addGATTCharacteristicDynamic(
        new UUID("0C1D2E3F-4051-6273-8495-A6B7C8D9EAFB"),
        ATT_PROPERTY_WRITE | ATT_PROPERTY_WRITE_WITHOUT_RESPONSE,
        kCommandCharacteristicId
    );

    BTstack.setup(device_name);
    start_advertising();
}

} // namespace

void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 1500) {
        delay(10);
    }
    randomSeed(static_cast<uint32_t>(micros()));

    strip.begin();
    strip.show();
    render_color_from_state();

    init_ble();
}

void loop() {
    BTstack.loop();
}
