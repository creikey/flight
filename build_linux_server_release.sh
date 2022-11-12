#!/usr/bin/env bash

mkdir thirdparty/opus/build
cd thirdparty/opus/build
cmake ..
cmake --build .
cd -

gcc -o flight_server -Wall -O2 -Ithirdparty -Ithirdparty/opus/include -Ithirdparty/enet/include -Ithirdparty/minilzo -Ithirdparty/Chipmunk2D/include -Ithirdparty/Chipmunk2D/include/chipmunk server_main.c server.c debugdraw.c gamestate.c sokol_impl.c thirdparty/minilzo/minilzo.c thirdparty/enet/*.c thirdparty/Chipmunk2D/src/*.c -lm -lpthread -ldl thirdparty/opus/build/libopus.a
