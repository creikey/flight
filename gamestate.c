#include <chipmunk.h>
#define QUEUE_IMPL
#include "queue.h"
#include "stdbool.h"
#include "types.h"

#include "ipsettings.h" // debug/developer settings

#include <stdio.h>  // assert logging
#include <string.h> // memset

// do not use any global variables to process gamestate

// super try not to depend on external libraries like enet or sokol to keep build process simple,
// gamestate its own portable submodule. If need to link to other stuff document here:
// - debug.c for debug drawing
// - chipmunk

enum
{
  PLAYERS = 1 << 0,
  BOXES = 1 << 1,
};

void __flight_assert(bool cond, const char *file, int line, const char *cond_string)
{
  if (!cond)
  {
    fprintf(stderr, "%s:%d | Assertion %s failed\n", file, line, cond_string);
  }
}

static V2 cp_to_v2(cpVect v)
{
  return (V2){.x = (float)v.x, .y = (float)v.y};
}

static cpVect v2_to_cp(V2 v)
{
  return cpv(v.x, v.y);
}

bool is_burning(Entity *missile)
{
  assert(missile->is_missile);
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
  return gs->time >= 0.5 && (gs->time - e->time_was_last_cloaked) <= TIMESTEP * 2.0f;
}

bool is_cloaked(GameState *gs, Entity *e, Entity *this_players_perspective)
{
  assert(this_players_perspective != NULL);
  assert(this_players_perspective->is_player);
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
  assert((BOX_UNLOCKS_TYPE)box < 64);
  return (BOX_UNLOCKS_TYPE)((BOX_UNLOCKS_TYPE)1 << ((BOX_UNLOCKS_TYPE)box));
}

static bool learned_boxes_has_box(BOX_UNLOCKS_TYPE learned, enum BoxType box)
{
  return (learned & box_unlock_number(box)) > 0;
}

void unlock_box(Player *player, enum BoxType box)
{
  assert(box < MAX_BOX_TYPES);
  assert(box != BoxInvalid);
  player->box_unlocks |= box_unlock_number(box);
}

bool box_unlocked(Player *player, enum BoxType box)
{
  assert(box < MAX_BOX_TYPES);
  if (box == BoxInvalid)
    return false;
  return learned_boxes_has_box(player->box_unlocks, box);
}

EntityID get_id(GameState *gs, Entity *e)
{
  if (e == NULL)
    return (EntityID){0};

  size_t index = (e - gs->entities);
  assert(index >= 0);
  assert(index < gs->cur_next_entity);

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
  assert(e->body != NULL || e->shape != NULL);
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
  assert(e->is_grid);
  int to_return = 0;

  BOXES_ITER(gs, cur, e)
  to_return++;

  return to_return;
}

void box_remove_from_boxes(GameState *gs, Entity *box)
{
  assert(box->is_box);
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
    assert(next_box->is_box);
    next_box->prev_box = get_id(gs, prev_box);
  }
  box->next_box = (EntityID){0};
  box->prev_box = (EntityID){0};
}

V2 player_vel(GameState *gs, Entity *e);
V2 entity_vel(GameState *gs, Entity *e)
{
  assert(e->is_box || e->is_player || e->body != NULL || e->is_explosion);
  if (e->is_box)
    return box_vel(e);
  if (e->is_player)
    return player_vel(gs, e);
  if (e->body != NULL)
    return cp_to_v2(cpBodyGetVelocity(e->body));
  if (e->is_explosion)
    return e->explosion_vel;
  assert(false);
  return (V2){0};
}

static THREADLOCAL float to_face = 0.0f;
static THREADLOCAL float nearest_dist = INFINITY;
static THREADLOCAL bool target_found = false;
static void on_missile_shape(cpShape *shape, cpContactPointSet *points, void *data)
{
  Entity *launcher = (Entity *)data;
  Entity *other = cp_shape_entity(shape);
  GameState *gs = entitys_gamestate(launcher);
  assert(other->is_box || other->is_player || other->is_missile);

  V2 to = V2sub(entity_pos(other), entity_pos(launcher));
  bool should_attack = true;
  if (other->is_box && box_grid(other) == box_grid(launcher))
    should_attack = false;
  if (other->owning_squad == launcher->owning_squad)
    should_attack = false;

  if (should_attack && V2length(to) < nearest_dist)
  {
    target_found = true;
    nearest_dist = V2length(to);

    // lookahead by their velocity
    V2 rel_velocity = V2sub(entity_vel(gs, other), entity_vel(gs, launcher));
    float dist = V2dist(entity_pos(other), entity_pos(launcher));

    float time_of_travel = sqrtf((2.0f * dist) / (MISSILE_BURN_FORCE / MISSILE_MASS));

    V2 other_future_pos = V2add(entity_pos(other), V2scale(rel_velocity, time_of_travel));

    V2 adjusted_to = V2sub(other_future_pos, entity_pos(launcher));

    to_face = V2angle(adjusted_to);
  }
}

LauncherTarget missile_launcher_target(GameState *gs, Entity *launcher)
{
  to_face = 0.0f;
  cpBody *tmp = cpBodyNew(0.0f, 0.0f);
  cpBodySetPosition(tmp, v2_to_cp(entity_pos(launcher)));
  cpShape *circle = cpCircleShapeNew(tmp, MISSILE_RANGE, cpv(0, 0));

  nearest_dist = INFINITY;
  to_face = 0.0f;
  target_found = false;
  cpSpaceShapeQuery(gs->space, circle, on_missile_shape, (void *)launcher);

  cpBodyFree(tmp);
  cpShapeFree(circle);
  return (LauncherTarget){.target_found = target_found, .facing_angle = to_face};
}

void on_entity_child_shape(cpBody *body, cpShape *shape, void *data);

// gs is for iterating over all child shapes and destroying those, too
static void destroy_body(GameState *gs, cpBody **body)
{
  if (*body != NULL)
  {
    cpBodyEachShape(*body, on_entity_child_shape, (void *)gs);
    cpSpaceRemoveBody(gs->space, *body);
    cpBodyFree(*body);
    *body = NULL;
  }
  *body = NULL;
}

void entity_destroy(GameState *gs, Entity *e)
{
  assert(e->exists);

  if (e->is_grid)
  {
    BOXES_ITER(gs, cur, e)
    entity_destroy(gs, cur);
  }
  if (e->is_box)
  {
    box_remove_from_boxes(gs, e);
  }

  if (e->shape != NULL)
  {
    cpSpaceRemoveShape(gs->space, e->shape);
    cpShapeFree(e->shape);
    e->shape = NULL;
  }
  destroy_body(gs, &e->body);

  Entity *front_of_free_list = get_entity(gs, gs->free_list);
  if (front_of_free_list != NULL)
    assert(!front_of_free_list->exists);
  int gen = e->generation;
  *e = (Entity){0};
  e->generation = gen;
  e->next_free_entity = gs->free_list;
  gs->free_list = get_id(gs, e);
}

void on_entity_child_shape(cpBody *body, cpShape *shape, void *data)
{
  entity_destroy((GameState *)data, cp_shape_entity(shape));
}

Entity *new_entity(GameState *gs)
{
  Entity *to_return = NULL;
  Entity *possible_free_list = get_entity_even_if_dead(gs, gs->free_list);
  if (possible_free_list != NULL)
  {
    assert(possible_free_list->generation == gs->free_list.generation);
    to_return = possible_free_list;
    assert(!to_return->exists);
    gs->free_list = to_return->next_free_entity;
  }
  else
  {
    assert(gs->cur_next_entity < gs->max_entities); // too many entities if fails
    to_return = &gs->entities[gs->cur_next_entity];
    gs->cur_next_entity++;
  }

  to_return->generation++;
  to_return->exists = true;
  return to_return;
}

