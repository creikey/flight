#include <chipmunk.h>
#define QUEUE_IMPL
#include "queue.h"
#include "stdbool.h"
#include "types.h"
#define PROFILING_IMPL
#include "profiling.h"

#include "buildsettings.h" // debug/developer settings

#include <stdio.h>  // flight_assert logging
#include <string.h> // memset

// do not use any global variables to process gamestate

// super try not to depend on external libraries like enet or sokol to keep build process simple,
// gamestate its own portable submodule. If need to link to other stuff document here:
// - debug.c for debug drawing
// - chipmunk

#ifdef ASSERT_DO_POPUP_AND_CRASH
#ifdef _WIN32
#ifndef UNICODE
#define UNICODE // I think?
#endif
#include <windows.h>

LPWSTR
fromUTF8(
    const char *src,
    size_t src_length, /* = 0 */
    size_t *out_length /* = NULL */
)
{
  if (!src)
  {
    return NULL;
  }

  if (src_length == 0)
  {
    src_length = strlen(src);
  }
  int length = MultiByteToWideChar(CP_UTF8, 0, src, (int)src_length, 0, 0);
  LPWSTR output_buffer = (LPWSTR)malloc((length + 1) * sizeof(wchar_t));
  if (output_buffer)
  {
    MultiByteToWideChar(CP_UTF8, 0, src, (int)src_length, output_buffer, (int)length);
    output_buffer[length] = L'\0';
  }
  if (out_length)
  {
    *out_length = length;
  }
  return output_buffer;
}
#endif // win32
#endif // ASSERT_DO_POPUP_AND_CRASH

FILE *log_file = NULL;

void quit_with_popup(const char *message_utf8, const char *title_utf8)
{
#ifdef _WIN32
  size_t message_out_len = 0;
  size_t title_out_len = 0;
  LPWSTR message_wchar = fromUTF8(message_utf8, strlen(message_utf8), &message_out_len);
  LPWSTR title_wchar = fromUTF8(title_utf8, strlen(title_utf8), &title_out_len);
  int msgboxID = MessageBox(
      NULL,
      message_wchar,
      title_wchar,
      MB_ICONEXCLAMATION | MB_OK);
  (void)msgboxID;

  free(message_wchar);
  free(title_wchar);

  (void)message_out_len;
  (void)title_out_len;
#endif
  exit(0);
}

void __flight_assert(bool cond, const char *file, int line, const char *cond_string)
{
  if (!cond)
  {
#define MESSAGE_BUFFER_SIZE 2048
    char message_buffer[MESSAGE_BUFFER_SIZE] = {0};
    snprintf(message_buffer, MESSAGE_BUFFER_SIZE, "%s:%d | Assertion %s failed\n", file, line, cond_string);
    Log("%s", message_buffer);
    if (log_file != NULL)
    {
      fprintf(log_file, "%s", message_buffer);
    }
#ifdef ASSERT_DO_POPUP_AND_CRASH
    char dialogbox_message[MESSAGE_BUFFER_SIZE] = {0};
    snprintf(dialogbox_message, MESSAGE_BUFFER_SIZE, "Critical error! Please report this in #bug-reports with a screenshot, description of what you were doing, and the file 'atris.log' located next to the executable\n%s\nClosing now.\n", message_buffer);
    quit_with_popup(dialogbox_message, "Assertion Failed");
#endif
  }
}

bool is_burning(Entity *missile)
{
  flight_assert(missile->is_missile);
  return missile->time_burned_for < MISSILE_BURN_TIME;
}

bool was_entity_deleted(GameState *gs, EntityID id)
{
  if (id.generation == 0)
    return false; // generation 0 means null entity ID, not a deleted entity
  Entity *the_entity = &gs->entities[id.index];
  return (!the_entity->exists || the_entity->generation != id.generation);
}

Entity *get_entity_even_if_dead(GameState *gs, EntityID id)
{
  if (id.generation == 0)
  {
    return NULL;
  }
  if (!(id.index < gs->cur_next_entity || gs->cur_next_entity == 0))
    return NULL;
  if (!(id.index < gs->max_entities))
    return NULL;
  Entity *to_return = &gs->entities[id.index];
  // don't validate the generation either
  return to_return;
}

// may return null if it doesn't exist anymore
Entity *get_entity(GameState *gs, EntityID id)
{

  Entity *to_return = get_entity_even_if_dead(gs, id);
  if (was_entity_deleted(gs, id))
    return NULL;
  return to_return;
}

bool cloaking_active(GameState *gs, Entity *e)
{
  // cloaking doesn't work for first 1/2 second of game because when initializing
  // everything needs to be uncloaked
  return elapsed_time(gs) >= 0.5 && (elapsed_time(gs) - e->time_was_last_cloaked) <= TIMESTEP * 2.0;
}

bool is_cloaked(GameState *gs, Entity *e, Entity *this_players_perspective)
{
  flight_assert(this_players_perspective != NULL);
  flight_assert(this_players_perspective->is_player);
  bool cloaked = cloaking_active(gs, e);
  if (e->is_player)
  {
    return cloaked && e->owning_squad != this_players_perspective->owning_squad;
  }
  else
  {
    return cloaked && this_players_perspective->owning_squad != e->last_cloaked_by_squad;
  }
}

static BOX_UNLOCKS_TYPE box_unlock_number(enum BoxType box)
{
  flight_assert((BOX_UNLOCKS_TYPE)box < 64);
  return (BOX_UNLOCKS_TYPE)((BOX_UNLOCKS_TYPE)1 << ((BOX_UNLOCKS_TYPE)box));
}

static bool learned_boxes_has_box(BOX_UNLOCKS_TYPE learned, enum BoxType box)
{
  return (learned & box_unlock_number(box)) > 0;
}

void unlock_box(Player *player, enum BoxType box)
{
  flight_assert(box < MAX_BOX_TYPES);
  flight_assert(box != BoxInvalid);
  player->box_unlocks |= box_unlock_number(box);
}

bool box_unlocked(Player *player, enum BoxType box)
{
  flight_assert(box < MAX_BOX_TYPES);
  if (box == BoxInvalid)
    return false;
  return learned_boxes_has_box(player->box_unlocks, box);
}

EntityID get_id(GameState *gs, Entity *e)
{
  if (e == NULL)
    return (EntityID){0};

  size_t index = (e - gs->entities);
  flight_assert(index >= 0);
  flight_assert(index < gs->cur_next_entity);

  return (EntityID){
      .generation = e->generation,
      .index = (unsigned int)index,
  };
}

static Entity *cp_shape_entity(cpShape *shape)
{
  return (Entity *)cpShapeGetUserData(shape);
}

static Entity *cp_body_entity(cpBody *body)
{
  return (Entity *)cpBodyGetUserData(body);
}

static GameState *cp_space_gs(cpSpace *space)
{
  return (GameState *)cpSpaceGetUserData(space);
}

static GameState *entitys_gamestate(Entity *e)
{
  flight_assert(e->body != NULL || e->shape != NULL);
  if (e->shape != NULL)
  {
    return cp_space_gs(cpShapeGetSpace(e->shape));
  }
  if (e->body != NULL)
  {
    return cp_space_gs(cpBodyGetSpace(e->body));
  }
  return NULL;
}

int grid_num_boxes(GameState *gs, Entity *e)
{
  flight_assert(e->is_grid);
  int to_return = 0;

  BOXES_ITER(gs, cur, e)
  to_return++;

  return to_return;
}

void box_remove_from_boxes(GameState *gs, Entity *box)
{
  flight_assert(box->is_box);
  Entity *prev_box = get_entity(gs, box->prev_box);
  Entity *next_box = get_entity(gs, box->next_box);
  if (prev_box != NULL)
  {
    if (prev_box->is_box)
      prev_box->next_box = get_id(gs, next_box);
    else if (prev_box->is_grid)
      prev_box->boxes = get_id(gs, next_box);
  }
  if (next_box != NULL)
  {
    flight_assert(next_box->is_box);
    next_box->prev_box = get_id(gs, prev_box);
  }
  box->next_box = (EntityID){0};
  box->prev_box = (EntityID){0};
}

cpVect player_vel(GameState *gs, Entity *e);
cpVect entity_vel(GameState *gs, Entity *e)
{
  flight_assert(e->is_box || e->is_player || e->body != NULL || e->is_explosion);
  if (e->is_box)
    return box_vel(e);
  if (e->is_player)
    return player_vel(gs, e);
  if (e->body != NULL)
    return (cpBodyGetVelocity(e->body));
  if (e->is_explosion)
    return e->explosion_vel;
  flight_assert(false);
  return (cpVect){0};
}

typedef struct QueryResult
{
  cpShape *shape;
  cpVect pointA;
  cpVect pointB;
} QueryResult;
static THREADLOCAL char query_result_data[QUEUE_SIZE_FOR_ELEMENTS(sizeof(QueryResult), 256)] = {0};
// the data starts off NULL, on the first call sets it to result data
static THREADLOCAL Queue query_result = {0};

static void shape_query_callback(cpShape *shape, cpContactPointSet *points, void *data)
{
  flight_assert(points->count >= 1); // bad, not exactly sure what the points look like. Just taking the first one for now. @Robust good debug drawing for this and figure it out. Make debug rects fade away instead of only drawn for one frame, makes one off things visible
  QueryResult *new = queue_push_element(&query_result);
  if (new == NULL)
  {
    (void)queue_pop_element(&query_result);
    new = queue_push_element(&query_result);
  }
  new->shape = shape;
  new->pointA = points->points[0].pointA;
  new->pointB = points->points[0].pointB;
}

// shapes are pushed to query result
static void shape_query(cpSpace *space, cpShape *shape)
{
  queue_init(&query_result, sizeof(QueryResult), query_result_data, ARRLEN(query_result_data));
  queue_clear(&query_result);
  cpSpaceShapeQuery(space, shape, shape_query_callback, NULL);
}

// shapes are pushed to query result
static void circle_query(cpSpace *space, cpVect pos, double radius)
{
  cpBody *tmp_body = cpBodyNew(0, 0);
  cpBodySetPosition(tmp_body, pos);
  cpShape *tmp_shape = cpCircleShapeNew(tmp_body, radius, cpv(0, 0));
  shape_query(space, tmp_shape);
  cpBodyFree(tmp_body);
  cpShapeFree(tmp_shape);
}

static void rect_query(cpSpace *space, BoxCentered box)
{
  cpBody *tmp_body = cpBodyNew(0, 0);
  cpBodySetPosition(tmp_body, box.pos);
  cpBodySetAngle(tmp_body, box.rotation);
  cpShape *tmp_shape = cpBoxShapeNew(tmp_body, box.size.x * 2.0, box.size.y * 2.0, 0.0);
  shape_query(space, tmp_shape);
  cpBodyFree(tmp_body);
  cpShapeFree(tmp_shape);
}

static THREADLOCAL cpShape *found_merge_shape = NULL;
static void raycast_query_callback(cpShape *shape, cpVect point, cpVect normal, cpFloat alpha, void *data)
{
  Entity *grid_to_exclude = (Entity *)data;
  flight_assert(grid_to_exclude != NULL);

  if (cp_shape_entity(shape)->is_box && cp_shape_entity(shape)->box_type == BoxMerge && box_grid(cp_shape_entity(shape)) != grid_to_exclude)
  {
    found_merge_shape = shape;
  }
}

static THREADLOCAL cpVect from_point = {0};
static int sort_bodies_callback(const void *a, const void *b)
{
  cpVect a_pos = cpBodyGetPosition(*(cpBody **)a);
  cpVect b_pos = cpBodyGetPosition(*(cpBody **)b);
  double a_dist = cpvdist(a_pos, from_point);
  double b_dist = cpvdist(b_pos, from_point);
  if (a_dist - b_dist < 0.0)
  {
    return -1;
  }
  else
  {
    return 1;
  }
  // return (int)(a_dist - b_dist);
}

LauncherTarget missile_launcher_target(GameState *gs, Entity *launcher)
{
  double to_face = 0.0;
  double nearest_dist = INFINITY;
  bool target_found = false;
  circle_query(gs->space, entity_pos(launcher), MISSILE_RANGE);
  QUEUE_ITER(&query_result, QueryResult, res)
  {
    cpShape *cur_shape = res->shape;
    Entity *other = cp_shape_entity(cur_shape);
    flight_assert(other->is_box || other->is_player || other->is_missile || other->is_orb);

    cpVect to = cpvsub(entity_pos(other), entity_pos(launcher));
    bool should_attack = true;
    if (other->is_box && box_grid(other) == box_grid(launcher))
      should_attack = false;
    if (other->owning_squad == launcher->owning_squad)
      should_attack = false;
    if (other->is_orb)
      should_attack = true;

    if (should_attack && cpvlength(to) < nearest_dist)
    {
      target_found = true;
      nearest_dist = cpvlength(to);

      // lookahead by their velocity
      cpVect rel_velocity = cpvsub(entity_vel(gs, other), entity_vel(gs, launcher));
      double dist = cpvdist(entity_pos(other), entity_pos(launcher)) - MISSILE_SPAWN_DIST;

      double time_of_travel = sqrt((2.0 * dist) / (MISSILE_BURN_FORCE / MISSILE_MASS));

      cpVect other_future_pos = cpvadd(entity_pos(other), cpvmult(rel_velocity, time_of_travel));

      cpVect adjusted_to = cpvsub(other_future_pos, entity_pos(launcher));

      to_face = cpvangle(adjusted_to);
    }
  }
  return (LauncherTarget){.target_found = target_found, .facing_angle = to_face};
}

void destroy_constraints(cpBody *body, cpConstraint *constraint, void *data)
{
  ((Entity *)cpConstraintGetUserData(constraint))->landed_constraint = NULL;
  cpSpaceRemoveConstraint(cpBodyGetSpace(body), constraint);
  cpConstraintFree(constraint);
}

void destroy_child_shape(cpBody *body, cpShape *shape, void *data)
{
  GameState *gs = (GameState *)data;
  if (cp_shape_entity(shape) == NULL)
  {
    // support the case where no parent entity *SPECIFICALLY* for grid_correct_for_holes,
    // where the entities that are part of the old grid are reused so entityids are preserved
    cpSpaceRemoveShape(gs->space, shape);
    cpShapeFree(shape);
  }
  else
  {
    entity_memory_free(gs, cp_shape_entity(shape));
  }
}

void entity_free_allocated(Entity *e)
{
#define MAYBE_FREE(variable, free_call) \
  if (variable != NULL)                 \
  {                                     \
    free_call(variable);                \
    variable = NULL;                    \
  }
  MAYBE_FREE(e->shape, cpShapeFree);
  MAYBE_FREE(e->body, cpBodyFree);
  MAYBE_FREE(e->landed_constraint, cpConstraintFree);
}

// Destroys the entity and puts its slot back into the free list. Doesn't obey game rules
// like making sure grids don't have holes in them, for that you want entity_destroy.
// *Does* free all owned memory/entities though, e.g grids free the boxes they own.
void entity_memory_free(GameState *gs, Entity *e)
{
  flight_assert(e->exists);

  if (e->is_grid)
  {
    BOXES_ITER(gs, cur, e)
    {
      entity_memory_free(gs, cur);
    }
  }
  if (e->is_box)
  {
    box_remove_from_boxes(gs, e);
  }
  if (e->shape != NULL)
  {
    cpSpaceRemoveShape(gs->space, e->shape);
  }
  if (e->landed_constraint != NULL)
  {
    cpSpaceRemoveConstraint(gs->space, e->landed_constraint);
  }
  if (e->body != NULL)
  {
    // need to do this here because body which constraint is attached to can be destroyed
    // NOT TRUE: can't do this here because the handle to the constraint cannot be set to NULL. Constraints are freed by the entities that own them
    cpBodyEachConstraint(e->body, destroy_constraints, NULL);
    cpBodyEachShape(e->body, destroy_child_shape, (void *)gs);
    cpSpaceRemoveBody(gs->space, e->body);
  }
  entity_free_allocated(e);
  Entity *front_of_free_list = get_entity(gs, gs->free_list);
  if (front_of_free_list != NULL)
    flight_assert(!front_of_free_list->exists);
  unsigned int gen = e->generation;
  *e = (Entity){0};
  e->generation = gen;
  e->next_free_entity = gs->free_list;
  gs->free_list = get_id(gs, e);
}

Entity *new_entity(GameState *gs)
{
  Entity *to_return = NULL;
  Entity *possible_free_list = get_entity_even_if_dead(gs, gs->free_list);
  if (possible_free_list != NULL)
  {
    flight_assert(possible_free_list->generation == gs->free_list.generation);
    to_return = possible_free_list;
    flight_assert(!to_return->exists);
    gs->free_list = to_return->next_free_entity;
  }
  else
  {
    flight_assert(gs->cur_next_entity < gs->max_entities); // too many entities if fails
    to_return = &gs->entities[gs->cur_next_entity];
    gs->cur_next_entity++;
  }

  to_return->generation++;
  to_return->exists = true;
  return to_return;
}

