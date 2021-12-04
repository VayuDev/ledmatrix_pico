/**
 * Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdio.h>

#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "hub75.pio.h"
#include "pico/multicore.h"

#include <stdlib.h>
#include <string.h>
#include <zstd.h>

#pragma clang diagnostic push
#pragma ide diagnostic ignored "EndlessLoop"
#define DATA_BASE_PIN 0
#define DATA_N_PINS 6
#define ROWSEL_BASE_PIN 6
#define ROWSEL_N_PINS 5
#define CLK_PIN 11
#define STROBE_PIN 12
#define OEN_PIN 13

#define WIDTH 64
#define HEIGHT 32

#include "esp-uart.h"


void hang() {
    fflush(stdout);
    while(true) {
        sleep_ms(1);
    }
}


static uint32_t buffers[2][WIDTH * HEIGHT] = { 0 };

void received_packet(NetCtx *ctx, uint8_t* data, size_t len, void* user) {
    if(len != 32 * 64 * 3) {
        printf("Received packet with invalid length %i\n", (int)len);
        return;
    }
    int* currentBuffer = user;
    printf("Received packet of length %i, current buffer: %i\n", (int)len, (int)*currentBuffer);
    for(int i = 0; i < WIDTH * HEIGHT; ++i) {
        buffers[*currentBuffer][i] = (data[i * 3 + 2] << 16) | (data[i * 3 + 1] << 8) | (data[i * 3 + 0] << 0);
    }
    multicore_fifo_push_blocking(*currentBuffer);
    multicore_fifo_pop_blocking();
    *currentBuffer = !*currentBuffer;
}

void clear_screen(int *currentBuffer) {
    memset(buffers[*currentBuffer], 0, sizeof(buffers[*currentBuffer]));
    multicore_fifo_push_blocking(*currentBuffer);
    multicore_fifo_pop_blocking();
    *currentBuffer = !*currentBuffer;
}

void show_loading_animation(int* currentBuffer, int index, uint8_t colorOffset) {
    memset(buffers[*currentBuffer], 0, sizeof(buffers[*currentBuffer]));
    buffers[*currentBuffer][index % sizeof(buffers[*currentBuffer])] = 255 << (colorOffset * 8);
    multicore_fifo_push_blocking(*currentBuffer);
    multicore_fifo_pop_blocking();
    *currentBuffer = !*currentBuffer;
}

bool send_greeting(NetCtx* ctx) {
    clear_screen(ctx->user);
    const char* greeting = "S38:SIMPLIFIED-SBC-MQTT-CLIENT-PICO-MATRIX";
    if(!esp_send_tcp_data(ctx, greeting, strlen(greeting))) {
        printf("Failed to send hello\n");
        return false;
    }
    printf("Sending greeting success!\n");
    return true;
}

// These two function are super messy. This is due to the fact that a) we can't do recursion but b) at any point, we
// can get notified that the wifi or tcp connection dropped c) i'm to lazy to clean it up.
// If wifi drops, we have to reconnect tcp as well, but what if wifi drops again? As we can't do recursion (limited stack space),
// we need to save that error (ctx->wifiErrorButStillinWifiConnect = true), return it one layer up and reconnect there.
bool reconnect_tcp(NetCtx *ctx, void* user) {
start:
    esp_send_at_command_expect(ctx, "AT+CIPCLOSE\r\n", "OK\r\n");
    int tries = 0;
    while(true) {
        printf("Trying to (re)connect tcp...\n");
        if(esp_connect(ctx, "sauron", 1883)) {
            break;
        }
        if(ctx->wifiErrorButStillinWifiConnect) {
            return false;
        }
        printf("Connection attempt failed\n");
        show_loading_animation(user, tries++, 1);
        sleep_ms(1000);
    }
    printf("(Re)Connected tcp!\n");
    if(!send_greeting(ctx))
        goto start;

    return true;
}

void reconnect_wifi(NetCtx *ctx, void* user) {
    sleep_ms(2000);
start:
    ctx->connectingWifi = true;
    int tries = 0;
    while(true) {
        printf("Trying to (re)connect wifi...\n");
        esp_send_at_command_expect(ctx, "AT+CWQAP\r\n", "OK\r\n");
        if(esp_send_at_command_expect(ctx, "AT+CWJAP_CUR=\"" WIFI_NAME "\",\"" WIFI_PASSWORD "\"\r\n", "OK\r\n")) {
            break;
        }
        show_loading_animation(user, tries++, 0);
        sleep_ms(1000);
    }
    sleep_ms(1000);
    printf("(Re)Connected wifi!\n");

    // avoid recursion here
    if(!reconnect_tcp(ctx, user))
        goto start;

    ctx->connectingWifi = false;
}

void second_core_func() {
    printf("Core 1 setup...\n");

    NetCtx ctxStack = { 0 };
    NetCtx *ctx = &ctxStack;
    int currentBuffer = 1;
    ctx->user = &currentBuffer;
    ctx->handlePacket = received_packet;
    ctx->onConnectionClose = reconnect_tcp;
    ctx->onWifiDisconnect = reconnect_wifi;

    esp_setup(ctx);
    reconnect_wifi(ctx, ctx->user);
    //reconnect_tcp(ctx, ctx->user);
    printf("TCP connected, sleeping now for 500ms\n");
    sleep_ms(500);

    printf("Init done, starting buisness logic!\n");
    while(true) {
        esp_receive_response(ctx, NULL, 0);
    }
}

uint32_t CORE1_STACK[(24 * 1024)/4];

int main() {
    stdio_init_all();
    set_sys_clock_khz(250 * 1000, true);
    printf("Starting...\n");
    sleep_ms(5000);


    memset(buffers, 10, sizeof(buffers));

    multicore_launch_core1_with_stack(second_core_func, CORE1_STACK, sizeof(CORE1_STACK));


    PIO pio = pio0;
    uint sm_data = 0;
    uint sm_row = 1;

    uint data_prog_offs = pio_add_program(pio, &hub75_data_rgb888_program);
    uint row_prog_offs = pio_add_program(pio, &hub75_row_program);
    hub75_data_rgb888_program_init(pio, sm_data, data_prog_offs, DATA_BASE_PIN, CLK_PIN);
    hub75_row_program_init(pio, sm_row, row_prog_offs, ROWSEL_BASE_PIN, ROWSEL_N_PINS, STROBE_PIN);

    static uint32_t gc_row[2][WIDTH] = { 0 };

    uint32_t *buffer = buffers[0];
    int i = 0;
    while (1) {
        uint64_t start = to_us_since_boot(get_absolute_time());

        while(multicore_fifo_rvalid()) {
            buffer = buffers[multicore_fifo_pop_blocking()];
            multicore_fifo_push_blocking(1);
        }
        /*for(int x = 0; x < WIDTH; ++x) {
            for(int y = 0; y < HEIGHT; ++y) {
                buffer[y * WIDTH + x] = Wheel(y + x + i / 10);
            }
        }*/
        i += 1;
        for (int rowsel = 0; rowsel < HEIGHT / 2; ++rowsel) {
            for (int x = 0; x < WIDTH; ++x) {
                //gc_row[0][x] = gamma_correct_565_888(img[rowsel * WIDTH + x]);
                //gc_row[1][x] = gamma_correct_565_888(img[((1u << ROWSEL_N_PINS) + rowsel) * WIDTH + x]);
                gc_row[0][x] = buffer[rowsel * WIDTH + x];
                gc_row[1][x] = buffer[(HEIGHT / 2 + rowsel) * WIDTH + x];
            }
            for (int bit = 0; bit < 8; ++bit) {
                hub75_data_rgb888_set_shift(pio, sm_data, data_prog_offs, bit);
                for (int x = 0; x < WIDTH; ++x) {
                    pio_sm_put_blocking(pio, sm_data, gc_row[0][x]);
                    pio_sm_put_blocking(pio, sm_data, gc_row[1][x]);
                }
                // Dummy pixel per lane
                pio_sm_put_blocking(pio, sm_data, 0);
                pio_sm_put_blocking(pio, sm_data, 0);
                // SM is finished when it stalls on empty TX FIFO
                hub75_wait_tx_stall(pio, sm_data);
                // Also check that previous OEn pulse is finished, else things can get out of sequence
                hub75_wait_tx_stall(pio, sm_row);

                // Latch row data, pulse output enable for new row.
                pio_sm_put_blocking(pio, sm_row, rowsel | (100u * (1u << bit) << 5));
            }
        }
        uint64_t end = to_us_since_boot(get_absolute_time());
        //printf("Took: %u us\n", (unsigned)((end - start)));
        //fflush(stdout);
    }

}

#pragma clang diagnostic pop