void create_body(GameState *gs, Entity *e)
{
  assert(gs->space != NULL);

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

V2 player_vel(GameState *gs, Entity *player)
{
  assert(player->is_player);
  Entity *potential_seat = get_entity(gs, player->currently_inside_of_box);
  if (potential_seat != NULL)
  {
    return cp_to_v2(cpBodyGetVelocity(get_entity(gs, potential_seat->shape_parent_entity)->body));
  }
  else
  {
    return cp_to_v2(cpBodyGetVelocity(player->body));
  }
}

void grid_create(GameState *gs, Entity *e)
{
  e->is_grid = true;
  create_body(gs, e);
}

void entity_set_rotation(Entity *e, float rot)
{
  assert(e->body != NULL);
  cpBodySetAngle(e->body, rot);
}

void entity_set_pos(Entity *e, V2 pos)
{
  assert(e->is_grid);
  assert(e->body != NULL);
  cpBodySetPosition(e->body, v2_to_cp(pos));
}

// size is (1/2 the width, 1/2 the height)
void create_rectangle_shape(GameState *gs, Entity *e, Entity *parent, V2 pos, V2 size, float mass)
{
  if (e->shape != NULL)
  {
    cpSpaceRemoveShape(gs->space, e->shape);
    cpShapeFree(e->shape);
    e->shape = NULL;
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
  e->shape = (cpShape *)cpPolyShapeInitRaw(cpPolyShapeAlloc(), parent->body, 4, verts, 0.0f); // this cast is done in chipmunk, not sure why it works
  cpShapeSetUserData(e->shape, (void *)e);
  cpShapeSetMass(e->shape, mass);
  cpSpaceAddShape(gs->space, e->shape);
}

#define PLAYER_SHAPE_FILTER cpShapeFilterNew(CP_NO_GROUP, PLAYERS, CP_ALL_CATEGORIES)

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
  unlock_box(player, BoxMedbay);
  unlock_box(player, BoxSolarPanel);
  unlock_box(player, BoxScanner);
#endif
}

void create_missile(GameState *gs, Entity *e)
{
  create_body(gs, e);
  create_rectangle_shape(gs, e, e, (V2){0}, V2scale(MISSILE_COLLIDER_SIZE, 0.5f), PLAYER_MASS);
  e->is_missile = true;
}

void create_player_entity(GameState *gs, Entity *e)
{
  e->is_player = true;
  e->no_save_to_disk = true;
  create_body(gs, e);
  create_rectangle_shape(gs, e, e, (V2){0}, V2scale(PLAYER_SIZE, 0.5f), PLAYER_MASS);
  cpShapeSetFilter(e->shape, PLAYER_SHAPE_FILTER);
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
void box_create(GameState *gs, Entity *new_box, Entity *grid, V2 pos)
{
  new_box->is_box = true;
  assert(gs->space != NULL);
  assert(grid->is_grid);

  float halfbox = BOX_SIZE / 2.0f;

  create_rectangle_shape(gs, new_box, grid, pos, (V2){halfbox, halfbox}, 1.0f);

  cpShapeSetFilter(new_box->shape, cpShapeFilterNew(CP_NO_GROUP, BOXES, CP_ALL_CATEGORIES));

  box_add_to_boxes(gs, grid, new_box);
}

// removes boxes from grid, then ensures that the rule that grids must not have
// holes in them is applied.
static void grid_correct_for_holes(GameState *gs, struct Entity *grid)
{
  int num_boxes = grid_num_boxes(gs, grid);
  if (num_boxes == 0)
  {
    entity_destroy(gs, grid);
    return;
  }
  if (num_boxes == 1)
    return;

    // could be a gap between boxes in the grid, separate into multiple grids

    // goal: create list of "real grids" from this grid that have boxes which are
    // ONLY connected horizontally and vertically. whichever one of these "real grids"
    // has the most blocks stays the current grid, so
    // if a player is inhabiting this ship it stays that ship.
    // The other "real grids" are allocated as new grids

#define MAX_SEPARATE_GRIDS 8
  EntityID separate_grids[MAX_SEPARATE_GRIDS] = {0};
  int cur_separate_grid_index = 0;
  int cur_separate_grid_size = 0;
  int processed_boxes = 0;

  int biggest_separate_grid_index = 0;
  int biggest_separate_grid_length = 0;

  // process all boxes into separate, but correctly connected, grids
  while (processed_boxes < num_boxes)
  {
    // grab an unprocessed box, one not in separate_grids, to start the flood fill
    Entity *unprocessed = get_entity(gs, grid->boxes);
    assert(unprocessed != NULL);
    assert(unprocessed->is_box);
    box_remove_from_boxes(gs, unprocessed); // no longer in the boxes list of the grid

    // flood fill from this unprocessed box, adding each result to cur_separate_grid_index,
    // removing each block from the grid
    // https://en.wikipedia.org/wiki/Flood_fill
    {
      // queue stuff @Robust use factored datastructure
      EntityID Q = get_id(gs, unprocessed);
      Entity *N = NULL;
      while (true)
      {
        assert(!was_entity_deleted(gs, Q));
        N = get_entity(gs, Q);
        if (N == NULL) // must mean that the queue is empty
          break;
        Q = N->next_box;
        if (true) // if node "inside", this is always true
        {
          N->next_box = separate_grids[cur_separate_grid_index];
          separate_grids[cur_separate_grid_index] = get_id(gs, N);
          cur_separate_grid_size++;
          processed_boxes++;

          V2 cur_local_pos = entity_shape_pos(N);
          const V2 dirs[] = {
              (V2){
                  .x = -1.0f, .y = 0.0f},
              (V2){
                  .x = 1.0f, .y = 0.0f},
              (V2){
                  .x = 0.0f, .y = 1.0f},
              (V2){
                  .x = 0.0f, .y = -1.0f},
          };
          int num_dirs = sizeof(dirs) / sizeof(*dirs);

          for (int ii = 0; ii < num_dirs; ii++)
          {
            V2 dir = dirs[ii];
            // @Robust @Speed faster method, not O(N^2), of getting the box
            // in the direction currently needed
            V2 wanted_local_pos = V2add(cur_local_pos, V2scale(dir, BOX_SIZE));
            EntityID box_in_direction = (EntityID){0};
            BOXES_ITER(gs, cur, grid)
            {
              if (V2equal(entity_shape_pos(cur), wanted_local_pos, 0.01f))
              {
                box_in_direction = get_id(gs, cur);
                break;
              }
            }

            Entity *newbox = get_entity(gs, box_in_direction);
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

    if (cur_separate_grid_size > biggest_separate_grid_length)
    {
      biggest_separate_grid_length = cur_separate_grid_size;
      biggest_separate_grid_index = cur_separate_grid_index;
    }
    cur_separate_grid_index++;
    assert(cur_separate_grid_index < MAX_SEPARATE_GRIDS);
    cur_separate_grid_size = 0;
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
    if (sepgrid_i == biggest_separate_grid_index)
    {
      new_grid = grid;
    }
    else
    {
      new_grid = new_entity(gs);
      grid_create(gs, new_grid);
      cpBodySetPosition(new_grid->body, cpBodyGetPosition(grid->body));
      cpBodySetAngle(new_grid->body, cpBodyGetAngle(grid->body));
    }

    Entity *cur = get_entity(gs, cur_separate_grid);
    while (cur != NULL)
    {
      Entity *next = get_entity(gs, cur->next_box);
      box_create(gs, cur, new_grid, entity_shape_pos(cur)); // destroys next/prev fields on cur
      cur = next;
    }

    cpBodySetVelocity(new_grid->body, cpBodyGetVelocityAtWorldPoint(grid->body, v2_to_cp(grid_com(new_grid))));
    cpBodySetAngularVelocity(new_grid->body, entity_angular_velocity(grid));
  }
}

static void grid_remove_box(GameState *gs, struct Entity *grid, struct Entity *box)
{
  assert(grid->is_grid);
  assert(box->is_box);
  entity_destroy(gs, box);
  grid_correct_for_holes(gs, grid);
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
        V2 local_collision_point = cp_to_v2(cpBodyWorldToLocal(missile->body, collision_point));
        if (local_collision_point.x > MISSILE_COLLIDER_SIZE.x * 0.2f)
        {
          missile->damage += MISSILE_DAMAGE_THRESHOLD * 2.0f;
        }
      }
    }
  }

  // if(entity_a->is_missile) {getPointFunc = cpArbiterGetPointA;
  // if(entity_b->is_missile) getPointFunc = cpArbiterGetPointB;

  float damage = V2length(cp_to_v2(cpArbiterTotalImpulse(arb))) * COLLISION_DAMAGE_SCALING;

  if (entity_a->is_box && entity_a->box_type == BoxExplosive)
    entity_a->damage += 2.0f * EXPLOSION_DAMAGE_THRESHOLD;
  if (entity_b->is_box && entity_b->box_type == BoxExplosive)
    entity_b->damage += 2.0f * EXPLOSION_DAMAGE_THRESHOLD;

  if (damage > 0.05f)
  {
    // Log("Collision with damage %f\n", damage);
    entity_a->damage += damage;
    entity_b->damage += damage;
  }

  // b must be the key passed into the post step removed, the key is cast into its shape
  // cpSpaceAddPostStepCallback(space, (cpPostStepFunc)postStepRemove, b, NULL);
  // cpSpaceAddPostStepCallback(space, (cpPostStepFunc)postStepRemove, a, NULL);
}

// must be called with zero initialized game state, because copies the server side computing!
void initialize(GameState *gs, void *entity_arena, size_t entity_arena_size)
{
  bool is_server_side = gs->server_side_computing;
  *gs = (GameState){0};
  memset(entity_arena, 0, entity_arena_size); // SUPER critical. Random vals in the entity data causes big problem
  gs->entities = (Entity *)entity_arena;
  gs->max_entities = (unsigned int)(entity_arena_size / sizeof(Entity));
  gs->space = cpSpaceNew();
  cpSpaceSetUserData(gs->space, (cpDataPointer)gs);                          // needed in the handler
  cpCollisionHandler *handler = cpSpaceAddCollisionHandler(gs->space, 0, 0); // @Robust limit collision type to just blocks that can be damaged
  handler->postSolveFunc = on_damage;
  gs->server_side_computing = is_server_side;
}
void destroy(GameState *gs)
{
  // can't zero out gs data because the entity memory arena is reused
  // on deserialization
  for (size_t i = 0; i < gs->max_entities; i++)
  {
    if (gs->entities[i].exists)
      entity_destroy(gs, &gs->entities[i]);
  }
  cpSpaceFree(gs->space);
  gs->space = NULL;

  for (size_t i = 0; i < gs->cur_next_entity; i++)
  {
    if (gs->entities[i].exists)
      gs->entities[i] = (Entity){0};
  }
  gs->cur_next_entity = 0;
}
// center of mass, not the literal position
V2 grid_com(Entity *grid)
{
  return cp_to_v2(cpBodyLocalToWorld(grid->body, cpBodyGetCenterOfGravity(grid->body)));
}

V2 grid_vel(Entity *grid)
{
  return cp_to_v2(cpBodyGetVelocity(grid->body));
}
V2 grid_world_to_local(Entity *grid, V2 world)
{
  return cp_to_v2(cpBodyWorldToLocal(grid->body, v2_to_cp(world)));
}
V2 grid_local_to_world(Entity *grid, V2 local)
{
  assert(grid->is_grid);
  return cp_to_v2(cpBodyLocalToWorld(grid->body, v2_to_cp(local)));
}
// returned snapped position is in world coordinates
V2 grid_snapped_box_pos(Entity *grid, V2 world)
{
  V2 local = grid_world_to_local(grid, world);
  local.x /= BOX_SIZE;
  local.y /= BOX_SIZE;
  local.x = roundf(local.x);
  local.y = roundf(local.y);
  local.x *= BOX_SIZE;
  local.y *= BOX_SIZE;

  return cp_to_v2(cpBodyLocalToWorld(grid->body, v2_to_cp(local)));
}

// for boxes does not include box's compass rotation
float entity_rotation(Entity *e)
{
  assert(e->body != NULL || e->shape != NULL);
  if (e->body != NULL)
    return (float)cpBodyGetAngle(e->body);
  else
    return (float)cpBodyGetAngle(cpShapeGetBody(e->shape));
}

float entity_angular_velocity(Entity *grid)
{
  return (float)cpBodyGetAngularVelocity(grid->body);
}
Entity *box_grid(Entity *box)
{
  if (box == NULL)
    return NULL;
  assert(box->is_box);
  return (Entity *)cpBodyGetUserData(cpShapeGetBody(box->shape));
}
// in local space
V2 entity_shape_pos(Entity *box)
{
  return cp_to_v2(cpShapeGetCenterOfGravity(box->shape));
}
float entity_shape_mass(Entity *box)
{
  assert(box->shape != NULL);
  return (float)cpShapeGetMass(box->shape);
}
float box_rotation(Entity *box)
{
  return (float)cpBodyGetAngle(cpShapeGetBody(box->shape));
}
V2 entity_pos(Entity *e)
{
  if (e->is_box)
  {
    return V2add(entity_pos(box_grid(e)), V2rotate(entity_shape_pos(e), entity_rotation(box_grid(e))));
  }
  else if (e->is_explosion)
  {
    return e->explosion_pos;
  }
  else
  {
    assert(e->body != NULL);
    return cp_to_v2(cpBodyGetPosition(e->body));
  }
}

struct BodyData
{
  V2 pos;
  V2 vel;
  float rotation;
  float angular_velocity;
};

void populate(cpBody *body, struct BodyData *data)
{
  data->pos = cp_to_v2(cpBodyGetPosition(body));
  data->vel = cp_to_v2(cpBodyGetVelocity(body));
  data->rotation = (float)cpBodyGetAngle(body);
  data->angular_velocity = (float)cpBodyGetAngularVelocity(body);
}

void update_from(cpBody *body, struct BodyData *data)
{
  cpBodySetPosition(body, v2_to_cp(data->pos));
  cpBodySetVelocity(body, v2_to_cp(data->vel));
  cpBodySetAngle(body, data->rotation);
  cpBodySetAngularVelocity(body, data->angular_velocity);
}

typedef struct SerState
{
  unsigned char *bytes;
  bool serializing;
  size_t cursor; // points to next available byte, is the size of current message after serializing something
  size_t max_size;
  Entity *for_player;
  size_t max_entity_index; // for error checking
  bool write_varnames;
  bool save_or_load_from_disk;

  // output
  uint32_t version;
} SerState;

typedef struct SerMaybeFailure
{
  bool failed;
  int line;
  const char *expression;
} SerMaybeFailure;
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
      memcpy(ser->bytes + ser->cursor, var_name, var_name_len);
      ser->cursor += var_name_len;
    }
    for (int b = 0; b < data_len; b++)
    {
      ser->bytes[ser->cursor] = data[b];
      ser->cursor += 1;
      SER_ASSERT(ser->cursor < ser->max_size);
    }
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
    }
    for (int b = 0; b < data_len; b++)
    {
      data[b] = ser->bytes[ser->cursor];
      ser->cursor += 1;
      SER_ASSERT(ser->cursor <= ser->max_size);
    }
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
  VMoreBoxes,
  VMissileMerge,
  VMax, // this minus one will be the version used
};

