#pragma once

#define MAX_PLAYERS 4
#define BOX_SIZE 0.5f
#define MAX_HAND_REACH 1.0f
#define GOLD_COLLECT_RADIUS 0.3f
#define MAX_GRIDS 32
#define MAX_BOXES_PER_GRID 32
#define BOX_MASS 1.0f

// @Robust remove this include somehow, needed for sqrt and cos
#include <math.h>

// including headers from headers bad
#ifndef SOKOL_GP_INCLUDED

void sgp_set_color(float, float, float, float);

// @Robust use double precision for all vectors, when passed back to sokol
// somehow automatically or easily cast to floats
typedef struct sgp_vec2
{
    float x, y;
} sgp_vec2;

typedef sgp_vec2 sgp_point;

#endif

#ifndef CHIPMUNK_H
typedef void cpSpace;
typedef void cpBody;
typedef void cpShape;
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

// gotta update the serialization functions when this changes
struct GameState
{
    cpSpace *space;

    float time;

    V2 goldpos;

    struct Player
    {
        bool connected;

        int currently_inhabiting_index; // is equal to -1 when not inhabiting a grid
        V2 pos;
        V2 vel;
        float spice_taken_away; // at 1.0, out of spice
        float goldness;         // how much the player is a winner

        // input
        V2 movement;
        bool inhabit;

        V2 build; // @Robust this is messy, clean up?
        bool dobuild;
        int grid_index;
    } players[MAX_PLAYERS];

    // if body or shape is null, then that grid/box has been freed

    // important that this memory does not move around, each box shape in it has a pointer to its grid struct, stored in the box's shapes user_data
    struct Grid
    {
        cpBody *body;

        struct Box
        {
            cpShape *shape;
            float damage;
        } boxes[MAX_BOXES_PER_GRID]; // @Robust this needs to be dynamically allocated, huge disparity in how many blocks a body can have...
    } grids[MAX_GRIDS];
};

struct ServerToClient
{
    struct GameState *cur_gs;
    int your_player;
};

struct ClientToServer
{
    V2 movement;
    bool inhabit;
    V2 build;
    bool dobuild;
    int grid_index;
};

// server
void server(void *data); // data parameter required from thread api...

// gamestate
void initialize(struct GameState *gs); // must do this to place boxes into it and process
void destroy(struct GameState *gs);
void process(struct GameState *gs, float dt); // does in place
struct Grid *closest_to_point_in_radius(struct GameState *gs, V2 point, float radius);
void into_bytes(struct ServerToClient *gs, char *out_bytes, int *out_len, int max_len);
void from_bytes(struct ServerToClient *gs, char *bytes, int max_len);

// player
void reset_player(struct Player *p);

// grid
void grid_new(struct Grid *to_modify, struct GameState *gs, V2 pos);
V2 grid_com(struct Grid *grid);
V2 grid_pos(struct Grid *grid);
V2 grid_vel(struct Grid *grid);
V2 grid_snapped_box_pos(struct Grid *grid, V2 world); // returns the snapped pos in world coords
float grid_rotation(struct Grid *grid);
float grid_angular_velocity(struct Grid *grid);
void box_new(struct Box *to_modify, struct GameState *gs, struct Grid *grid, V2 pos);
V2 box_pos(struct Box *box);
float box_rotation(struct Box *box);

// debug draw
void dbg_drawall();
void dbg_line(V2 from, V2 to);
void dbg_rect(V2 center);

// helper
#define SKIPNULL(thing) \
    if (thing == NULL)  \
    continue

// all the math is static so that it can be defined in each compilation unit its included in

#define PI 3.14159f

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

// also known as atan2
static float V2angle(V2 vec)
{
    return atan2f(vec.y, vec.x);
}

static V2 V2sub(V2 a, V2 b)
{
    return (V2){
        .x = a.x - b.x,
        .y = a.y - b.y,
    };
}

static inline float clamp01(float f)
{
    return fmax(0.0f, fmin(f, 1.0f));
}

static float fract(float f)
{
    return f - floorf(f);
}

static float lerp(float a, float b, float f)
{
    return a * (1.0f - f) + (b * f);
}

static V2 V2lerp(V2 a, V2 b, float factor)
{
    V2 to_return = {0};
    to_return.x = lerp(a.x, b.x, factor);
    to_return.y = lerp(a.y, b.y, factor);

    return to_return;
}

typedef struct Color
{
    float r, g, b, a;
} Color;

static Color colhex(int r, int g, int b)
{
    return (Color){
        .r = (float)r / 255.0,
        .g = (float)g / 255.0,
        .b = (float)b / 255.0,
        .a = 1.0f,
    };
}

static Color Collerp(Color a, Color b, float factor)
{
    Color to_return = {0};
    to_return.r = lerp(a.r, b.r, factor);
    to_return.g = lerp(a.g, b.g, factor);
    to_return.b = lerp(a.b, b.b, factor);
    to_return.a = lerp(a.a, b.a, factor);

    return to_return;
}

static void set_color(Color c)
{
    sgp_set_color(c.r, c.g, c.b, c.a);
}

#define WHITE (Color){.r=1.0f,.g=1.0f,.b=1.0f,.a=1.0f}
#define GOLD colhex(255, 215, 0)