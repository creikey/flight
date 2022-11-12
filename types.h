#pragma once

#define MAX_PLAYERS 16
#define MAX_ENTITIES 1024*25
#define BOX_SIZE 0.25f
#define PLAYER_SIZE ((V2){.x = BOX_SIZE, .y = BOX_SIZE})
#define PLAYER_MASS 0.5f
#define PLAYER_JETPACK_FORCE 1.5f
// #define PLAYER_JETPACK_FORCE 20.0f
#define PLAYER_JETPACK_SPICE_PER_SECOND 0.1f
#define MAX_HAND_REACH 1.0f
#define GOLD_COLLECT_RADIUS 0.3f
#define BUILD_BOX_SNAP_DIST_TO_SHIP 0.2f
#define BOX_MASS 1.0f
#define COLLISION_DAMAGE_SCALING 0.15f
#define THRUSTER_FORCE 12.0f
#define THRUSTER_ENERGY_USED_PER_SECOND 0.005f
#define VISION_RADIUS 12.0f
#define MAX_SERVER_TO_CLIENT 1024 * 512 // maximum size of serialized gamestate buffer
#define MAX_CLIENT_TO_SERVER 1024*10 // maximum size of serialized inputs and mic data
#define SUN_RADIUS 10.0f
#define INSTANT_DEATH_DISTANCE_FROM_SUN 2000.0f
#define SUN_POS ((V2){50.0f,0.0f})
#define SUN_GRAVITY_STRENGTH (9.0e2f)
#define SOLAR_ENERGY_PER_SECOND 0.02f
#define DAMAGE_TO_PLAYER_PER_BLOCK 0.1f
#define BATTERY_CAPACITY DAMAGE_TO_PLAYER_PER_BLOCK*0.7f
#define PLAYER_ENERGY_RECHARGE_PER_SECOND 0.1f
#define EXPLOSION_TIME 0.5f
#define EXPLOSION_PUSH_STRENGTH 5.0f
#define EXPLOSION_DAMAGE_PER_SEC 10.0f
#define EXPLOSION_RADIUS 1.0f
#define EXPLOSION_DAMAGE_THRESHOLD 0.2f // how much damage until it explodes
#define GOLD_UNLOCK_RADIUS 1.0f
#define TIME_BETWEEN_WORLD_SAVE 30.0f

// VOIP
#define VOIP_PACKET_BUFFER_SIZE 15	 // audio. Must be bigger than 2
#define VOIP_EXPECTED_FRAME_COUNT 480
#define VOIP_SAMPLE_RATE 48000
#define VOIP_TIME_PER_PACKET 1.0f / ((float)(VOIP_SAMPLE_RATE/VOIP_EXPECTED_FRAME_COUNT)) // in seconds
#define VOIP_PACKET_MAX_SIZE 4000
#define VOIP_DISTANCE_WHEN_CANT_HEAR (BOX_SIZE*13.0f)

#define TIMESTEP (1.0f / 60.0f) // not required to simulate at this, but this defines what tick the game is on
#define TIME_BETWEEN_INPUT_PACKETS (1.0f / 20.0f)
#define SERVER_PORT 2551
#define INPUT_BUFFER 6

// must make this header and set the target address, just #define SERVER_ADDRESS "127.0.0.1"
#include "ipsettings.h" // don't leak IP!

#include "miniaudio.h" // @Robust BAD. using miniaudio mutex construct for server thread synchronization. AWFUL!

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

#ifndef OPUS_TYPES_H
typedef int opus_int32;
#endif

#ifndef _STDBOOL

#define bool _Bool
#define false 0
#define true 1

#endif

typedef sgp_vec2 V2;
typedef sgp_point P2;

#define Log(...){                                     \
    fprintf(stdout, "%s:%d | ", __FILE__, __LINE__); \
    fprintf(stdout, __VA_ARGS__);}

