#pragma once

#include "ipsettings.h"

#define MAX_BOX_TYPES 64
#define MAX_PLAYERS 16
#define MAX_ENTITIES 1024 * 25
#define BOX_SIZE 0.25f
#define PLAYER_SIZE ((V2){.x = BOX_SIZE, .y = BOX_SIZE})
#define PLAYER_MASS 0.5f
#define PLAYER_JETPACK_FORCE 1.5f
// #define PLAYER_JETPACK_FORCE 20.0f
#define PLAYER_JETPACK_SPICE_PER_SECOND 0.1f
#define SCANNER_ENERGY_USE 0.05f
#define MAX_HAND_REACH 1.0f
#define SCANNER_SCAN_RATE 1.0f
#define SCANNER_RADIUS 1.0f
#define GOLD_COLLECT_RADIUS 0.3f
#define BUILD_BOX_SNAP_DIST_TO_SHIP 0.2f
#define BOX_MASS 1.0f
#define COLLISION_DAMAGE_SCALING 0.15f
#define THRUSTER_FORCE 12.0f
#define THRUSTER_ENERGY_USED_PER_SECOND 0.005f
#define VISION_RADIUS 12.0f
#define MAX_SERVER_TO_CLIENT 1024 * 512 // maximum size of serialized gamestate buffer
#define MAX_CLIENT_TO_SERVER 1024 * 10  // maximum size of serialized inputs and mic data
#define SUN_RADIUS 10.0f
#define INSTANT_DEATH_DISTANCE_FROM_SUN 2000.0f
#define SUN_POS ((V2){50.0f, 0.0f})
#ifdef NO_GRAVITY
#define SUN_GRAVITY_STRENGTH 0.0f
#else
#define SUN_GRAVITY_STRENGTH (9.0e2f)
#endif
#define SOLAR_ENERGY_PER_SECOND 0.04f
#define DAMAGE_TO_PLAYER_PER_BLOCK 0.1f
#define BATTERY_CAPACITY 1.5f
#define PLAYER_ENERGY_RECHARGE_PER_SECOND 0.2f
#define EXPLOSION_TIME 0.5f
#define EXPLOSION_PUSH_STRENGTH 5.0f
#define EXPLOSION_DAMAGE_PER_SEC 10.0f
#define EXPLOSION_RADIUS 1.0f
#define EXPLOSION_DAMAGE_THRESHOLD 0.2f // how much damage until it explodes
#define GOLD_UNLOCK_RADIUS 1.0f
#define TIME_BETWEEN_WORLD_SAVE 30.0f

// VOIP
#define VOIP_PACKET_BUFFER_SIZE 15 // audio. Must be bigger than 2
#define VOIP_EXPECTED_FRAME_COUNT 480
#define VOIP_SAMPLE_RATE 48000
// in seconds
#define VOIP_TIME_PER_PACKET (1.0f / ((float)((float)VOIP_SAMPLE_RATE / VOIP_EXPECTED_FRAME_COUNT))) 
#define VOIP_PACKET_MAX_SIZE 4000
#define VOIP_DISTANCE_WHEN_CANT_HEAR (VISION_RADIUS * 0.8f)

// multiplayer
#define MAX_REPREDICTION_TIME (TIMESTEP * 50.0f)
#define TIME_BETWEEN_SEND_GAMESTATE (1.0f / 20.0f)
#define TIME_BETWEEN_INPUT_PACKETS (1.0f / 20.0f)
#define TIMESTEP (1.0f / 60.0f) // server required to simulate at this, defines what tick the game is on
#define SERVER_PORT 2551
#define LOCAL_INPUT_QUEUE_MAX 90 // please god let you not have more than 90 frames of game latency
#define INPUT_QUEUE_MAX 15

// fucks up serialization if you change this, fix it if you do that!
#define BOX_UNLOCKS_TYPE uint64_t

// cross platform threadlocal variables
#if defined(WIN32) || defined(_WIN32) || defined(__WIN32__) || defined(__NT__)
#define THREADLOCAL __declspec(thread)
#else
#define THREADLOCAL __thread
#endif

// must make this header and set the target address, just #define SERVER_ADDRESS "127.0.0.1"
#include "ipsettings.h" // don't leak IP!

#include "miniaudio.h" // @Robust BAD. using miniaudio mutex construct for server thread synchronization. AWFUL!

// @Robust remove this include somehow, needed for sqrt and cos
#include <math.h>
#include <stdint.h> // tick is unsigned integer
#include <stdio.h>  // logging on errors for functions

// defined in gamestate.c. Janky
#ifndef assert
#define assert(condition) __flight_assert(condition, __FILE__, __LINE__, #condition)
#endif

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

