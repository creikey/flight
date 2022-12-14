#pragma once

#include "buildsettings.h"

#define MAX_BOX_TYPES 64
#define ZOOM_MIN 0.04   // smaller means you can zoom out more
#define ZOOM_MAX 1500.0 // bigger means you can zoom in more
#define MAX_PLAYERS 16
#define MAX_SUNS 8
#define MAX_ENTITIES 1024 * 25
#define BOX_SIZE 0.25f
#define MERGE_MAX_DIST (BOX_SIZE / 2.0f + 0.01f)
#define PLAYER_SIZE ((cpVect){.x = BOX_SIZE, .y = BOX_SIZE})
#define PLAYER_MASS 0.5f
#ifdef FAT_THRUSTERS
#define PLAYER_JETPACK_FORCE 200.0f
#else
#define PLAYER_JETPACK_FORCE 3.5f
#endif
#define PLAYER_JETPACK_TORQUE 0.05f
#define MISSILE_RANGE 4.0f
#define MISSILE_BURN_TIME 1.5f
#define MISSILE_ARM_TIME 0.5f
#define MISSILE_BURN_FORCE 4.0f
#define MISSILE_MASS 1.0f
// how many missiles grown per second
#define MISSILE_DAMAGE_THRESHOLD 0.2f
#define MISSILE_CHARGE_RATE 0.5f
// centered on the sprite
#define MISSILE_SPRITE_SIZE ((cpVect){.x = BOX_SIZE, .y = BOX_SIZE})
#define MISSILE_COLLIDER_SIZE ((cpVect){.x = BOX_SIZE * 0.5f, .y = BOX_SIZE * 0.5f})
#define MISSILE_SPAWN_DIST (sqrt((BOX_SIZE / 2.0) * (BOX_SIZE / 2.0) * 2.0) + MISSILE_COLLIDER_SIZE.x / 2.0 + 0.1)
#define PLAYER_JETPACK_ROTATION_ENERGY_PER_SECOND 0.2f
#define PLAYER_JETPACK_SPICE_PER_SECOND 0.08f
#define PLAYER_BIG_SCALING 300.0

#define ORB_MASS 4.0
#define ORB_RADIUS 1.0
#define ORB_HEAT_FORCE_MULTIPLIER 5.0
#define ORB_DRAG_CONSTANT 1.0
#define ORB_FROZEN_DRAG_CONSTANT 10.0
#define ORB_HEAT_MAX_DETECTION_DIST 100.0
#define ORB_HEAL_RATE 0.2
#define ORB_MAX_FORCE 200.0

#define SCANNER_ENERGY_USE 0.05f
#define MAX_HAND_REACH 1.0f
#define SCANNER_SCAN_RATE 0.5f
#define SCANNER_RADIUS 1.0f
#define GOLD_COLLECT_RADIUS 0.3f
#define BUILD_BOX_SNAP_DIST_TO_SHIP 0.2f
#define BOX_MASS 1.0f
#define COLLISION_DAMAGE_SCALING 0.15f
#define THRUSTER_FORCE 24.0f
#define THRUSTER_ENERGY_USED_PER_SECOND 0.005f
#define GYROSCOPE_ENERGY_USED_PER_SECOND 0.005f
#define GYROSCOPE_TORQUE 1.5f
#define GYROSCOPE_PROPORTIONAL_INERTIAL_RESPONSE 0.7 // between 0-1. How strongly responds to rotation, to stop the rotation
#define CLOAKING_ENERGY_USE 0.1f
#define CLOAKING_PANEL_SIZE BOX_SIZE * 3.0f
#define VISION_RADIUS 12.0f
#define MAX_SERVER_TO_CLIENT 1024 * 512 // maximum size of serialized gamestate buffer
#define MAX_CLIENT_TO_SERVER 1024 * 10  // maximum size of serialized inputs and mic data
#define GRAVITY_CONSTANT 0.05f
#define GRAVITY_SMALLEST 0.01f // used to determine when gravity is clamped to 0.0f
#define INSTANT_DEATH_DISTANCE_FROM_CENTER 10000.0f
#define SOLAR_ENERGY_PER_SECOND 0.09f
#define DAMAGE_TO_PLAYER_PER_BLOCK 0.1f
#define BATTERY_CAPACITY 1.5f
#define PLAYER_ENERGY_RECHARGE_PER_SECOND 0.2
#define EXPLOSION_TIME 0.5f
#define EXPLOSION_DAMAGE_PER_SEC 10.0f
#define EXPLOSION_DAMAGE_THRESHOLD 0.2f // how much damage until it explodes
#define GOLD_UNLOCK_RADIUS 1.0f

