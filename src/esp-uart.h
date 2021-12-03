#pragma once

#include <stdio.h>

#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "hub75.pio.h"
#include "pico/multicore.h"

#include <stdlib.h>
#include <string.h>
#include <zstd.h>


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


size_t esp_receive_response(NetCtx* ctx, uint8_t* dst, size_t dstLen);

size_t esp_send_at_command(NetCtx* ctx, const char* command, uint8_t* response, size_t responseMaxLength);

bool esp_send_at_command_expect(NetCtx* ctx, const char* command, const char* expectedResult);

bool esp_connect(NetCtx* ctx, const char* remoteIp, uint16_t remotePort);

bool esp_send_tcp_data(NetCtx* ctx, const uint8_t* data, size_t len);

void esp_setup(NetCtx* ctx);