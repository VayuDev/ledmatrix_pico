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

typedef enum RecvState {
    RECEIVING_S,
    RECEIVING_LENGTH,
    RECEIVING_DATA
} RecvState;

struct NetCtx;
typedef struct NetCtx NetCtx;
struct NetCtx {
    uint8_t receiveBuffer[10 * 1024];
    size_t receiveBufferOffset;
    size_t recvLength;
    RecvState state;
    void* user;
    bool connectingTcp;
    bool connectingWifi;
    void (*handlePacket)(NetCtx *ctx, uint8_t* data, size_t dataLen, void* user);
    void (*onConnectionClose)(NetCtx *ctx, void* user);
    void (*onWifiDisconnect)(NetCtx *ctx, void* user);
    ZSTD_DCtx *zstdDCtx;
};

void hang() {
    fflush(stdout);
    while(true) {
        sleep_ms(1);
    }
}

size_t receive_response(NetCtx* ctx, uint8_t* dst, size_t dstLen) {
    const size_t recvBufferMaxSize = 1024 * 4;
    uint8_t recvBuffer[1024 * 4];

    enum State {
        NEW_LINE,
        NORMAL,
        READ_PLUS,
        READ_PLUS_I,
        READ_PLUS_IP,
        READ_PLUS_IPD,
        READ_CR
    } state = NEW_LINE;
    size_t responseLength = 0;
    size_t lastLineStart = 0;
    while(responseLength < recvBufferMaxSize) {
        uint8_t* dst = &recvBuffer[responseLength];
        responseLength++;

        uart_read_blocking(uart0, dst, 1);
        //printf("Read: %c\n", *dst);
        //fflush(stdout);
        if(*dst == '+' && state == NEW_LINE) {
            state = READ_PLUS;
        } else if(*dst == 'I' && state == READ_PLUS) {
            state = READ_PLUS_I;
        } else if(*dst == 'P' && state == READ_PLUS_I) {
            state = READ_PLUS_IP;
        } else if(*dst == 'D' && state == READ_PLUS_IP) {
            state = READ_PLUS_IPD;
        } else if(*dst == ',' && state == READ_PLUS_IPD) {
            int len = 0;
            char current;
            do {
                //printf("Reading char... ");
                current = uart_getc(uart0);
                //printf("read!\n");
                if(current >= '0' && current <= '9') {
                    len *= 10;
                    len += current - '0';
                }
            } while(current != ':');
            //printf("Len: %i\n", (int)len);
           // uint8_t buff[len];
            //uart_read_blocking(uart0, buff, len);
            for(int i = 0; i < len; ++i) {
                if(!uart_is_readable_within_us(uart0, 100 * 1000)) {
                    printf("Timeout reading IPD data! Read %i from %i", i, (int)len);
                    break;
                }
                char current = uart_getc(uart0);
                //char current = buff[i];
                switch(ctx->state) {
                    case RECEIVING_S:
                        if(current != 'S') {
                            printf("protocol error! 1\n");
                            break;
                        }
                        ctx->state = RECEIVING_LENGTH;
                        ctx->recvLength = 0;
                        ctx->receiveBufferOffset = 0;
                        break;
                    case RECEIVING_LENGTH:
                        if(current >= '0' && current <= '9') {
                            ctx->recvLength *= 10;
                            ctx->recvLength += current - '0';
                        } else if(current == ':') {
                            ctx->state = RECEIVING_DATA;
                        } else {
                            printf("protocol error! 2\n");
                            ctx->state = RECEIVING_S;
                        }
                        break;
                    case RECEIVING_DATA:
                        ctx->receiveBuffer[ctx->receiveBufferOffset++] = current;
                        if(ctx->receiveBufferOffset >= ctx->recvLength) {
                            //printf("Decompressing... ");
                            if(!ctx->zstdDCtx) {
                                ctx->zstdDCtx = ZSTD_createDCtx();
                            }
                            size_t decompressedSize = ZSTD_decompressDCtx(ctx->zstdDCtx, ctx->receiveBuffer + ctx->recvLength, sizeof(ctx->receiveBuffer) - ctx->recvLength, ctx->receiveBuffer, ctx->recvLength);
                            //printf("done!\n");
                            if(ZSTD_isError(decompressedSize)) {
                                printf("ZSTD decompression error: %s\n", ZSTD_getErrorName(decompressedSize));
                            } else {
                                if(ctx->handlePacket)
                                    ctx->handlePacket(ctx, ctx->receiveBuffer + ctx->recvLength, decompressedSize, ctx->user);
                                //printf("Handled packet\n");
                            }
                            //printf("Received compressed data of length %i\n", (int)ctx->recvLength);
                            ctx->state = RECEIVING_S;
                            ctx->recvLength = 0;
                            ctx->receiveBufferOffset = 0;
                        }
                        break;
                }
            }
            //printf("IPD data read\n");
            state = NORMAL;
            lastLineStart = responseLength;
        } else if(state == READ_PLUS || state == READ_PLUS_IP || state == READ_PLUS_IPD) {
            state = NORMAL;
        }
        if(state == NEW_LINE) {
            state = NORMAL;
        }
        //printf("uart read 2: %c (%i)\n", *dst, (int)*dst);
        //fflush(stdout);

        if(state == READ_CR) {
            if(*dst == '\n') {
                if(memcmp(recvBuffer + lastLineStart, "OK\r\n", responseLength - lastLineStart) == 0
                   || memcmp(recvBuffer + lastLineStart, "FAIL\r\n", responseLength - lastLineStart) == 0
                   || memcmp(recvBuffer + lastLineStart, "ERROR\r\n", responseLength - lastLineStart) == 0
                   || memcmp(recvBuffer + lastLineStart, "SEND OK\r\n", responseLength - lastLineStart) == 0
                   || memcmp(recvBuffer + lastLineStart, "SEND FAIL\r\n", responseLength - lastLineStart) == 0) {
                    break;
                } else if(memcmp(recvBuffer + lastLineStart, "CLOSED\r\n", responseLength - lastLineStart) == 0) {
                    if(ctx->onConnectionClose && !ctx->connectingTcp) {
                        ctx->onConnectionClose(ctx, ctx->user);
                    }
                } else if(memcmp(recvBuffer + lastLineStart, "WIFI DISCONNECT\r\n", responseLength - lastLineStart) == 0) {
                    if(ctx->onWifiDisconnect && !ctx->connectingWifi) {
                        ctx->onWifiDisconnect(ctx, ctx->user);
                    }
                } else {
                    /*printf("Matching:\n");
                    for(uint8_t* c = recvBuffer + lastLineStart; c < recvBuffer + responseLength; ++c) {
                        printf("Doesn't match: %c\n", *c);
                    }*/
                }
                state = NEW_LINE;
                lastLineStart = responseLength;
            } else {
                state = NORMAL;
            }
        } else if(*dst == '\r') {
            state = READ_CR;
        }
    }
    size_t returnLength = responseLength < dstLen ? responseLength : dstLen;
    if(dst != NULL)
        memcpy(dst, recvBuffer + responseLength - returnLength, returnLength);
    return returnLength;
}