#ifndef TIME_BETWEEN_WORLD_SAVE
#define TIME_BETWEEN_WORLD_SAVE 30.0f
#endif

#define MISSILE_EXPLOSION_PUSH 2.5f
#define MISSILE_EXPLOSION_RADIUS 0.4f

#define BOMB_EXPLOSION_PUSH 5.0f
#define BOMB_EXPLOSION_RADIUS 1.0f

// VOIP
#define VOIP_PACKET_BUFFER_SIZE 15 // audio. Must be bigger than 2
#define VOIP_EXPECTED_FRAME_COUNT 240
#define VOIP_SAMPLE_RATE (48000 / 2)

// in seconds
#define VOIP_TIME_PER_PACKET (1.0f / ((float)((float)VOIP_SAMPLE_RATE / VOIP_EXPECTED_FRAME_COUNT)))
#define VOIP_PACKET_MAX_SIZE 4000
#define VOIP_DISTANCE_WHEN_CANT_HEAR (VISION_RADIUS * 0.8f)

// multiplayer
#define CAUTIOUS_MULTIPLIER 0.8 // how overboard to go with the time ahead predicting, makes it less likely that inputs are lost
#define TICKS_BEHIND_DO_SNAP 6 // when this many ticks behind, instead of dilating time SNAP to the healthy ticks ahead
#define MAX_MS_SPENT_REPREDICTING 30.0f
#define TIME_BETWEEN_SEND_GAMESTATE (1.0f / 20.0f)
#define TIME_BETWEEN_INPUT_PACKETS (1.0f / 20.0f)
#define TIMESTEP (1.0f / 60.0f)  // server required to simulate at this, defines what tick the game is on
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

#define ARRLEN(x) (sizeof(x) / sizeof((x)[0]))

#include "cpVect.h"    // offers vector functions and types for the structs
#include "miniaudio.h" // @Robust BAD. using miniaudio mutex construct for server thread synchronization. AWFUL!

#include <math.h>   // sqrt and cos vector functions
#include <stdint.h> // tick is unsigned integer
#include <stdio.h>  // logging on errors for functions

// defined in gamestate.c. Janky
#define flight_assert(condition) __flight_assert(condition, __FILE__, __LINE__, #condition)

// including headers from headers bad
#include <chipmunk.h> // unfortunate but needs cpSpace, cpBody, cpShape etc

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

extern FILE *log_file;
#include <time.h> // the time in logging!

void fill_time_string(char *to_fill, size_t max_length);

#define Log(...)                                                           \
  {                                                                        \
    char time_string[2048] = {0};                                          \
    fill_time_string(time_string, 2048);                                   \
    fprintf(stdout, "%s | %s:%d | ", time_string, __FILE__, __LINE__);     \
    fprintf(stdout, __VA_ARGS__);                                          \
    if (log_file != NULL)                                                  \
    {                                                                      \
      fprintf(log_file, "%s | %s:%d | ", time_string, __FILE__, __LINE__); \
      fprintf(log_file, __VA_ARGS__);                                      \
    }                                                                      \
  }

enum BoxType
{
  BoxInvalid, // zero initialized box is invalid!
  BoxHullpiece,
  BoxThruster,
  BoxBattery,
  BoxCockpit,
  BoxMedbay,
  BoxSolarPanel,
  BoxExplosive,
  BoxScanner,
  BoxGyroscope,
  BoxCloaking,
  BoxMissileLauncher,
  BoxMerge,
  BoxLast,
};

