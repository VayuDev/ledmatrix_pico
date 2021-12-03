#include "esp-uart.h"


size_t esp_receive_response(NetCtx *ctx, uint8_t *dst, size_t dstLen) {
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

size_t esp_send_at_command(NetCtx *ctx, const char *command, uint8_t *response, size_t responseMaxLength) {
    size_t commandLen = strlen(command);
    uart_write_blocking(uart0, command, commandLen);
    return esp_receive_response(ctx, response, responseMaxLength);
}

bool esp_send_at_command_expect(NetCtx *ctx, const char *command, const char *expectedResult) {
    ctx->connectingWifi = true;
    size_t expectedResultLen = strlen(expectedResult);
    char result[expectedResultLen + 1];
    size_t bytesReceived = esp_send_at_command(ctx, command, result, expectedResultLen);
    result[bytesReceived] = 0;
    ctx->connectingWifi = false;
    if(bytesReceived != expectedResultLen || memcmp(expectedResult, result, expectedResultLen) != 0) {
        printf("AT Command '%s'; expected '%s', got '%s' (%i)\n", command, expectedResult, result, (int)bytesReceived);
        fflush(stdout);
        return false;
    }
    return true;
}

bool esp_connect(NetCtx *ctx, const char *remoteIp, uint16_t remotePort) {
    ctx->connectingTcp = true;
    char buffer[129];
    snprintf(buffer, 128, "AT+CIPSTART=\"TCP\",\"%s\",%i,10\r\n", remoteIp, (int)remotePort);
    size_t bytesReceived = esp_send_at_command(ctx, buffer, buffer, 128);
    ctx->connectingTcp = false;
    buffer[bytesReceived] = 0;
    if(memcmp("OK\r\n", buffer, 4) == 0 || strstr(buffer, "ALREADY CONNECTED") != NULL) {
        printf("connect ok\n");
        return true;
    }
    printf("connect failed\n");
    return false;
}

bool esp_send_tcp_data(NetCtx *ctx, const uint8_t *data, size_t len) {
    char buffer[128];
    int startLen = snprintf(buffer, 128, "AT+CIPSEND=%u\r\n", (unsigned)len);
    uart_write_blocking(uart0, buffer, startLen);
    while(uart_getc(uart0) != '>');
    uart_write_blocking(uart0, data, len);

    esp_receive_response(ctx, buffer, 9);
    if(memcmp(buffer, "SEND OK\r\n", 9) != 0) {
        buffer[11] = 0;
        printf("Sending failed, expected 'SEND OK', got %s\n", buffer);
        return false;
    }
    return true;
}

void esp_setup(NetCtx* ctx) {
    uart_init(uart0, 119200);
    gpio_set_function(16, GPIO_FUNC_UART);
    gpio_set_function(17, GPIO_FUNC_UART);
    uart_set_format(uart0, 8, 1, UART_PARITY_NONE);
    uart_set_translate_crlf(uart0, false);
    while(uart_is_readable(uart0)) {
        uart_getc(uart0);
    }
    esp_send_at_command_expect(ctx, "AT+RESTORE\r\n", "OK\r\n");
    printf("ESP-01 reset\n");
    sleep_ms(500);

    const char* setBaudRate = "AT+UART_CUR=119200,8,1,0,0\r\n";
    uart_write_blocking(uart0, setBaudRate, strlen(setBaudRate));
    esp_receive_response(ctx, NULL, 0);
    uart_deinit(uart0);
    uart_init(uart0, 119200);
    uart_set_format(uart0, 8, 1, UART_PARITY_NONE);
    uart_set_translate_crlf(uart0, false);
    printf("ESP-01 now running at 119200 baud\n");

    sleep_ms(500);

    esp_send_at_command_expect(ctx, "ATE0\r\n", "OK\r\n");
    printf("Turned echo off\n");
    {
        char response[512] = { 0 };
        esp_send_at_command(ctx, "AT+GMR\r\n", response, 511);
        printf("ESP-01 Version: %s\n", response);
    }

    esp_send_at_command_expect(ctx, "AT+CWMODE_CUR=1\r\n", "OK\r\n");
    printf("Set wifi to station mode\n");
    esp_send_at_command_expect(ctx, "AT+CWDHCP_CUR=1,1\r\n", "OK\r\n");
    printf("DHCP enabled\n");
}