enum BoxType
{
	BoxHullpiece,
	BoxThruster,
	BoxBattery,
	BoxCockpit,
	BoxMedbay,
	BoxSolarPanel,
	BoxExplosive,
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
typedef struct EntityID
{
	unsigned int generation; // VERY IMPORTANT if 0 then EntityID points to nothing, generation >= 1
	unsigned int index;      // index into the entity arena
} EntityID;

static bool entityids_same(EntityID a, EntityID b)
{
	return (a.generation == b.generation) && (a.index == b.index);
}

// when updated, must update serialization, AND comparison
// function in main.c
typedef struct InputFrame
{
	uint64_t tick;
	size_t id; // each input has unique, incrementing, I.D, so server doesn't double process inputs. Inputs to server should be ordered from 0-max like biggest id-smallest. This is done so if packet loss server still processes input
	V2 movement;

	bool seat_action;
	EntityID seat_to_inhabit;
	V2 hand_pos; // local to player transationally but not rotationally unless field below is not null, then it's local to that grid
	// @BeforeShip bounds check on the hand_pos so that players can't reach across the entire map
	EntityID grid_hand_pos_local_to; // when not null, hand_pos is local to this grid. this prevents bug where at high speeds the built block is in the wrong place on the selected grid

	bool dobuild;
	enum BoxType build_type;
	enum CompassRotation build_rotation;
} InputFrame;

typedef struct Entity
{
	bool exists;
	EntityID next_free_entity;
	unsigned int generation;

	bool no_save_to_disk; // stuff generated later on, like player's bodies or space stations that respawn.

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
	EntityID currently_inside_of_box;
	float goldness;         // how much the player is a winner

	// explosion
	bool is_explosion;
	V2 explosion_pos;
	V2 explosion_vel;
	float explosion_progresss; // in seconds

	// grids
	bool is_grid;
	float total_energy_capacity;
	EntityID boxes;

	// boxes
	bool is_box;
	bool always_visible; // always serialized to the player
	enum BoxType box_type;
	bool is_explosion_unlock;
	EntityID next_box;
	EntityID prev_box; // doubly linked so can remove in middle of chain
	enum CompassRotation compass_rotation;
	bool indestructible;
	float wanted_thrust; // the thrust command applied to the thruster
	float thrust;      // the actual thrust it can provide based on energy sources in the grid
	float energy_used; // battery
	float sun_amount; // solar panel, between 0 and 1
	EntityID player_who_is_inside_of_me;
} Entity;

typedef struct Player
{
	bool connected;
	bool unlocked_bombs;
	EntityID entity;
	EntityID last_used_medbay;
	InputFrame input;
} Player;
// gotta update the serialization functions when this changes
typedef struct GameState
{
	cpSpace* space;

	double time;

	V2 goldpos;

	Player players[MAX_PLAYERS];

	EntityID cur_spacestation;

	// Entity arena
	// ent:ity pointers can't move around because of how the physics engine handles user data.
	// if you really need this, potentially refactor to store entity IDs instead of pointers
	// in the shapes and bodies of chipmunk. Would require editing the library I think
	Entity* entities;
	unsigned int max_entities;    // maximum number of entities possible in the entities list
	unsigned int cur_next_entity; // next entity to pass on request of a new entity if the free list is empty
	EntityID free_list;
} GameState;

#define PLAYERS_ITER(players, cur) for(Player * cur = players; cur < players+MAX_PLAYERS; cur++) if(cur->connected)

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

typedef struct OpusPacket {
	bool exists;
	struct OpusPacket* next;

	char data[VOIP_PACKET_MAX_SIZE];
	opus_int32 length;
} OpusPacket;

typedef struct OpusBuffer {
	OpusPacket packets[VOIP_PACKET_BUFFER_SIZE];
	OpusPacket* next;
} OpusBuffer;

typedef struct ServerToClient
{
	struct GameState* cur_gs;
	OpusBuffer* playback_buffer;
	int your_player;
} ServerToClient;


typedef struct ClientToServer
{
	OpusBuffer* mic_data; // on serialize, flushes this of packets. On deserialize, fills it
	InputFrame inputs[INPUT_BUFFER];
} ClientToServer;

// server
void server(void* info); // data parameter required from thread api...

// gamestate
EntityID create_spacestation(GameState* gs);
void initialize(struct GameState* gs, void* entity_arena, size_t entity_arena_size);
void destroy(struct GameState* gs);
void process(struct GameState* gs, float dt); // does in place
Entity* closest_to_point_in_radius(struct GameState* gs, V2 point, float radius);
uint64_t tick(struct GameState* gs);

// all of these return if successful or not
bool server_to_client_serialize(struct ServerToClient* msg, char* bytes, size_t* out_len, size_t max_len, Entity* for_this_player, bool to_disk);
bool server_to_client_deserialize(struct ServerToClient* msg, char* bytes, size_t max_len, bool from_disk);
bool client_to_server_deserialize(GameState* gs, struct ClientToServer* msg, char* bytes, size_t max_len);
bool client_to_server_serialize(GameState* gs, struct ClientToServer* msg, char* bytes, size_t* out_len, size_t max_len);

// entities
Entity* get_entity(struct GameState* gs, EntityID id);
Entity* new_entity(struct GameState* gs);
EntityID get_id(struct GameState* gs, Entity* e);
V2 entity_pos(Entity* e);
void entity_set_rotation(Entity* e, float rot);
void entity_set_pos(Entity* e, V2 pos);
float entity_rotation(Entity* e);
void entity_ensure_in_orbit(Entity* e);
void entity_destroy(GameState* gs, Entity* e);
#define BOX_CHAIN_ITER(gs, cur, starting_box) for (Entity *cur = get_entity(gs, starting_box); cur != NULL; cur = get_entity(gs, cur->next_box))
#define BOXES_ITER(gs, cur, grid_entity_ptr) BOX_CHAIN_ITER(gs, cur, (grid_entity_ptr)->boxes)

// grid
void grid_create(struct GameState* gs, Entity* e);
void box_create(struct GameState* gs, Entity* new_box, Entity* grid, V2 pos);
V2 grid_com(Entity* grid);
V2 grid_vel(Entity* grid);
V2 box_vel(Entity* box);
V2 grid_local_to_world(Entity* grid, V2 local);
V2 grid_world_to_local(Entity* grid, V2 world);
V2 grid_snapped_box_pos(Entity* grid, V2 world); // returns the snapped pos in world coords
float entity_angular_velocity(Entity* grid);
V2 entity_shape_pos(Entity* box);
float box_rotation(Entity* box);

// thruster
V2 box_facing_vector(Entity* box);
V2 thruster_force(Entity* box);

// debug draw
void dbg_drawall();
void dbg_line(V2 from, V2 to);
void dbg_rect(V2 center);

typedef struct ServerThreadInfo {
	ma_mutex info_mutex;
	const char* world_save;
	bool should_quit;
} ServerThreadInfo;

static void clear_buffer(OpusBuffer* buff)
{
	*buff = (OpusBuffer){ 0 };
}

// you push a packet, get the return value, and fill it with data. It's that easy!
static OpusPacket* push_packet(OpusBuffer* buff)
{
	OpusPacket* to_return = NULL;
	for (size_t i = 0; i < VOIP_PACKET_BUFFER_SIZE; i++)
		if (!buff->packets[i].exists)
		{
			to_return = &buff->packets[i];
			break;
		}

	// no free packet found in the buffer
	if (to_return == NULL)
	{
		Log("Opus Buffer Full\n");
		clear_buffer(buff);
		to_return = &buff->packets[0];
#if 0
		to_return = buff->next;
		buff->next = buff->next->next;
#endif
	}

	*to_return = (OpusPacket){ 0 };
	to_return->exists = true;

	// add to the end of the linked list chain
	if (buff->next != NULL)
	{
		OpusPacket* cur = buff->next;
		while (cur->next != NULL) cur = cur->next;
		cur->next = to_return;
	}
	else {
		buff->next = to_return;
	}

	return to_return;
}

// how many unpopped packets there are, can't check for null on pop_packet because
// could be a skipped packet. This is used in a for loop to flush a packet buffer
static int num_queued_packets(OpusBuffer* buff)
{
	int to_return = 0;
	for (size_t i = 0; i < VOIP_PACKET_BUFFER_SIZE; i++)
		if (buff->packets[i].exists) to_return++;
	return to_return;
}

static OpusPacket* get_packet_at_index(OpusBuffer* buff, int i)
{
	OpusPacket* to_return = buff->next;
	int index_at = 0;
	while (index_at < i)
	{
		if (to_return->next == NULL)
		{
			Log("FAILED TO GET TO INDEX %d\n", i);
			return to_return;
		}
		to_return = to_return->next;
		index_at++;
	}
	return to_return;
}

// returns null if the packet was dropped, like if the buffer was too full
static OpusPacket* pop_packet(OpusBuffer* buff)
{
#if 0
	if (buff->skipped_packets > 0) {
		buff->skipped_packets--;
		return NULL;
	}
#endif

	OpusPacket* to_return = buff->next;
	if (buff->next != NULL) buff->next = buff->next->next;
	if (to_return != NULL) to_return->exists = false; // feels janky to do this
	return to_return;
}

#define DeferLoop(start, end) \
    for (int _i_ = ((start), 0); _i_ == 0; _i_ += 1, (end))

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

static float V2lengthsqr(V2 v)
{
	return v.x * v.x + v.y * v.y;
}

static float V2length(V2 v)
{
	return sqrtf(V2lengthsqr(v));
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

static float V2distsqr(V2 from, V2 to)
{
	return V2lengthsqr(V2sub(to, from));
}

static float V2dist(V2 from, V2 to)
{
	return sqrtf(V2distsqr(from, to));
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