static inline bool box_interactible(enum BoxType type)
{
  enum BoxType types[] = {
      BoxCockpit,
      BoxMedbay,
      BoxMerge,
      BoxScanner,
  };
  for (int i = 0; i < ARRLEN(types); i++)
    if (types[i] == type)
      return true;
  return false;
}

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

static inline bool entityids_same(EntityID a, EntityID b)
{
  return (a.generation == b.generation) && (a.index == b.index);
}

// when updated, must update serialization, comparison in main.c, and the server
// on input received processing function
typedef struct InputFrame
{
  uint64_t tick;
  cpVect movement;
  double rotation;

  int take_over_squad; // -1 means not taking over any squad
  bool accept_cur_squad_invite;
  bool reject_cur_squad_invite;
  EntityID invite_this_player; // null means inviting nobody! @Robust make it so just sends interact pos input, and server processes who to invite. This depends on client side prediction + proper input processing at the right tick.

  bool seat_action;
  cpVect hand_pos; // local to player transationally but not rotationally

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

  double damage;  // used by box, player, and orb
  cpBody *body;   // used by grid, player, and box
  cpShape *shape; // must be a box so shape_size can be set appropriately, and serialized

  // players and boxes can be cloaked
  // If this is within 2 timesteps of the current game time, the entity is invisible.
  double time_was_last_cloaked;
  enum Squad last_cloaked_by_squad;

  // for serializing the shape
  // @Robust remove shape_parent_entity from this struct, use the shape's body to figure out
  // what the shape's parent entity is
  bool is_circle_shape;
  EntityID shape_parent_entity; // can't be zero if shape is nonzero
  double shape_radius; // only when circle shape
  cpVect shape_size; // only when rect shape

  // player
  bool is_player;
  enum Squad owning_squad; // also controls what the player can see, because of cloaking!
  EntityID currently_inside_of_box;
  enum Squad squad_invited_to; // if squad none, then no squad invite
  double goldness;             // how much the player is a winner

  // explosion
  bool is_explosion;
  cpVect explosion_pos;
  cpVect explosion_vel;
  double explosion_progress; // in seconds
  double explosion_push_strength;
  double explosion_radius;

  // sun
  bool is_sun;
  cpVect sun_vel;
  cpVect sun_pos;
  double sun_mass;
  double sun_radius;

  // missile
  bool is_missile;
  double time_burned_for; // until MISSILE_BURN_TIME. Before MISSILE_ARM_TIME cannot explode

  // orb
  bool is_orb;

  // grids
  bool is_grid;
  double total_energy_capacity;
  EntityID boxes;

  // boxes
  bool is_box;
  enum BoxType box_type;
  bool is_platonic;    // can't be destroyed, unaffected by physical forces
  bool always_visible; // always serialized to the player. @Robust check if not used
  EntityID next_box;   // for the grid!
  EntityID prev_box;   // doubly linked so can remove in middle of chain
  enum CompassRotation compass_rotation;
  bool indestructible;

  // used by multiple boxes that use power to see if it should show low power warning.
  // not serialized, populated during client side prediction. For the medbay
  // is only valid if the medbay has somebody inside of it
  double energy_effectiveness; // from 0 to 1, how effectively the box is operating given available power

  // merger
  bool wants_disconnect; // don't serialized, termporary value not used across frames

  // missile launcher
  double missile_construction_charge;

  // used by medbay and cockpit
  EntityID player_who_is_inside_of_me;


  // only serialized when box_type is thruster or gyroscope, used for both. Thrust
  // can mean rotation thrust!
  double wanted_thrust; // the thrust command applied to the thruster
  double thrust;        // the actual thrust it can provide based on energy sources in the grid
  
  // only gyroscope, velocity not serialized. Cosmetic
  double gyrospin_angle;
  double gyrospin_velocity;

  // only serialized when box_type is battery
  double energy_used; // battery, between 0 battery capacity. You have to look through code to figure out what that is! haha sucker!

  // only serialized when box_type is solar panel
  double sun_amount; // solar panel, between 0 and 1

  // cloaking only
  double cloaking_power; // 0.0 if unable to be used because no power, 1.0 if fully cloaking!

  // scanner only stuff!
  EntityID currently_scanning;
  double currently_scanning_progress; // when 1.0, scans it!
  BOX_UNLOCKS_TYPE blueprints_learned;
  double scanner_head_rotate_speed; // not serialized, cosmetic
  double scanner_head_rotate;
  cpVect platonic_nearest_direction;  // normalized
  double platonic_detection_strength; // from zero to one
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

// use i.sun to access the current sun's pointer
typedef struct SunIter
{
  int i;
  Entity *sun;
} SunIter;
#define SUNS_ITER(gs_ptr)                      \
  for (SunIter i = {0}; i.i < MAX_SUNS; i.i++) \
    if ((i.sun = get_entity(gs_ptr, (gs_ptr)->suns[i.i])) != NULL)

// gotta update the serialization functions when this changes
typedef struct GameState
{
  cpSpace *space;

  uint64_t tick;
  double subframe_time; // @Robust remove this, I don't think it's used anymore

  Player players[MAX_PLAYERS];
  EntityID suns[MAX_SUNS]; // can't have holes in it for serialization

  cpVect platonic_positions[MAX_BOX_TYPES]; // don't want to search over every entity to get the nearest platonic box!

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
static inline double rotangle(enum CompassRotation rot)
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
void create_initial_world(GameState *gs);
void initialize(struct GameState *gs, void *entity_arena, size_t entity_arena_size);
void destroy(struct GameState *gs);
void process_fixed_timestep(GameState *gs);
// if is subframe, doesn't always increment the tick. When enough
// subframe time has been processed, increments the tick
void process(struct GameState *gs, double dt); // does in place
Entity *closest_box_to_point_in_radius(struct GameState *gs, cpVect point, double radius, bool (*filter_func)(Entity *));
uint64_t tick(struct GameState *gs);
double elapsed_time(GameState *gs);
double sun_dist_no_gravity(Entity *sun);

void quit_with_popup(const char *message_utf8, const char *title_utf8);

// all of these return if successful or not
bool server_to_client_serialize(struct ServerToClient *msg, unsigned char *bytes, size_t *out_len, size_t max_len, Entity *for_this_player, bool to_disk);
bool server_to_client_deserialize(struct ServerToClient *msg, unsigned char *bytes, size_t max_len, bool from_disk);
bool client_to_server_deserialize(GameState *gs, struct ClientToServer *msg, unsigned char *bytes, size_t max_len);
bool client_to_server_serialize(GameState *gs, struct ClientToServer *msg, unsigned char *bytes, size_t *out_len, size_t max_len);

// entities
bool is_burning(Entity *missile);
Entity *get_entity(struct GameState *gs, EntityID id);
Entity *new_entity(struct GameState *gs);
EntityID get_id(struct GameState *gs, Entity *e);
cpVect entity_pos(Entity *e);
void entity_set_rotation(Entity *e, double rot);
void entity_set_pos(Entity *e, cpVect pos);
double entity_rotation(Entity *e);
void entity_ensure_in_orbit(GameState *gs, Entity *e);
void entity_destroy(GameState *gs, Entity *e);
#define BOX_CHAIN_ITER(gs, cur, starting_box) for (Entity *cur = get_entity(gs, starting_box); cur != NULL; cur = get_entity(gs, cur->next_box))
#define BOXES_ITER(gs, cur, grid_entity_ptr) BOX_CHAIN_ITER(gs, cur, (grid_entity_ptr)->boxes)
typedef struct LauncherTarget
{
  bool target_found;
  double facing_angle; // in global coords
} LauncherTarget;
LauncherTarget missile_launcher_target(GameState *gs, Entity *launcher);

// grid
void grid_create(struct GameState *gs, Entity *e);
void box_create(struct GameState *gs, Entity *new_box, Entity *grid, cpVect pos);
Entity *box_grid(Entity *box);
cpVect grid_com(Entity *grid);
cpVect grid_vel(Entity *grid);
cpVect box_vel(Entity *box);
cpVect grid_local_to_world(Entity *grid, cpVect local);
cpVect grid_world_to_local(Entity *grid, cpVect world);
cpVect grid_snapped_box_pos(Entity *grid, cpVect world); // returns the snapped pos in world coords
double entity_angular_velocity(Entity *grid);
cpVect entity_shape_pos(Entity *box);
double box_rotation(Entity *box);

// thruster
cpVect box_facing_vector(Entity *box);
cpVect thruster_force(Entity *box);

// debug draw
void dbg_drawall();
void dbg_line(cpVect from, cpVect to);
void dbg_rect(cpVect center);

typedef struct ServerThreadInfo
{
  ma_mutex info_mutex;
  const char *world_save;
  bool should_quit;
} ServerThreadInfo;
// all the math is static so that it can be defined in each compilation unit its included in

typedef struct AABB
{
  double x, y, width, height;
} AABB;

static inline AABB centered_at(cpVect point, cpVect size)
{
  return (AABB){
      .x = point.x - size.x / 2.0f,
      .y = point.y - size.y / 2.0f,
      .width = size.x,
      .height = size.y,
  };
}

static inline bool has_point(AABB aabb, cpVect point)
{
  return point.x > aabb.x && point.x < aabb.x + aabb.width && point.y > aabb.y && point.y < aabb.y + aabb.height;
}

static inline double cpvprojectval(cpVect vec, cpVect onto)
{
  double length_onto = cpvlength(onto);
  return cpvdot(vec, onto) / (length_onto * length_onto);
}

// spins around by theta
static inline cpVect cpvspin(cpVect vec, double theta)
{
  return (cpVect){
      .x = vec.x * cos(theta) - vec.y * sin(theta),
      .y = vec.x * sin(theta) + vec.y * cos(theta),
  };
}

// also known as atan2
static inline double cpvangle(cpVect vec)
{
  return atan2(vec.y, vec.x);
}

typedef struct BoxCentered
{
  cpVect pos;
  double rotation;
  cpVect size; // half width and half height, centered on position
} BoxCentered;

static inline bool box_has_point(BoxCentered box, cpVect point)
{
  cpVect local_point = cpvspin(cpvsub(point, box.pos), -box.rotation);
  return has_point((AABB){.x = -box.size.x/2.0, .y = -box.size.y/2.0, .width = box.size.x, .height = box.size.y}, local_point);
}

static double sign(double f)
{
  if (f >= 0.0f)
    return 1.0f;
  else
    return -1.0f;
}

static inline double clamp01(double f)
{
  return fmax(0.0f, fmin(f, 1.0f));
}

static inline double clamp(double f, double minimum, double maximum)
{
  if (f < minimum)
    return minimum;
  if (f > maximum)
    return maximum;
  return f;
}

static inline double cpvanglediff(cpVect a, cpVect b)
{
  double acos_input = cpvdot(a, b) / (cpvlength(a) * cpvlength(b));
  acos_input = clamp(acos_input, -1.0f, 1.0f);
  flight_assert(acos_input >= -1.0f && acos_input <= 1.0f);
  return acos(acos_input) * sign(cpvdot(a, b));
}

static inline double fract(double f)
{
  return f - floor(f);
}

static inline double lerp(double a, double b, double f)
{
  return a * (1.0f - f) + (b * f);
}

static inline double lerp_angle(double p_from, double p_to, double p_weight)
{
  double difference = fmod(p_to - p_from, (float)TAU);
  double distance = fmod(2.0f * difference, (float)TAU) - difference;
  return p_from + distance * p_weight;
}

static inline cpVect cpvfloor(cpVect p)
{
  return (cpVect){floor(p.x), floor(p.y)};
}

static inline cpVect cpvfract(cpVect p)
{
  return (cpVect){fract(p.x), fract(p.y)};
}

// for random generation
static inline double hash11(double p)
{
  p = fract(p * .1031f);
  p *= p + 33.33f;
  p *= p + p;
  return fract(p);
}

static inline double deg2rad(double deg)
{
  return (deg / 360.0f) * 2.0f * PI;
}