// pos, mass, radius
EntityID create_sun(GameState *gs, Entity *new_sun, cpVect pos, cpVect vel, double mass, double radius)
{
  flight_assert(new_sun != NULL);
  new_sun->is_sun = true;
  new_sun->sun_pos = pos;
  new_sun->sun_vel = vel;
  new_sun->sun_mass = mass;
  new_sun->sun_radius = radius;
  new_sun->always_visible = true;

  return get_id(gs, new_sun);
}

void create_body(GameState *gs, Entity *e)
{
  flight_assert(gs->space != NULL);

  if (e->body != NULL)
  {
    cpSpaceRemoveBody(gs->space, e->body);
    cpBodyFree(e->body);
    e->body = NULL;
  }

  cpBody *body = cpSpaceAddBody(gs->space, cpBodyNew(0.0, 0.0)); // zeros for mass/moment of inertia means automatically calculated from its collision shapes
  e->body = body;
  cpBodySetUserData(e->body, (void *)e);
}

// must always call this after creating a constraint
void on_create_constraint(Entity *e, cpConstraint *c)
{
  cpConstraintSetUserData(c, (cpDataPointer)e);
}

cpVect player_vel(GameState *gs, Entity *player)
{
  flight_assert(player->is_player);
  Entity *potential_seat = get_entity(gs, player->currently_inside_of_box);
  if (potential_seat != NULL && !potential_seat->is_box)
  {
    Log("Weird ass motherfucking bug where the seat inside of is an explosion or some shit\n");
    return (cpBodyGetVelocity(player->body));
    // flight_assert(potential_seat->is_box);
  }
  else
  {
    if (potential_seat != NULL)
    {
      return (cpBodyGetVelocity(get_entity(gs, potential_seat->shape_parent_entity)->body));
    }
  }
  return (cpBodyGetVelocity(player->body));
}

void grid_create(GameState *gs, Entity *e)
{
  e->is_grid = true;
  create_body(gs, e);
}

void entity_set_rotation(Entity *e, double rot)
{
  flight_assert(e->body != NULL);
  cpBodySetAngle(e->body, rot);
}

void entity_set_pos(Entity *e, cpVect pos)
{
  flight_assert(e->body != NULL);
  cpBodySetPosition(e->body, (pos));
}

// IMPORTANT: all shapes must exist in one of these categories, as by default chipmunk assigns an object
// to be in every category. Which is bad because that doesn't make sense for entities
enum
{
  DEFAULT = 1 << 0,
  BOXES = 1 << 1,
  MERGE_BOXES = 1 << 2,
};
static const cpShapeFilter FILTER_ONLY_BOXES = {CP_NO_GROUP, BOXES, BOXES};
static const cpShapeFilter FILTER_BOXES = {CP_NO_GROUP, DEFAULT | BOXES, CP_ALL_CATEGORIES};
static const cpShapeFilter FILTER_MERGE_BOX = {CP_NO_GROUP, DEFAULT | BOXES | MERGE_BOXES, CP_ALL_CATEGORIES};
static const cpShapeFilter FILTER_ONLY_MERGE_BOX = {CP_NO_GROUP, MERGE_BOXES, CP_ALL_CATEGORIES};
static const cpShapeFilter FILTER_DEFAULT = {CP_NO_GROUP, DEFAULT, CP_ALL_CATEGORIES};
#define PLAYER_FILTER FILTER_DEFAULT

// size is (1/2 the width, 1/2 the height)
void create_rectangle_shape(GameState *gs, Entity *e, Entity *parent, cpVect pos, cpVect size, double mass)
{
  PROFILE_SCOPE("Create rectangle shape")
  {
    // @Robust remove this garbage
    if (e->shape != NULL)
    {
      PROFILE_SCOPE("Freeing shape")
      {
        cpSpaceRemoveShape(gs->space, e->shape);
        cpShapeFree(e->shape);
        e->shape = NULL;
      }
    }

    cpBB box = cpBBNew(-size.x + pos.x, -size.y + pos.y, size.x + pos.x, size.y + pos.y);
    cpVect verts[4] = {
        cpv(box.r, box.b),
        cpv(box.r, box.t),
        cpv(box.l, box.t),
        cpv(box.l, box.b),
    };

    e->shape_size = size;
    e->shape_parent_entity = get_id(gs, parent);
    e->shape = (cpShape *)cpPolyShapeInitRaw(cpPolyShapeAlloc(), parent->body, 4, verts, 0.0); // this cast is done in chipmunk, not sure why it works
    cpShapeSetUserData(e->shape, (void *)e);
    PROFILE_SCOPE("Setting mass")
    {
      cpShapeSetMass(e->shape, mass);
    }
    PROFILE_SCOPE("Adding shape")
    {
      cpSpaceAddShape(gs->space, e->shape);
    }
    cpShapeSetFilter(e->shape, FILTER_DEFAULT);
  }
}
void create_circle_shape(GameState *gs, Entity *e, double radius)
{
  e->shape = cpCircleShapeNew(e->body, ORB_RADIUS, cpv(0, 0));
  e->shape_parent_entity = get_id(gs, e);
  e->shape_radius = radius;
  cpShapeSetMass(e->shape, ORB_MASS);
  cpShapeSetUserData(e->shape, (void *)e);
  cpSpaceAddShape(gs->space, e->shape);
  cpShapeSetFilter(e->shape, FILTER_DEFAULT);
}

void create_player(Player *player)
{
  // default box unlocks, required for survival and growth
#ifdef UNLOCK_ALL
  for (enum BoxType t = BoxInvalid + 1; t < BoxLast; t++)
    unlock_box(player, t);
#else
  unlock_box(player, BoxHullpiece);
  unlock_box(player, BoxThruster);
  unlock_box(player, BoxBattery);
  unlock_box(player, BoxCockpit);
  unlock_box(player, BoxGyroscope);
  unlock_box(player, BoxMedbay);
  unlock_box(player, BoxSolarPanel);
  unlock_box(player, BoxScanner);
#endif
}

void create_orb(GameState *gs, Entity *e)
{
  create_body(gs, e);
  create_circle_shape(gs, e, ORB_RADIUS);
  e->is_circle_shape = true;
  e->is_orb = true;
}

void create_missile(GameState *gs, Entity *e)
{
  create_body(gs, e);
  create_rectangle_shape(gs, e, e, (cpVect){0}, cpvmult(MISSILE_COLLIDER_SIZE, 0.5), PLAYER_MASS);
  e->is_missile = true;
}

void create_player_entity(GameState *gs, Entity *e)
{
  e->is_player = true;
  e->always_visible = true;
  e->no_save_to_disk = true;
  create_body(gs, e);
  create_rectangle_shape(gs, e, e, (cpVect){0}, cpvmult(PLAYER_SIZE, 0.5), PLAYER_MASS);
  cpShapeSetFilter(e->shape, PLAYER_FILTER);
}

void box_add_to_boxes(GameState *gs, Entity *grid, Entity *box_to_add)
{
  box_to_add->next_box = get_id(gs, get_entity(gs, grid->boxes));
  box_to_add->prev_box = get_id(gs, grid);
  if (get_entity(gs, box_to_add->next_box) != NULL)
  {
    get_entity(gs, box_to_add->next_box)->prev_box = get_id(gs, box_to_add);
  }
  grid->boxes = get_id(gs, box_to_add);
}

// box must be passed as a parameter as the box added to chipmunk uses this pointer in its
// user data. pos is in local coordinates. Adds the box to the grid's chain of boxes
// Must pass in a type so it knows what filter to give the collision shape
void create_box(GameState *gs, Entity *new_box, Entity *grid, cpVect pos, enum BoxType type)
{
  new_box->is_box = true;
  flight_assert(gs->space != NULL);
  flight_assert(grid->is_grid);

  double halfbox = BOX_SIZE / 2.0;

  create_rectangle_shape(gs, new_box, grid, pos, (cpVect){halfbox, halfbox}, 1.0);

  new_box->box_type = type;
  switch (type)
  {
  case BoxMerge:
    cpShapeSetFilter(new_box->shape, FILTER_MERGE_BOX);
    break;
  default:
    cpShapeSetFilter(new_box->shape, FILTER_BOXES);
    break;
  }

  box_add_to_boxes(gs, grid, new_box);
}

int platonic_detection_compare(const void *a, const void *b)
{
  PlatonicDetection *a_detection = (PlatonicDetection *)a;
  PlatonicDetection *b_detection = (PlatonicDetection *)b;
  double a_intensity = a_detection->intensity;
  double b_intensity = b_detection->intensity;
  if (a_detection->intensity == 0.0)
    a_intensity = INFINITY;
  if (b_detection->intensity == 0.0)
    b_intensity = INFINITY;
  double result = (a_intensity - b_intensity);
  if (result < 0.0)
  {
    return -1;
  }
  else if (result > 0.0)
  {
    return 1;
  }
  else
  {
    return 0;
  }
}

cpVect box_compass_vector(Entity *box)
{

  flight_assert(box->is_box);
  cpVect to_return = (cpVect){.x = 1.0, .y = 0.0};
  to_return = cpvspin(to_return, rotangle(box->compass_rotation));

  return to_return;
}
#include <time.h>
void fill_time_string(char *to_fill, size_t max_length)
{
#ifdef _WIN32
  time_t rawtime;
  struct tm timeinfo = {0};

  time(&rawtime);
  localtime_s(&timeinfo, &rawtime);

  asctime_s(to_fill, max_length, &timeinfo);
#else
  time_t rawtime;
  struct tm *timeinfo;

  time(&rawtime);
  timeinfo = localtime(&rawtime);

  char *output = asctime(timeinfo);
  size_t length = strlen(output);
  strncpy(to_fill, output, length);
#endif
  size_t filled_length = strlen(to_fill);
  // to_fill[filled_length - 1] = '\0'; // remove the newline
  to_fill[filled_length - 2] = '\0'; // remove the newline
  to_fill[filled_length - 3] = '\0'; // remove the newline
  // to_fill[filled_length - 4] = '\0'; // remove the newline
}

Entity *grid_box_at_local_pos(GameState *gs, Entity *grid, cpVect wanted_local_pos)
{
  Entity *box_in_direction = NULL;
  BOXES_ITER(gs, cur, grid)
  {
    if (cpvnear(entity_shape_pos(cur), wanted_local_pos, 0.01))
    {
      box_in_direction = cur;
      break;
    }
  }
  if (box_in_direction != NULL)
    flight_assert(box_in_direction->is_box);
  if (box_in_direction != NULL)
    flight_assert(box_grid(box_in_direction) == grid);
  return box_in_direction;
}

bool merge_box_is_merged(GameState *gs, Entity *merge_box)
{
  flight_assert(merge_box->is_box);
  flight_assert(merge_box->box_type == BoxMerge);
  cpVect facing = box_compass_vector(merge_box);
  Entity *potentially_merged_with = grid_box_at_local_pos(gs, box_grid(merge_box), cpvadd(entity_shape_pos(merge_box), cpvmult(facing, BOX_SIZE)));
  if (potentially_merged_with != NULL && cpvnear(box_compass_vector(potentially_merged_with), cpvmult(facing, -1.0), 0.01))
  {
    return true;
  }
  else
  {
    return false;
  }
}

bool could_learn_from_scanner(Player *for_player, Entity *box)
{
  flight_assert(box->is_box);
  flight_assert(box->box_type == BoxScanner);
  return (for_player->box_unlocks | box->blueprints_learned) != for_player->box_unlocks;
}

bool box_enterable(Entity *box)
{
  return box->box_type == BoxMedbay || box->box_type == BoxCockpit;
}

bool box_interactible(GameState *gs, Player *for_player, Entity *box)
{
  (void)for_player; // make sure to handle case where for_player is NULL if you end up using this variable
  flight_assert(box->is_box);

  if (box->box_type == BoxMerge)
  {
    return merge_box_is_merged(gs, box);
  }
  else if (box->box_type == BoxLandingGear)
  {
    return box->sees_possible_landing || box->landed_constraint != NULL;
  }
  else
  {
    return false;
  }
}

// removes boxes from grid, then ensures that the rule that grids must not have
// holes in them is applied.
static void grid_correct_for_holes(GameState *gs, struct Entity *grid)
{
  int num_boxes = grid_num_boxes(gs, grid);
  if (num_boxes == 0)
  {
    entity_memory_free(gs, grid);
    return;
  }
  if (num_boxes == 1)
    return;

    // could be a gap between boxes in the grid, separate into multiple grids

    // goal: create list of "real grids" from this grid that have boxes which are
    // ONLY connected horizontally and vertically.

#define MAX_SEPARATE_GRIDS 8
  EntityID separate_grids[MAX_SEPARATE_GRIDS] = {0};
  int cur_separate_grid_index = 0;
  int processed_boxes = 0;

  // process all boxes into separate, but correctly connected, grids
  while (processed_boxes < num_boxes)
  {
    // grab an unprocessed box, one not in separate_grids, to start the flood fill
    Entity *unprocessed = get_entity(gs, grid->boxes);
    flight_assert(unprocessed != NULL);
    flight_assert(unprocessed->is_box);
    box_remove_from_boxes(gs, unprocessed); // no longer in the boxes list of the grid

    uint32_t biggest_box_index = 0;

    // flood fill from this unprocessed box, adding each result to cur_separate_grid_index,
    // removing each block from the grid
    // https://en.wikipedia.org/wiki/Flood_fill
    {
      // queue stuff @Robust use factored datastructure
      EntityID Q = get_id(gs, unprocessed);
      Entity *N = NULL;
      while (true)
      {
        flight_assert(!was_entity_deleted(gs, Q));
        N = get_entity(gs, Q);
        if (N == NULL) // must mean that the queue is empty
          break;
        Q = N->next_box;
        if (true) // if node "inside", this is always true
        {
          N->next_box = separate_grids[cur_separate_grid_index];
          separate_grids[cur_separate_grid_index] = get_id(gs, N);
          processed_boxes++;

          if (get_id(gs, N).index > biggest_box_index)
          {
            biggest_box_index = get_id(gs, N).index;
          }

          cpVect cur_local_pos = entity_shape_pos(N);
          const cpVect dirs[] = {
              (cpVect){
                  .x = -1.0, .y = 0.0},
              (cpVect){
                  .x = 1.0, .y = 0.0},
              (cpVect){
                  .x = 0.0, .y = 1.0},
              (cpVect){
                  .x = 0.0, .y = -1.0},
          };
          int num_dirs = sizeof(dirs) / sizeof(*dirs);

          for (int ii = 0; ii < num_dirs; ii++)
          {
            cpVect dir = dirs[ii];
            EntityID box_in_direction = (EntityID){0};
            // @Robust @Speed faster method, not O(N^2), of getting the box
            // in the direction currently needed
            cpVect compass_vect = box_compass_vector(N);
            if (N->box_type == BoxMerge && N->wants_disconnect && cpvnear(compass_vect, dir, 0.01) && merge_box_is_merged(gs, N))
            {
            }
            else
            {
              cpVect wanted_local_pos = cpvadd(cur_local_pos, cpvmult(dir, BOX_SIZE));
              box_in_direction = get_id(gs, grid_box_at_local_pos(gs, grid, wanted_local_pos));
            }

            Entity *newbox = get_entity(gs, box_in_direction);

            if (newbox != NULL && newbox->box_type == BoxMerge && newbox->wants_disconnect && cpvnear(cpvmult(box_compass_vector(newbox), -1.0), dir, 0.01))
            {
              newbox = NULL;
            }

            if (newbox != NULL)
            {
              box_remove_from_boxes(gs, newbox);
              newbox->next_box = Q;
              Q = box_in_direction;
            }
          }
        }
      }
    }

    cur_separate_grid_index++;
    flight_assert(cur_separate_grid_index < MAX_SEPARATE_GRIDS);
  }

  // create new grids for all lists of boxes except for the biggest one.
  // delete the boxes out of the current grid as I pull boxes into separate ones
  // which are no longer connected
  for (int sepgrid_i = 0; sepgrid_i < MAX_SEPARATE_GRIDS; sepgrid_i++)
  {
    EntityID cur_separate_grid = separate_grids[sepgrid_i];
    if (get_entity(gs, cur_separate_grid) == NULL)
      continue; // this separate grid is empty

    Entity *new_grid;
    new_grid = new_entity(gs);
    grid_create(gs, new_grid);
    cpBodySetPosition(new_grid->body, cpBodyGetPosition(grid->body));
    cpBodySetAngle(new_grid->body, cpBodyGetAngle(grid->body));

    Entity *cur = get_entity(gs, cur_separate_grid);
    while (cur != NULL)
    {
      Entity *next = get_entity(gs, cur->next_box);
      cpVect new_shape_position = entity_shape_pos(cur);

      // leaks the allocated shape for the box so center of mass calcs from the original grid are correct. Shapes are freed when grid is destroyed after construction of new replacement grids
      // important that a new entity isn't created for the shapes so entity references to those shapes are still valid
      cpShapeSetUserData(cur->shape, NULL);
      cur->shape = NULL;

      create_box(gs, cur, new_grid, new_shape_position, cur->box_type); // destroys next/prev fields on cur
      cur = next;
    }

    cpBodySetVelocity(new_grid->body, cpBodyGetVelocityAtWorldPoint(grid->body, (grid_com(new_grid))));
    cpBodySetAngularVelocity(new_grid->body, cpBodyGetAngularVelocity(grid->body));
  }

  // destroys all the box shapes and the entities attached to those shapes
  entity_memory_free(gs, grid);
}

