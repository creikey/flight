#pragma once

// @Robust remove this include somehow, needed for sqrt and cos
#include <math.h>

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

#define bool _Bool
#define false 0
#define true 1

#endif

typedef sgp_vec2 V2;
typedef sgp_point P2;

#define Log(...)                                     \
    fprintf(stdout, "%s:%d | ", __FILE__, __LINE__); \
    fprintf(stdout, __VA_ARGS__)
#define MAX_BOXES 32
#define MAX_PLAYERS 4
#define BOX_SIZE 0.5f

struct Body
{
    P2 position;
    P2 old_position;

    float rotation;
    float old_rotation;

    V2 acceleration;
    float angular_acceleration;
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
void server(void *data);

// gamestate
void process(struct GameState *gs, float dt); // does in place

// debug draw
void dbg_drawall();
void dbg_line(V2 from, V2 to);
void dbg_rect(V2 center);

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

static float V2length(V2 v)
{
    return sqrtf(v.x * v.x + v.y * v.y);
}

static V2 V2normalize(V2 v)
{
    return V2scale(v, 1.0f / V2length(v));
}

static float V2dot(V2 a, V2 b)
{
    return a.x * b.x + a.y * b.y;
}

static float V2projectvalue(V2 vec, V2 onto)
{
    float length_onto = V2length(onto);
    return V2dot(vec, onto) / (length_onto * length_onto);
}

static V2 V2project(V2 vec, V2 onto)
{
    return V2scale(onto, V2projectvalue(vec, onto));
}

static V2 V2rotate(V2 vec, float theta)
{
    return (V2){
        .x = vec.x * cos(theta) - vec.y * sin(theta),
        .y = vec.x * sin(theta) + vec.y * cos(theta),
    };
}

static V2 V2sub(V2 a, V2 b)
{
    return (V2){
        .x = a.x - b.x,
        .y = a.y - b.y,
    };
}