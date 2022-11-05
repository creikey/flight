#pragma once

#define MAX_PLAYERS 4
#define MAX_ENTITIES 1024
#define BOX_SIZE 0.25f
#define PLAYER_SIZE ((V2){.x = BOX_SIZE, .y = BOX_SIZE})
#define PLAYER_MASS 0.5f
#define PLAYER_JETPACK_FORCE 1.0f
#define PLAYER_JETPACK_SPICE_PER_SECOND 0.1f
#define MAX_HAND_REACH 1.0f
#define GOLD_COLLECT_RADIUS 0.3f
#define BUILD_BOX_SNAP_DIST_TO_SHIP 0.2f
#define BOX_MASS 1.0f
#define THRUSTER_FORCE 4.0f
#define THRUSTER_ENERGY_USED_PER_SECOND 0.05f
#define VISION_RADIUS 8.0f

#define TIMESTEP (1.0f / 60.0f) // not required to simulate at this, but this defines what tick the game is on
#define TIME_BETWEEN_INPUT_PACKETS (1.0f / 20.0f)
#define SERVER_PORT 2551
#define INPUT_BUFFER 6

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
	BoxCockpit,
	BoxLast,
};

enum CompassRotation
{
	Right,
	Down,
	Left,
	Up,
	RotationLast,
};

// when generation is 0, invalid ID
typedef struct
{
	unsigned int generation; // if 0 then EntityID points to nothing, generation >= 1
	unsigned int index;      // index into the entity arena
} EntityID;

static bool entityids_same(EntityID a, EntityID b)
{
	return (a.generation == b.generation) && (a.index == b.index);
}

// when updated, must update serialization, AND comparison
// function in main.c
typedef struct
{
	uint64_t tick;
	size_t id; // each input has unique, incrementing, I.D, so server doesn't double process inputs. Inputs to server should be ordered from 0-max like biggest id-smallest. This is done so if packet loss server still processes input
	V2 movement;

	bool seat_action;
	EntityID seat_to_inhabit;
	V2 hand_pos;
	EntityID grid_hand_pos_local_to; // when not null, hand_pos is local to this grid. this prevents bug where 

	bool dobuild;
	enum BoxType build_type;
	enum CompassRotation build_rotation;
} InputFrame;

typedef struct Entity
{
	bool exists;
	EntityID next_free_entity;
	unsigned int generation;

	float damage;   // used by box and player
	cpBody* body;   // used by grid, player, and box
	cpShape* shape; // must be a box so shape_size can be set appropriately, and serialized


	// for serializing the shape
	// @Robust remove shape_parent_entity from this struct, use the shape's body to figure out
	// what the shape's parent entity is
	EntityID shape_parent_entity; // can't be zero if shape is nonzero
	V2 shape_size;

	// player
	bool is_player;
	EntityID currently_piloting_seat;
	float spice_taken_away; // at 1.0, out of spice
	float goldness;         // how much the player is a winner

	// grids
	bool is_grid;
	float total_energy_capacity;
	EntityID boxes;

	// boxes
	bool is_box;
	enum BoxType box_type;
	EntityID next_box;
	EntityID prev_box; // doubly linked so can remove in middle of chain
	enum CompassRotation compass_rotation;
	float wanted_thrust; // the thrust command applied to the thruster
	float thrust;      // the actual thrust it can provide based on energy sources in the grid
	float energy_used; // battery
	EntityID piloted_by;
} Entity;

typedef struct Player
{
	bool connected;
	EntityID entity;
	InputFrame input;
} Player;
// gotta update the serialization functions when this changes
typedef struct GameState
{
	cpSpace* space;

	double time;

	V2 goldpos;

	Player players[MAX_PLAYERS];

	// Entity arena
	// ent:ity pointers can't move around because of how the physics engine handles user data.
	// if you really need this, potentially refactor to store entity IDs instead of pointers
	// in the shapes and bodies of chipmunk. Would require editing the library I think
	Entity* entities;
	unsigned int max_entities;    // maximum number of entities possible in the entities list
	unsigned int cur_next_entity; // next entity to pass on request of a new entity if the free list is empty
	EntityID free_list;
} GameState;

#define PI 3.14159f

// returns in radians
static float rotangle(enum CompassRotation rot)
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

typedef struct ServerToClient
{
	struct GameState* cur_gs;
	int your_player;
} ServerToClient;

struct ClientToServer
{
	InputFrame inputs[INPUT_BUFFER];
};

// server
void server(void* data); // data parameter required from thread api...

