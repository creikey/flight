#include <chipmunk.h>
#include "types.h"

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

void __assert(bool cond, const char *file, int line, const char *cond_string)
{
    if (!cond)
    {
        fprintf(stderr, "%s:%d | Assertion %s failed\n", file, line, cond_string);
    }
}

#define assert(condition) __assert(condition, __FILE__, __LINE__, #condition)

static V2 cp_to_v2(cpVect v)
{
    return (V2){.x = v.x, .y = v.y};
}

static cpVect v2_to_cp(V2 v)
{
    return cpv(v.x, v.y);
}

bool was_entity_deleted(struct GameState *gs, EntityID id)
{
    if(id.generation == 0) return false; // generation 0 means null entity ID, not a deleted entity
    Entity *the_entity = &gs->entities[id.index];
    return (!the_entity->exists || the_entity->generation != id.generation);
}

// may return null if it doesn't exist anymore
Entity *get_entity(struct GameState *gs, EntityID id)
{
    if (id.generation == 0)
    {
        return NULL;
    }
    assert(id.index < gs->cur_next_entity);
    assert(id.index < gs->max_entities);
    Entity *to_return = &gs->entities[id.index];
    if (was_entity_deleted(gs, id))
        return NULL;
    return to_return;
}

EntityID get_id(struct GameState *gs, Entity *e)
{
    if (e == NULL)
        return (EntityID){0};

    int index = e - gs->entities;
    assert(index >= 0);
    assert(index < gs->cur_next_entity);

    return (EntityID){
        .generation = e->generation,
        .index = index,
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

static struct GameState *cp_space_gs(cpSpace *space)
{
    return (struct GameState *)cpSpaceGetUserData(space);
}

int grid_num_boxes(struct GameState *gs, Entity *e)
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

void on_entity_child_shape(cpBody* body, cpShape* shape, void* data);
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
    if (e->body != NULL)
    {
        cpBodyEachShape(e->body, on_entity_child_shape, (void*)gs);
        cpSpaceRemoveBody(gs->space, e->body);
        cpBodyFree(e->body);
        e->body = NULL;
    }
    e->body = NULL;
    e->shape = NULL;

    Entity *front_of_free_list = get_entity(gs, gs->free_list);
    if (front_of_free_list != NULL)
        assert(!front_of_free_list->exists);
    int gen = e->generation;
    *e = (Entity){0};
    e->generation = gen;
    e->next_free_entity = gs->free_list;
    gs->free_list = get_id(gs, e);
}

void on_entity_child_shape(cpBody* body, cpShape* shape, void* data)
{
    entity_destroy((GameState*)data, cp_shape_entity(shape));
}

Entity *new_entity(struct GameState *gs)
{
    Entity *to_return = NULL;
    if (get_entity(gs, gs->free_list) != NULL)
    {
        to_return = get_entity(gs, gs->free_list);
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

void create_body(struct GameState *gs, Entity *e)
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

void grid_create(struct GameState *gs, Entity *e)
{
    e->is_grid = true;
    create_body(gs, e);
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

void create_player(struct GameState *gs, Entity *e)
{
    e->is_player = true;
    create_body(gs, e);
    create_rectangle_shape(gs, e, e, (V2){0}, V2scale(PLAYER_SIZE, 0.5f), PLAYER_MASS);
    cpShapeSetFilter(e->shape, cpShapeFilterNew(CP_NO_GROUP, PLAYERS, CP_ALL_CATEGORIES));
}

// box must be passed as a parameter as the box added to chipmunk uses this pointer in its
// user data. pos is in local coordinates. Adds the box to the grid's chain of boxes
void box_create(struct GameState *gs, Entity *new_box, Entity *grid, V2 pos)
{
    new_box->is_box = true;
    assert(gs->space != NULL);
    assert(grid->is_grid);

    float halfbox = BOX_SIZE / 2.0f;

    create_rectangle_shape(gs, new_box, grid, pos, (V2){halfbox, halfbox}, 1.0f);

    cpShapeSetFilter(new_box->shape, cpShapeFilterNew(CP_NO_GROUP, BOXES, CP_ALL_CATEGORIES));

    new_box->next_box = get_id(gs, get_entity(gs, grid->boxes));
    new_box->prev_box = get_id(gs, grid);
    if (get_entity(gs, new_box->next_box) != NULL)
    {
        get_entity(gs, new_box->next_box)->prev_box = get_id(gs, new_box);
    }
    grid->boxes = get_id(gs, new_box);
}

// removes boxes from grid, then ensures that the rule that grids must not have
// holes in them is applied.
static void grid_remove_box(struct GameState *gs, struct Entity *grid, struct Entity *box)
{
    assert(grid->is_grid);
    assert(box->is_box);
    entity_destroy(gs, box);

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
                        (V2){.x = -1.0f, .y = 0.0f},
                        (V2){.x = 1.0f, .y = 0.0f},
                        (V2){.x = 0.0f, .y = 1.0f},
                        (V2){.x = 0.0f, .y = -1.0f},
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
                            if (V2cmp(entity_shape_pos(cur), wanted_local_pos, 0.01f))
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
        cpBodySetAngularVelocity(new_grid->body, grid_angular_velocity(grid));
    }
}

static void postStepRemove(cpSpace *space, void *key, void *data)
{
    cpShape *b = (cpShape *)key;
    Entity *e = cp_shape_entity(b);
    // @Robust why not just do these deletions in the update loop? save on a lot of complexity
    if (e->damage > 1.0f)
    {
        if (e->is_box)
            grid_remove_box(cp_space_gs(space), cp_body_entity(cpShapeGetBody(b)), e);
    }
}

static cpBool on_damage(cpArbiter *arb, cpSpace *space, cpDataPointer userData)
{
    cpShape *a, *b;
    cpArbiterGetShapes(arb, &a, &b);

    Entity *entity_a, *entity_b;
    entity_a = cp_shape_entity(a);
    entity_b = cp_shape_entity(b);

    float damage = V2length(cp_to_v2(cpArbiterTotalImpulse(arb))) * 0.25f;
    if (damage > 0.05f)
    {
        // Log("Collision with damage %f\n", damage);
        entity_a->damage += damage;
        entity_b->damage += damage;
    }

    // b must be the key passed into the post step removed, the key is cast into its shape
    cpSpaceAddPostStepCallback(space, (cpPostStepFunc)postStepRemove, b, NULL);
    cpSpaceAddPostStepCallback(space, (cpPostStepFunc)postStepRemove, a, NULL);

    return true; // keep colliding
}

void initialize(struct GameState *gs, void *entity_arena, int entity_arena_size)
{
    *gs = (struct GameState){0};
    memset(entity_arena, 0, entity_arena_size); // SUPER critical. Random vals in the entity data causes big problem
    gs->entities = (Entity *)entity_arena;
    gs->max_entities = entity_arena_size / sizeof(Entity);
    gs->space = cpSpaceNew();
    cpSpaceSetUserData(gs->space, (cpDataPointer)gs);                          // needed in the handler
    cpCollisionHandler *handler = cpSpaceAddCollisionHandler(gs->space, 0, 0); // @Robust limit collision type to just blocks that can be damaged
    handler->postSolveFunc = on_damage;
}
void destroy(struct GameState *gs)
{
    // can't zero out gs data because the entity memory arena is reused
    // on deserialization
    for (int i = 0; i < gs->max_entities; i++)
    {
        if (gs->entities[i].exists)
            entity_destroy(gs, &gs->entities[i]);
    }
    cpSpaceFree(gs->space);
    gs->space = NULL;
    gs->cur_next_entity = 0;
}
// center of mass, not the literal position
V2 grid_com(Entity *grid)
{
    return cp_to_v2(cpBodyLocalToWorld(grid->body, cpBodyGetCenterOfGravity(grid->body)));
}

V2 entity_pos(Entity *grid)
{
    return cp_to_v2(cpBodyGetPosition(grid->body));
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
float entity_rotation(Entity *grid)
{
    return cpBodyGetAngle(grid->body);
}
float grid_angular_velocity(Entity *grid)
{
    return cpBodyGetAngularVelocity(grid->body);
}
Entity *box_grid(Entity *box)
{
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
    return cpShapeGetMass(box->shape);
}
V2 box_pos(Entity *box)
{
    assert(box->is_box);
    return V2add(entity_pos(box_grid(box)), V2rotate(entity_shape_pos(box), entity_rotation(box_grid(box))));
}
float box_rotation(Entity *box)
{
    return cpBodyGetAngle(cpShapeGetBody(box->shape));
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

#define WRITE_VARNAMES true // debugging feature horrible for network
typedef struct SerState
{
    char *bytes;
    bool serializing;
    int cursor; // points to next available byte, is the size of current message after serializing something
    int max_size;
} SerState;
#define SER_VAR_NAME(var_pointer, name)                                                                                      \
    {                                                                                                             \
        const char *var_name = name;                                                                      \
        size_t var_name_len = 0;                                                                                  \
        if (WRITE_VARNAMES)                                                                                       \
        {                                                                                                         \
            var_name_len = strlen(var_name);                                                                      \
        }                                                                                                         \
        if (ser->serializing)                                                                                     \
        {                                                                                                         \
            if (WRITE_VARNAMES)                                                                                   \
            {                                                                                                     \
                memcpy(ser->bytes + ser->cursor, var_name, var_name_len);                                         \
                ser->cursor += var_name_len;                                                                      \
            }                                                                                                     \
            for (int b = 0; b < sizeof(*var_pointer); b++)                                                        \
            {                                                                                                     \
                ser->bytes[ser->cursor] = ((char *)var_pointer)[b];                                               \
                ser->cursor += 1;                                                                                 \
                assert(ser->cursor < ser->max_size);                                                              \
            }                                                                                                     \
        }                                                                                                         \
        else                                                                                                      \
        {                                                                                                         \
            if (WRITE_VARNAMES)                                                                                   \
            {                                                                                                     \
                char *read_name = malloc(sizeof *read_name * (var_name_len + 1));                                 \
                for (int i = 0; i < var_name_len; i++)                                                            \
                {                                                                                                 \
                    read_name[i] = ser->bytes[ser->cursor];                                                       \
                    ser->cursor += 1;                                                                             \
                    assert(ser->cursor < ser->max_size);                                                          \
                }                                                                                                 \
                read_name[var_name_len] = '\0';                                                                   \
                if (strcmp(read_name, var_name) != 0)                                                             \
                {                                                                                                 \
                    printf("%s:%d | Expected variable %s but got %s\n", __FILE__, __LINE__, var_name, read_name); \
                }                                                                                                 \
                free(read_name);                                                                                  \
            }                                                                                                     \
            for (int b = 0; b < sizeof(*var_pointer); b++)                                                        \
            {                                                                                                     \
                ((char *)var_pointer)[b] = ser->bytes[ser->cursor];                                               \
                ser->cursor += 1;                                                                                 \
                assert(ser->cursor < ser->max_size);                                                              \
            }                                                                                                     \
        }                                                                                                         \
    }
#define SER_VAR(var_pointer) SER_VAR_NAME(var_pointer, #var_pointer)

void ser_V2(SerState *ser, V2 *var)
{
    SER_VAR(&var->x);
    SER_VAR(&var->y);
}

void ser_bodydata(SerState *ser, struct BodyData *data)
{
    ser_V2(ser, &data->pos);
    ser_V2(ser, &data->vel);
    SER_VAR(&data->rotation);
    SER_VAR(&data->angular_velocity);
}

void ser_entityid(SerState *ser, EntityID *id)
{
    SER_VAR(&id->generation);
    SER_VAR(&id->index);
}

void ser_inputframe(SerState *ser, struct InputFrame *i)
{
    SER_VAR(&i->movement);
    SER_VAR(&i->inhabit);
    SER_VAR(&i->build);
    SER_VAR(&i->dobuild);
    SER_VAR(&i->build_type);
    SER_VAR(&i->build_rotation);
    ser_entityid(ser, &i->grid_to_build_on);
}

void ser_player(SerState *ser, struct Player *p)
{
    SER_VAR(&p->connected);
    if (p->connected)
    {
        ser_entityid(ser, &p->entity);
        ser_inputframe(ser, &p->input);
    }
}

void ser_entity(SerState *ser, struct GameState *gs, Entity *e)
{
    SER_VAR(&e->generation);
    SER_VAR(&e->damage);

    bool has_body = ser->serializing && e->body != NULL;
    SER_VAR(&has_body);

    if (has_body)
    {
        struct BodyData body_data;
        if (ser->serializing)
            populate(e->body, &body_data);
        ser_bodydata(ser, &body_data);
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
        SER_VAR(&e->shape_size);
        ser_entityid(ser, &e->shape_parent_entity);


        V2 shape_pos;
        if (ser->serializing)
            shape_pos = entity_shape_pos(e);
        SER_VAR(&shape_pos);

        float shape_mass;
        if (ser->serializing)
            shape_mass = entity_shape_mass(e);
        SER_VAR(&shape_mass)

        Entity * parent = get_entity(gs, e->shape_parent_entity);
        if(parent == NULL)
        {
            printf("Null shape parent\n");
        }
        if (!ser->serializing)
        {
            create_rectangle_shape(gs, e, parent , shape_pos, e->shape_size, shape_mass);
        }
    }

    SER_VAR(&e->is_player);
    if (e->is_player)
    {
        ser_entityid(ser, &e->currently_piloting_seat);
        SER_VAR(&e->spice_taken_away);
        SER_VAR(&e->goldness);
    }

    SER_VAR(&e->is_grid);
    if (e->is_grid)
    {
        SER_VAR(&e->total_energy_capacity);
        ser_entityid(ser, &e->boxes);
    }

    SER_VAR(&e->is_box);
    if (e->is_box)
    {
        SER_VAR(&e->box_type);
        ser_entityid(ser, &e->next_box);
        ser_entityid(ser, &e->prev_box);
        SER_VAR(&e->compass_rotation);
        SER_VAR(&e->thrust);
        SER_VAR(&e->energy_used);
    }
}

void ser_server_to_client(SerState *ser, ServerToClient *s)
{
    struct GameState *gs = s->cur_gs;

    int cur_next_entity = 0;
    if (ser->serializing)
        cur_next_entity = gs->cur_next_entity;
    SER_VAR(&cur_next_entity);

    if (!ser->serializing)
    {
        destroy(gs);
        memset((void *)gs->entities, 0, sizeof(*gs->entities) * gs->max_entities);
        initialize(gs, gs->entities, gs->max_entities * sizeof(*gs->entities));
        gs->cur_next_entity = cur_next_entity;
    }

    SER_VAR(&s->your_player);
    SER_VAR(&gs->time);

    ser_V2(ser, &gs->goldpos);

    for (int i = 0; i < MAX_PLAYERS; i++)
    {
        ser_player(ser, &gs->players[i]);
    }

    if (ser->serializing)
    {
        for (int i = 0; i < gs->cur_next_entity; i++)
        {
            Entity *e = &gs->entities[i];
            if (e->exists)
            {
                if(e->is_player)
                {
                    SER_VAR(&i);
                    ser_entity(ser, gs, e);
                }
                else if(e->is_grid)
                {
                    // serialize boxes always after bodies, so that by the time the boxes
                    // are loaded in the parent body is loaded in and can be referenced.
                    SER_VAR(&i);
                    ser_entity(ser, gs, e);
                    BOXES_ITER(gs, cur, e)
                    {
                        EntityID cur_id = get_id(gs, cur);
                        SER_VAR_NAME(&cur_id.index, "&i");
                        ser_entity(ser, gs, cur);
                    }
                }
            }
        }
        int end_of_entities = -1;
        SER_VAR_NAME(&end_of_entities, "&i");
    }
    else
    {
        while (true)
        {
            int next_index;
            SER_VAR_NAME(&next_index, "&i");
            if (next_index == -1)
                break;
            assert(next_index < gs->max_entities);
            Entity *e = &gs->entities[next_index];
            e->exists = true;
            ser_entity(ser, gs, e);
            gs->cur_next_entity = max(gs->cur_next_entity, next_index + 1);
        }
        for (int i = 0; i < gs->cur_next_entity; i++)
        {
            Entity *e = &gs->entities[i];
            if (!e->exists)
            {
                e->next_free_entity = gs->free_list;
                gs->free_list = get_id(gs, e);
            }
        }
    }
}

void into_bytes(struct ServerToClient *msg, char *bytes, int *out_len, int max_len)
{
    assert(msg->cur_gs != NULL);
    assert(msg != NULL);

    SerState ser = (SerState){
        .bytes = bytes,
        .serializing = true,
        .cursor = 0,
        .max_size = max_len,
    };

    ser_server_to_client(&ser, msg);
    *out_len = ser.cursor + 1; // @Robust not sure why I need to add one to cursor, ser.cursor should be the length..
}

void from_bytes(struct ServerToClient *msg, char *bytes, int max_len)
{
    assert(msg->cur_gs != NULL);
    assert(msg != NULL);

    SerState ser = (SerState){
        .bytes = bytes,
        .serializing = false,
        .cursor = 0,
        .max_size = max_len,
    };

    ser_server_to_client(&ser, msg);
}

// has to be global var because can only get this information
static cpShape *closest_to_point_in_radius_result = NULL;
static float closest_to_point_in_radius_result_largest_dist = 0.0f;
static void closest_point_callback_func(cpShape *shape, cpContactPointSet *points, void *data)
{
    assert(points->count == 1);
    if (!cp_shape_entity(shape)->is_box)
        return;
    float dist = V2length(cp_to_v2(cpvsub(points->points[0].pointA, points->points[0].pointB)));
    // float dist = -points->points[0].distance;
    if (dist > closest_to_point_in_radius_result_largest_dist)
    {
        closest_to_point_in_radius_result_largest_dist = dist;
        closest_to_point_in_radius_result = shape;
    }
}

Entity *closest_to_point_in_radius(struct GameState *gs, V2 point, float radius)
{
    closest_to_point_in_radius_result = NULL;
    closest_to_point_in_radius_result_largest_dist = 0.0f;

    cpBody *tmpbody = cpBodyNew(0.0f, 0.0f);
    cpShape *circle = cpCircleShapeNew(tmpbody, radius, v2_to_cp(point));
    cpSpaceShapeQuery(gs->space, circle, closest_point_callback_func, NULL);

    cpShapeFree(circle);
    cpBodyFree(tmpbody);

    if (closest_to_point_in_radius_result != NULL)
    {
        // @Robust query here for only boxes that are part of ships, could get nasty...
        return cp_body_entity(cpShapeGetBody(closest_to_point_in_radius_result));
    }

    return NULL;
}

V2 thruster_direction(Entity *box)
{
    assert(box->is_box && box->box_type == BoxThruster);
    V2 to_return = (V2){.x = 1.0f, .y = 0.0f};

    to_return = V2rotate(to_return, rotangle(box->compass_rotation));
    to_return = V2rotate(to_return, box_rotation(box));

    return to_return;
}

V2 thruster_force(Entity *box)
{
    return V2scale(thruster_direction(box), -box->thrust * THRUSTER_FORCE);
}

uint64_t tick(struct GameState *gs)
{
    return (uint64_t)floor(gs->time / ((double)TIMESTEP));
}

void process(struct GameState *gs, float dt)
{
    assert(gs->space != NULL);

    assert(dt == TIMESTEP); // @TODO fix tick being incremented every time
    gs->time += dt;

    // process input
    for (int i = 0; i < MAX_PLAYERS; i++)
    {
        struct Player *player = &gs->players[i];
        if (!player->connected)
            continue;
        Entity *p = get_entity(gs, player->entity);
        if (p == NULL)
        {
            p = new_entity(gs);
            create_player(gs, p);
            player->entity = get_id(gs, p);
        }
        assert(p->is_player);

        // update gold win condition
        if (V2length(V2sub(cp_to_v2(cpBodyGetPosition(p->body)), gs->goldpos)) < GOLD_COLLECT_RADIUS)
        {
            p->goldness += 0.1;
            p->spice_taken_away = 0.0f;
            gs->goldpos = (V2){.x = hash11(gs->time) * 20.0f, .y = hash11(gs->time - 13.6f) * 20.0f};
        }

        if (get_entity(gs, p->currently_piloting_seat) == NULL)
        {
            p->currently_piloting_seat = (EntityID){0};
        }

// @Todo do getting inside pilot seat
#if 0
        if (p->input.inhabit)
        {
            p->input.inhabit = false; // "handle" the input
            if (p->currently_inhabiting_index == -1)
            {

                // @Robust mask to only ship boxes of things the player can inhabit
                cpPointQueryInfo query_info = {0};
                cpShape *result = cpSpacePointQueryNearest(gs->space, v2_to_cp(p->pos), 0.1f, cpShapeFilterNew(CP_NO_GROUP, CP_ALL_CATEGORIES, CP_ALL_CATEGORIES), &query_info);
                if (result != NULL)
                {
                    // result is assumed to be a box shape
                    struct Grid *g = (struct Grid *)cpBodyGetUserData(cpShapeGetBody(result));
                    int ship_to_inhabit = -1;
                    for (int ii = 0; ii < MAX_GRIDS; ii++)
                    {
                        SKIPNULL(gs->grids[ii].body);
                        if (&gs->grids[ii] == g)
                        {
                            ship_to_inhabit = ii;
                            break;
                        }
                    }

                    // don't allow inhabiting a grid that's already inhabited
                    for (int ii = 0; ii < MAX_PLAYERS; ii++)
                    {
                        if (gs->players[ii].currently_inhabiting_index == ship_to_inhabit)
                        {
                            Log("Attempted to inhabit already taken ship\n");
                            ship_to_inhabit = -1;
                        }
                    }

                    if (ship_to_inhabit == -1)
                    {
                        Log("Couldn't find ship to inhabit even though point collision returned something\n");
                    }
                    else
                    {
                        p->currently_inhabiting_index = ship_to_inhabit;
                    }
                }
                else
                {
                    Log("No ship above player at point %f %f\n", p->pos.x, p->pos.y);
                }
            }
            else
            {
                p->vel = grid_vel(&gs->grids[p->currently_inhabiting_index]);
                p->currently_inhabiting_index = -1;
            }
        }
#endif

        // process movement
        {
            // no cheating by making movement bigger than length 1
            float movement_strength = V2length(player->input.movement);
            if (movement_strength != 0.0f)
            {
                player->input.movement = V2scale(V2normalize(player->input.movement), clamp(V2length(player->input.movement), 0.0f, 1.0f));
            }
            cpBodyApplyForceAtWorldPoint(p->body, v2_to_cp(V2scale(player->input.movement, PLAYER_JETPACK_FORCE)), cpBodyGetPosition(p->body));
            p->spice_taken_away += movement_strength * dt * PLAYER_JETPACK_SPICE_PER_SECOND;
// @Todo do pilot seat
#if 0
            {
                struct Grid *g = &gs->grids[p->currently_inhabiting_index];
                V2 target_new_pos = V2lerp(p->pos, grid_com(g), dt * 20.0f);
                p->vel = V2scale(V2sub(target_new_pos, p->pos), 1.0f / dt); // set vel correctly so newly built grids have the correct velocity copied from it

                // set thruster thrust from movement
                {
                    float energy_available = g->total_energy_capacity;

                    V2 target_direction = {0};
                    if (V2length(p->input.movement) > 0.0f)
                    {
                        target_direction = V2normalize(p->input.movement);
                    }
                    for (int ii = 0; ii < MAX_BOXES_PER_GRID; ii++)
                    {
                        SKIPNULL(g->boxes[ii].shape);
                        if (g->boxes[ii].type != BoxThruster)
                            continue;

                        float wanted_thrust = -V2dot(target_direction, thruster_direction(&g->boxes[ii]));
                        wanted_thrust = clamp01(wanted_thrust);

                        float needed_energy = wanted_thrust * THRUSTER_ENERGY_USED_PER_SECOND * dt;
                        energy_available -= needed_energy;

                        if (energy_available > 0.0f)
                            g->boxes[ii].thrust = wanted_thrust;
                        else
                            g->boxes[ii].thrust = 0.0f;
                    }
                }
                // cpBodyApplyForceAtWorldPoint(g->body, v2_to_cp(V2scale(p->input.movement, 5.0f)), v2_to_cp(grid_com(g)));
                // bigger the ship, the more efficient the spice usage
            }
#endif
        }

#if 1 // building 
        if (player->input.dobuild)
        {
            player->input.dobuild = false; // handle the input. if didn't do this, after destruction of hovered box, would try to build on its grid with grid_index...

            cpPointQueryInfo info = {0};
            V2 world_build = player->input.build;

            // @Robust sanitize this input so player can't build on any grid in the world
            Entity *target_grid = get_entity(gs, player->input.grid_to_build_on);
            if (target_grid != NULL)
            {
                world_build = grid_local_to_world(target_grid, player->input.build);
            }
            cpShape *nearest = cpSpacePointQueryNearest(gs->space, v2_to_cp(world_build), 0.01f, cpShapeFilterNew(CP_NO_GROUP, CP_ALL_CATEGORIES, BOXES), &info);
            if (nearest != NULL)
            {
                Entity *cur_box = cp_shape_entity(nearest);
                Entity *cur_grid = cp_body_entity(cpShapeGetBody(nearest));
                grid_remove_box(gs, cur_grid, cur_box);
                p->spice_taken_away -= 0.1f;
            }
            else if (target_grid == NULL)
            {
                Entity *new_grid = new_entity(gs);
                grid_create(gs, new_grid);
                p->spice_taken_away += 0.1f;
                entity_set_pos(new_grid, world_build);

                Entity *new_box = new_entity(gs);
                box_create(gs, new_box, new_grid, (V2){0});
                new_box->box_type = player->input.build_type;
                new_box->compass_rotation = player->input.build_rotation;
                cpBodySetVelocity(new_grid->body, cpBodyGetVelocity(p->body));
            }
            else
            {
                Entity *new_box = new_entity(gs);
                box_create(gs, new_box, target_grid, grid_world_to_local(target_grid, world_build));
                new_box->box_type = player->input.build_type;
                new_box->compass_rotation = player->input.build_rotation;
                p->spice_taken_away += 0.1f;
            }
        }
#endif
        if (p->spice_taken_away >= 1.0f)
        {
            entity_destroy(gs, p);
            player->entity = (EntityID){0};
        }

        p->spice_taken_away = clamp01(p->spice_taken_away);
    }

// @Todo add thrust from thruster blocks
#if 0
    for (int i = 0; i < MAX_GRIDS; i++)
    {
        SKIPNULL(gs->grids[i].body);

        struct Box *batteries[MAX_BOXES_PER_GRID] = {0};
        int cur_battery = 0;
        for (int ii = 0; ii < MAX_BOXES_PER_GRID; ii++)
        {
            SKIPNULL(gs->grids[i].boxes[ii].shape);
            if (gs->grids[i].boxes[ii].type == BoxBattery)
            {
                assert(cur_battery < MAX_BOXES_PER_GRID);
                batteries[cur_battery] = &gs->grids[i].boxes[ii];
                cur_battery++;
            }
        }
        int batteries_len = cur_battery;

        float thruster_energy_consumption_per_second = 0.0f;
        for (int ii = 0; ii < MAX_BOXES_PER_GRID; ii++)
        {
            SKIPNULL(gs->grids[i].boxes[ii].shape);
            if (gs->grids[i].boxes[ii].type == BoxThruster)
            {
                float energy_to_consume = gs->grids[i].boxes[ii].thrust * THRUSTER_ENERGY_USED_PER_SECOND * dt;
                struct Box *max_capacity_battery = NULL;
                float max_capacity_battery_energy_used = 1.0f;
                for (int iii = 0; iii < batteries_len; iii++)
                {
                    if (batteries[iii]->energy_used < max_capacity_battery_energy_used)
                    {
                        max_capacity_battery = batteries[iii];
                        max_capacity_battery_energy_used = batteries[iii]->energy_used;
                    }
                }

                if (max_capacity_battery != NULL && (1.0f - max_capacity_battery->energy_used) > energy_to_consume)
                {
                    max_capacity_battery->energy_used += energy_to_consume;
                    cpBodyApplyForceAtWorldPoint(gs->grids[i].body, v2_to_cp(thruster_force(&gs->grids[i].boxes[ii])), v2_to_cp(box_pos(&gs->grids[i].boxes[ii])));
                }
            }
        }

        gs->grids[i].total_energy_capacity = 0.0f;
        for (int ii = 0; ii < batteries_len; ii++)
        {
            gs->grids[i].total_energy_capacity += 1.0f - batteries[ii]->energy_used;
        }
    }
#endif

    cpSpaceStep(gs->space, dt);
}