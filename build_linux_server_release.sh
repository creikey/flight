#!/usr/bin/env bash

mkdir -p thirdparty/opus/build || exit 1
cd thirdparty/opus/build || exit 1
cmake .. || exit 1
cmake --build . || exit 1
cd - || exit 1

gcc -o flight_server -Wall -O2 -DNDEBUG -DRELEASE -Ithirdparty -Ithirdparty/opus/include -Ithirdparty/enet/include -Ithirdparty/minilzo -Ithirdparty/Chipmunk2D/include -Ithirdparty/Chipmunk2D/include/chipmunk server_main.c server.c debugdraw.c gamestate.c sokol_impl.c thirdparty/minilzo/minilzo.c thirdparty/enet/*.c thirdparty/Chipmunk2D/src/*.c -lm -lpthread -ldl thirdparty/opus/build/libopus.a || exit 1
