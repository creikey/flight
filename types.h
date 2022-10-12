#pragma once

// including headers from headers bad
#ifndef SOKOL_GP_INCLUDED

typedef struct sgp_vec2
{
    float x, y;
} sgp_vec2;

typedef sgp_vec2 sgp_point;

#endif

#include <stdbool.h>

#ifndef _STDBOOL

#define bool  _Bool
#define false 0
#define true  1

#endif

typedef sgp_vec2 V2;
typedef sgp_point P2;

#define MAX_BOXES 32
#define MAX_PLAYERS 2
#define BOX_SIZE 0.5f
#define TIMESTEP 1.0f / 60.0f

struct Body
{
    P2 position;
    P2 old_position;
    V2 acceleration;
};

struct Player
{
    struct Body body;
    bool connected;
    V2 input;
};

struct GameState
{
    struct Player players[MAX_PLAYERS];

    int num_boxes;
    struct Box
    {
        struct Body body;
    } boxes[MAX_BOXES];
};

struct ServerToClient
{
    struct GameState cur_gs;
    int your_player;
};

struct ClientToServer
{
    V2 input;
};

// server
void server();

// gamestate
void process(struct GameState * gs, float dt); // does in place


// all the math is static so that it can be defined in each compilation unit its included in

static V2 V2add(V2 a, V2 b)
{
    return (V2){
        .x = a.x + b.x,
        .y = a.y + b.y,
    };
}

static V2 V2scale(V2 a, float f)
{
    return (V2){
        .x = a.x * f,
        .y = a.y * f,
    };
}

static V2 V2sub(V2 a, V2 b)
{
    return (V2){
        .x = a.x - b.x,
        .y = a.y - b.y,
    };
}