#pragma once

#define MAX_PLAYERS 4
#define BOX_SIZE 0.25f
#define MAX_HAND_REACH 1.0f
#define GOLD_COLLECT_RADIUS 0.3f
#define MAX_GRIDS 32
#define BUILD_BOX_SNAP_DIST_TO_SHIP 0.2f
#define MAX_BOXES_PER_GRID 32
#define BOX_MASS 1.0f
#define THRUSTER_FORCE 4.0f
#define THRUSTER_ENERGY_USED_PER_SECOND 0.05f

#define TIMESTEP (1.0f / 60.0f) // not required to simulate at this, but this defines what tick the game is on
#define TIME_BETWEEN_INPUT_PACKETS (1.0f / 20.0f)
#define SERVER_PORT 2551
#define INPUT_BUFFER 4

// must make this header and set the target address, just #define SERVER_ADDRESS "127.0.0.1"
#include "ipsettings.h" // don't leak IP!

// @Robust remove this include somehow, needed for sqrt and cos
#include <math.h>
#include <stdint.h> // tick is unsigned integer
#include <stdio.h>  // logging on errors for functions

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

enum BoxType
{
    BoxHullpiece,
    BoxThruster,
    BoxBattery,
    BoxLast,
};

enum Rotation
{
    Right,
    Down,
    Left,
    Up,
    RotationLast,
};

struct InputFrame
{
    uint64_t tick;
    V2 movement;
    bool inhabit;

    // if grid_index != -1, this is in local coordinates to the grid
    V2 build;
    bool dobuild;
    enum BoxType build_type;
    enum Rotation build_rotation;
    int grid_index;
};

// gotta update the serialization functions when this changes
struct GameState
{
    cpSpace *space;

    uint64_t tick;
    double time;

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
        // @Cleanup make this a frameinput struct instead of copying over all the fields like this

        struct InputFrame input;
    } players[MAX_PLAYERS];

    // if body or shape is null, then that grid/box has been freed

    // important that this memory does not move around, each box shape in it has a pointer to its grid struct, stored in the box's shapes user_data
    struct Grid
    {
        cpBody *body;

        struct Box
        {
            enum BoxType type;
            enum Rotation rotation;

            // thruster
            float thrust; // must be between 0 and 1
            
            // battery
            float energy_used; // must be between 0 and 1

            cpShape *shape;
            float damage;
        } boxes[MAX_BOXES_PER_GRID]; // @Robust this needs to be dynamically allocated, huge disparity in how many blocks a body can have...
    } grids[MAX_GRIDS];
};

#define PI 3.14159f

// returns in radians
static float rotangle(enum Rotation rot)
{
    switch (rot)
    {
    case Right:
        return 0.0f;
        break;
    case Down:
        return -PI / 2.0f;
        break;
    case Left:
        return -PI;
        break;
    case Up:
        return -3.0f * PI / 2.0f;
        break;
    default:
        Log("Unknown rotation %d\n", rot);
        return -0.0f;
        break;
    }
}

struct ServerToClient
{
    struct GameState *cur_gs;
    int your_player;
};

struct ClientToServer
{
    struct InputFrame inputs[INPUT_BUFFER];
};

// server
void server(void *data); // data parameter required from thread api...

// gamestate
void initialize(struct GameState *gs); // must do this to place boxes into it and process
void destroy(struct GameState *gs);
void process(struct GameState *gs, float dt); // does in place
struct Grid *closest_to_point_in_radius(struct GameState *gs, V2 point, float radius);
uint64_t tick(struct GameState *gs);
void into_bytes(struct ServerToClient *gs, char *out_bytes, int *out_len, int max_len);
void from_bytes(struct ServerToClient *gs, char *bytes, int max_len);

// player
void reset_player(struct Player *p);

// grid
void grid_new(struct Grid *to_modify, struct GameState *gs, V2 pos);
V2 grid_com(struct Grid *grid);
V2 grid_pos(struct Grid *grid);
V2 grid_vel(struct Grid *grid);
V2 grid_local_to_world(struct Grid *grid, V2 local);
V2 grid_world_to_local(struct Grid *grid, V2 world);
V2 grid_snapped_box_pos(struct Grid *grid, V2 world); // returns the snapped pos in world coords
float grid_rotation(struct Grid *grid);
float grid_angular_velocity(struct Grid *grid);
void box_new(struct Box *to_modify, struct GameState *gs, struct Grid *grid, V2 pos);
V2 box_pos(struct Box *box);
float box_rotation(struct Box *box);

// thruster
V2 thruster_direction(struct Box *box);
V2 thruster_force(struct Box *box);

// debug draw
void dbg_drawall();
void dbg_line(V2 from, V2 to);
void dbg_rect(V2 center);

// helper
#define SKIPNULL(thing) \
    if (thing == NULL)  \
    continue

// all the math is static so that it can be defined in each compilation unit its included in

typedef struct AABB
{
    float x, y, width, height;
} AABB;

static bool has_point(AABB aabb, V2 point)
{
    return point.x > aabb.x && point.x < aabb.x + aabb.width && point.y > aabb.y && point.y < aabb.y + aabb.height;
}

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

static bool V2cmp(V2 a, V2 b, float eps)
{
    return V2length(V2sub(a, b)) < eps;
}

static inline float clamp01(float f)
{
    return fmax(0.0f, fmin(f, 1.0f));
}

static inline float clamp(float f, float minimum, float maximum)
{
    if (f < minimum)
        return minimum;
    if (f > maximum)
        return maximum;
    return f;
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

#define WHITE \
    (Color) { .r = 1.0f, .g = 1.0f, .b = 1.0f, .a = 1.0f }
#define RED \
    (Color) { .r = 1.0f, .g = 0.0f, .b = 0.0f, .a = 1.0f }
#define GOLD colhex(255, 215, 0)