// @Robust probably get rid of this as separate function, just use SER_VAR
SerMaybeFailure ser_V2(SerState *ser, V2 *var)
{
  SER_VAR(&var->x);
  SER_VAR(&var->y);
  SER_ASSERT(!isnan(var->x));
  SER_ASSERT(!isnan(var->y));
  return ser_ok;
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
  SER_MAYBE_RETURN(ser_V2(ser, &i->movement));
  SER_VAR(&i->rotation);
  SER_VAR(&i->take_over_squad);
  SER_ASSERT(i->take_over_squad >= 0 || i->take_over_squad == -1);
  SER_ASSERT(i->take_over_squad < SquadLast);
  SER_VAR(&i->accept_cur_squad_invite);
  SER_VAR(&i->reject_cur_squad_invite);
  SER_MAYBE_RETURN(ser_entityid(ser, &i->invite_this_player));

  SER_VAR(&i->seat_action);
  SER_MAYBE_RETURN(ser_V2(ser, &i->hand_pos));

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
  SER_VAR(&e->no_save_to_disk); // @Robust this is always false when saving to disk?
  SER_VAR(&e->generation);
  SER_VAR(&e->damage);

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
    SER_MAYBE_RETURN(ser_V2(ser, &e->shape_size));
    SER_MAYBE_RETURN(ser_entityid(ser, &e->shape_parent_entity));
    Entity *parent = get_entity(gs, e->shape_parent_entity);
    SER_ASSERT(parent != NULL);

    V2 shape_pos;
    if (ser->serializing)
      shape_pos = entity_shape_pos(e);
    SER_MAYBE_RETURN(ser_V2(ser, &shape_pos));

    float shape_mass;
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
      create_rectangle_shape(gs, e, parent, shape_pos, e->shape_size, shape_mass);
      cpShapeSetFilter(e->shape, filter);
    }
  }

  if (ser->version >= VMoreBoxes && !ser->save_or_load_from_disk)
    SER_VAR(&e->time_was_last_cloaked);

  if (ser->version >= VMissileMerge)
    SER_VAR(&e->owning_squad);

  SER_VAR(&e->is_player);
  if (e->is_player)
  {
    SER_ASSERT(e->no_save_to_disk);

    SER_MAYBE_RETURN(ser_entityid(ser, &e->currently_inside_of_box));
    if (ser->version < VMissileMerge)
    {
      SER_VAR_NAME(&e->owning_squad, "&e->presenting_squad");
    }
    SER_VAR(&e->squad_invited_to);
    SER_VAR(&e->goldness);
  }

  SER_VAR(&e->is_explosion);
  if (e->is_explosion)
  {
    SER_MAYBE_RETURN(ser_V2(ser, &e->explosion_pos));
    SER_MAYBE_RETURN(ser_V2(ser, &e->explosion_vel));
    SER_VAR(&e->explosion_progresss);
  }

  SER_VAR(&e->is_grid);
  if (e->is_grid)
  {
    SER_VAR(&e->total_energy_capacity);
    SER_MAYBE_RETURN(ser_entityid(ser, &e->boxes));
  }

  if (ser->version >= VMissileMerge)
  {
    SER_VAR(&e->is_missile)
    if (e->is_missile)
    {
      SER_VAR(&e->time_burned_for);
    }
  }

  SER_VAR(&e->is_box);
  if (e->is_box)
  {
    SER_VAR(&e->box_type);
    SER_VAR(&e->is_platonic);

    if (ser->version >= VMoreBoxes)
      SER_VAR(&e->owning_squad);

    SER_VAR(&e->always_visible);
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
    case BoxGyroscope:
      SER_VAR(&e->thrust);
      SER_VAR(&e->wanted_thrust);
      break;
    case BoxBattery:
      SER_VAR(&e->energy_used);
      break;
    case BoxSolarPanel:
      SER_VAR(&e->sun_amount);
      break;
    case BoxScanner:
      SER_MAYBE_RETURN(ser_entityid(ser, &e->currently_scanning));
      SER_VAR(&e->currently_scanning_progress);
      SER_VAR(&e->blueprints_learned);
      SER_VAR(&e->scanner_head_rotate);
      SER_VAR(&e->platonic_nearest_direction);
      SER_VAR(&e->platonic_detection_strength);
      break;
    case BoxCloaking:
      SER_VAR(&e->cloaking_power);
      break;
    case BoxMissileLauncher:
      SER_VAR(&e->missile_construction_charge);
      break;
    default:
      break;
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

  if (!ser->save_or_load_from_disk)
    SER_MAYBE_RETURN(ser_opus_packets(ser, s->audio_playback_buffer));

  GameState *gs = s->cur_gs;

  // completely reset and destroy all gamestate data
  if (!ser->serializing)
  {
    // avoid a memset here very expensive. que rico!
    destroy(gs);
    initialize(gs, gs->entities, gs->max_entities * sizeof(*gs->entities));
    gs->cur_next_entity = 0; // updated on deserialization
  }

  int cur_next_entity = 0;
  if (ser->serializing)
    cur_next_entity = gs->cur_next_entity;
  SER_VAR(&cur_next_entity);
  SER_ASSERT(cur_next_entity <= ser->max_entity_index);

  SER_VAR(&s->your_player);
  SER_VAR(&gs->time);

  SER_MAYBE_RETURN(ser_V2(ser, &gs->goldpos));

  if (!ser->save_or_load_from_disk) // don't save player info to disk, this is filled on connection/disconnection
  {
    // @Robust save player data with their ID or something somehow. Like local backup of their account
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
  if (ser->serializing)
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
          SER_ENTITY();
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
            this_box_in_range |= (ser->for_player != NULL && V2distsqr(entity_pos(ser->for_player), entity_pos(cur_box)) < VISION_RADIUS * VISION_RADIUS); // only in vision radius
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
              size_t the_index = (size_t)cur_id.index; // super critical. Type of &i is size_t. @Robust add debug info in serialization for what size the expected type is, maybe string nameof the type
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
  else
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
  return ser_ok;
}

// for_this_player can be null then the entire world will be sent
bool server_to_client_serialize(struct ServerToClient *msg, unsigned char *bytes, size_t *out_len, size_t max_len, Entity *for_this_player, bool to_disk)
{
  assert(msg->cur_gs != NULL);
  assert(msg != NULL);

  SerState ser = (SerState){
      .bytes = bytes,
      .serializing = true,
      .cursor = 0,
      .max_size = max_len,
      .for_player = for_this_player,
      .max_entity_index = msg->cur_gs->cur_next_entity,
      .version = VMax - 1,
  };

  if (for_this_player == NULL) // @Robust jank
  {
    ser.save_or_load_from_disk = true;
  }

  ser.write_varnames = to_disk;
#ifdef WRITE_VARNAMES
  ser.write_varnames = true;
#endif

  SerMaybeFailure result = ser_server_to_client(&ser, msg);
  *out_len = ser.cursor + 1; // @Robust not sure why I need to add one to cursor, ser.cursor should be the length..
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
  assert(msg->cur_gs != NULL);
  assert(msg != NULL);

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
    QUEUE_ITER(msg->input_data, cur_header)
    {
      if (i < to_skip)
      {
        i++;
      }
      else
      {
        InputFrame *cur = (InputFrame *)cur_header->data;
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

bool client_to_server_serialize(GameState *gs, struct ClientToServer *msg, unsigned char *bytes, size_t *out_len, size_t max_len)
{
  SerState ser = (SerState){
      .bytes = bytes,
      .serializing = true,
      .cursor = 0,
      .max_size = max_len,
      .for_player = NULL,
      .max_entity_index = gs->cur_next_entity,
      .version = VMax - 1,
  };
#ifdef WRITE_VARNAMES
  ser.write_varnames = true;
#endif

  SerMaybeFailure result = ser_client_to_server(&ser, msg);
  *out_len = ser.cursor + 1; // see other comment for server to client
  if (result.failed)
  {
    Log("Failed to serialize client to server because %s was false, line %d\n", result.expression, result.line);
    return false;
  }
  else
  {
    return true;
  }
}

bool client_to_server_deserialize(GameState *gs, struct ClientToServer *msg, unsigned char *bytes, size_t max_len)
{
  SerState servar = (SerState){
      .bytes = bytes,
      .serializing = false,
      .cursor = 0,
      .max_size = max_len,
      .max_entity_index = gs->cur_next_entity,
      .save_or_load_from_disk = false,
  };
#ifdef WRITE_VARNAMES
  servar.write_varnames = true;
#endif

  SerState *ser = &servar;
  SerMaybeFailure result = ser_client_to_server(ser, msg);
  if (result.failed)
  {
    Log("Failed to deserialize client to server on line %d because of %s\n", result.line, result.expression);
    return false;
  }
  else
  {
    return true;
  }
}

static void cloaking_shield_callback_func(cpShape *shape, cpContactPointSet *points, void *data)
{
  Entity *from_cloaking_box = (Entity *)data;
  GameState *gs = entitys_gamestate(from_cloaking_box);
  Entity *to_cloak = cp_shape_entity(shape);

  to_cloak->time_was_last_cloaked = gs->time;
  to_cloak->last_cloaked_by_squad = from_cloaking_box->owning_squad;
}

// has to be global var because can only get this information
static THREADLOCAL cpShape *closest_to_point_in_radius_result = NULL;
static THREADLOCAL float closest_to_point_in_radius_result_largest_dist = 0.0f;
static THREADLOCAL bool (*closest_to_point_in_radius_filter_func)(Entity *);
static void closest_point_callback_func(cpShape *shape, cpContactPointSet *points, void *data)
{
  assert(points->count == 1);
  Entity *e = cp_shape_entity(shape);
  if (!e->is_box)
    return;

  if (closest_to_point_in_radius_filter_func != NULL && !closest_to_point_in_radius_filter_func(e))
    return;
  float dist = V2length(cp_to_v2(cpvsub(points->points[0].pointA, points->points[0].pointB)));
  // float dist = -points->points[0].distance;
  if (dist > closest_to_point_in_radius_result_largest_dist)
  {
    closest_to_point_in_radius_result_largest_dist = dist;
    closest_to_point_in_radius_result = shape;
  }
}

// filter func null means everything is ok, if it's not null and returns false, that means
// exclude it from the selection. This returns the closest box entity!
Entity *closest_box_to_point_in_radius(struct GameState *gs, V2 point, float radius, bool (*filter_func)(Entity *))
{
  closest_to_point_in_radius_result = NULL;
  closest_to_point_in_radius_result_largest_dist = 0.0f;
  closest_to_point_in_radius_filter_func = filter_func;
  cpBody *tmpbody = cpBodyNew(0.0f, 0.0f);
  cpShape *circle = cpCircleShapeNew(tmpbody, radius, v2_to_cp(point));
  cpSpaceShapeQuery(gs->space, circle, closest_point_callback_func, NULL);

  cpShapeFree(circle);
  cpBodyFree(tmpbody);

  if (closest_to_point_in_radius_result != NULL)
  {
    // @Robust query here for only boxes that are part of ships, could get nasty...
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

static float cur_explosion_damage = 0.0f;
static V2 explosion_origin = {0};
static void explosion_callback_func(cpShape *shape, cpContactPointSet *points, void *data)
{
  GameState *gs = (GameState *)data;
  cp_shape_entity(shape)->damage += cur_explosion_damage;
  Entity *parent = get_entity(gs, cp_shape_entity(shape)->shape_parent_entity);
  V2 from_pos = entity_pos(cp_shape_entity(shape));
  V2 impulse = V2scale(V2normalize(V2sub(from_pos, explosion_origin)), EXPLOSION_PUSH_STRENGTH);
  assert(parent->body != NULL);
  cpBodyApplyImpulseAtWorldPoint(parent->body, v2_to_cp(impulse), v2_to_cp(from_pos));
}

static void do_explosion(GameState *gs, Entity *explosion, float dt)
{
  cur_explosion_damage = dt * EXPLOSION_DAMAGE_PER_SEC;
  explosion_origin = explosion->explosion_pos;

  cpBody *tmpbody = cpBodyNew(0.0f, 0.0f);
  cpShape *circle = cpCircleShapeNew(tmpbody, EXPLOSION_RADIUS, v2_to_cp(explosion_origin));

  cpSpaceShapeQuery(gs->space, circle, explosion_callback_func, (void *)gs);

  cpShapeFree(circle);
  cpBodyFree(tmpbody);
}

V2 box_facing_vector(Entity *box)
{
  assert(box->is_box);
  V2 to_return = (V2){.x = 1.0f, .y = 0.0f};

  to_return = V2rotate(to_return, rotangle(box->compass_rotation));
  to_return = V2rotate(to_return, box_rotation(box));

  return to_return;
}

V2 thruster_force(Entity *box)
{
  return V2scale(box_facing_vector(box), -box->thrust * THRUSTER_FORCE);
}

uint64_t tick(GameState *gs)
{
  return (uint64_t)floor(gs->time / ((double)TIMESTEP));
}

Entity *grid_to_build_on(GameState *gs, V2 world_hand_pos)
{
  return box_grid(closest_box_to_point_in_radius(gs, world_hand_pos, BUILD_BOX_SNAP_DIST_TO_SHIP, NULL));
}

V2 potentially_snap_hand_pos(GameState *gs, V2 world_hand_pos)
{
  Entity *potential_grid = grid_to_build_on(gs, world_hand_pos);
  if (potential_grid != NULL)
  {
    world_hand_pos = grid_snapped_box_pos(potential_grid, world_hand_pos);
  }
  return world_hand_pos;
}

V2 get_world_hand_pos(GameState *gs, InputFrame *input, Entity *player)
{
  return potentially_snap_hand_pos(gs, V2add(entity_pos(player), input->hand_pos));
}

bool batteries_have_capacity_for(GameState *gs, Entity *grid, float *energy_left_over, float energy_to_use)
{
  float seen_energy = 0.0f;
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

// returns any energy unable to burn
float batteries_use_energy(GameState *gs, Entity *grid, float *energy_left_over, float energy_to_use)
{
  if (*energy_left_over > 0.0f)
  {
    float energy_to_use_from_leftover = fminf(*energy_left_over, energy_to_use);
    *energy_left_over -= energy_to_use_from_leftover;
    energy_to_use -= energy_to_use_from_leftover;
  }
  BOXES_ITER(gs, possible_battery, grid)
  {
    if (possible_battery->box_type == BoxBattery)
    {
      Entity *battery = possible_battery;
      float energy_to_burn_from_this_battery = fminf(BATTERY_CAPACITY - battery->energy_used, energy_to_use);
      battery->energy_used += energy_to_burn_from_this_battery;
      energy_to_use -= energy_to_burn_from_this_battery;
      if (energy_to_use <= 0.0f)
        return 0.0f;
    }
  }
  return energy_to_use;
}

float sun_gravity_at_point(V2 p)
{
  if (V2length(V2sub(p, SUN_POS)) > SUN_NO_MORE_ELECTRICITY_OR_GRAVITY)
    return 0.0f;
  return SUN_GRAVITY_STRENGTH;
}

void entity_ensure_in_orbit(Entity *e)
{
  assert(e->body != NULL);

  cpVect pos = v2_to_cp(V2sub(entity_pos(e), SUN_POS));
  cpFloat r = cpvlength(pos);
  cpFloat v = cpfsqrt(sun_gravity_at_point(cp_to_v2(pos)) / r) / r;
  cpBodySetVelocity(e->body, cpvmult(cpvperp(pos), v));
}

V2 box_vel(Entity *box)
{
  assert(box->is_box);
  Entity *grid = box_grid(box);
  return cp_to_v2(cpBodyGetVelocityAtWorldPoint(grid->body, v2_to_cp(entity_pos(box))));
}

void create_bomb_station(GameState *gs, V2 pos, enum BoxType platonic_type)
{

#define BOX_AT_TYPE(grid, pos, type)      \
  {                                       \
    Entity *box = new_entity(gs);         \
    box_create(gs, box, grid, pos);       \
    box->box_type = type;                 \
    box->indestructible = indestructible; \
  }
#define BOX_AT(grid, pos) BOX_AT_TYPE(grid, pos, BoxHullpiece)

  bool indestructible = false;
  Entity *grid = new_entity(gs);
  grid_create(gs, grid);
  entity_set_pos(grid, pos);
  entity_ensure_in_orbit(grid);
  Entity *platonic_box = new_entity(gs);
  box_create(gs, platonic_box, grid, (V2){0});
  platonic_box->box_type = platonic_type;
  platonic_box->is_platonic = true;
  BOX_AT_TYPE(grid, ((V2){BOX_SIZE, 0}), BoxExplosive);
  BOX_AT_TYPE(grid, ((V2){BOX_SIZE * 2, 0}), BoxHullpiece);
  BOX_AT_TYPE(grid, ((V2){BOX_SIZE * 3, 0}), BoxHullpiece);
  BOX_AT_TYPE(grid, ((V2){BOX_SIZE * 4, 0}), BoxHullpiece);

  indestructible = true;
  for (float y = -BOX_SIZE * 5.0; y <= BOX_SIZE * 5.0; y += BOX_SIZE)
  {
    BOX_AT_TYPE(grid, ((V2){BOX_SIZE * 5.0, y}), BoxHullpiece);
  }
  for (float x = -BOX_SIZE * 5.0; x <= BOX_SIZE * 5.0; x += BOX_SIZE)
  {
    BOX_AT_TYPE(grid, ((V2){x, BOX_SIZE * 5.0}), BoxHullpiece);
    BOX_AT_TYPE(grid, ((V2){x, -BOX_SIZE * 5.0}), BoxHullpiece);
  }
  indestructible = false;
  BOX_AT_TYPE(grid, ((V2){-BOX_SIZE * 6.0, BOX_SIZE * 5.0}), BoxExplosive);
  BOX_AT_TYPE(grid, ((V2){-BOX_SIZE * 6.0, BOX_SIZE * 3.0}), BoxExplosive);
  BOX_AT_TYPE(grid, ((V2){-BOX_SIZE * 6.0, BOX_SIZE * 1.0}), BoxExplosive);
  BOX_AT_TYPE(grid, ((V2){-BOX_SIZE * 6.0, -BOX_SIZE * 2.0}), BoxExplosive);
  BOX_AT_TYPE(grid, ((V2){-BOX_SIZE * 6.0, -BOX_SIZE * 3.0}), BoxExplosive);
  BOX_AT_TYPE(grid, ((V2){-BOX_SIZE * 6.0, -BOX_SIZE * 5.0}), BoxExplosive);
}

void create_hard_shell_station(GameState *gs, V2 pos, enum BoxType platonic_type)
{

#define BOX_AT_TYPE(grid, pos, type)      \
  {                                       \
    Entity *box = new_entity(gs);         \
    box_create(gs, box, grid, pos);       \
    box->box_type = type;                 \
    box->indestructible = indestructible; \
  }
#define BOX_AT(grid, pos) BOX_AT_TYPE(grid, pos, BoxHullpiece)

  bool indestructible = false;
  Entity *grid = new_entity(gs);
  grid_create(gs, grid);
  entity_set_pos(grid, pos);
  entity_ensure_in_orbit(grid);
  Entity *platonic_box = new_entity(gs);
  box_create(gs, platonic_box, grid, (V2){0});
  platonic_box->box_type = platonic_type;
  platonic_box->is_platonic = true;
  BOX_AT_TYPE(grid, ((V2){BOX_SIZE * 2, 0}), BoxHullpiece);
  BOX_AT_TYPE(grid, ((V2){BOX_SIZE * 3, 0}), BoxHullpiece);
  BOX_AT_TYPE(grid, ((V2){BOX_SIZE * 4, 0}), BoxHullpiece);

  indestructible = true;
  for (float y = -BOX_SIZE * 5.0; y <= BOX_SIZE * 5.0; y += BOX_SIZE)
  {
    BOX_AT_TYPE(grid, ((V2){BOX_SIZE * 5.0, y}), BoxHullpiece);
  }
  for (float x = -BOX_SIZE * 5.0; x <= BOX_SIZE * 5.0; x += BOX_SIZE)
  {
    BOX_AT_TYPE(grid, ((V2){x, BOX_SIZE * 5.0}), BoxHullpiece);
    BOX_AT_TYPE(grid, ((V2){x, -BOX_SIZE * 5.0}), BoxHullpiece);
  }
  indestructible = false;
}
void create_initial_world(GameState *gs)
{
#ifdef DEBUG_WORLD
  Log("Creating debug world\n");
  create_bomb_station(gs, (V2){-5.0f, 0.0f}, BoxExplosive);
  create_bomb_station(gs, (V2){0.0f, 5.0f}, BoxGyroscope);
  create_hard_shell_station(gs, (V2){-5.0f, 5.0f}, BoxCloaking);
#else
  create_bomb_station(gs, (V2){-50.0f, 0.0f}, BoxExplosive);
  create_hard_shell_station(gs, (V2){0.0f, 100.0f}, BoxGyroscope);
  create_bomb_station(gs, (V2){0.0f, -100.0f}, BoxCloaking);
  create_bomb_station(gs, (V2){100.0f, 100.0f}, BoxMissileLauncher);
#endif
}

void exit_seat(GameState *gs, Entity *seat_in, Entity *p)
{
  V2 pilot_seat_exit_spot = V2add(entity_pos(seat_in), V2scale(box_facing_vector(seat_in), BOX_SIZE));
  cpBodySetPosition(p->body, v2_to_cp(pilot_seat_exit_spot));
  // cpBodySetVelocity(p->body, v2_to_cp(player_vel(gs, p)));
  cpBodySetVelocity(p->body, cpBodyGetVelocity(box_grid(seat_in)->body));
}

void process_fixed_timestep(GameState *gs)
{
  process(gs, TIMESTEP);
}

void process(GameState *gs, float dt)
{
  assert(gs->space != NULL);

  gs->time += dt;

  // process input
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
      entity_ensure_in_orbit(p);
      if (medbay != NULL)
      {
        exit_seat(gs, medbay, p);
        p->damage = 0.95f;
      }
    }
    assert(p->is_player);
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
    p->damage = 0.0f;
#endif
    // update gold win condition
    if (V2length(V2sub(cp_to_v2(cpBodyGetPosition(p->body)), gs->goldpos)) < GOLD_COLLECT_RADIUS)
    {
      p->goldness += 0.1f;
      p->damage = 0.0f;
      gs->goldpos = (V2){.x = hash11((float)gs->time) * 20.0f, .y = hash11((float)gs->time - 13.6f) * 20.0f};
    }
#if 1
    V2 world_hand_pos = get_world_hand_pos(gs, &player->input, p);
    if (player->input.seat_action)
    {
      player->input.seat_action = false; // "handle" the input
      Entity *seat_maybe_in = get_entity(gs, p->currently_inside_of_box);
      if (seat_maybe_in == NULL) // not in any seat
      {
        cpPointQueryInfo query_info = {0};
        cpShape *result = cpSpacePointQueryNearest(gs->space, v2_to_cp(world_hand_pos), 0.1f, cpShapeFilterNew(CP_NO_GROUP, CP_ALL_CATEGORIES, BOXES), &query_info);
        if (result != NULL)
        {
          Entity *potential_seat = cp_shape_entity(result);
          assert(potential_seat->is_box);

          if (potential_seat->box_type == BoxScanner) // learn everything from the scanner
          {
            player->box_unlocks |= potential_seat->blueprints_learned;
          }
          if (potential_seat->box_type == BoxCockpit || potential_seat->box_type == BoxMedbay) // @Robust check by feature flag instead of box type
          {
            // don't let players get inside of cockpits that somebody else is already inside of
            if (get_entity(gs, potential_seat->player_who_is_inside_of_me) == NULL)
            {
              p->currently_inside_of_box = get_id(gs, potential_seat);
              potential_seat->player_who_is_inside_of_me = get_id(gs, p);
              if (potential_seat->box_type == BoxMedbay)
                player->last_used_medbay = p->currently_inside_of_box;
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

    // process movement
    {
      // no cheating by making movement bigger than length 1
      V2 movement_this_tick = (V2){0};
      float rotation_this_tick = 0.0f;
      if (V2length(player->input.movement) > 0.0f)
      {
        movement_this_tick = V2scale(V2normalize(player->input.movement), clamp(V2length(player->input.movement), 0.0f, 1.0f));
        player->input.movement = (V2){0};
      }
      if (fabsf(player->input.rotation) > 0.0f)
      {
        rotation_this_tick = player->input.rotation;
        if (rotation_this_tick > 1.0f)
          rotation_this_tick = 1.0f;
        if (rotation_this_tick < -1.0f)
          rotation_this_tick = -1.0f;
        player->input.rotation = 0.0f;
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
        cpShapeSetFilter(p->shape, PLAYER_SHAPE_FILTER);
        cpBodyApplyForceAtWorldPoint(p->body, v2_to_cp(V2scale(movement_this_tick, PLAYER_JETPACK_FORCE)), cpBodyGetPosition(p->body));
        cpBodySetTorque(p->body, rotation_this_tick * PLAYER_JETPACK_TORQUE);
        p->damage += V2length(movement_this_tick) * dt * PLAYER_JETPACK_SPICE_PER_SECOND;
        p->damage += fabsf(rotation_this_tick) * dt * PLAYER_JETPACK_ROTATION_ENERGY_PER_SECOND;
      }
      else
      {
        assert(seat_inside_of->is_box);
        cpShapeSetFilter(p->shape, CP_SHAPE_FILTER_NONE); // no collisions while in a seat
        cpBodySetPosition(p->body, v2_to_cp(entity_pos(seat_inside_of)));
        cpBodySetVelocity(p->body, v2_to_cp(box_vel(seat_inside_of)));

        // share cloaking with box
        p->time_was_last_cloaked = seat_inside_of->time_was_last_cloaked;
        p->last_cloaked_by_squad = seat_inside_of->last_cloaked_by_squad;

        // set thruster thrust from movement
        if (seat_inside_of->box_type == BoxCockpit)
        {
          Entity *g = get_entity(gs, seat_inside_of->shape_parent_entity);

          V2 target_direction = {0};
          if (V2length(movement_this_tick) > 0.0f)
          {
            target_direction = V2normalize(movement_this_tick);
          }
          BOXES_ITER(gs, cur, g)
          {
            if (cur->box_type == BoxThruster)
            {

              float wanted_thrust = -V2dot(target_direction, box_facing_vector(cur));
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
      player->input.dobuild = false; // handle the input. if didn't do this, after destruction of hovered box, would try to build on its grid with grid_index...

      cpPointQueryInfo info = {0};
      V2 world_build = world_hand_pos;

      // @Robust sanitize this input so player can't build on any grid in the world
      Entity *target_grid = grid_to_build_on(gs, world_hand_pos);
      cpShape *maybe_box_to_destroy = cpSpacePointQueryNearest(gs->space, v2_to_cp(world_build), 0.01f, cpShapeFilterNew(CP_NO_GROUP, CP_ALL_CATEGORIES, BOXES), &info);
      if (maybe_box_to_destroy != NULL)
      {
        Entity *cur_box = cp_shape_entity(maybe_box_to_destroy);
        if (!cur_box->indestructible && !cur_box->is_platonic)
        {
          Entity *cur_grid = cp_body_entity(cpShapeGetBody(maybe_box_to_destroy));
          p->damage -= DAMAGE_TO_PLAYER_PER_BLOCK * ((BATTERY_CAPACITY - cur_box->energy_used) / BATTERY_CAPACITY);
          grid_remove_box(gs, cur_grid, cur_box);
        }
      }
      else if (box_unlocked(player, player->input.build_type))
      {
        // creating a box
        p->damage += DAMAGE_TO_PLAYER_PER_BLOCK;
        V2 created_box_position;
        if (p->damage < 1.0f) // player can't create a box that kills them by making it
        {
          if (target_grid == NULL)
          {
            Entity *new_grid = new_entity(gs);
            grid_create(gs, new_grid);
            entity_set_pos(new_grid, world_build);
            cpBodySetVelocity(new_grid->body, v2_to_cp(player_vel(gs, p)));
            target_grid = new_grid;
            created_box_position = (V2){0};
          }
          else
          {
            created_box_position = grid_world_to_local(target_grid, world_build);
          }
          Entity *new_box = new_entity(gs);
          box_create(gs, new_box, target_grid, created_box_position);
          new_box->owning_squad = player->squad;
          grid_correct_for_holes(gs, target_grid); // no holey ship for you!
          new_box->box_type = player->input.build_type;
          new_box->compass_rotation = player->input.build_rotation;
          if (new_box->box_type == BoxScanner)
            new_box->blueprints_learned = player->box_unlocks;
          if (new_box->box_type == BoxBattery)
            new_box->energy_used = BATTERY_CAPACITY;
        }
      }
    }
#endif
    if (p->damage >= 1.0f)
    {
      entity_destroy(gs, p);
      player->entity = (EntityID){0};
    }

    p->damage = clamp01(p->damage);
  }

  // process entities
  for (size_t i = 0; i < gs->cur_next_entity; i++)
  {
    Entity *e = &gs->entities[i];
    if (!e->exists)
      continue;

    // sun processing
    {
      cpVect pos_rel_sun = v2_to_cp(V2sub(entity_pos(e), SUN_POS));
      cpFloat sqdist = cpvlengthsq(pos_rel_sun);
      if (e->body != NULL && sqdist > (INSTANT_DEATH_DISTANCE_FROM_SUN * INSTANT_DEATH_DISTANCE_FROM_SUN))
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
          cpVect rel_to_sun = cpvsub(cpBodyGetPosition(body), v2_to_cp(SUN_POS));
          cpBodySetPosition(body, cpvadd(v2_to_cp(SUN_POS), cpvmult(cpvnormalize(rel_to_sun), INSTANT_DEATH_DISTANCE_FROM_SUN)));
        }
        else
        {
          entity_destroy(gs, e);
        }
        continue;
      }
      if (!e->is_grid) // grids aren't damaged (this edge case sucks!)
      {
        sqdist = cpvlengthsq(cpvsub(v2_to_cp(entity_pos(e)), v2_to_cp(SUN_POS)));
        if (sqdist < (SUN_RADIUS * SUN_RADIUS))
        {
          e->damage += 10.0f * dt;
        }
      }

      if (e->body != NULL)
      {
        cpVect g = cpvmult(pos_rel_sun, -sun_gravity_at_point(entity_pos(e)) / (sqdist * cpfsqrt(sqdist)));
        cpBodyUpdateVelocity(e->body, g, 1.0f, dt);
      }
    }

    if (e->is_explosion)
    {
      e->explosion_progresss += dt;
      e->explosion_pos = V2add(e->explosion_pos, V2scale(e->explosion_vel, dt));
      do_explosion(gs, e, dt);
      if (e->explosion_progresss >= EXPLOSION_TIME)
      {
        entity_destroy(gs, e);
      }
    }

    if (e->is_missile)
    {
      if (is_burning(e))
      {
        e->time_burned_for += dt;
        cpBodyApplyForceAtWorldPoint(e->body, v2_to_cp(V2rotate((V2){.x = MISSILE_BURN_FORCE, .y = 0.0f}, entity_rotation(e))), v2_to_cp(entity_pos(e)));
      }
      if (e->damage >= MISSILE_DAMAGE_THRESHOLD)
      {

        Entity *explosion = new_entity(gs);
        explosion->is_explosion = true;
        explosion->explosion_pos = entity_pos(e);
        explosion->explosion_vel = cp_to_v2(cpBodyGetVelocity(e->body));
        entity_destroy(gs, e);
      }
    }

    if (e->is_box)
    {
      if (e->is_platonic)
      {
        e->damage = 0.0f;
        gs->platonic_positions[(int)e->box_type] = entity_pos(e);
      }
      if (e->box_type == BoxExplosive && e->damage >= EXPLOSION_DAMAGE_THRESHOLD)
      {
        Entity *explosion = new_entity(gs);
        explosion->is_explosion = true;
        explosion->explosion_pos = entity_pos(e);
        explosion->explosion_vel = grid_vel(box_grid(e));
        if (!e->is_platonic)
          grid_remove_box(gs, get_entity(gs, e->shape_parent_entity), e);
      }
      if (e->damage >= 1.0f)
      {
        grid_remove_box(gs, get_entity(gs, e->shape_parent_entity), e);
      }
    }
    if (e->is_grid)
    {
      Entity *grid = e;
      // calculate how much energy solar panels provide
      float energy_to_add = 0.0f;
      BOXES_ITER(gs, cur_box, grid)
      {
        if (cur_box->box_type == BoxSolarPanel)
        {
          cur_box->sun_amount = clamp01(V2dot(box_facing_vector(cur_box), V2normalize(V2sub(SUN_POS, entity_pos(cur_box)))));

          // less sun the farther away you are!
          cur_box->sun_amount *= lerp(1.0f, 0.0f, clamp01(V2length(V2sub(entity_pos(cur_box), SUN_POS)) / SUN_NO_MORE_ELECTRICITY_OR_GRAVITY));
          energy_to_add += cur_box->sun_amount * SOLAR_ENERGY_PER_SECOND * dt;
        }
      }

      // apply all of the energy to all connected batteries
      BOXES_ITER(gs, cur, grid)
      {
        if (energy_to_add <= 0.0f)
          break;
        if (cur->box_type == BoxBattery)
        {
          float energy_sucked_up_by_battery = cur->energy_used < energy_to_add ? cur->energy_used : energy_to_add;
          cur->energy_used -= energy_sucked_up_by_battery;
          energy_to_add -= energy_sucked_up_by_battery;
        }
        assert(energy_to_add >= 0.0f);
      }

      // any energy_to_add existing now can also be used to power thrusters/medbay
      float non_battery_energy_left_over = energy_to_add;

      // use the energy, stored in the batteries, in various boxes
      BOXES_ITER(gs, cur_box, grid)
      {
        if (cur_box->box_type == BoxThruster)
        {

          float energy_to_consume = cur_box->wanted_thrust * THRUSTER_ENERGY_USED_PER_SECOND * dt;
          if (energy_to_consume > 0.0f)
          {
            cur_box->thrust = 0.0f;
            float energy_unconsumed = batteries_use_energy(gs, grid, &non_battery_energy_left_over, energy_to_consume);
            cur_box->thrust = (1.0f - energy_unconsumed / energy_to_consume) * cur_box->wanted_thrust;
            if (cur_box->thrust >= 0.0f)
              cpBodyApplyForceAtWorldPoint(grid->body, v2_to_cp(thruster_force(cur_box)), v2_to_cp(entity_pos(cur_box)));
          }
        }
        if (cur_box->box_type == BoxGyroscope)
        {
          float energy_to_consume = fabsf(cur_box->wanted_thrust * GYROSCOPE_ENERGY_USED_PER_SECOND * dt);
          if (energy_to_consume > 0.0f)
          {
            cur_box->thrust = 0.0f;
            float energy_unconsumed = batteries_use_energy(gs, grid, &non_battery_energy_left_over, energy_to_consume);
            cur_box->thrust = (1.0f - energy_unconsumed / energy_to_consume) * cur_box->wanted_thrust;
            if (fabsf(cur_box->thrust) >= 0.0f)
              cpBodySetTorque(grid->body, cpBodyGetTorque(grid->body) + cur_box->thrust * GYROSCOPE_TORQUE);
          }
        }
        if (cur_box->box_type == BoxMedbay)
        {
          Entity *potential_meatbag_to_heal = get_entity(gs, cur_box->player_who_is_inside_of_me);
          if (potential_meatbag_to_heal != NULL)
          {
            float wanted_energy_use = fminf(potential_meatbag_to_heal->damage, PLAYER_ENERGY_RECHARGE_PER_SECOND * dt);
            if (wanted_energy_use > 0.0f)
            {
              float energy_unconsumed = batteries_use_energy(gs, grid, &non_battery_energy_left_over, wanted_energy_use);
              potential_meatbag_to_heal->damage -= (1.0f - energy_unconsumed / wanted_energy_use) * wanted_energy_use;
            }
          }
        }
        if (cur_box->box_type == BoxCloaking)
        {
          float energy_unconsumed = batteries_use_energy(gs, grid, &non_battery_energy_left_over, CLOAKING_ENERGY_USE * dt);
          if (energy_unconsumed >= CLOAKING_ENERGY_USE * dt)
          {
            cur_box->cloaking_power = lerp(cur_box->cloaking_power, 0.0, dt * 3.0f);
          }
          else
          {
            cur_box->cloaking_power = lerp(cur_box->cloaking_power, 1.0, dt * 3.0f);
            cpBody *tmp = cpBodyNew(0.0, 0.0);
            cpBodySetPosition(tmp, v2_to_cp(entity_pos(cur_box)));
            cpBodySetAngle(tmp, entity_rotation(cur_box));
            // subtract a little from the panel size so that boxes just at the boundary of the panel
            // aren't (sometimes cloaked)/(sometimes not) from floating point imprecision
            cpShape *box_shape = cpBoxShapeNew(tmp, CLOAKING_PANEL_SIZE - 0.03f, CLOAKING_PANEL_SIZE - 0.03f, 0.0);
            cpSpaceShapeQuery(gs->space, box_shape, cloaking_shield_callback_func, (void *)cur_box);
            cpShapeFree(box_shape);
            cpBodyFree(tmp);
          }
        }
        if (cur_box->box_type == BoxMissileLauncher)
        {
          LauncherTarget target = missile_launcher_target(gs, cur_box);

          if (cur_box->missile_construction_charge < 1.0f)
          {
            float want_use_energy = dt * MISSILE_CHARGE_RATE;
            float energy_charged = want_use_energy - batteries_use_energy(gs, grid, &non_battery_energy_left_over, want_use_energy);
            cur_box->missile_construction_charge += energy_charged;
          }

          if (target.target_found && cur_box->missile_construction_charge >= 1.0f)
          {
            cur_box->missile_construction_charge = 0.0f;
            Entity *new_missile = new_entity(gs);
            create_missile(gs, new_missile);
            new_missile->owning_squad = cur_box->owning_squad; // missiles have teams and attack eachother!
            float missile_spawn_dist = sqrtf((BOX_SIZE / 2.0f) * (BOX_SIZE / 2.0f) * 2.0f) + MISSILE_COLLIDER_SIZE.x / 2.0f + 0.1f;
            cpBodySetPosition(new_missile->body, v2_to_cp(V2add(entity_pos(cur_box), V2rotate((V2){.x = missile_spawn_dist, 0.0f}, target.facing_angle))));
            cpBodySetAngle(new_missile->body, target.facing_angle);
            cpBodySetVelocity(new_missile->body, v2_to_cp(box_vel(cur_box)));
          }
        }
        if (cur_box->box_type == BoxScanner)
        {
          // set the nearest platonic solid! only on server as only the server sees everything
          if (gs->server_side_computing)
          {
            float energy_unconsumed = batteries_use_energy(gs, grid, &non_battery_energy_left_over, SCANNER_ENERGY_USE * dt);
            if (energy_unconsumed >= SCANNER_ENERGY_USE * dt)
            {
              cur_box->platonic_detection_strength = 0.0f;
              cur_box->platonic_nearest_direction = (V2){0};
            }
            else
            {
              V2 from_pos = entity_pos(cur_box);
              V2 nearest = {0};
              float nearest_dist = INFINITY;
              for (int i = 0; i < MAX_BOX_TYPES; i++)
              {
                V2 cur_pos = gs->platonic_positions[i];
                if (V2length(cur_pos) > 0.0f) // zero is uninitialized, the platonic solid doesn't exist (probably) @Robust do better
                {
                  float length_to_cur = V2dist(from_pos, cur_pos);
                  if (length_to_cur < nearest_dist)
                  {
                    nearest_dist = length_to_cur;
                    nearest = cur_pos;
                  }
                }
              }
              if (nearest_dist < INFINITY)
              {
                cur_box->platonic_nearest_direction = V2normalize(V2sub(nearest, from_pos));
                cur_box->platonic_detection_strength = fmaxf(0.1f, 1.0f - fminf(1.0f, nearest_dist / 100.0f));
              }
              else
              {
                cur_box->platonic_nearest_direction = (V2){0};
                cur_box->platonic_detection_strength = 0.0f;
              }
            }
          }

          // unlock the nearest platonic solid!
          scanner_has_learned = cur_box->blueprints_learned;
          Entity *to_learn = closest_box_to_point_in_radius(gs, entity_pos(cur_box), SCANNER_RADIUS, scanner_filter);
          if (to_learn != NULL)
            assert(to_learn->is_box);

          EntityID new_id = get_id(gs, to_learn);

          if (!entityids_same(cur_box->currently_scanning, new_id))
          {
            cur_box->currently_scanning_progress = 0.0f;
            cur_box->currently_scanning = new_id;
          }

          float target_head_rotate_speed = cur_box->platonic_detection_strength > 0.0f ? 3.0f : 0.0f;
          if (to_learn != NULL)
          {
            cur_box->currently_scanning_progress += dt * SCANNER_SCAN_RATE;
            target_head_rotate_speed *= 30.0f * cur_box->currently_scanning_progress;
          }
          else
            cur_box->currently_scanning_progress = 0.0f;

          if (cur_box->currently_scanning_progress >= 1.0f)
          {
            cur_box->blueprints_learned |= box_unlock_number(to_learn->box_type);
          }

          cur_box->scanner_head_rotate_speed = lerp(cur_box->scanner_head_rotate_speed, target_head_rotate_speed, dt * 3.0f);
          cur_box->scanner_head_rotate += cur_box->scanner_head_rotate_speed * dt;
          cur_box->scanner_head_rotate = fmodf(cur_box->scanner_head_rotate, 2.0f * PI);
        }
      }
    }
  }

  cpSpaceStep(gs->space, dt);
}
