#!/usr/bin/env bash

gcc -o flight_server -Wall -O2 -Ithirdparty -Ithirdparty/enet/include -Ithirdparty/minilzo -Ithirdparty/Chipmunk2D/include -Ithirdparty/Chipmunk2D/include/chipmunk server_main.c server.c debugdraw.c gamestate.c sokol_impl.c thirdparty/minilzo/minilzo.c thirdparty/enet/*.c thirdparty/Chipmunk2D/src/*.c -lm -lpthread