// gamestate
void initialize(struct GameState* gs, void* entity_arena, size_t entity_arena_size);
void destroy(struct GameState* gs);
void process(struct GameState* gs, float dt); // does in place
Entity* closest_to_point_in_radius(struct GameState* gs, V2 point, float radius);
uint64_t tick(struct GameState* gs);
void into_bytes(struct ServerToClient* gs, char* out_bytes, size_t* out_len, size_t max_len);
void from_bytes(struct ServerToClient* gs, char* bytes, size_t max_len);

// entities
Entity* get_entity(struct GameState* gs, EntityID id);
Entity* new_entity(struct GameState* gs);
EntityID get_id(struct GameState* gs, Entity* e);
V2 entity_pos(Entity* e);
void entity_set_rotation(Entity* e, float rot);
void entity_set_pos(Entity* e, V2 pos);
float entity_rotation(Entity* e);
#define BOX_CHAIN_ITER(gs, cur, starting_box) for (Entity *cur = get_entity(gs, starting_box); cur != NULL; cur = get_entity(gs, cur->next_box))
#define BOXES_ITER(gs, cur, grid_entity_ptr) BOX_CHAIN_ITER(gs, cur, (grid_entity_ptr)->boxes)

// player
void player_destroy(struct Player* p);
void player_new(struct Player* p);

// grid
void grid_create(struct GameState* gs, Entity* e);
void box_create(struct GameState* gs, Entity* new_box, Entity* grid, V2 pos);
V2 grid_com(Entity* grid);
V2 grid_vel(Entity* grid);
V2 grid_local_to_world(Entity* grid, V2 local);
V2 grid_world_to_local(Entity* grid, V2 world);
V2 grid_snapped_box_pos(Entity* grid, V2 world); // returns the snapped pos in world coords
float entity_angular_velocity(Entity* grid);
V2 entity_shape_pos(Entity* box);
V2 box_pos(Entity* box); // returns in world coords
float box_rotation(Entity* box);

// thruster
V2 box_facing_vector(Entity* box);
V2 thruster_force(Entity* box);

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
	return (V2) {
		.x = a.x + b.x,
			.y = a.y + b.y,
	};
}

static V2 V2scale(V2 a, float f)
{
	return (V2) {
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
	return (V2) {
		.x = vec.x * cosf(theta) - vec.y * sinf(theta),
			.y = vec.x * sinf(theta) + vec.y * cosf(theta),
	};
}

// also known as atan2
static float V2angle(V2 vec)
{
	return atan2f(vec.y, vec.x);
}

static V2 V2sub(V2 a, V2 b)
{
	return (V2) {
		.x = a.x - b.x,
			.y = a.y - b.y,
	};
}

static bool V2equal(V2 a, V2 b, float eps)
{
	return V2length(V2sub(a, b)) < eps;
}

static inline float clamp01(float f)
{
	return fmaxf(0.0f, fminf(f, 1.0f));
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

static V2 V2floor(V2 p)
{
	return (V2) { floorf(p.x), floorf(p.y) };
}

static V2 V2fract(V2 p)
{
	return (V2) { fract(p.x), fract(p.y) };
}
/*
float noise(V2 p)
{
	V2 id = V2floor(p);
	V2 f = V2fract(p);

	V2 u = V2dot(f, f) * (3.0f - 2.0f * f);

	return mix(mix(random(id + V2(0.0, 0.0)),
		random(id + V2(1.0, 0.0)), u.x),
		mix(random(id + V2(0.0, 1.0)),
			random(id + V2(1.0, 1.0)), u.x),
		u.y);
}

float fbm(V2 p)
{
	float f = 0.0;
	float gat = 0.0;

	for (float octave = 0.; octave < 5.; ++octave)
	{
		float la = pow(2.0, octave);
		float ga = pow(0.5, octave + 1.);
		f += ga * noise(la * p);
		gat += ga;
	}

	f = f / gat;

	return f;
}
*/

static V2 V2lerp(V2 a, V2 b, float factor)
{
	V2 to_return = { 0 };
	to_return.x = lerp(a.x, b.x, factor);
	to_return.y = lerp(a.y, b.y, factor);

	return to_return;
}

// for random generation
static float hash11(float p)
{
	p = fract(p * .1031f);
	p *= p + 33.33f;
	p *= p + p;
	return fract(p);
}

typedef struct Color
{
	float r, g, b, a;
} Color;

static Color colhex(int r, int g, int b)
{
	return (Color) {
		.r = (float)r / 255.0f,
			.g = (float)g / 255.0f,
			.b = (float)b / 255.0f,
			.a = 1.0f,
	};
}

static Color colhexcode(int hexcode)
{
	// 0x020509;
	int r = (hexcode >> 16) & 0xFF;
	int g = (hexcode >> 8) & 0xFF;
	int b = (hexcode >> 0) & 0xFF;
	return colhex(r, g, b);
}

static Color Collerp(Color a, Color b, float factor)
{
	Color to_return = { 0 };
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