size_t send_at_command(NetCtx* ctx, const char* command, bool expectedEcho, uint8_t* response, size_t responseMaxLength) {
    size_t commandLen = strlen(command);
    uart_write_blocking(uart0, command, commandLen);
    bool readCR;
    int linesRead = 0;
    while(linesRead < (expectedEcho ? 2 : 0)) {
        char c = uart_getc(uart0);
        //printf("uart read 1: %c (%i)\n", c, (int)c);
        //fflush(stdout);
        if(c == '\r')
            readCR = true;
        if(c == '\n' && readCR) {
            linesRead += 1;
        }
    }

    return receive_response(ctx, response, responseMaxLength);
}

bool send_at_command_ne_expect(NetCtx* ctx, const char* command, const char* expectedResult) {
    ctx->connectingWifi = true;
    size_t expectedResultLen = strlen(expectedResult);
    char result[expectedResultLen + 1];
    size_t bytesReceived = send_at_command(ctx, command, false, result, expectedResultLen);
    result[bytesReceived] = 0;
    ctx->connectingWifi = false;
    if(bytesReceived != expectedResultLen || memcmp(expectedResult, result, expectedResultLen) != 0) {
        printf("AT Command '%s'; expected '%s', got '%s' (%i)\n", command, expectedResult, result, (int)bytesReceived);
        fflush(stdout);
        return false;
    }
    return true;
}

bool connect(NetCtx* ctx, const char* remoteIp, uint16_t remotePort) {
    ctx->connectingTcp = true;
    char buffer[129];
    snprintf(buffer, 128, "AT+CIPSTART=\"TCP\",\"%s\",%i,10\r\n", remoteIp, (int)remotePort);
    size_t bytesReceived = send_at_command(ctx, buffer, false, buffer, 128);
    ctx->connectingTcp = false;
    buffer[bytesReceived] = 0;
    if(memcmp("OK\r\n", buffer, 4) == 0 || strstr(buffer, "ALREADY CONNECTED") != NULL) {
        printf("connect ok\n");
        return true;
    }
    printf("connect failed\n");
    return false;
}