#include "queue.h"
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

#define Log(...)                                     \
  {                                                  \
    fprintf(stdout, "%s:%d | ", __FILE__, __LINE__); \
    fprintf(stdout, __VA_ARGS__);                    \
  }

enum BoxType
{
  BoxInvalid,  // zero initialized box is invalid!
  BoxHullpiece,
  BoxThruster,
  BoxBattery,
  BoxCockpit,
  BoxMedbay,
  BoxSolarPanel,
  BoxExplosive,
  BoxScanner,
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

enum Squad
{
  SquadNone,
  SquadRed,
  SquadGreen,
  SquadBlue,
  SquadPurple,
  SquadLast,
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

// when updated, must update serialization, comparison in main.c, and the server
// on input received processing function
typedef struct InputFrame
{
  uint64_t tick;
  V2 movement;

  int take_over_squad; // -1 means not taking over any squad
  bool accept_cur_squad_invite;
  bool reject_cur_squad_invite;
  EntityID invite_this_player; // null means inviting nobody! @Robust make it so just sends interact pos input, and server processes who to invite. This depends on client side prediction + proper input processing at the right tick.

  bool seat_action;
  V2 hand_pos; // local to player transationally but not rotationally
  // @BeforeShip bounds check on the hand_pos so that players can't reach across the entire map

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
  cpBody *body;   // used by grid, player, and box
  cpShape *shape; // must be a box so shape_size can be set appropriately, and serialized

  // for serializing the shape
  // @Robust remove shape_parent_entity from this struct, use the shape's body to figure out
  // what the shape's parent entity is
  EntityID shape_parent_entity; // can't be zero if shape is nonzero
  V2 shape_size;

  // player
  bool is_player;
  enum Squad presenting_squad;
  EntityID currently_inside_of_box;
  enum Squad squad_invited_to; // if squad none, then no squad invite
  float goldness;              // how much the player is a winner

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
  enum BoxType box_type;
  bool is_platonic; // can't be destroyed, unaffected by physical forces
  bool always_visible; // always serialized to the player. @Robust check if not used
  EntityID next_box; // for the grid!
  EntityID prev_box; // doubly linked so can remove in middle of chain
  enum CompassRotation compass_rotation;
  bool indestructible;
  
  // used by medbay and cockpit
  EntityID player_who_is_inside_of_me; 
  
  // only serialized when box_type is thruster
  float wanted_thrust; // the thrust command applied to the thruster
  float thrust;        // the actual thrust it can provide based on energy sources in the grid
  
  // only serialized when box_type is battery
  float energy_used;   // battery, between 0 battery capacity. You have to look through code to figure out what that is! haha sucker!
  
  // only serialized when box_type is solar panel
  float sun_amount;    // solar panel, between 0 and 1
  
  // scanner only stuff!
  EntityID currently_scanning;
  float currently_scanning_progress; // when 1.0, scans it!
  BOX_UNLOCKS_TYPE blueprints_learned; // @Robust make this same type as blueprints
  float scanner_head_rotate_speed; // not serialized, cosmetic
  float scanner_head_rotate;
  V2 platonic_nearest_direction; // normalized
  float platonic_detection_strength; // from zero to one
} Entity;


typedef struct Player
{
  bool connected;
  BOX_UNLOCKS_TYPE box_unlocks; // each bit is that box's unlock
  enum Squad squad;
  EntityID entity;
  EntityID last_used_medbay;
  InputFrame input;
} Player;
// gotta update the serialization functions when this changes
typedef struct GameState
{
  cpSpace *space;

  double time; // @Robust separate tick integer not prone to precision issues. Could be very large as is saved to disk!

  V2 goldpos;

  Player players[MAX_PLAYERS];
  
  V2 platonic_positions[MAX_BOX_TYPES]; // don't want to search over every entity to get the nearest platonic box!
  
  bool server_side_computing; // some things only the server should know and calculate, like platonic locations

  // Entity arena
  // ent:ity pointers can't move around because of how the physics engine handles user data.
  // if you really need this, potentially refactor to store entity IDs instead of pointers
  // in the shapes and bodies of chipmunk. Would require editing the library I think
  Entity *entities;
  unsigned int max_entities;    // maximum number of entities possible in the entities list
  unsigned int cur_next_entity; // next entity to pass on request of a new entity if the free list is empty
  EntityID free_list;
} GameState;

#define PLAYERS_ITER(players, cur)                                \
  for (Player *cur = players; cur < players + MAX_PLAYERS; cur++) \
    if (cur->connected)

#define PI 3.14159f
#define TAU (PI * 2.0f)

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

typedef struct OpusPacket
{
  opus_int32 length;
  unsigned char data[VOIP_PACKET_MAX_SIZE];
} OpusPacket;

typedef struct ServerToClient
{
  struct GameState *cur_gs;
  Queue *audio_playback_buffer;
  int your_player;
} ServerToClient;

typedef struct ClientToServer
{
  Queue *mic_data;   // on serialize, flushes this of packets. On deserialize, fills it
  Queue *input_data; // does not flush on serialize! must be in order of tick
} ClientToServer;

#define DeferLoop(start, end) \
  for (int _i_ = ((start), 0); _i_ == 0; _i_ += 1, (end))

// server
void server(void *info); // data parameter required from thread api...
void create_player(Player *player);
bool box_unlocked(Player *player, enum BoxType box);

// gamestate
EntityID create_initial_world(GameState *gs);
void initialize(struct GameState *gs, void *entity_arena, size_t entity_arena_size);
void destroy(struct GameState *gs);
void process_fixed_timestep(GameState *gs);
void process(struct GameState *gs, float dt); // does in place
Entity *closest_box_to_point_in_radius(struct GameState *gs, V2 point, float radius, bool(*filter_func)(Entity*));
uint64_t tick(struct GameState *gs);

// all of these return if successful or not
bool server_to_client_serialize(struct ServerToClient *msg, unsigned char*bytes, size_t *out_len, size_t max_len, Entity *for_this_player, bool to_disk);
bool server_to_client_deserialize(struct ServerToClient *msg, unsigned char*bytes, size_t max_len, bool from_disk);
bool client_to_server_deserialize(GameState *gs, struct ClientToServer *msg, unsigned char*bytes, size_t max_len);
bool client_to_server_serialize(GameState *gs, struct ClientToServer *msg, unsigned char*bytes, size_t *out_len, size_t max_len);

// entities
Entity *get_entity(struct GameState *gs, EntityID id);
Entity *new_entity(struct GameState *gs);
EntityID get_id(struct GameState *gs, Entity *e);
V2 entity_pos(Entity *e);
void entity_set_rotation(Entity *e, float rot);
void entity_set_pos(Entity *e, V2 pos);
float entity_rotation(Entity *e);
void entity_ensure_in_orbit(Entity *e);
void entity_destroy(GameState *gs, Entity *e);
#define BOX_CHAIN_ITER(gs, cur, starting_box) for (Entity *cur = get_entity(gs, starting_box); cur != NULL; cur = get_entity(gs, cur->next_box))
#define BOXES_ITER(gs, cur, grid_entity_ptr) BOX_CHAIN_ITER(gs, cur, (grid_entity_ptr)->boxes)

// grid
void grid_create(struct GameState *gs, Entity *e);
void box_create(struct GameState *gs, Entity *new_box, Entity *grid, V2 pos);
Entity *box_grid(Entity *box);
V2 grid_com(Entity *grid);
V2 grid_vel(Entity *grid);
V2 box_vel(Entity *box);
V2 grid_local_to_world(Entity *grid, V2 local);
V2 grid_world_to_local(Entity *grid, V2 world);
V2 grid_snapped_box_pos(Entity *grid, V2 world); // returns the snapped pos in world coords
float entity_angular_velocity(Entity *grid);
V2 entity_shape_pos(Entity *box);
float box_rotation(Entity *box);

// thruster
V2 box_facing_vector(Entity *box);
V2 thruster_force(Entity *box);

// debug draw
void dbg_drawall();
void dbg_line(V2 from, V2 to);
void dbg_rect(V2 center);

typedef struct ServerThreadInfo
{
  ma_mutex info_mutex;
  const char *world_save;
  bool should_quit;
} ServerThreadInfo;
// all the math is static so that it can be defined in each compilation unit its included in

typedef struct AABB
{
  float x, y, width, height;
} AABB;

static AABB centered_at(V2 point, V2 size)
{
  return (AABB){
      .x = point.x - size.x / 2.0f,
      .y = point.y - size.y / 2.0f,
      .width = size.x,
      .height = size.y,
  };
}

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
  return (V2){
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
  return (V2){
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

static float lerp_angle(float p_from, float p_to, float p_weight)
{
  float difference = fmodf(p_to - p_from, (float)TAU);
  float distance = fmodf(2.0f * difference, (float)TAU) - difference;
  return p_from + distance * p_weight;
}

static V2 V2floor(V2 p)
{
  return (V2){floorf(p.x), floorf(p.y)};
}

static V2 V2fract(V2 p)
{
  return (V2){fract(p.x), fract(p.y)};
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
  V2 to_return = {0};
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
  return (Color){
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
