#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
/* Define the CYW43 architecture header before pulling in the SDK headers */
#define PICO_CYW43_ARCH_HEADER pico/cyw43_arch/arch_threadsafe_background.h

#define LED_PIN 2
#define NUM_LEDS 16
#define UDP_PORT 4210
#define PACKET_BUFFER 128
#define AP_SSID "PSL_UDP"
#define AP_PASSWORD "psludp123"
#define AUTH_TYPE CYW43_AUTH_WPA2_AES_PSK

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"

#include "hardware/pio.h"
#include "hardware/clocks.h"

#include "lwipopts.h"
#include "lwip/inet.h"
#include "lwip/ip_addr.h"
#include "lwip/ip4_addr.h"
#include "lwip/udp.h"

#include "dhcpserver.h"
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
        pio_sm_put_blocking(led_pio, led_sm, grb << 8u);
    }
}

static void render_motion_color(float pitch, float roll, float yaw) {
    float norm_roll = clampf((roll + 3.14159f) / (2.0f * 3.14159f), 0.0f, 1.0f);
    float norm_yaw = clampf((yaw + 3.14159f) / (2.0f * 3.14159f), 0.0f, 1.0f);
    float norm_pitch = clampf((pitch + (3.14159f / 2.0f)) / 3.14159f, 0.0f, 1.0f);
    float hue = fmodf(norm_yaw * 360.0f + norm_roll * 120.0f, 360.0f);
    float saturation = clampf(0.35f + norm_roll * 0.65f, 0.2f, 1.0f);
    float brightness = clampf(0.2f + norm_pitch * 0.8f, 0.05f, 1.0f);

    uint8_t r, g, b;
    hsv_to_rgb(hue, saturation, brightness, &r, &g, &b);
    uint32_t color = ((uint32_t)g << 16) | ((uint32_t)r << 8) | b;
    ws2812_write_color(color);
}

static void handle_motion_packet(const char *packet, size_t len) {
    char buffer[PACKET_BUFFER];
    size_t copy_len = len < sizeof(buffer) - 1 ? len : sizeof(buffer) - 1;
    memcpy(buffer, packet, copy_len);
    buffer[copy_len] = '\0';

    float pitch = 0.0f;
    float roll = 0.0f;
    float yaw = 0.0f;
    if (sscanf(buffer, "%f,%f,%f", &pitch, &roll, &yaw) == 3) {
        render_motion_color(pitch, roll, yaw);
    }
}

static void udp_motion_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port) {
    (void)arg;
    (void)pcb;
    (void)addr;
    (void)port;
    if (!p) {
        return;
    }
    char buffer[PACKET_BUFFER];
    size_t copy_len = pbuf_copy_partial(p, buffer, sizeof(buffer) - 1, 0);
    buffer[copy_len] = '\0';
    handle_motion_packet(buffer, copy_len);
    pbuf_free(p);
}

static void start_udp_listener(void) {
    struct udp_pcb *pcb = udp_new();
    if (!pcb) {
        printf("Failed to allocate UDP pcb\n");
        return;
    }
    err_t err = udp_bind(pcb, IP_ADDR_ANY, UDP_PORT);
    if (err != ERR_OK) {
        printf("UDP bind failed: %d\n", err);
        udp_remove(pcb);
        return;
    }
    udp_recv(pcb, udp_motion_recv, NULL);
    printf("UDP listener ready on port %d\n", UDP_PORT);
}

int main(void) {
    stdio_init_all();
    printf("Starting PSL_UDP motion controller\n");

    if (cyw43_arch_init()) {
        printf("cyw43 init failed\n");
        return 1;
    }
    cyw43_arch_enable_ap_mode(AP_SSID, AP_PASSWORD, AUTH_TYPE);

    ip4_addr_t ip4;
    ip4_addr_t mask4;
    IP4_ADDR(&ip4, 192, 168, 4, 1);
    IP4_ADDR(&mask4, 255, 255, 255, 0);

    ip_addr_t ap_ip;
    ip_addr_t ap_mask;
    ip_addr_set_ip4_u32(&ap_ip, ip4.addr);
    ip_addr_set_ip4_u32(&ap_mask, mask4.addr);

    dhcp_server_t lease_server;
    dhcp_server_init(&lease_server, &ap_ip, &ap_mask);

    ws2812_init();
    ws2812_write_color(0x00102040);

    printf("Access point %s ready. Send UDP to 192.168.4.1:%d\n", AP_SSID, UDP_PORT);
    start_udp_listener();

    dhcp_server_deinit(&lease_server);
    cyw43_arch_deinit();
    return 0;
}