bool send_tcp_data(NetCtx* ctx, const uint8_t* data, size_t len) {
    char buffer[128];
    int startLen = snprintf(buffer, 128, "AT+CIPSEND=%u\r\n", (unsigned)len);
    uart_write_blocking(uart0, buffer, startLen);
    while(uart_getc(uart0) != '>');
    uart_write_blocking(uart0, data, len);

    receive_response(ctx, buffer, 9);
    if(memcmp(buffer, "SEND OK\r\n", 9) != 0) {
        buffer[11] = 0;
        printf("Sending failed, expected 'SEND OK', got %s\n", buffer);
        return false;
    }
    return true;
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

void reconnect_tcp(NetCtx *ctx, void* user) {
start:
    send_at_command_ne_expect(ctx, "AT+CIPCLOSE\r\n", "OK\r\n");
    while(true) {
        printf("Trying to (re)connect tcp...\n");
        if(connect(ctx, "sauron", 1883)) {
            break;
        }
        printf("Connection attempt failed\n");
        sleep_ms(1000);
    }
    printf("(Re)Connected tcp!\n");
    const char* greeting = "S38:SIMPLIFIED-SBC-MQTT-CLIENT-PICO-MATRIX";
    if(!send_tcp_data(ctx, greeting, strlen(greeting))) {
        printf("Failed to send hello\n");
        goto start;
    }
    printf("Sending greeting success!\n");
}

void reconnect_wifi(NetCtx *ctx, void* user) {
    sleep_ms(2000);
    printf("Now trying to connect to WiFi...\n");
    while(true) {
        printf("Trying to (re)connect wifi...\n");
        if(send_at_command_ne_expect(ctx, "AT+CWJAP_CUR="WIFI_NAME"," WIFI_PASSWORD "\r\n", "OK\r\n")) {
            break;
        }
        sleep_ms(1000);
    }
    sleep_ms(1000);
    printf("(Re)Connected wifi!\n");
}

void second_core_func() {
    printf("Sending data to ESP-01\n");
    uart_init(uart0, 119200);
    gpio_set_function(16, GPIO_FUNC_UART);
    gpio_set_function(17, GPIO_FUNC_UART);
    uart_set_format(uart0, 8, 1, UART_PARITY_NONE);
    uart_set_translate_crlf(uart0, false);
    while(uart_is_readable(uart0)) {
        uart_getc(uart0);
    }

    NetCtx ctxStack = { 0 };
    NetCtx *ctx = &ctxStack;
    int currentBuffer = 1;
    ctx->user = &currentBuffer;
    ctx->handlePacket = received_packet;
    ctx->onConnectionClose = reconnect_tcp;
    ctx->onWifiDisconnect = reconnect_wifi;

    send_at_command_ne_expect(ctx, "AT+RESTORE\r\n", "OK\r\n");
    printf("ESP-01 reset\n");
    sleep_ms(500);

    const char* setBaudRate = "AT+UART_CUR=119200,8,1,0,0\r\n";
    uart_write_blocking(uart0, setBaudRate, strlen(setBaudRate));
    receive_response(ctx, NULL, 0);
    uart_deinit(uart0);
    uart_init(uart0, 119200);
    uart_set_format(uart0, 8, 1, UART_PARITY_NONE);
    uart_set_translate_crlf(uart0, false);
    printf("ESP-01 now running at 119200 baud\n");

    sleep_ms(500);

    send_at_command_ne_expect(ctx, "ATE0\r\n", "OK\r\n");
    printf("Turned echo off\n");
    {
        char response[512] = { 0 };
        send_at_command(ctx, "AT+GMR\r\n", false, response, 511);
        printf("ESP-01 Version: %s\n", response);
    }

    send_at_command_ne_expect(ctx, "AT+CWMODE_CUR=1\r\n", "OK\r\n");
    printf("Set wifi to station mode\n");
    send_at_command_ne_expect(ctx, "AT+CWDHCP_CUR=1,1\r\n", "OK\r\n");
    printf("DHCP enabled\n");
    reconnect_wifi(ctx, ctx->user);
    reconnect_tcp(ctx, ctx->user);
    printf("TCP connected, sleeping now for 500ms\n");
    sleep_ms(500);


    multicore_fifo_push_blocking(0);

    while(true) {
        receive_response(ctx, NULL, 0);
    }
}

uint32_t CORE1_STACK[(20 * 1024)/4];

int main() {
    stdio_init_all();
    set_sys_clock_khz(250 * 1000, true);
    printf("Starting...\n");
    sleep_ms(5000);


    memset(buffers, 10, sizeof(buffers));

    multicore_launch_core1_with_stack(second_core_func, CORE1_STACK, sizeof(CORE1_STACK));
    multicore_fifo_pop_blocking();

    printf("Init done, starting buisness logic!\n");

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