static void on_damage(cpArbiter *arb, cpSpace *space, cpDataPointer userData)
{
  cpShape *a, *b;
  cpArbiterGetShapes(arb, &a, &b);

  Entity *entity_a, *entity_b;
  entity_a = cp_shape_entity(a);
  entity_b = cp_shape_entity(b);

  Entity *potential_missiles[] = {entity_a, entity_b};
  for (Entity **missile_ptr = potential_missiles; missile_ptr - potential_missiles < ARRLEN(potential_missiles); missile_ptr++)
  {
    Entity *missile = entity_a;
    cpVect (*getPointFunc)(const cpArbiter *arb, int i) = NULL;
    if (missile == entity_a)
      getPointFunc = cpArbiterGetPointA;
    if (missile == entity_b)
      getPointFunc = cpArbiterGetPointB;

    if (missile->is_missile)
    {
      int count = cpArbiterGetCount(arb);
      for (int i = 0; i < count; i++)
      {
        cpVect collision_point = getPointFunc(arb, i);
        cpVect local_collision_point = (cpBodyWorldToLocal(missile->body, collision_point));
        if (local_collision_point.x > MISSILE_COLLIDER_SIZE.x * 0.2)
        {
          missile->damage += MISSILE_DAMAGE_THRESHOLD * 2.0;
        }
      }
    }
  }

  double damage = cpvlength((cpArbiterTotalImpulse(arb))) * COLLISION_DAMAGE_SCALING;

  if (entity_a->is_box && entity_a->box_type == BoxExplosive)
    entity_a->damage += 2.0 * EXPLOSION_DAMAGE_THRESHOLD;
  if (entity_b->is_box && entity_b->box_type == BoxExplosive)
    entity_b->damage += 2.0 * EXPLOSION_DAMAGE_THRESHOLD;

  if (damage > 0.05)
  {
    entity_a->damage += damage;
    entity_b->damage += damage;
  }
}

// must be called with zero initialized game state, because copies the server side computing!
void initialize(GameState *gs, void *entity_arena, size_t entity_arena_size)
{
  PROFILE_SCOPE("Initialize")
  {
    bool is_server_side = gs->server_side_computing;
    *gs = (GameState){0};
    gs->entities = (Entity *)entity_arena;
    gs->max_entities = (unsigned int)(entity_arena_size / sizeof(Entity));
    gs->space = cpSpaceNew();
    cpSpaceSetUserData(gs->space, (cpDataPointer)gs); // needed in the handler
    cpCollisionHandler *handler = cpSpaceAddCollisionHandler(gs->space, 0, 0);
    handler->postSolveFunc = on_damage;
    gs->server_side_computing = is_server_side;
  }
}
void destroy(GameState *gs)
{
  PROFILE_SCOPE("Destroy")
  {
    // can't zero out gs data because the entity memory arena is reused
    // on deserialization
    for (size_t i = 0; i < gs->cur_next_entity; i++)
    {
      if (gs->entities[i].exists)
      {
        entity_free_allocated(&gs->entities[i]);
        gs->entities[i] = (Entity){0}; // IMPORTANT expects zeroed for initialize
      }
    }
    cpSpaceFree(gs->space);
    gs->space = NULL;
    gs->cur_next_entity = 0;
  }
}
// center of mass, not the literal position
cpVect grid_com(Entity *grid)
{
  return (cpBodyLocalToWorld(grid->body, cpBodyGetCenterOfGravity(grid->body)));
}

cpVect grid_vel(Entity *grid)
{
  return (cpBodyGetVelocity(grid->body));
}
cpVect grid_world_to_local(Entity *grid, cpVect world)
{
  return (cpBodyWorldToLocal(grid->body, (world)));
}
cpVect grid_local_to_world(Entity *grid, cpVect local)
{
  flight_assert(grid->is_grid);
  return (cpBodyLocalToWorld(grid->body, (local)));
}
// returned snapped position is in world coordinates
cpVect grid_snapped_box_pos(Entity *grid, cpVect world)
{
  cpVect local = grid_world_to_local(grid, world);
  local.x /= BOX_SIZE;
  local.y /= BOX_SIZE;
  local.x = round(local.x);
  local.y = round(local.y);
  local.x *= BOX_SIZE;
  local.y *= BOX_SIZE;

  return (cpBodyLocalToWorld(grid->body, (local)));
}

// for boxes does not include box's compass rotation
double entity_rotation(Entity *e)
{
  flight_assert(e->body != NULL || e->shape != NULL);
  if (e->body != NULL)
    return (float)cpBodyGetAngle(e->body);
  else
    return (float)cpBodyGetAngle(cpShapeGetBody(e->shape));
}

double entity_angular_velocity(Entity *grid)
{
  return (float)cpBodyGetAngularVelocity(grid->body);
}
Entity *box_grid(Entity *box)
{
  if (box == NULL)
    return NULL;
  flight_assert(box->is_box);
  return (Entity *)cpBodyGetUserData(cpShapeGetBody(box->shape));
}
// in local space
cpVect entity_shape_pos(Entity *e)
{
  flight_assert(e != NULL);
  flight_assert(e->shape != NULL);
  return (cpShapeGetCenterOfGravity(e->shape));
}
double entity_shape_mass(Entity *box)
{
  flight_assert(box->shape != NULL);
  return (float)cpShapeGetMass(box->shape);
}
double box_rotation(Entity *box)
{
  return (float)cpBodyGetAngle(cpShapeGetBody(box->shape));
}

cpVect entity_pos(Entity *e)
{
  if (e->is_box)
  {
    return cpvadd(entity_pos(box_grid(e)), cpvspin(entity_shape_pos(e), entity_rotation(box_grid(e))));
  }
  else if (e->is_explosion)
  {
    return e->explosion_pos;
  }
  else if (e->is_sun)
  {
    return e->sun_pos;
  }
  else
  {
    flight_assert(e->body != NULL);
    return (cpBodyGetPosition(e->body));
  }
}

struct BodyData
{
  cpVect pos;
  cpVect vel;
  double rotation;
  double angular_velocity;
};

void populate(cpBody *body, struct BodyData *data)
{
  data->pos = (cpBodyGetPosition(body));
  data->vel = (cpBodyGetVelocity(body));
  data->rotation = (float)cpBodyGetAngle(body);
  data->angular_velocity = (float)cpBodyGetAngularVelocity(body);
}

void update_from(cpBody *body, struct BodyData *data)
{
  cpBodySetPosition(body, (data->pos));
  cpBodySetVelocity(body, (data->vel));
  cpBodySetAngle(body, data->rotation);
  cpBodySetAngularVelocity(body, data->angular_velocity);
}

