# Program for driving a 64x32 HUB75 LED Matrix using a Raspberry Pi Pico over WiFi

This repository contains the code for driving a 64x32 HUB75 LED Matrix like those sold by
adafruit over WiFi with a Raspberry Pi Pico. For WiFi a ESP-01s is used. The communication protocol is
custom to simplify the code and the frames are compressed using ZSTD to increase throughput. It's possible
to achieve around 28 FPS. Please see my 
[Reddit post](https://www.reddit.com/r/raspberry_pi/comments/r7v4xo/raspberry_pi_pico_driving_64x32_ledmatrix_over/) 
for more details.

## Getting Started

This program has some very bad documentation, but here are the basics to get started:
1. Connect a Raspberry Pi Pico to a HUB75 LED Matrix. The pin configuration is the same as shown
[here](https://github.com/raspberrypi/pico-examples/tree/master/pio/hub75).
2. Connect a ESP-01S over UART to pin 16 and 17 on the Pico.
3. Clone this repo (make sure to specify --recursive)
4. Adjust WiFi name and password (mkdir build; cmake .. -DCMAKE_BUILD_TYPE=Release "-DWIFI_PASSWORD=1234" "-DWIFI_NAME=a b c")
5. Adjust hostname to connect to (see main.c, mine is *sauron* or *mqtt*)
6. Compile and upload the program (cp build/ledmatrix_pico.uf2 /path/to/pico)

## Communication Protocol
The communication protocol works as follows:
1. Every packet starts with S
2. Then comes the length of the remaining packet as ascii after the colon
3. Then comes the colon (':')
4. Then comes the data

The client starts of by sending it's id in the data packet. Then it will receive data packets constantly,
containing the frame data as a zstd compressed RGB888 64x32 row-first bitmap.

## I couldn't follow you/I have something I need to tell you

If you have any questions, feel free to contact me via reddit or e-mail (address see commits).