const static SerMaybeFailure ser_ok = {0};
#define SER_ASSERT(cond)                                                                            \
  if (!(cond))                                                                                      \
  {                                                                                                 \
    __flight_assert(false, __FILE__, __LINE__, #cond);                                              \
    if (ser->save_or_load_from_disk)                                                                \
    {                                                                                               \
      Log("While saving/loading, serialization assertion failed %s on line %d\n", #cond, __LINE__); \
    }                                                                                               \
    else                                                                                            \
    {                                                                                               \
      return (SerMaybeFailure){.failed = true, .line = __LINE__, .expression = #cond};              \
    }                                                                                               \
  }
#define SER_MAYBE_RETURN(maybe_failure)     \
  {                                         \
    SerMaybeFailure result = maybe_failure; \
    if (result.failed)                      \
      return result;                        \
  }
SerMaybeFailure ser_data(SerState *ser, char *data, size_t data_len, const char *name, const char *file, int line)
{
  char var_name[512] = {0};
  size_t var_name_len = 0;
  if (ser->write_varnames)
  {
    snprintf(var_name, 512, "%d%s", line, name); // can't have separator before the name, when comparing names skips past the digit
    var_name_len = strlen(var_name);
  }
  if (ser->serializing)
  {
    if (ser->write_varnames)
    {
      // the name
      memcpy(ser->bytes + ser->cursor, var_name, var_name_len);
      ser->cursor += var_name_len;
      SER_ASSERT(ser->cursor < ser->max_size);

      // the size, compressed to a short
      SER_ASSERT(data_len < 65535); // uh oh stinky!
      uint16_t size_to_write = (uint16_t)data_len;
      memcpy(ser->bytes + ser->cursor, &size_to_write, sizeof(size_to_write));
      ser->cursor += sizeof(size_to_write);
      SER_ASSERT(ser->cursor < ser->max_size);
    }
    size_t new_cursor = ser->cursor + data_len;
    SER_ASSERT(new_cursor < ser->max_size);
    memcpy(ser->bytes + ser->cursor, data, data_len);
    ser->cursor = new_cursor;

    /*
    for (int b = 0; b < data_len; b++)
    {
      ser->bytes[ser->cursor] = data[b];
      ser->cursor += 1;
      SER_ASSERT(ser->cursor < ser->max_size);
    }
    */
  }
  else
  {
    if (ser->write_varnames)
    {
      // deserialize and check the var name

      // skip past the digits
      size_t num_digits = 0;
      while (ser->bytes[ser->cursor] >= '0' && ser->bytes[ser->cursor] <= '9')
      {
        ser->cursor += 1;
        SER_ASSERT(ser->cursor <= ser->max_size);
        num_digits += 1;
        if (num_digits >= 10)
        {
          return (SerMaybeFailure){
              .expression = "Way too many digits as a line number before a field name",
              .failed = true,
              .line = __LINE__,
          };
        }
      }
      // cursor is now on a non digit, the start of the name
      char read_name[512] = {0};
      size_t just_field_name_length = strlen(name);
      for (size_t i = 0; i < just_field_name_length; i++)
      {
        read_name[i] = ser->bytes[ser->cursor];
        ser->cursor += 1;
        SER_ASSERT(ser->cursor <= ser->max_size);
      }

      // now compare!
      SER_ASSERT(strcmp(read_name, name) == 0);

      // deserialize and check the size too!
      SER_ASSERT(data_len < 65535); // uh oh stinky!
      uint16_t expected_size = (uint16_t)data_len;
      uint16_t got_size = 0;
      for (int b = 0; b < sizeof(got_size); b++)
      {
        ((char *)&got_size)[b] = ser->bytes[ser->cursor];
        ser->cursor += 1;
        SER_ASSERT(ser->cursor <= ser->max_size);
      }
      SER_ASSERT(got_size == expected_size);
    }
    size_t new_cursor = ser->cursor + data_len;
    SER_ASSERT(new_cursor <= ser->max_size);
    memcpy(data, ser->bytes + ser->cursor, data_len);
    ser->cursor = new_cursor;
  }
  return ser_ok;
}
SerMaybeFailure ser_var(SerState *ser, char *var_pointer, size_t var_size, const char *name, const char *file, int line)
{
  return ser_data(ser, var_pointer, var_size, name, file, line);
}
#define SER_DATA(data_pointer, data_length) SER_MAYBE_RETURN(ser_data(ser, data_pointer, data_length, #data_pointer, __FILE__, __LINE__))
#define SER_VAR_NAME(var_pointer, name) SER_MAYBE_RETURN(ser_var(ser, (char *)var_pointer, sizeof(*var_pointer), name, __FILE__, __LINE__))
#define SER_VAR(var_pointer) SER_VAR_NAME(var_pointer, #var_pointer)

enum GameVersion
{
  VInitial,
  VNoGold,
  VSafeSun,
  VMax, // this minus one will be the version used
};

SerMaybeFailure ser_V2(SerState *ser, cpVect *var)
{
  SER_VAR(&var->x);
  SER_VAR(&var->y);
  SER_ASSERT(!isnan(var->x));
  SER_ASSERT(!isnan(var->y));
  return ser_ok;
}

// for when you only need 32 bit float precision in a vector2,
// but it's a double
SerMaybeFailure ser_fV2(SerState *ser, cpVect *var)
{
  float x;
  float y;
  if (ser->serializing)
  {
    x = (float)var->x;
    y = (float)var->y;
  }
  SER_VAR(&x);
  SER_VAR(&y);
  SER_ASSERT(!isnan(x));
  SER_ASSERT(!isnan(y));

  var->x = x;
  var->y = y;
  return ser_ok;
}

SerMaybeFailure ser_f(SerState *ser, double *d)
{

  float f;
  if (ser->serializing)
    f = (float)*d;
  SER_VAR(&f);
  SER_ASSERT(!isnan(f));
  *d = f;
  return ser_ok;

  // if you're ever sketched out by floating point precision you can use this to test...
  /*  double  f;
  if (ser->serializing)
    f = (double)*d;
  SER_VAR(&f);
  SER_ASSERT(!isnan(f));
  *d = f;
  return ser_ok;*/
}

SerMaybeFailure ser_bodydata(SerState *ser, struct BodyData *data)
{
  SER_MAYBE_RETURN(ser_V2(ser, &data->pos));
  SER_MAYBE_RETURN(ser_V2(ser, &data->vel));
  SER_VAR(&data->rotation);
  SER_VAR(&data->angular_velocity);
  SER_ASSERT(!isnan(data->rotation));
  SER_ASSERT(!isnan(data->angular_velocity));
  return ser_ok;
}

SerMaybeFailure ser_entityid(SerState *ser, EntityID *id)
{
  SER_VAR(&id->generation);
  SER_VAR(&id->index);
  if (id->generation > 0)
    SER_ASSERT(id->index < ser->max_entity_index);
  return ser_ok;
}

SerMaybeFailure ser_inputframe(SerState *ser, InputFrame *i)
{
  SER_VAR(&i->tick);
  SER_MAYBE_RETURN(ser_fV2(ser, &i->movement));
  SER_VAR(&i->rotation);
  SER_VAR(&i->take_over_squad);
  SER_ASSERT(i->take_over_squad >= 0 || i->take_over_squad == -1);
  SER_ASSERT(i->take_over_squad < SquadLast);
  SER_VAR(&i->accept_cur_squad_invite);
  SER_VAR(&i->reject_cur_squad_invite);
  SER_MAYBE_RETURN(ser_entityid(ser, &i->invite_this_player));

  SER_VAR(&i->seat_action);
  SER_VAR(&i->interact_action);
  SER_MAYBE_RETURN(ser_fV2(ser, &i->hand_pos));

  SER_VAR(&i->dobuild);
  SER_VAR(&i->build_type);
  SER_ASSERT(i->build_type >= 0);
  SER_ASSERT(i->build_type < BoxLast);
  SER_VAR(&i->build_rotation);

  return ser_ok;
}

SerMaybeFailure ser_no_player(SerState *ser)
{
  bool connected = false;
  SER_VAR_NAME(&connected, "&p->connected");

  return ser_ok;
}

SerMaybeFailure ser_player(SerState *ser, Player *p)
{
  SER_VAR(&p->connected);
  if (p->connected)
  {
    SER_VAR(&p->box_unlocks);

    SER_VAR(&p->squad);
    SER_MAYBE_RETURN(ser_entityid(ser, &p->entity));
    SER_MAYBE_RETURN(ser_entityid(ser, &p->last_used_medbay));
    SER_MAYBE_RETURN(ser_inputframe(ser, &p->input));
  }

  return ser_ok;
}

SerMaybeFailure ser_entity(SerState *ser, GameState *gs, Entity *e)
{
  PROFILE_SCOPE("Ser entity")
  {
    SER_VAR(&e->no_save_to_disk);
    SER_VAR(&e->always_visible);
    SER_VAR(&e->generation);
    SER_MAYBE_RETURN(ser_f(ser, &e->damage));

    bool has_body = ser->serializing && e->body != NULL;
    SER_VAR(&has_body);

    if (has_body)
    {
      struct BodyData body_data;
      if (ser->serializing)
        populate(e->body, &body_data);
      SER_MAYBE_RETURN(ser_bodydata(ser, &body_data));
      if (!ser->serializing)
      {
        create_body(gs, e);
        update_from(e->body, &body_data);
      }
    }

    bool has_shape = ser->serializing && e->shape != NULL;
    SER_VAR(&has_shape);
    if (has_shape)
    {
      SER_VAR(&e->is_circle_shape);
      if (e->is_circle_shape)
      {
        SER_MAYBE_RETURN(ser_f(ser, &e->shape_radius));
      }
      else
      {
        SER_MAYBE_RETURN(ser_fV2(ser, &e->shape_size));
      }

      SER_MAYBE_RETURN(ser_entityid(ser, &e->shape_parent_entity));
      Entity *parent = get_entity(gs, e->shape_parent_entity);
      SER_ASSERT(parent != NULL);

      cpVect shape_pos;
      if (ser->serializing)
        shape_pos = entity_shape_pos(e);
      SER_MAYBE_RETURN(ser_fV2(ser, &shape_pos));

      double shape_mass;
      if (ser->serializing)
        shape_mass = entity_shape_mass(e);
      SER_VAR(&shape_mass);
      SER_ASSERT(!isnan(shape_mass));

      cpShapeFilter filter;
      if (ser->serializing)
      {
        filter = cpShapeGetFilter(e->shape);
      }
      SER_VAR(&filter.categories);
      SER_VAR(&filter.group);
      SER_VAR(&filter.mask);
      if (!ser->serializing)
      {
        if (e->is_circle_shape)
        {
          create_circle_shape(gs, e, e->shape_radius);
        }
        else
        {
          create_rectangle_shape(gs, e, parent, shape_pos, e->shape_size, shape_mass);
        }
        cpShapeSetFilter(e->shape, filter);
      }
    }

    if (!ser->save_or_load_from_disk)
    {
      SER_MAYBE_RETURN(ser_f(ser, &e->time_was_last_cloaked));
    }

    SER_VAR(&e->owning_squad);

    SER_VAR(&e->is_player);
    if (e->is_player)
    {
      SER_ASSERT(e->no_save_to_disk);

      SER_MAYBE_RETURN(ser_entityid(ser, &e->currently_inside_of_box));
      SER_VAR(&e->squad_invited_to);

      if (ser->version < VNoGold)
      {
        double goldness;
        SER_VAR_NAME(&goldness, "&e->goldness");
      }
    }

    SER_VAR(&e->is_explosion);
    if (e->is_explosion)
    {
      SER_MAYBE_RETURN(ser_V2(ser, &e->explosion_pos));
      SER_MAYBE_RETURN(ser_V2(ser, &e->explosion_vel));
      SER_MAYBE_RETURN(ser_f(ser, &e->explosion_progress));
      SER_MAYBE_RETURN(ser_f(ser, &e->explosion_push_strength));
      SER_MAYBE_RETURN(ser_f(ser, &e->explosion_radius));
    }

    SER_VAR(&e->is_sun);
    if (e->is_sun)
    {
      SER_MAYBE_RETURN(ser_V2(ser, &e->sun_vel));
      SER_MAYBE_RETURN(ser_V2(ser, &e->sun_pos));
      SER_MAYBE_RETURN(ser_f(ser, &e->sun_mass));
      SER_MAYBE_RETURN(ser_f(ser, &e->sun_radius));
      if (ser->version >= VSafeSun)
        SER_VAR(&e->sun_is_safe);
    }

    SER_VAR(&e->is_grid);
    if (e->is_grid)
    {
      SER_MAYBE_RETURN(ser_f(ser, &e->total_energy_capacity));
      SER_MAYBE_RETURN(ser_entityid(ser, &e->boxes));
    }

    SER_VAR(&e->is_missile);
    if (e->is_missile)
    {
      SER_MAYBE_RETURN(ser_f(ser, &e->time_burned_for));
    }

    SER_VAR(&e->is_orb);
    if (e->is_orb)
    {
    }

    SER_VAR(&e->is_box);
    if (e->is_box)
    {
      SER_VAR(&e->box_type);
      SER_VAR(&e->is_platonic);

      SER_VAR(&e->owning_squad);

      SER_MAYBE_RETURN(ser_entityid(ser, &e->next_box));
      SER_MAYBE_RETURN(ser_entityid(ser, &e->prev_box));
      SER_VAR(&e->compass_rotation);
      SER_VAR(&e->indestructible);
      switch (e->box_type)
      {
      case BoxMedbay:
      case BoxCockpit:
        if (!ser->save_or_load_from_disk)
          SER_MAYBE_RETURN(ser_entityid(ser, &e->player_who_is_inside_of_me));
        break;
      case BoxThruster:
        SER_MAYBE_RETURN(ser_f(ser, &e->thrust));
        SER_MAYBE_RETURN(ser_f(ser, &e->wanted_thrust));
        break;
      case BoxGyroscope:
        SER_MAYBE_RETURN(ser_f(ser, &e->thrust));
        SER_MAYBE_RETURN(ser_f(ser, &e->wanted_thrust));
        SER_MAYBE_RETURN(ser_f(ser, &e->gyrospin_angle));
        SER_MAYBE_RETURN(ser_f(ser, &e->gyrospin_velocity));
        break;
      case BoxBattery:
        SER_MAYBE_RETURN(ser_f(ser, &e->energy_used));
        break;
      case BoxSolarPanel:
        SER_MAYBE_RETURN(ser_f(ser, &e->sun_amount));
        break;
      case BoxScanner:
        SER_MAYBE_RETURN(ser_entityid(ser, &e->currently_scanning));
        SER_MAYBE_RETURN(ser_f(ser, &e->currently_scanning_progress));
        SER_VAR(&e->blueprints_learned);
        SER_MAYBE_RETURN(ser_f(ser, &e->scanner_head_rotate));
        for (int i = 0; i < SCANNER_MAX_PLATONICS; i++)
        {
          SER_MAYBE_RETURN(ser_V2(ser, &e->detected_platonics[i].direction));
          SER_MAYBE_RETURN(ser_f(ser, &e->detected_platonics[i].intensity));
        }
        for (int i = 0; i < SCANNER_MAX_POINTS; i++)
        {
          SER_VAR(&e->scanner_points[i]);
        }
        break;
      case BoxCloaking:
        SER_MAYBE_RETURN(ser_f(ser, &e->cloaking_power));
        break;
      case BoxMissileLauncher:
        SER_MAYBE_RETURN(ser_f(ser, &e->missile_construction_charge));
        break;
      case BoxLandingGear:
      {
        SER_MAYBE_RETURN(ser_entityid(ser, &e->shape_to_land_on));
        break;
      }
      default:
        break;
      }
    }
  }
  return ser_ok;
}

SerMaybeFailure ser_opus_packets(SerState *ser, Queue *mic_or_speaker_data)
{
  bool no_more_packets = false;
  if (ser->serializing)
  {
    size_t queued = queue_num_elements(mic_or_speaker_data);
    for (size_t i = 0; i < queued; i++)
    {
      SER_VAR(&no_more_packets);
      OpusPacket *cur = (OpusPacket *)queue_pop_element(mic_or_speaker_data);
      bool isnull = cur == NULL;
      SER_VAR(&isnull);
      if (!isnull && cur != NULL) // cur != NULL is to suppress VS warning
      {
        SER_VAR(&cur->length);
        SER_DATA((char *)cur->data, cur->length);
      }
    }
    no_more_packets = true;
    SER_VAR(&no_more_packets);
  }
  else
  {
    while (true)
    {
      SER_VAR(&no_more_packets);
      if (no_more_packets)
        break;
      OpusPacket *cur = (OpusPacket *)queue_push_element(mic_or_speaker_data);
      OpusPacket dummy;
      if (cur == NULL)
        cur = &dummy; // throw away this packet
      bool isnull = false;
      SER_VAR(&isnull);
      if (!isnull)
      {
        SER_VAR(&cur->length);
        SER_ASSERT(cur->length < VOIP_PACKET_MAX_SIZE);
        SER_ASSERT(cur->length >= 0);
        SER_DATA((char *)cur->data, cur->length);
      }
    }
  }
  return ser_ok;
}

SerMaybeFailure ser_server_to_client(SerState *ser, ServerToClient *s)
{
  SER_VAR(&ser->version);
  SER_ASSERT(ser->version >= 0);
  SER_ASSERT(ser->version < VMax);
  SER_VAR(&ser->git_release_tag);

  if (ser->git_release_tag > GIT_RELEASE_TAG)
  {
    char msg[2048] = {0};
    snprintf(msg, 2048, "Current game build %d is old, download the server's build %d! The most recent one in discord!\n", GIT_RELEASE_TAG, ser->git_release_tag);
    quit_with_popup(msg, "Old Game Build");
    SER_ASSERT(ser->git_release_tag <= GIT_RELEASE_TAG);
  }

  if (!ser->save_or_load_from_disk)
    SER_MAYBE_RETURN(ser_opus_packets(ser, s->audio_playback_buffer));

  GameState *gs = s->cur_gs;

  // completely reset and destroy all gamestate data
  if (!ser->serializing)
  {
    PROFILE_SCOPE("Destroy old gamestate")
    {
      // avoid a memset here very expensive. que rico!
      destroy(gs);
      initialize(gs, gs->entities, gs->max_entities * sizeof(*gs->entities));
      gs->cur_next_entity = 0; // updated on deserialization
    }
  }

  int cur_next_entity = 0;
  if (ser->serializing)
    cur_next_entity = gs->cur_next_entity;
  SER_VAR(&cur_next_entity);
  SER_ASSERT(cur_next_entity <= ser->max_entity_index);

  SER_VAR(&s->your_player);

  SER_VAR(&gs->tick);
  SER_VAR(&gs->subframe_time);

  if (!ser->save_or_load_from_disk) // don't save player info to disk, this is filled on connection/disconnection
  {
    for (size_t i = 0; i < MAX_PLAYERS; i++)
    {
      if (get_entity(gs, gs->players[i].entity) != NULL && is_cloaked(gs, get_entity(gs, gs->players[i].entity), ser->for_player))
      {
        SER_MAYBE_RETURN(ser_no_player(ser));
      }
      else
      {
        SER_MAYBE_RETURN(ser_player(ser, &gs->players[i]));
      }
    }
  }

  for (int i = 0; i < MAX_SUNS; i++)
  {
    bool suns_done = get_entity(gs, gs->suns[i]) == NULL;
    SER_VAR(&suns_done);
    if (suns_done)
      break;
    SER_MAYBE_RETURN(ser_entityid(ser, &gs->suns[i]));
  }

  if (ser->serializing)
  {
    PROFILE_SCOPE("Serialize entities")
    {
      bool entities_done = false;
      for (size_t i = 0; i < gs->cur_next_entity; i++)
      {
        Entity *e = &gs->entities[i];
#define DONT_SEND_BECAUSE_CLOAKED(entity) (!ser->save_or_load_from_disk && ser->for_player != NULL && is_cloaked(gs, entity, ser->for_player))
#define SER_ENTITY()       \
  SER_VAR(&entities_done); \
  SER_VAR(&i);             \
  SER_MAYBE_RETURN(ser_entity(ser, gs, e))
        if (e->exists && !(ser->save_or_load_from_disk && e->no_save_to_disk) && !DONT_SEND_BECAUSE_CLOAKED(e))
        {
          if (!e->is_box && !e->is_grid)
          {
            Entity *cur_entity = e;
            bool this_entity_in_range = ser->save_or_load_from_disk;
            this_entity_in_range |= ser->for_player == NULL;
            this_entity_in_range |= (ser->for_player != NULL && cpvdistsq(entity_pos(ser->for_player), entity_pos(cur_entity)) < VISION_RADIUS * VISION_RADIUS); // only in vision radius
            // don't have to check if the entity is cloaked because this is checked above
            if (cur_entity->always_visible)
              this_entity_in_range = true;

            if (this_entity_in_range)
            {
              SER_ENTITY();
            }
          }
          if (e->is_grid)
          {
            bool serialized_grid_yet = false;
            // serialize boxes always after bodies, so that by the time the boxes
            // are loaded in the parent body is loaded in and can be referenced.
            BOXES_ITER(gs, cur_box, e)
            {
              bool this_box_in_range = ser->save_or_load_from_disk;
              this_box_in_range |= ser->for_player == NULL;
              this_box_in_range |= (ser->for_player != NULL && cpvdistsq(entity_pos(ser->for_player), entity_pos(cur_box)) < VISION_RADIUS * VISION_RADIUS); // only in vision radius
              if (DONT_SEND_BECAUSE_CLOAKED(cur_box))
                this_box_in_range = false;
              if (cur_box->always_visible)
                this_box_in_range = true;
              if (this_box_in_range)
              {
                if (!serialized_grid_yet)
                {
                  serialized_grid_yet = true;
                  SER_ENTITY();
                }

                // serialize this box
                EntityID cur_id = get_id(gs, cur_box);
                SER_ASSERT(cur_id.index < gs->max_entities);
                SER_VAR(&entities_done);
                size_t the_index = (size_t)cur_id.index; // super critical. Type of &i is size_t. Checked when write varnames is true though!
                SER_VAR_NAME(&the_index, "&i");
                SER_MAYBE_RETURN(ser_entity(ser, gs, cur_box));
              }
            }
          }
        }
#undef SER_ENTITY
      }
      entities_done = true;
      SER_VAR(&entities_done);
    }
  }
  else
  {
    PROFILE_SCOPE("Deserialize entities")
    {
      Entity *last_grid = NULL;
      while (true)
      {
        bool entities_done = false;
        SER_VAR(&entities_done);
        if (entities_done)
          break;
        size_t next_index;
        SER_VAR_NAME(&next_index, "&i");
        SER_ASSERT(next_index < gs->max_entities);
        SER_ASSERT(next_index >= 0);
        Entity *e = &gs->entities[next_index];
        e->exists = true;
        // unsigned int possible_next_index = (unsigned int)(next_index + 2); // plus two because player entity refers to itself on deserialization
        unsigned int possible_next_index = (unsigned int)(next_index + 1);
        gs->cur_next_entity = gs->cur_next_entity < possible_next_index ? possible_next_index : gs->cur_next_entity;
        SER_MAYBE_RETURN(ser_entity(ser, gs, e));

        if (e->is_box)
        {
          SER_ASSERT(last_grid != NULL);
          SER_ASSERT(get_entity(gs, e->shape_parent_entity) != NULL);
          SER_ASSERT(last_grid == get_entity(gs, e->shape_parent_entity));
          e->prev_box = (EntityID){0};
          e->next_box = (EntityID){0};
          box_add_to_boxes(gs, last_grid, e);
        }

        if (e->is_grid)
        {
          e->boxes = (EntityID){0};
          last_grid = e;
        }
      }

      PROFILE_SCOPE("Add to free list")
      {
        for (size_t i = 0; i < gs->cur_next_entity; i++)
        {
          Entity *e = &gs->entities[i];
          if (!e->exists)
          {
            if (e->generation == 0)
              e->generation = 1; // 0 generation reference is invalid, means null
            e->next_free_entity = gs->free_list;
            gs->free_list = get_id(gs, e);
          }
        }
      }
    }
  }
  return ser_ok;
}

// On serialize:
// 1. put struct's data into bytes, for a specific player or not, and to disk or not
// 2. output the number of bytes it took to put the struct's data into bytes

// On deserialize:
// 1. put bytes into a struct for a specific player, from disk or not
// 2. perform operations on those bytes to initialize the struct

// for_this_player can be null then the entire world will be sent
bool server_to_client_serialize(struct ServerToClient *msg, unsigned char *bytes, size_t *out_len, size_t max_len, Entity *for_this_player, bool to_disk)
{
  flight_assert(msg->cur_gs != NULL);
  flight_assert(msg != NULL);

  SerState ser = (SerState){
      .bytes = bytes,
      .serializing = true,
      .cursor = 0,
      .max_size = max_len,
      .for_player = for_this_player,
      .max_entity_index = msg->cur_gs->cur_next_entity,
      .version = VMax - 1,
      .save_or_load_from_disk = to_disk,
  };

  ser.write_varnames = to_disk;
#ifdef WRITE_VARNAMES
  ser.write_varnames = true;
#endif

  SerMaybeFailure result = ser_server_to_client(&ser, msg);
  *out_len = ser.cursor + 1; // not sure why I need to add one to cursor, ser.cursor should be the length. It seems to work without the +1 but I have no way to ensure that it works completely when removing the +1...
  if (result.failed)
  {
    Log("Failed to serialize on line %d because of %s\n", result.line, result.expression);
    return false;
  }
  else
  {
    return true;
  }
}

bool server_to_client_deserialize(struct ServerToClient *msg, unsigned char *bytes, size_t max_len, bool from_disk)
{
  flight_assert(msg->cur_gs != NULL);
  flight_assert(msg != NULL);

  SerState servar = (SerState){
      .bytes = bytes,
      .serializing = false,
      .cursor = 0,
      .max_size = max_len,
      .max_entity_index = msg->cur_gs->max_entities,
      .save_or_load_from_disk = from_disk,
  };

  if (from_disk)
    servar.write_varnames = true;

#ifdef WRITE_VARNAMES
  servar.write_varnames = true;
#endif

  SerState *ser = &servar;
  SerMaybeFailure result = ser_server_to_client(ser, msg);
  if (result.failed)
  {
    Log("Failed to deserialize server to client on line %d because of %s\n", result.line, result.expression);
    return false;
  }
  else
  {
    return true;
  }
}

// only serializes up to the maximum inputs the server holds
SerMaybeFailure ser_client_to_server(SerState *ser, ClientToServer *msg)
{
  SER_VAR(&ser->version);
  SER_MAYBE_RETURN(ser_opus_packets(ser, msg->mic_data));

  // serialize input packets
  size_t num;
  if (ser->serializing)
  {
    num = queue_num_elements(msg->input_data);
    if (num > INPUT_QUEUE_MAX)
      num = INPUT_QUEUE_MAX;
  }
  SER_VAR(&num);
  SER_ASSERT(num <= INPUT_QUEUE_MAX);
  if (ser->serializing)
  {
    size_t to_skip = queue_num_elements(msg->input_data) - num;
    size_t i = 0;
    QUEUE_ITER(msg->input_data, InputFrame, cur)
    {
      if (i < to_skip)
      {
        i++;
      }
      else
      {
        SER_MAYBE_RETURN(ser_inputframe(ser, cur));
      }
    }
  }
  else
  {
    for (size_t i = 0; i < num; i++)
    {
      InputFrame *new_frame = (InputFrame *)queue_push_element(msg->input_data);
      SER_ASSERT(new_frame != NULL);
      SER_MAYBE_RETURN(ser_inputframe(ser, new_frame));
    }
  }
  return ser_ok;
}
size_t ser_size(SerState *ser)
{
  flight_assert(ser->cursor > 0); // if this fails, haven't serialized anything yet!
  return ser->cursor + 1;
}

SerState init_serializing(GameState *gs, unsigned char *bytes, size_t max_size, Entity *for_player, bool to_disk)
{
  bool write_varnames = to_disk;
#ifdef WRITE_VARNAMES
  write_varnames = true;
#endif
  return (SerState){
      .bytes = bytes,
      .serializing = true,
      .cursor = 0,
      .max_entity_index = gs->cur_next_entity,
      .max_size = max_size,
      .for_player = for_player,
      .version = VMax - 1,
      .save_or_load_from_disk = to_disk,
      .write_varnames = write_varnames,
  };
}

SerState init_deserializing(GameState *gs, unsigned char *bytes, size_t max_size, bool from_disk)
{
  bool has_varnames = from_disk;
#ifdef WRITE_VARNAMES
  has_varnames = true;
#endif
  return (SerState){
      .bytes = bytes,
      .serializing = false,
      .cursor = 0,
      .max_size = max_size,
      .max_entity_index = gs->max_entities,
      .for_player = NULL,
      .save_or_load_from_disk = from_disk,
      .write_varnames = has_varnames,
  };
}

// filter func null means everything is ok, if it's not null and returns false, that means
// exclude it from the selection. This returns the closest box entity!
Entity *closest_box_to_point_in_radius(struct GameState *gs, cpVect point, double radius, bool (*filter_func)(Entity *))
{
  cpShape *closest_to_point_in_radius_result = NULL;
  double closest_to_point_in_radius_result_largest_dist = 0.0;

  circle_query(gs->space, point, radius);
  QUEUE_ITER(&query_result, QueryResult, res)
  {
    cpShape *shape = res->shape;

    Entity *e = cp_shape_entity(shape);
    if (!e->is_box)
      continue;

    if (filter_func != NULL && !filter_func(e))
      continue;
    double dist_sqr = cpvlengthsq((cpvsub(res->pointA, res->pointB)));
    // double dist = -points->points[0].distance;
    if (dist_sqr > closest_to_point_in_radius_result_largest_dist)
    {
      closest_to_point_in_radius_result_largest_dist = dist_sqr;
      closest_to_point_in_radius_result = shape;
    }
  }
  if (closest_to_point_in_radius_result != NULL)
  {
    return cp_shape_entity(closest_to_point_in_radius_result);
  }
  return NULL;
}

static THREADLOCAL BOX_UNLOCKS_TYPE scanner_has_learned = 0;
static bool scanner_filter(Entity *e)
{
  if (!e->is_box)
    return false;
  if (learned_boxes_has_box(scanner_has_learned, e->box_type))
    return false;
  return true;
}

static void do_explosion(GameState *gs, Entity *explosion, double dt)
{
  double cur_explosion_damage = dt * EXPLOSION_DAMAGE_PER_SEC;
  cpVect explosion_origin = explosion->explosion_pos;
  double explosion_push_strength = explosion->explosion_push_strength;
  circle_query(gs->space, explosion_origin, explosion->explosion_radius);
  QUEUE_ITER(&query_result, QueryResult, res)
  {
    cpShape *shape = res->shape;
    cp_shape_entity(shape)->damage += cur_explosion_damage;
    Entity *parent = get_entity(gs, cp_shape_entity(shape)->shape_parent_entity);
    cpVect from_pos = entity_pos(cp_shape_entity(shape));
    cpVect impulse = cpvmult(cpvnormalize(cpvsub(from_pos, explosion_origin)), explosion_push_strength);
    flight_assert(parent->body != NULL);
    cpBodyApplyImpulseAtWorldPoint(parent->body, (impulse), (from_pos));
  }
}

cpVect box_facing_vector(Entity *box)
{
  flight_assert(box->is_box);
  cpVect to_return = (cpVect){.x = 1.0, .y = 0.0};

  to_return = box_compass_vector(box);
  to_return = cpvspin(to_return, box_rotation(box));

  return to_return;
}

enum CompassRotation facing_vector_to_compass(Entity *grid_to_transplant_to, Entity *grid_facing_vector_from, cpVect facing_vector)
{
  flight_assert(grid_to_transplant_to->body != NULL);
  flight_assert(grid_to_transplant_to->is_grid);

  cpVect from_target = cpvadd(entity_pos(grid_to_transplant_to), facing_vector);
  cpVect local_target = grid_world_to_local(grid_to_transplant_to, from_target);
  cpVect local_facing = local_target;

  enum CompassRotation dirs[] = {
      Right,
      Left,
      Up,
      Down};

  int smallest = -1;
  double smallest_dist = INFINITY;
  for (int i = 0; i < ARRLEN(dirs); i++)
  {
    cpVect point = cpvspin((cpVect){.x = 1.0}, rotangle(dirs[i]));
    double dist = cpvdist(point, local_facing);
    if (dist < smallest_dist)
    {
      smallest_dist = dist;
      smallest = i;
    }
  }
  flight_assert(smallest != -1);
  return dirs[smallest];
}

cpVect thruster_force(Entity *box)
{
  return cpvmult(box_facing_vector(box), -box->thrust * THRUSTER_FORCE);
}

uint64_t tick(GameState *gs)
{
  return gs->tick;
}

double elapsed_time(GameState *gs)
{
  return ((double)gs->tick * TIMESTEP) + gs->subframe_time;
}

Entity *grid_to_build_on(GameState *gs, cpVect world_hand_pos)
{
  return box_grid(closest_box_to_point_in_radius(gs, world_hand_pos, BUILD_BOX_SNAP_DIST_TO_SHIP, NULL));
}

cpVect potentially_snap_hand_pos(GameState *gs, cpVect world_hand_pos)
{
  Entity *potential_grid = grid_to_build_on(gs, world_hand_pos);
  if (potential_grid != NULL)
  {
    world_hand_pos = grid_snapped_box_pos(potential_grid, world_hand_pos);
  }
  return world_hand_pos;
}

cpVect get_world_hand_pos(GameState *gs, InputFrame *input, Entity *player)
{
  if (cpvlength(input->hand_pos) > MAX_HAND_REACH)
  {
    // no cheating with long hand!
    input->hand_pos = cpvmult(cpvnormalize(input->hand_pos), MAX_HAND_REACH);
  }
  return potentially_snap_hand_pos(gs, cpvadd(entity_pos(player), input->hand_pos));
}

bool batteries_have_capacity_for(GameState *gs, Entity *grid, double *energy_left_over, double energy_to_use)
{
  double seen_energy = 0.0;
  BOXES_ITER(gs, possible_battery, grid)
  {
    if (possible_battery->box_type == BoxBattery)
    {
      Entity *battery = possible_battery;
      seen_energy += BATTERY_CAPACITY - battery->energy_used;
      if (seen_energy >= energy_to_use + *energy_left_over)
        return true;
    }
  }
  return false;
}

// returns any effectiveness
double batteries_use_energy(GameState *gs, Entity *grid, double *energy_left_over, double energy_to_use)
{
  if (energy_to_use == 0.0)
  {
    return 1.0;
  }
  double energy_wanting_to_use = energy_to_use;
  if (*energy_left_over > 0.0)
  {
    double energy_to_use_from_leftover = fmin(*energy_left_over, energy_to_use);
    *energy_left_over -= energy_to_use_from_leftover;
    energy_to_use -= energy_to_use_from_leftover;
  }
  BOXES_ITER(gs, possible_battery, grid)
  {
    if (possible_battery->box_type == BoxBattery)
    {
      Entity *battery = possible_battery;
      double energy_to_burn_from_this_battery = fmin(BATTERY_CAPACITY - battery->energy_used, energy_to_use);
      battery->energy_used += energy_to_burn_from_this_battery;
      energy_to_use -= energy_to_burn_from_this_battery;
      if (energy_to_use <= 0.0)
        return 1.0;
    }
  }
  double to_return = 1.0 - (energy_to_use / energy_wanting_to_use);
  flight_assert(to_return >= 0.0);
  flight_assert(to_return <= 1.0);
  return to_return;
}

double sun_dist_no_gravity(Entity *sun)
{
  // return (GRAVITY_CONSTANT * (SUN_MASS * mass / (distance * distance))) / mass;
  // 0.01 = (GRAVITY_CONSTANT * (SUN_MASS / (distance_sqr)));
  // 0.01 / GRAVITY_CONSTANT = SUN_MASS / distance_sqr;
  // distance = sqrt( SUN_MASS / (0.01 / GRAVITY_CONSTANT) )
  return sqrt(sun->sun_mass / (GRAVITY_SMALLEST / GRAVITY_CONSTANT));
}

double entity_mass(Entity *m)
{
  if (m->body != NULL)
    return (float)cpBodyGetMass(m->body);
  else if (m->is_box)
    return BOX_MASS;
  else if (m->is_sun)
    return m->sun_mass;
  else
  {
    flight_assert(false);
    return 0.0;
  }
}

cpVect sun_gravity_accel_for_entity(Entity *entity_with_gravity, Entity *sun)
{
#ifdef NO_GRAVITY
  return (cpVect){0};
#else

  if (cpvlength(cpvsub(entity_pos(entity_with_gravity), entity_pos(sun))) > sun_dist_no_gravity(sun))
    return (cpVect){0};
  cpVect rel_vector = cpvsub(entity_pos(entity_with_gravity), entity_pos(sun));
  double mass = entity_mass(entity_with_gravity);
  flight_assert(mass != 0.0);
  double distance_sqr = cpvlengthsq(rel_vector);
  // return (GRAVITY_CONSTANT * (SUN_MASS * mass / (distance * distance))) / mass;
  // the mass divides out

  // on top
  double accel_magnitude = (GRAVITY_CONSTANT * (sun->sun_mass / (distance_sqr)));
  if (distance_sqr <= sun->sun_radius)
  {
    accel_magnitude *= -1.0;
    if (distance_sqr <= sun->sun_radius * 0.25)
      accel_magnitude = 0.0;
  }
  cpVect towards_sun = cpvnormalize(cpvmult(rel_vector, -1.0));
  return cpvmult(towards_sun, accel_magnitude);
#endif // NO_GRAVITY
}

void entity_set_velocity(Entity *e, cpVect vel)
{
  if (e->body != NULL)
    cpBodySetVelocity(e->body, (vel));
  else if (e->is_sun)
    e->sun_vel = vel;
  else
    flight_assert(false);
}

void entity_ensure_in_orbit(GameState *gs, Entity *e)
{
  cpVect total_new_vel = {0};
  SUNS_ITER(gs)
  {
    cpVect gravity_accel = sun_gravity_accel_for_entity(e, i.sun);
    if (cpvlength(gravity_accel) > 0.0)
    {
      double dist = cpvlength(cpvsub(entity_pos(e), entity_pos(i.sun)));
      cpVect orthogonal_to_gravity = cpvnormalize(cpvspin(gravity_accel, PI / 2.0));
      cpVect wanted_vel = cpvmult(orthogonal_to_gravity, sqrt(cpvlength(gravity_accel) * dist));

      total_new_vel = cpvadd(total_new_vel, (wanted_vel));
    }
  }
  entity_set_velocity(e, (total_new_vel));

  // cpVect pos = (cpvsub(entity_pos(e), SUN_POS));
  // cpFloat r = cpvlength(pos);
  // cpFloat v = cpfsqrt(sun_gravity_accel_at_point((pos), e) / r) / r;
  // cpBodySetVelocity(e->body, cpvmult(cpvperp(pos), v));
}

cpVect box_vel(Entity *box)
{
  flight_assert(box->is_box);
  Entity *grid = box_grid(box);
  return (cpBodyGetVelocityAtWorldPoint(grid->body, (entity_pos(box))));
}

void create_bomb_station(GameState *gs, cpVect pos, enum BoxType platonic_type)
{

  enum CompassRotation rot = Right;

#define BOX_AT_TYPE(grid, pos, type)      \
  {                                       \
    Entity *box = new_entity(gs);         \
    create_box(gs, box, grid, pos, type); \
    box->compass_rotation = rot;          \
    box->indestructible = indestructible; \
  }
#define BOX_AT(grid, pos) BOX_AT_TYPE(grid, pos, BoxHullpiece)

  bool indestructible = false;
  Entity *grid = new_entity(gs);
  grid_create(gs, grid);
  entity_set_pos(grid, pos);
  Entity *platonic_box = new_entity(gs);
  create_box(gs, platonic_box, grid, (cpVect){0}, platonic_type);
  platonic_box->is_platonic = true;
  BOX_AT_TYPE(grid, ((cpVect){BOX_SIZE, 0}), BoxExplosive);
  BOX_AT_TYPE(grid, ((cpVect){BOX_SIZE * 2, 0}), BoxHullpiece);
  BOX_AT_TYPE(grid, ((cpVect){BOX_SIZE * 3, 0}), BoxHullpiece);
  BOX_AT_TYPE(grid, ((cpVect){BOX_SIZE * 4, 0}), BoxHullpiece);

  indestructible = true;
  for (double y = -BOX_SIZE * 5.0; y <= BOX_SIZE * 5.0; y += BOX_SIZE)
  {
    BOX_AT_TYPE(grid, ((cpVect){BOX_SIZE * 5.0, y}), BoxHullpiece);
  }
  for (double x = -BOX_SIZE * 5.0; x <= BOX_SIZE * 5.0; x += BOX_SIZE)
  {
    BOX_AT_TYPE(grid, ((cpVect){x, BOX_SIZE * 5.0}), BoxHullpiece);
    BOX_AT_TYPE(grid, ((cpVect){x, -BOX_SIZE * 5.0}), BoxHullpiece);
  }
  indestructible = false;
  BOX_AT_TYPE(grid, ((cpVect){-BOX_SIZE * 6.0, BOX_SIZE * 5.0}), BoxExplosive);
  BOX_AT_TYPE(grid, ((cpVect){-BOX_SIZE * 6.0, BOX_SIZE * 3.0}), BoxExplosive);
  BOX_AT_TYPE(grid, ((cpVect){-BOX_SIZE * 6.0, BOX_SIZE * 1.0}), BoxExplosive);
  BOX_AT_TYPE(grid, ((cpVect){-BOX_SIZE * 6.0, -BOX_SIZE * 2.0}), BoxExplosive);
  BOX_AT_TYPE(grid, ((cpVect){-BOX_SIZE * 6.0, -BOX_SIZE * 3.0}), BoxExplosive);
  BOX_AT_TYPE(grid, ((cpVect){-BOX_SIZE * 6.0, -BOX_SIZE * 5.0}), BoxExplosive);

  entity_ensure_in_orbit(gs, grid);
}

void create_hard_shell_station(GameState *gs, cpVect pos, enum BoxType platonic_type)
{

  enum CompassRotation rot = Right;

  bool indestructible = false;
  Entity *grid = new_entity(gs);
  grid_create(gs, grid);
  entity_set_pos(grid, pos);
  Entity *platonic_box = new_entity(gs);
  create_box(gs, platonic_box, grid, (cpVect){0}, platonic_type);
  platonic_box->is_platonic = true;
  BOX_AT_TYPE(grid, ((cpVect){BOX_SIZE * 2, 0}), BoxHullpiece);
  BOX_AT_TYPE(grid, ((cpVect){BOX_SIZE * 3, 0}), BoxHullpiece);
  BOX_AT_TYPE(grid, ((cpVect){BOX_SIZE * 4, 0}), BoxHullpiece);

  indestructible = true;
  for (double y = -BOX_SIZE * 5.0; y <= BOX_SIZE * 5.0; y += BOX_SIZE)
  {
    BOX_AT_TYPE(grid, ((cpVect){BOX_SIZE * 5.0, y}), BoxHullpiece);
    BOX_AT_TYPE(grid, ((cpVect){-BOX_SIZE * 5.0, y}), BoxHullpiece);
  }
  for (double x = -BOX_SIZE * 5.0; x <= BOX_SIZE * 5.0; x += BOX_SIZE)
  {
    BOX_AT_TYPE(grid, ((cpVect){x, BOX_SIZE * 5.0}), BoxHullpiece);
    BOX_AT_TYPE(grid, ((cpVect){x, -BOX_SIZE * 5.0}), BoxHullpiece);
  }
  entity_ensure_in_orbit(gs, grid);
  indestructible = false;
}
void create_initial_world(GameState *gs)
{
  const double mass_multiplier = 10.0;
  EntityID suns[] = {
      create_sun(gs, new_entity(gs), ((cpVect){500.0, 0.0}), ((cpVect){0.0, 0.0}), 1000000.0 * mass_multiplier, 70.0),
      create_sun(gs, new_entity(gs), ((cpVect){-7000.0, -50.0}), ((cpVect){0.0, 0.0}), 100000.0 * mass_multiplier, 20.0),
  };

  get_entity(gs, suns[0])->sun_is_safe = true;

  for (int i = 0; i < ARRLEN(suns); i++)
  {
    gs->suns[i] = suns[i];
  }
#ifndef DEBUG_WORLD
  Log("Creating release world\n");

#define ORB_AT(pos)               \
  {                               \
    Entity *orb = new_entity(gs); \
    create_orb(gs, orb);          \
    entity_set_pos(orb, pos);     \
  }
  for (int x = -10; x > -1000; x -= 20)
  {
    ORB_AT(cpv(x, 0.0));
  }
#define SUN_POS(index) (entity_pos(get_entity(gs, suns[index])))
  create_bomb_station(gs, cpvadd(SUN_POS(0), cpv(0.0, 300.0)), BoxExplosive);
  create_bomb_station(gs, cpvadd(SUN_POS(0), cpv(-300.0, 0.0)), BoxLandingGear);
  create_bomb_station(gs, cpvadd(SUN_POS(0), cpv(0.0, -300.0)), BoxCloaking);
  create_bomb_station(gs, cpvadd(SUN_POS(0), cpv(300.0, 0.0)), BoxMissileLauncher);
  create_bomb_station(gs, cpvadd(SUN_POS(1), cpv(0.0, 300.0)), BoxMerge);
#undef SUN_POS
#else

#if 1 // basic test
  {
    bool indestructible = false;
    enum CompassRotation rot = Right;
    Entity *grid = new_entity(gs);
    grid_create(gs, grid);
    entity_set_pos(grid, cpv(-1.5, 0.0));
    BOX_AT_TYPE(grid, cpv(0.0, 0.0), BoxHullpiece);
    BOX_AT_TYPE(grid, cpv(BOX_SIZE, 0.0), BoxHullpiece);
    entity_ensure_in_orbit(gs, grid);
  }
#endif

#if 1 // present the stations
  Log("Creating debug world\n");

  // pos, mass, radius
  create_bomb_station(gs, (cpVect){-5.0, 0.0}, BoxExplosive);
  create_bomb_station(gs, (cpVect){0.0, 5.0}, BoxGyroscope);
  create_hard_shell_station(gs, (cpVect){-5.0, 5.0}, BoxCloaking);
#endif

#if 0 // scanner box
  bool indestructible = false;
  enum CompassRotation rot = Right;
  {
    Entity *grid = new_entity(gs);
    grid_create(gs, grid);
    entity_set_pos(grid, cpv(1.5, 0.0));
    BOX_AT_TYPE(grid, cpv(0.0, 0.0), BoxExplosive);
    BOX_AT_TYPE(grid, cpv(-BOX_SIZE, 0.0), BoxScanner);
    BOX_AT_TYPE(grid, cpv(-BOX_SIZE * 2.0, 0.0), BoxSolarPanel);
    BOX_AT_TYPE(grid, cpv(-BOX_SIZE * 3.0, 0.0), BoxSolarPanel);
    entity_ensure_in_orbit(gs, grid);
  }

  {
    Entity *grid = new_entity(gs);
    grid_create(gs, grid);
    entity_set_pos(grid, cpv(0.0, 10.0));
    BOX_AT_TYPE(grid, cpv(0.0, 0.0), BoxHullpiece);
    entity_ensure_in_orbit(gs, grid);
  }

#endif

#if 1 // landing gear box
  bool indestructible = false;
  cpVect from = cpv(4.0 * BOX_SIZE, 0.0);
  enum CompassRotation rot = Right;
  {
    Entity *grid = new_entity(gs);
    grid_create(gs, grid);
    entity_set_pos(grid, from);
    BOX_AT_TYPE(grid, ((cpVect){0.0, 0.0}), BoxLandingGear);
    // BOX_AT(grid, ((cpVect){0.0, -BOX_SIZE}));
    // BOX_AT_TYPE(grid, ((cpVect){BOX_SIZE, 0.0}), BoxMerge);
    entity_ensure_in_orbit(gs, grid);
    cpBodySetVelocity(grid->body, cpv(0.1, 0.0));
  }
  {
    Entity *grid = new_entity(gs);
    grid_create(gs, grid);
    entity_set_pos(grid, cpvadd(from, cpv(2.0 * BOX_SIZE, 0.0)));
    BOX_AT_TYPE(grid, ((cpVect){0.0, 0.0}), BoxHullpiece);
    entity_ensure_in_orbit(gs, grid);
  }
#endif

#if 0  // merge box
  bool indestructible = false;
  double theta = deg2rad(65.0);
  cpVect from = (cpVect){BOX_SIZE * 4.0, -1};
  enum CompassRotation rot = Right;
  {
    Entity *grid = new_entity(gs);
    grid_create(gs, grid);
    entity_set_pos(grid, cpvadd(from, cpvspin((cpVect){.x = -BOX_SIZE * 9.0}, theta)));
    cpBodySetAngle(grid->body, theta + PI);
    rot = Left;
    BOX_AT_TYPE(grid, ((cpVect){0.0, 0.0}), BoxMerge);
    BOX_AT(grid, ((cpVect){0.0, -BOX_SIZE}));
    BOX_AT_TYPE(grid, ((cpVect){BOX_SIZE, 0.0}), BoxMerge);
    entity_ensure_in_orbit(gs, grid);
  }

  {
    Entity *grid = new_entity(gs);
    grid_create(gs, grid);
    entity_set_pos(grid, from);
    cpBodySetAngle(grid->body, theta);
    rot = Left;
    BOX_AT_TYPE(grid, ((cpVect){-BOX_SIZE, 0.0}), BoxMerge);
    rot = Down;
    BOX_AT_TYPE(grid, ((cpVect){0.0, 0.0}), BoxMerge);
    rot = Up;
    BOX_AT_TYPE(grid, ((cpVect){0.0, BOX_SIZE}), BoxMerge);
    cpBodySetVelocity(grid->body, (cpvspin((cpVect){-0.4, 0.0}, theta)));
    entity_ensure_in_orbit(gs, grid);
  }
#endif // grid tests

#endif // debug world
}

// does not actually set seat in variables so can be used on respawn
void exit_seat(GameState *gs, Entity *seat_in, Entity *p)
{
  cpVect pilot_seat_exit_spot = cpvadd(entity_pos(seat_in), cpvmult(box_facing_vector(seat_in), BOX_SIZE));
  cpBodySetPosition(p->body, (pilot_seat_exit_spot));
  // cpBodySetVelocity(p->body, (player_vel(gs, p)));
  cpBodySetVelocity(p->body, cpBodyGetVelocity(box_grid(seat_in)->body));
}

void shape_integrity_check(cpShape *shape, void *data)
{
  flight_assert(cpShapeGetUserData(shape) != NULL);
  flight_assert(cp_shape_entity(shape)->exists);
  flight_assert(cp_shape_entity(shape)->shape == shape);
}

void body_integrity_check(cpBody *body, void *data)
{
  flight_assert(cpBodyGetUserData(body) != NULL);
  flight_assert(cp_body_entity(body)->exists);
  flight_assert(cp_body_entity(body)->body == body);
}
void player_get_in_seat(GameState *gs, Player *player, Entity *seat)
{
  Entity *p = get_entity(gs, player->entity);
  p->currently_inside_of_box = get_id(gs, seat);
  seat->player_who_is_inside_of_me = get_id(gs, p);
  if (seat->box_type == BoxMedbay)
    player->last_used_medbay = p->currently_inside_of_box;
}

void process(struct GameState *gs, double dt)
{
  PROFILE_SCOPE("Gameplay processing")
  {
    flight_assert(gs->space != NULL);

#ifdef CHIPMUNK_INTEGRITY_CHECK
    PROFILE_SCOPE("Chipmunk Integrity Checks")
    {
      cpSpaceEachShape(gs->space, shape_integrity_check, NULL);
      cpSpaceEachBody(gs->space, body_integrity_check, NULL);
    }
#endif

    gs->tick++;

    PROFILE_SCOPE("sun gravity")
    {
      SUNS_ITER(gs)
      {
        Entity *from_sun = i.sun;
        cpVect accel = {0};
        SUNS_ITER(gs)
        {
          Entity *other_sun = i.sun;
          if (other_sun != from_sun)
          {
            accel = cpvadd(accel, sun_gravity_accel_for_entity(from_sun, other_sun));
          }
        }
#ifndef NO_GRAVITY
        if (!from_sun->sun_is_safe)
        {
          from_sun->sun_vel = cpvadd(from_sun->sun_vel, cpvmult(accel, dt));
          from_sun->sun_pos = cpvadd(from_sun->sun_pos, cpvmult(from_sun->sun_vel, dt));

          if (cpvlength(from_sun->sun_pos) >= INSTANT_DEATH_DISTANCE_FROM_CENTER)
          {
            from_sun->sun_vel = cpvmult(from_sun->sun_vel, -0.8);
            from_sun->sun_pos = cpvmult(cpvnormalize(from_sun->sun_pos), INSTANT_DEATH_DISTANCE_FROM_CENTER);
          }
        }
#endif
      }
    }

    PROFILE_SCOPE("input processing")
    {

      PLAYERS_ITER(gs->players, player)
      {
        if (player->input.take_over_squad >= 0)
        {
          if (player->input.take_over_squad == SquadNone)
          {
            player->squad = SquadNone;
          }
          else
          {
            bool squad_taken = false;
            PLAYERS_ITER(gs->players, other_player)
            {
              if (other_player->squad == player->input.take_over_squad)
              {
                squad_taken = true;
                break;
              }
            }
            if (!squad_taken)
              player->squad = player->input.take_over_squad;
          }
          player->input.take_over_squad = -1;
        }

        // squad invites
        Entity *possibly_to_invite = get_entity(gs, player->input.invite_this_player);
        if (player->input.invite_this_player.generation > 0)
          player->input.invite_this_player = (EntityID){0}; // just in case
        if (player->squad != SquadNone && possibly_to_invite != NULL && possibly_to_invite->is_player)
        {
          possibly_to_invite->squad_invited_to = player->squad;
        }
        Entity *p = get_entity(gs, player->entity);
        // player respawning
        if (p == NULL)
        {
          p = new_entity(gs);
          create_player_entity(gs, p);
          player->entity = get_id(gs, p);
          Entity *medbay = get_entity(gs, player->last_used_medbay);
          entity_ensure_in_orbit(gs, p);
          if (medbay != NULL)
          {
            p->damage = 0.95;
            if (get_entity(gs, medbay->player_who_is_inside_of_me) == NULL)
            {
              player_get_in_seat(gs, player, medbay);
            }
            else
            {
              exit_seat(gs, medbay, p);
            }
          }
        }
        flight_assert(p->is_player);
        p->owning_squad = player->squad;

        if (p->squad_invited_to != SquadNone)
        {
          if (player->input.accept_cur_squad_invite)
          {
            player->squad = p->squad_invited_to;
            p->squad_invited_to = SquadNone;
            player->input.accept_cur_squad_invite = false;
          }
          if (player->input.reject_cur_squad_invite)
          {
            p->squad_invited_to = SquadNone;
            player->input.reject_cur_squad_invite = false;
          }
        }

#ifdef INFINITE_RESOURCES
        p->damage = 0.0;
#endif
#if 1

        cpVect world_hand_pos = get_world_hand_pos(gs, &player->input, p);
        if (player->input.interact_action)
        {
          player->input.interact_action = false;
          cpPointQueryInfo query_info = {0};
          cpShape *result = cpSpacePointQueryNearest(gs->space, (world_hand_pos), 0.1, FILTER_ONLY_BOXES, &query_info);
          if (result != NULL)
          {
            Entity *potential_seat = cp_shape_entity(result);
            flight_assert(potential_seat->is_box);

            // IMPORTANT: if you update these, make sure you update box_interactible so
            // the button prompt still works
            if (potential_seat->box_type == BoxMerge) // disconnect!
            {
              potential_seat->wants_disconnect = true;
              grid_correct_for_holes(gs, box_grid(potential_seat));
              flight_assert(potential_seat->exists);
              flight_assert(potential_seat->is_box);
              flight_assert(potential_seat->box_type == BoxMerge);
            }
            if (potential_seat->box_type == BoxLandingGear)
            {
              potential_seat->toggle_landing = true;
            }
          }
        }

        if (player->input.seat_action)
        {
          player->input.seat_action = false; // "handle" the input
          Entity *seat_maybe_in = get_entity(gs, p->currently_inside_of_box);
          if (seat_maybe_in == NULL) // not in any seat
          {
            cpPointQueryInfo query_info = {0};
            cpShape *result = cpSpacePointQueryNearest(gs->space, (world_hand_pos), 0.1, FILTER_ONLY_BOXES, &query_info);
            if (result != NULL)
            {
              Entity *potential_seat = cp_shape_entity(result);
              flight_assert(potential_seat->is_box);
              // IMPORTANT: if you update these, make sure you update box_enterable so
              // the button prompt still works
              if (potential_seat->box_type == BoxCockpit || potential_seat->box_type == BoxMedbay)
              {
                // don't let players get inside of cockpits that somebody else is already inside of
                if (get_entity(gs, potential_seat->player_who_is_inside_of_me) == NULL)
                {
                  player_get_in_seat(gs, player, potential_seat);
                }
              }
            }
            else
            {
              Log("No seat to get into for a player at point %f %f\n", world_hand_pos.x, world_hand_pos.y);
            }
          }
          else
          {
            exit_seat(gs, seat_maybe_in, p);
            seat_maybe_in->player_who_is_inside_of_me = (EntityID){0};
            p->currently_inside_of_box = (EntityID){0};
          }
        }
#endif

        // general player logic
        {
          circle_query(gs->space, entity_pos(p), SCANNER_RADIUS);
          QUEUE_ITER(&query_result, QueryResult, res)
          {
            Entity *maybe_scanner = cp_shape_entity(res->shape);
            if (maybe_scanner->box_type == BoxScanner && could_learn_from_scanner(player, maybe_scanner))
            {
              player->box_unlocks |= maybe_scanner->blueprints_learned;
              // @Cosmetic here
            }
          }
        }

        // process movement
        {
          // no cheating by making movement bigger than length 1
          cpVect movement_this_tick = (cpVect){0};
          double rotation_this_tick = 0.0;
          if (cpvlength(player->input.movement) > 0.0)
          {
            movement_this_tick = cpvmult(cpvnormalize(player->input.movement), clamp(cpvlength(player->input.movement), 0.0, 1.0));
            player->input.movement = (cpVect){0};
          }
          if (fabs(player->input.rotation) > 0.0)
          {
            rotation_this_tick = player->input.rotation;
            if (rotation_this_tick > 1.0)
              rotation_this_tick = 1.0;
            if (rotation_this_tick < -1.0)
              rotation_this_tick = -1.0;
            player->input.rotation = 0.0;
          }
          Entity *seat_inside_of = get_entity(gs, p->currently_inside_of_box);

          // strange rare bug I saw happen, related to explosives, but no idea how to
          // reproduce. @Robust put a breakpoint here, reproduce, and fix it!
          if (seat_inside_of != NULL && !seat_inside_of->is_box)
          {
            Log("Strange thing happened where player was in non box seat!\n");
            seat_inside_of = NULL;
            p->currently_inside_of_box = (EntityID){0};
          }

          if (seat_inside_of == NULL)
          {
            cpShapeSetFilter(p->shape, PLAYER_FILTER);
            cpBodyApplyForceAtWorldPoint(p->body, (cpvmult(movement_this_tick, PLAYER_JETPACK_FORCE)), cpBodyGetPosition(p->body));
            cpBodySetTorque(p->body, rotation_this_tick * PLAYER_JETPACK_TORQUE);
            p->damage += cpvlength(movement_this_tick) * dt * PLAYER_JETPACK_SPICE_PER_SECOND;
            p->damage += fabs(rotation_this_tick) * dt * PLAYER_JETPACK_ROTATION_ENERGY_PER_SECOND;
          }
          else
          {
            flight_assert(seat_inside_of->is_box);
            cpShapeSetFilter(p->shape, CP_SHAPE_FILTER_NONE); // no collisions while in a seat
            cpBodySetPosition(p->body, (entity_pos(seat_inside_of)));
            cpBodySetVelocity(p->body, (box_vel(seat_inside_of)));

            // share cloaking with box
            p->time_was_last_cloaked = seat_inside_of->time_was_last_cloaked;
            p->last_cloaked_by_squad = seat_inside_of->last_cloaked_by_squad;

            // set thruster thrust from movement
            if (seat_inside_of->box_type == BoxCockpit)
            {
              Entity *g = get_entity(gs, seat_inside_of->shape_parent_entity);

              cpVect target_direction = {0};
              if (cpvlength(movement_this_tick) > 0.0)
              {
                target_direction = cpvnormalize(movement_this_tick);
              }
              BOXES_ITER(gs, cur, g)
              {
                if (cur->box_type == BoxThruster)
                {

                  double wanted_thrust = -cpvdot(target_direction, box_facing_vector(cur));
                  wanted_thrust = clamp01(wanted_thrust);
                  cur->wanted_thrust = wanted_thrust;
                }
                if (cur->box_type == BoxGyroscope)
                {
                  cur->wanted_thrust = rotation_this_tick;
                }
              }
            }
          }
        }

#if 1 // building
        if (player->input.dobuild)
        {
          player->input.dobuild = false; // handle the input. if didn't do this, after destruction of hovered box, would try to build on it again the next frame. @Robust handle the input in one place

          cpPointQueryInfo info = {0};
          cpVect world_build = world_hand_pos;

          Entity *target_grid = grid_to_build_on(gs, world_hand_pos);
          cpShape *maybe_box_to_destroy = cpSpacePointQueryNearest(gs->space, (world_build), 0.01, cpShapeFilterNew(CP_NO_GROUP, BOXES, BOXES), &info);
          if (maybe_box_to_destroy != NULL)
          {
            Entity *cur_box = cp_shape_entity(maybe_box_to_destroy);
            if (!cur_box->indestructible && !cur_box->is_platonic)
            {
              p->damage -= DAMAGE_TO_PLAYER_PER_BLOCK * ((BATTERY_CAPACITY - cur_box->energy_used) / BATTERY_CAPACITY);
              cur_box->flag_for_destruction = true;
            }
          }
          else if (box_unlocked(player, player->input.build_type))
          {
            // creating a box
            p->damage += DAMAGE_TO_PLAYER_PER_BLOCK;
            cpVect created_box_position;
            if (p->damage < 1.0) // player can't create a box that kills them by making it
            {
              if (target_grid == NULL)
              {
                Entity *new_grid = new_entity(gs);
                grid_create(gs, new_grid);
                entity_set_pos(new_grid, world_build);
                cpBodySetVelocity(new_grid->body, (player_vel(gs, p)));
                target_grid = new_grid;
                created_box_position = (cpVect){0};
              }
              else
              {
                created_box_position = grid_world_to_local(target_grid, world_build);
              }
              Entity *new_box = new_entity(gs);
              create_box(gs, new_box, target_grid, created_box_position, player->input.build_type);
              new_box->owning_squad = player->squad;
              grid_correct_for_holes(gs, target_grid); // no holey ship for you!
              new_box->compass_rotation = player->input.build_rotation;
              if (new_box->box_type == BoxScanner)
                new_box->blueprints_learned = player->box_unlocks;
              if (new_box->box_type == BoxBattery)
                new_box->energy_used = BATTERY_CAPACITY;
            }
          }
        }
#endif
        if (p->damage >= 1.0)
        {
          p->flag_for_destruction = true;
          player->entity = (EntityID){0};
        }

        p->damage = clamp01(p->damage);
      }
    }

    PROFILE_SCOPE("process entities")
    {
      ENTITIES_ITER(gs, e)
      if (!e->flag_for_destruction)
      {
        if (e->body != NULL && cpvlengthsq((entity_pos(e))) > (INSTANT_DEATH_DISTANCE_FROM_CENTER * INSTANT_DEATH_DISTANCE_FROM_CENTER))
        {
#ifdef INTENSIVE_PROFILING
          PROFILE_SCOPE("instant death")
#endif
          {
            bool platonic_found = false;
            if (e->is_grid)
            {
              BOXES_ITER(gs, cur_box, e)
              {
                if (cur_box->is_platonic)
                {
                  platonic_found = true;
                  break;
                }
              }
            }
            if (platonic_found)
            {
              cpBody *body = e->body;
              cpBodySetVelocity(body, cpvmult(cpBodyGetVelocity(body), -0.5));
              cpVect rel_to_center = cpvsub(cpBodyGetPosition(body), (cpVect){0});
              cpBodySetPosition(body, cpvmult(cpvnormalize(rel_to_center), INSTANT_DEATH_DISTANCE_FROM_CENTER));
            }
            else
            {
              e->flag_for_destruction = true;
            }
          }
        }

        // sun processing for this current entity
#ifndef NO_SUNS
        PROFILE_SCOPE("this entity sun processing")
        {
          SUNS_ITER(gs)
          {
            cpVect pos_rel_sun = (cpvsub(entity_pos(e), (entity_pos(i.sun))));
            cpFloat sqdist = cpvlengthsq(pos_rel_sun);

            if (i.sun->sun_is_safe)
            {
              bool is_entity_dangerous = false;
              is_entity_dangerous |= e->is_missile;
              if (e->is_box)
              {
                is_entity_dangerous |= e->box_type == BoxExplosive;
              }
              if (is_entity_dangerous && sqdist < sun_dist_no_gravity(i.sun) * sun_dist_no_gravity(i.sun))
              {
                e->flag_for_destruction = true;
              }
            }

            if (!e->is_grid) // grids aren't damaged (this edge case sucks!)
            {
#ifdef INTENSIVE_PROFILING
              PROFILE_SCOPE("Grid processing")
#endif
              {
                if (sqdist < (i.sun->sun_radius * i.sun->sun_radius))
                {
                  e->damage += 10.0 * dt;
                }
              }
            }

            if (e->body != NULL)
            {
#ifdef INTENSIVE_PROFILING
              PROFILE_SCOPE("Body processing")
#endif
              {
                cpVect accel = sun_gravity_accel_for_entity(e, i.sun);
                cpVect new_vel = entity_vel(gs, e);
                new_vel = cpvadd(new_vel, cpvmult(accel, dt));
                cpBodySetVelocity(e->body, (new_vel));
              }
            }
          }
        }
#endif

        if (e->is_explosion)
        {
          PROFILE_SCOPE("Explosion")
          {
            e->explosion_progress += dt;
            e->explosion_pos = cpvadd(e->explosion_pos, cpvmult(e->explosion_vel, dt));
            do_explosion(gs, e, dt);
            if (e->explosion_progress >= EXPLOSION_TIME)
            {
              e->flag_for_destruction = true;
            }
          }
        }

        if (e->is_orb)
        {
          PROFILE_SCOPE("Orb")
          {
            circle_query(gs->space, entity_pos(e), ORB_HEAT_MAX_DETECTION_DIST);
            cpVect final_force = cpv(0, 0);
            QUEUE_ITER(&query_result, QueryResult, res)
            {
              Entity *potential_aggravation = cp_shape_entity(res->shape);
              if (potential_aggravation->is_box && potential_aggravation->box_type == BoxThruster && fabs(potential_aggravation->thrust) > 0.1)
              {
                final_force = cpvadd(final_force, cpvmult(cpvsub(entity_pos(potential_aggravation), entity_pos(e)), ORB_HEAT_FORCE_MULTIPLIER));
              }
            }
            if (cpvlength(final_force) > ORB_MAX_FORCE)
            {
              final_force = cpvmult(cpvnormalize(final_force), ORB_MAX_FORCE);
            }
            // add drag
            final_force = cpvadd(final_force, cpvmult(entity_vel(gs, e), -1.0 * lerp(ORB_DRAG_CONSTANT, ORB_FROZEN_DRAG_CONSTANT, e->damage)));
            cpBodyApplyForceAtWorldPoint(e->body, final_force, entity_pos(e));
            e->damage -= dt * ORB_HEAL_RATE;
            e->damage = clamp01(e->damage);
          }
        }

        if (e->is_missile)
        {
          PROFILE_SCOPE("Missile")
          {
            if (is_burning(e))
            {
              e->time_burned_for += dt;
              cpBodyApplyForceAtWorldPoint(e->body, (cpvspin((cpVect){.x = MISSILE_BURN_FORCE, .y = 0.0}, entity_rotation(e))), (entity_pos(e)));
            }
            if (e->damage >= MISSILE_DAMAGE_THRESHOLD && e->time_burned_for >= MISSILE_ARM_TIME)
            {
              Entity *explosion = new_entity(gs);
              explosion->is_explosion = true;
              explosion->explosion_pos = entity_pos(e);
              explosion->explosion_vel = cpBodyGetVelocity(e->body);
              explosion->explosion_push_strength = MISSILE_EXPLOSION_PUSH;
              explosion->explosion_radius = MISSILE_EXPLOSION_RADIUS;
              e->flag_for_destruction = true;
              continue;
            }
          }
        }

        if (e->is_box)
        {
#ifdef INTENSIVE_PROFILING
          PROFILE_SCOPE("Box processing")
#endif
          {
            if (e->box_type == BoxExplosive && e->damage >= EXPLOSION_DAMAGE_THRESHOLD)
            {
              Entity *explosion = new_entity(gs);
              explosion->is_explosion = true;
              explosion->explosion_pos = entity_pos(e);
              explosion->explosion_vel = grid_vel(box_grid(e));
              explosion->explosion_push_strength = BOMB_EXPLOSION_PUSH;
              explosion->explosion_radius = BOMB_EXPLOSION_RADIUS;
              if (!e->is_platonic)
                e->flag_for_destruction = true;
            }
            if (e->is_platonic)
            {
              e->damage = 0.0;
              gs->platonic_positions[(int)e->box_type] = entity_pos(e);
            }
            if (e->box_type == BoxMerge)
            {
#ifdef INTENSIVE_PROFILING
              PROFILE_SCOPE("Merge box")
#endif
              {
                Entity *from_merge = e;
                flight_assert(from_merge != NULL);

                Entity *grid_to_exclude = box_grid(from_merge);
                // Entity *other_merge = closest_box_to_point_in_radius(gs, entity_pos(from_merge), MERGE_MAX_DIST, merge_filter);
                cpVect along = box_facing_vector(from_merge);
                cpVect from = cpvadd(entity_pos(from_merge), cpvmult(along, BOX_SIZE / 2.0 + 0.03));
                cpVect to = cpvadd(from, cpvmult(along, MERGE_MAX_DIST));
                found_merge_shape = NULL;
                cpSpaceSegmentQuery(gs->space, from, to, 0.0, FILTER_ONLY_MERGE_BOX, raycast_query_callback, (void *)grid_to_exclude);
                cpShape *other_merge_shape = found_merge_shape;

                Entity *other_merge = NULL;
                if (other_merge_shape != NULL)
                  other_merge = cp_shape_entity(other_merge_shape);

                if (other_merge == NULL && from_merge->wants_disconnect)
                  from_merge->wants_disconnect = false;

                if (!from_merge->wants_disconnect && other_merge != NULL && !other_merge->wants_disconnect)
                {
#ifdef INTENSIVE_PROFILING
                  PROFILE_SCOPE("Do actual merge")
#endif
                  {
                    flight_assert(box_grid(from_merge) != box_grid(other_merge));

                    Entity *from_grid = box_grid(from_merge);
                    Entity *other_grid = box_grid(other_merge);

                    // the merges are near eachother, but are they facing eachother...
                    bool from_facing_other = cpvdot(box_facing_vector(from_merge), cpvnormalize(cpvsub(entity_pos(other_merge), entity_pos(from_merge)))) > 0.8;
                    bool other_facing_from = cpvdot(box_facing_vector(other_merge), cpvnormalize(cpvsub(entity_pos(from_merge), entity_pos(other_merge)))) > 0.8;

                    // using this stuff to detect if when the other grid's boxes are snapped, they'll be snapped
                    // to be next to the from merge box
                    cpVect actual_new_pos = grid_snapped_box_pos(from_grid, entity_pos(other_merge));
                    cpVect needed_new_pos = cpvadd(entity_pos(from_merge), cpvmult(box_facing_vector(from_merge), BOX_SIZE));
                    if (from_facing_other && other_facing_from && cpvnear(needed_new_pos, actual_new_pos, 0.01))
                    {
                      // do the merge
                      cpVect facing_vector_needed = cpvmult(box_facing_vector(from_merge), -1.0);
                      cpVect current_facing_vector = box_facing_vector(other_merge);
                      double angle_diff = cpvanglediff(current_facing_vector, facing_vector_needed);
                      if (angle_diff == FLT_MIN)
                        angle_diff = 0.0;
                      flight_assert(!isnan(angle_diff));

                      cpBodySetAngle(other_grid->body, cpBodyGetAngle(other_grid->body) + angle_diff);

                      cpVect moved_because_angle_change = cpvsub(needed_new_pos, entity_pos(other_merge));
                      cpBodySetPosition(other_grid->body, (cpvadd(entity_pos(other_grid), moved_because_angle_change)));

                      // cpVect snap_movement_vect = cpvsub(actual_new_pos, entity_pos(other_merge));
                      cpVect snap_movement_vect = (cpVect){0};

                      Entity *cur = get_entity(gs, other_grid->boxes);

                      other_grid->boxes = (EntityID){0};
                      while (cur != NULL)
                      {
                        Entity *next = get_entity(gs, cur->next_box);
                        cpVect world = entity_pos(cur);
                        enum CompassRotation new_rotation = facing_vector_to_compass(from_grid, other_grid, box_facing_vector(cur));
                        cur->compass_rotation = new_rotation;
                        cpVect new_cur_pos = grid_snapped_box_pos(from_grid, cpvadd(snap_movement_vect, world));
                        create_box(gs, cur, from_grid, grid_world_to_local(from_grid, new_cur_pos), cur->box_type); // destroys next/prev fields on cur
                        flight_assert(box_grid(cur) == box_grid(from_merge));
                        cur = next;
                      }
                      other_grid->flag_for_destruction = true;
                    }
                  }
                }
              }
            }
            if (e->damage >= 1.0)
            {
              e->flag_for_destruction = true;
            }
          }
        }

        if (e->is_grid)
        {
          // PROFILE_SCOPE("Grid processing")
          {
            Entity *grid = e;
            float e; // turn all references to e into errors
            (void)e;
            // calculate how much energy solar panels provide
            double energy_to_add = 0.0;
            BOXES_ITER(gs, cur_box, grid)
            {
              if (cur_box->box_type == BoxSolarPanel)
              {
                cur_box->sun_amount = 0.0;
                SUNS_ITER(gs)
                {
                  double new_sun = clamp01(fabs(cpvdot(box_facing_vector(cur_box), cpvnormalize(cpvsub(entity_pos(i.sun), entity_pos(cur_box))))));

                  // less sun the farther away you are!
                  new_sun *= lerp(1.0, 0.0, clamp01(cpvdist(entity_pos(cur_box), entity_pos(i.sun)) / sun_dist_no_gravity(i.sun)));
                  cur_box->sun_amount += new_sun;
                }
                cur_box->sun_amount = clamp01(cur_box->sun_amount);

                energy_to_add += cur_box->sun_amount * SOLAR_ENERGY_PER_SECOND * dt;
              }
            }

            // apply all of the energy to all connected batteries
            BOXES_ITER(gs, cur, grid)
            {
              if (energy_to_add <= 0.0)
                break;
              if (cur->box_type == BoxBattery)
              {
                double energy_sucked_up_by_battery = cur->energy_used < energy_to_add ? cur->energy_used : energy_to_add;
                cur->energy_used -= energy_sucked_up_by_battery;
                energy_to_add -= energy_sucked_up_by_battery;
              }
              flight_assert(energy_to_add >= 0.0);
            }

            // any energy_to_add existing now can also be used to power thrusters/medbay. Kind of like a temporary separate battery
            double non_battery_energy_left_over = energy_to_add;

            // use the energy, stored in the batteries, in various boxes
            BOXES_ITER(gs, cur_box, grid)
            {

              if (cur_box->box_type == BoxThruster)
              {
                cur_box->energy_effectiveness = batteries_use_energy(gs, grid, &non_battery_energy_left_over, cur_box->wanted_thrust * THRUSTER_ENERGY_USED_PER_SECOND * dt);
                cur_box->thrust = cur_box->energy_effectiveness * cur_box->wanted_thrust;
                if (cur_box->thrust > 0.0)
                {
                  cpBodyApplyForceAtWorldPoint(grid->body, (thruster_force(cur_box)), (entity_pos(cur_box)));
                  rect_query(gs->space, (BoxCentered){
                                            .pos = cpvadd(entity_pos(cur_box), cpvmult(box_facing_vector(cur_box), BOX_SIZE)),
                                            .rotation = box_rotation(cur_box),
                                            .size = cpv(BOX_SIZE / 2.0 - 0.03, BOX_SIZE / 2.0 - 0.03),
                                        });
                  QUEUE_ITER(&query_result, QueryResult, res)
                  {
                    flight_assert(cp_shape_entity(res->shape) != NULL);
                    cp_shape_entity(res->shape)->damage += THRUSTER_DAMAGE_PER_SEC * dt;
                  }
                }
              }
              if (cur_box->box_type == BoxGyroscope)
              {
                cur_box->gyrospin_velocity = lerp(cur_box->gyrospin_velocity, cur_box->thrust * 20.0, dt * 5.0);
                cur_box->gyrospin_angle += cur_box->gyrospin_velocity * dt;

                // wrap to keep the number small
                if (cur_box->gyrospin_angle > 2.0 * PI)
                {
                  cur_box->gyrospin_angle -= 2.0 * PI;
                }
                if (cur_box->gyrospin_angle < -2.0 * PI)
                {
                  cur_box->gyrospin_angle += 2.0 * PI;
                }

                if (cur_box->wanted_thrust == 0.0)
                {
                  cur_box->thrust = 0.0;
                }
                double thrust_to_want = cur_box->wanted_thrust;
                if (cur_box->wanted_thrust == 0.0)
                  thrust_to_want = clamp(-cpBodyGetAngularVelocity(grid->body) * GYROSCOPE_PROPORTIONAL_INERTIAL_RESPONSE, -1.0, 1.0);
                cur_box->energy_effectiveness = batteries_use_energy(gs, grid, &non_battery_energy_left_over, fabs(thrust_to_want * GYROSCOPE_ENERGY_USED_PER_SECOND * dt));
                cur_box->thrust = cur_box->energy_effectiveness * thrust_to_want;
                if (fabs(cur_box->thrust) >= 0.0)
                  cpBodySetTorque(grid->body, cpBodyGetTorque(grid->body) + cur_box->thrust * GYROSCOPE_TORQUE);
              }
              if (cur_box->box_type == BoxMedbay)
              {
                Entity *potential_meatbag_to_heal = get_entity(gs, cur_box->player_who_is_inside_of_me);
                if (potential_meatbag_to_heal != NULL)
                {
                  double wanted_energy_to_heal = fmin(potential_meatbag_to_heal->damage, PLAYER_ENERGY_RECHARGE_PER_SECOND * dt);
                  cur_box->energy_effectiveness = batteries_use_energy(gs, grid, &non_battery_energy_left_over, wanted_energy_to_heal);
                  potential_meatbag_to_heal->damage -= wanted_energy_to_heal * cur_box->energy_effectiveness;
                }
              }
              if (cur_box->box_type == BoxCloaking)
              {
                cur_box->energy_effectiveness = batteries_use_energy(gs, grid, &non_battery_energy_left_over, CLOAKING_ENERGY_USE * dt);
                cur_box->cloaking_power = lerp(cur_box->cloaking_power, cur_box->energy_effectiveness, dt * 3.0);
                if (cur_box->energy_effectiveness >= 1.0)
                {
                  rect_query(gs->space, (BoxCentered){
                                            .pos = entity_pos(cur_box),
                                            .rotation = entity_rotation(cur_box),
                                            // subtract a little from the panel size so that boxes just at the boundary of the panel
                                            // aren't (sometimes cloaked)/(sometimes not) from floating point imprecision
                                            .size = cpv(CLOAKING_PANEL_SIZE / 2.0 - 0.03, CLOAKING_PANEL_SIZE / 2.0 - 0.03),
                                        });
                  QUEUE_ITER(&query_result, QueryResult, res)
                  {
                    cpShape *shape = res->shape;
                    Entity *from_cloaking_box = cur_box;
                    Entity *to_cloak = cp_shape_entity(shape);

                    to_cloak->time_was_last_cloaked = elapsed_time(gs);
                    to_cloak->last_cloaked_by_squad = from_cloaking_box->owning_squad;
                  }
                }
              }
              if (cur_box->box_type == BoxMissileLauncher)
              {
                LauncherTarget target = missile_launcher_target(gs, cur_box);

                if (cur_box->missile_construction_charge < 1.0)
                {
                  double want_use_energy = dt * MISSILE_CHARGE_RATE;
                  cur_box->energy_effectiveness = batteries_use_energy(gs, grid, &non_battery_energy_left_over, want_use_energy);

                  cur_box->missile_construction_charge += cur_box->energy_effectiveness * want_use_energy;
                }

                if (target.target_found && cur_box->missile_construction_charge >= 1.0)
                {
                  cur_box->missile_construction_charge = 0.0;
                  Entity *new_missile = new_entity(gs);
                  create_missile(gs, new_missile);
                  new_missile->owning_squad = cur_box->owning_squad; // missiles have teams and attack eachother!
                  cpBodySetPosition(new_missile->body, (cpvadd(entity_pos(cur_box), cpvspin((cpVect){.x = MISSILE_SPAWN_DIST, 0.0}, target.facing_angle))));
                  cpBodySetAngle(new_missile->body, target.facing_angle);
                  cpBodySetVelocity(new_missile->body, (box_vel(cur_box)));
                }
              }
              if (cur_box->box_type == BoxScanner)
              {
                cur_box->energy_effectiveness = batteries_use_energy(gs, grid, &non_battery_energy_left_over, SCANNER_ENERGY_USE * dt);

                // only the server knows all the positions of all the solids
                if (gs->server_side_computing)
                {
                  for (int i = 0; i < SCANNER_MAX_POINTS; i++)
                    cur_box->scanner_points[i] = (struct ScannerPoint){0};
                  for (int i = 0; i < SCANNER_MAX_PLATONICS; i++)
                    cur_box->detected_platonics[i] = (PlatonicDetection){0};
                  if (cur_box->energy_effectiveness >= 1.0)
                  {
                    cpVect from_pos = entity_pos(cur_box);
                    PlatonicDetection detections[MAX_BOX_TYPES] = {0};
                    for (int i = 0; i < MAX_BOX_TYPES; i++)
                    {
                      cpVect cur_pos = gs->platonic_positions[i];
                      if (cpvlengthsq(cur_pos) > 0.0) // zero is uninitialized, the platonic solid doesn't exist (probably) @Robust do better
                      {
                        cpVect towards = cpvsub(cur_pos, from_pos);
                        double length_to_cur = cpvlength(towards);
                        detections[i].direction = cpvnormalize(towards);
                        detections[i].of_type = i;
                        detections[i].intensity = length_to_cur; // so it sorts correctly, changed to intensity correctly after sorting
                      }
                    }
                    qsort(detections, MAX_BOX_TYPES, sizeof(detections[0]), platonic_detection_compare);
                    for (int i = 0; i < SCANNER_MAX_PLATONICS; i++)
                    {
                      for (int detections_i = 0; detections_i < MAX_BOX_TYPES; detections_i++)
                      {
                        // so can be viewed in debugger what causes it not to be used
                        bool use_this_detection = true;
                        use_this_detection &= detections[detections_i].intensity > 0.0;
                        use_this_detection &= !detections[detections_i].used_in_scanner_closest_lightning_bolts;
                        use_this_detection &= !learned_boxes_has_box(cur_box->blueprints_learned, detections[detections_i].of_type);
                        if (use_this_detection)
                        {
                          detections[detections_i].used_in_scanner_closest_lightning_bolts = true;
                          cur_box->detected_platonics[i] = detections[detections_i];
                          cur_box->detected_platonics[i].intensity = max(0.1, 1.0 - clamp01(cur_box->detected_platonics[i].intensity / 100.0));
                          break;
                        }
                      }
                    }

                    // after the above logic detections has been modified, do not use

                    circle_query(gs->space, entity_pos(cur_box), SCANNER_MAX_RANGE);
                    cpBody *body_results[512] = {0};
                    size_t cur_results_len = 0;

                    QUEUE_ITER(&query_result, QueryResult, res)
                    {
                      cpBody *cur_body = cpShapeGetBody(res->shape);
                      bool unique_body = true;
                      for (int i = 0; i < cur_results_len; i++)
                      {
                        if (body_results[i] == cur_body)
                        {
                          unique_body = false;
                          break;
                        }
                      }
                      if (unique_body && cur_results_len < ARRLEN(body_results))
                      {
                        body_results[cur_results_len] = cur_body;
                        cur_results_len++;
                      }
                    }
                    from_point = entity_pos(cur_box);
                    size_t sizeof_element = sizeof(body_results[0]);
                    qsort(body_results, cur_results_len, sizeof_element, sort_bodies_callback);
                    size_t bodies_detected = cur_results_len < SCANNER_MAX_POINTS ? cur_results_len : SCANNER_MAX_POINTS;
                    for (int i = 0; i < bodies_detected; i++)
                    {
                      cpVect rel_vect = cpvsub(cpBodyGetPosition(body_results[i]), from_point);
                      double vect_length = cpvlength(rel_vect);
                      if (SCANNER_MIN_RANGE < vect_length && vect_length < SCANNER_MAX_RANGE)
                      {
                        cpVect radar_vect = cpvmult(cpvnormalize(rel_vect), clamp01(vect_length / SCANNER_MAX_VIEWPORT_RANGE));

                        enum ScannerPointKind kind = Platonic;
                        Entity *body_entity = cp_body_entity(body_results[i]);
                        if (body_entity->is_grid)
                        {
                          kind = Neutral;
                          BOXES_ITER(gs, cur_potential_platonic, body_entity)
                          {
                            if (cur_potential_platonic->is_platonic)
                            {
                              kind = Platonic;
                            }
                          }
                        }
                        else if (body_entity->is_player)
                        {
                          kind = Neutral;
                        }
                        else
                        {
                          kind = Enemy;
                        }
                        cpVect into_char_vect = cpvmult(radar_vect, 126.0);
                        flight_assert(fabs(into_char_vect.x) <= 126.0);
                        flight_assert(fabs(into_char_vect.y) <= 126.0);
                        cur_box->scanner_points[i] = (struct ScannerPoint){
                            .kind = (char)kind,
                            .x = (char)(into_char_vect.x),
                            .y = (char)(into_char_vect.y),
                        };
                      }
                    }
                  }
                }

                // unlock the nearest platonic solid!
                scanner_has_learned = cur_box->blueprints_learned;
                Entity *to_learn = closest_box_to_point_in_radius(gs, entity_pos(cur_box), SCANNER_RADIUS, scanner_filter);
                if (to_learn != NULL)
                  flight_assert(to_learn->is_box);

                EntityID new_id = get_id(gs, to_learn);

                if (!entityids_same(cur_box->currently_scanning, new_id))
                {
                  cur_box->currently_scanning_progress = 0.0;
                  cur_box->currently_scanning = new_id;
                }

                // double target_head_rotate_speed = cur_box->platonic_detection_strength > 0.0 ? 3.0 : 0.0;
                double target_head_rotate_speed = 3.0 * cur_box->energy_effectiveness;
                if (to_learn != NULL)
                {
                  cur_box->currently_scanning_progress += dt * SCANNER_SCAN_RATE;
                  target_head_rotate_speed *= 30.0 * cur_box->currently_scanning_progress;
                }
                else
                  cur_box->currently_scanning_progress = 0.0;

                if (cur_box->currently_scanning_progress >= 1.0)
                {
                  cur_box->blueprints_learned |= box_unlock_number(to_learn->box_type);
                }

                cur_box->scanner_head_rotate_speed = lerp(cur_box->scanner_head_rotate_speed, target_head_rotate_speed, dt * 3.0);
                cur_box->scanner_head_rotate += cur_box->scanner_head_rotate_speed * dt;
                cur_box->scanner_head_rotate = fmod(cur_box->scanner_head_rotate, 2.0 * PI);
              }
              if (cur_box->box_type == BoxLandingGear)
              {
                cur_box->sees_possible_landing = false; // false by default, if codepath which binds it sees landing it is set to true
                cpVect landing_point = cpvadd(entity_pos(cur_box), cpvmult(box_facing_vector(cur_box), BOX_SIZE / 2.0));
                Entity *must_have_shape = get_entity(gs, cur_box->shape_to_land_on);
                bool want_have_constraint = true;
                if (want_have_constraint)
                  want_have_constraint &= must_have_shape != NULL;
                if (want_have_constraint)
                  want_have_constraint &= must_have_shape->shape != NULL;
                if (want_have_constraint)
                  want_have_constraint &= cpShapeGetBody(must_have_shape->shape) != NULL;
                if (want_have_constraint)
                  want_have_constraint &= cpvdist(entity_pos(must_have_shape), landing_point) < BOX_SIZE + LANDING_GEAR_MAX_DIST;

                  // maybe merge all codepaths that delete constraint into one somehow, but seems hard
#define DELETE_CONSTRAINT(constraint)               \
  {                                                 \
    cpSpaceRemoveConstraint(gs->space, constraint); \
    cpConstraintFree(constraint);                   \
    constraint = NULL;                              \
    cur_box->shape_to_land_on = (EntityID){0};      \
  }
                if (want_have_constraint)
                {
                  flight_assert(must_have_shape != NULL);
                  cpBody *body_a = box_grid(cur_box)->body;
                  cpBody *body_b = cpShapeGetBody(must_have_shape->shape);
                  if (cur_box->landed_constraint != NULL && (cpConstraintGetBodyA(cur_box->landed_constraint) != body_a || cpConstraintGetBodyB(cur_box->landed_constraint) != body_b))
                  {
                    DELETE_CONSTRAINT(cur_box->landed_constraint);
                  }
                  if (cur_box->landed_constraint == NULL)
                  {
                    cur_box->landed_constraint = cpPivotJointNew(body_a, body_b, landing_point);
                    cpSpaceAddConstraint(gs->space, cur_box->landed_constraint);
                    on_create_constraint(cur_box, cur_box->landed_constraint);
                  }
                  if (cur_box->toggle_landing)
                  {
                    DELETE_CONSTRAINT(cur_box->landed_constraint);
                  }
                }
                else
                {
                  if (cur_box->landed_constraint != NULL)
                  {
                    DELETE_CONSTRAINT(cur_box->landed_constraint);
                  }

                  // maybe see something to land on
                  cpVect along = box_facing_vector(cur_box);
                  cpVect from = cpvadd(entity_pos(cur_box), cpvmult(along, BOX_SIZE / 2.0 + 0.03));
                  cpVect to = cpvadd(from, cpvmult(along, LANDING_GEAR_MAX_DIST));

                  cpSegmentQueryInfo query_result = {0};
                  cpShape *found = cpSpaceSegmentQueryFirst(gs->space, from, to, 0.0, FILTER_DEFAULT, &query_result);
                  if (found != NULL && cpShapeGetBody(found) != box_grid(cur_box)->body)
                  {
                    cur_box->sees_possible_landing = true;
                    if (cur_box->toggle_landing)
                      cur_box->shape_to_land_on = get_id(gs, cp_shape_entity(found));
                  }
                }

                cur_box->toggle_landing = false; // handle it
              }
            }
          }
        }
      }
    }

    PROFILE_SCOPE("Delete entities")
    {
      ENTITIES_ITER(gs, e)
      {
        if (e->flag_for_destruction)
        {
          Entity *grid = NULL;
          if (e->is_box)
            grid = box_grid(e);
          entity_memory_free(gs, e);
          if (grid != NULL)
            grid_correct_for_holes(gs, grid);
        }
      }
    }

    PROFILE_SCOPE("chipmunk physics processing")
    {
      cpSpaceStep(gs->space, dt);
    }
  }
}
