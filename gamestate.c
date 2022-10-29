#include <chipmunk.h>
#include "types.h"

#include <stdio.h> // assert logging

// do not use any global variables to process gamestate

// super try not to depend on external libraries like enet or sokol to keep build process simple,
// gamestate its own portable submodule. If need to link to other stuff document here:
// - debug.c for debug drawing
// - chipmunk

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

static struct Box *getbox(cpShape *shape)
{
    return (struct Box *)cpShapeGetUserData(shape);
}

static struct Grid *find_empty_grid(struct GameState *gs)
{
    // @Robust better memory mgmt
    struct Grid *empty_grid = NULL;
    for (int ii = 0; ii < MAX_GRIDS; ii++)
    {
        if (gs->grids[ii].body == NULL)
        {
            empty_grid = &gs->grids[ii];
            break;
        }
    }
    // @Robust cleanly fail when not enough grids
    assert(empty_grid != NULL);
    return empty_grid;
}

static int grid_num_boxes(struct Grid *g)
{
    int to_return = 0;
    for (int i = 0; i < MAX_BOXES_PER_GRID; i++)
    {
        SKIPNULL(g->boxes[i].shape);
        to_return++;
    }
    return to_return;
}

static void box_destroy(cpSpace *space, struct Box *box)
{
    cpSpaceRemoveShape(space, box->shape);
    cpShapeFree(box->shape);
    box->shape = NULL;
}

// space should be from gamestate, doesn't accept gamestate parameter so collision
// callbacks can use it
void grid_destroy(cpSpace *space, struct Grid *grid)
{
    for (int i = 0; i < MAX_BOXES_PER_GRID; i++)
    {
        SKIPNULL(grid->boxes[i].shape);

        box_destroy(space, &grid->boxes[i]);
    }

    cpSpaceRemoveBody(space, grid->body);
    cpBodyFree(grid->body);
    grid->body = NULL;
}

// removes boxe from grid, then ensures that the rule that grids must not have
// holes in them is applied.
// uses these forward declared serialization functions to duplicate a box
typedef struct SerState
{
    char *bytes;
    bool serializing;
    int cursor; // points to next available byte, is the size of current message after serializing something
    int max_size;
} SerState;
void ser_box(SerState *ser, struct Box *var, struct GameState *gs, struct Grid *g);
static void grid_remove_box(cpSpace *space, struct Grid *grid, struct Box *box)
{
    box_destroy(space, box);

    struct GameState *gs = (struct GameState *)cpSpaceGetUserData(space);
    int num_boxes = grid_num_boxes(grid);
    if (num_boxes == 0)
    {
        grid_destroy(space, grid);
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
    struct Box *separate_grids[MAX_SEPARATE_GRIDS][MAX_BOXES_PER_GRID] = {0};
    int cur_separate_grid_index = 0;
    int processed_boxes = 0;

    struct Box **biggest_separate_grid = separate_grids[0];
    int biggest_separate_grid_length = 0;

    // process all boxes into separate, but correctly connected, grids
    while (processed_boxes < num_boxes)
    {
        // grab an unprocessed box, one not in separate_grids, to start the flood fill
        struct Box *unprocessed = NULL;
        for (int i = 0; i < MAX_BOXES_PER_GRID; i++)
        {
            SKIPNULL(grid->boxes[i].shape);
            struct Box *cur = &grid->boxes[i];
            bool cur_has_been_processed = false;
            for (int sep_i = 0; sep_i < MAX_SEPARATE_GRIDS; sep_i++)
            {
                for (int sep_box_i = 0; sep_box_i < MAX_BOXES_PER_GRID; sep_box_i++)
                {
                    if (cur == separate_grids[sep_i][sep_box_i])
                    {
                        cur_has_been_processed = true;
                        break;
                    }
                }
                if (cur_has_been_processed)
                    break;
            }
            if (!cur_has_been_processed)
            {
                unprocessed = cur;
                break;
            }
        }
        assert(unprocessed != NULL);

        // flood fill from this unprocessed box, adding each result to a new separate grid
        // https://en.wikipedia.org/wiki/Flood_fill
        struct Box **cur_separate_grid = separate_grids[cur_separate_grid_index];
        cur_separate_grid_index++;
        int separate_grid_i = 0;
        {
            // queue stuff @Robust use factored datastructure
            struct Box *Q[MAX_BOXES_PER_GRID] = {0};
            int Q_i = 0;

            Q[Q_i] = unprocessed;
            Q_i++;
            struct Box *N = NULL;
            while (Q_i > 0)
            {
                N = Q[Q_i - 1];
                Q_i--;
                if (true) // if node "inside", this is always true
                {
                    cur_separate_grid[separate_grid_i] = N;
                    separate_grid_i++;
                    processed_boxes++;

                    V2 cur_local_pos = box_local_pos(N);
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
                        // @Robust faster method, not O(N^2), of getting the box
                        // in the direction currently needed
                        V2 wanted_local_pos = V2add(cur_local_pos, V2scale(dir, BOX_SIZE));
                        struct Box *box_in_direction = NULL;
                        for (int iii = 0; iii < MAX_BOXES_PER_GRID; iii++)
                        {
                            SKIPNULL(grid->boxes[iii].shape);
                            if (V2cmp(box_local_pos(&grid->boxes[iii]), wanted_local_pos, 0.01f))
                            {
                                box_in_direction = &grid->boxes[iii];
                                break;
                            }
                        }
                        if (box_in_direction != NULL)
                        {
                            // make sure not already added to the separate grid
                            bool already_in_separate_grid = false;
                            for (int sepgrid_i = 0; sepgrid_i < MAX_BOXES_PER_GRID; sepgrid_i++)
                            {
                                if (cur_separate_grid[sepgrid_i] == NULL)
                                    break; // assumed to be end of the current separate grid list
                                if (cur_separate_grid[sepgrid_i] == box_in_direction)
                                {
                                    already_in_separate_grid = true;
                                    break;
                                }
                            }
                            if (!already_in_separate_grid)
                            {
                                Q[Q_i] = box_in_direction;
                                Q_i++;
                            }
                        }
                    }
                }
            }
        }

        if (separate_grid_i > biggest_separate_grid_length)
        {
            biggest_separate_grid_length = separate_grid_i;
            biggest_separate_grid = cur_separate_grid;
        }
    }

    // create new grids for all lists of boxes except for the biggest one.
    // delete the boxes out of the current grid as I pull boxes into separate ones
    // which are no longer connected
    for (int sepgrid_i = 0; sepgrid_i < MAX_SEPARATE_GRIDS; sepgrid_i++)
    {
        if (separate_grids[sepgrid_i] == biggest_separate_grid)
            continue; // leave the boxes of the biggest separate untouched
        struct Box **cur_separate_grid = separate_grids[sepgrid_i];
        int cur_sepgrid_i = 0;
        if (cur_separate_grid[cur_sepgrid_i] == NULL)
            continue; // this separate grid is empty

        struct Grid *new_grid = find_empty_grid(gs);
        grid_new(new_grid, gs, grid_pos(grid)); // all grids have same pos but different center of mass (com)
        cpBodySetAngle(new_grid->body, grid_rotation(grid));

        int new_grid_box_i = 0;
        while (cur_separate_grid[cur_sepgrid_i] != NULL)
        {
            char box_bytes[128];

            char *cur = box_bytes;
            // duplicate the box by serializing it then deserializing it
            SerState ser = (SerState){
                .bytes = cur,
                .cursor = 0,
                .max_size = 128,
                .serializing = true,
            };
            ser_box(&ser, cur_separate_grid[cur_sepgrid_i], gs, grid);
            ser.cursor = 0;
            ser.serializing = false;
            ser_box(&ser, &new_grid->boxes[new_grid_box_i], gs, new_grid);

            cur_sepgrid_i++;
            new_grid_box_i++;
        }

        cpBodySetVelocity(new_grid->body, cpBodyGetVelocityAtWorldPoint(grid->body, v2_to_cp(grid_com(new_grid))));
        cpBodySetAngularVelocity(new_grid->body, grid_angular_velocity(grid));
        cur_sepgrid_i = 0;
        while (cur_separate_grid[cur_sepgrid_i] != NULL)
        {
            box_destroy(space, cur_separate_grid[cur_sepgrid_i]);
            cur_sepgrid_i++;
        }
    }
}

static void postStepRemove(cpSpace *space, void *key, void *data)
{
    cpShape *b = (cpShape *)key;
    if (getbox(b)->damage > 1.0f)
    {
        grid_remove_box(space, (struct Grid *)cpBodyGetUserData(cpShapeGetBody(b)), getbox(b));
    }
}

static cpBool on_damage(cpArbiter *arb, cpSpace *space, cpDataPointer userData)
{
    cpShape *a, *b;
    cpArbiterGetShapes(arb, &a, &b);

    float damage = V2length(cp_to_v2(cpArbiterTotalImpulse(arb))) * 0.25f;
    if (damage > 0.05f)
    {
        // Log("Collision with damage %f\n", damage);
        getbox(a)->damage += damage;
        getbox(b)->damage += damage;
    }

    // b must be the key passed into the post step removed, the key is cast into its shape
    cpSpaceAddPostStepCallback(space, (cpPostStepFunc)postStepRemove, b, NULL);
    cpSpaceAddPostStepCallback(space, (cpPostStepFunc)postStepRemove, a, NULL);

    return true; // keep colliding
}

void initialize(struct GameState *gs)
{
    gs->space = cpSpaceNew();
    cpSpaceSetUserData(gs->space, (cpDataPointer)gs);                          // needed in the handler
    cpCollisionHandler *handler = cpSpaceAddCollisionHandler(gs->space, 0, 0); // @Robust limit collision type to just blocks that can be damaged
    // handler->beginFunc = begin;
    handler->postSolveFunc = on_damage;
    // handler->postSolveFunc = postStepRemove;
    for (int i = 0; i < MAX_PLAYERS; i++)
    {
        reset_player(&gs->players[i]);
    }
}
void destroy(struct GameState *gs)
{
    for (int i = 0; i < MAX_GRIDS; i++)
    {
        SKIPNULL(gs->grids[i].body);
        grid_destroy(gs->space, &gs->grids[i]);
    }

    cpSpaceFree(gs->space);
    gs->space = NULL;
}

void reset_player(struct Player *p)
{
    *p = (struct Player){0};
    p->currently_inhabiting_index = -1;
}

// box must be passed as a parameter as the box added to chipmunk uses this pointer in its
// user data. pos is in local coordinates
void box_new(struct Box *to_modify, struct GameState *gs, struct Grid *grid, V2 pos)
{
    *to_modify = (struct Box){0};
    float halfbox = BOX_SIZE / 2.0f;
    cpBB box = cpBBNew(-halfbox + pos.x, -halfbox + pos.y, halfbox + pos.x, halfbox + pos.y);
    cpVect verts[4] = {
        cpv(box.r, box.b),
        cpv(box.r, box.t),
        cpv(box.l, box.t),
        cpv(box.l, box.b),
    };

    to_modify->shape = (cpShape *)cpPolyShapeInitRaw(cpPolyShapeAlloc(), grid->body, 4, verts, 0.0f); // this cast is done in chipmunk, not sure why it works

    // assumed to be grid in inhabit code as well
    cpShapeSetUserData(to_modify->shape, (void *)to_modify);
    cpShapeSetMass(to_modify->shape, BOX_MASS);
    cpSpaceAddShape(gs->space, to_modify->shape);
}

// the grid pointer passed gets referenced by the body
void grid_new(struct Grid *to_modify, struct GameState *gs, V2 pos)
{
    assert(gs->space != NULL);
    float halfbox = BOX_SIZE / 2.0f;

    cpBody *body = cpSpaceAddBody(gs->space, cpBodyNew(0.0, 0.0)); // zeros for mass/moment of inertia means automatically calculated from its collision shapes
    to_modify->body = body;
    cpBodySetPosition(body, v2_to_cp(pos));
    cpBodySetUserData(to_modify->body, (void *)to_modify);
}

// center of mass, not the literal position
V2 grid_com(struct Grid *grid)
{
    return cp_to_v2(cpBodyLocalToWorld(grid->body, cpBodyGetCenterOfGravity(grid->body)));
}
V2 grid_pos(struct Grid *grid)
{
    return cp_to_v2(cpBodyGetPosition(grid->body));
}
V2 grid_vel(struct Grid *grid)
{
    return cp_to_v2(cpBodyGetVelocity(grid->body));
}
V2 grid_world_to_local(struct Grid *grid, V2 world)
{
    return cp_to_v2(cpBodyWorldToLocal(grid->body, v2_to_cp(world)));
}
V2 grid_local_to_world(struct Grid *grid, V2 local)
{
    return cp_to_v2(cpBodyLocalToWorld(grid->body, v2_to_cp(local)));
}
// returned snapped position is in world coordinates
V2 grid_snapped_box_pos(struct Grid *grid, V2 world)
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
float grid_rotation(struct Grid *grid)
{
    return cpBodyGetAngle(grid->body);
}
float grid_angular_velocity(struct Grid *grid)
{
    return cpBodyGetAngularVelocity(grid->body);
}
struct Grid *box_grid(struct Box *box)
{
    return (struct Grid *)cpBodyGetUserData(cpShapeGetBody(box->shape));
}
V2 box_local_pos(struct Box *box)
{
    return cp_to_v2(cpShapeGetCenterOfGravity(box->shape));
}
V2 box_pos(struct Box *box)
{
    return V2add(grid_pos(box_grid(box)), V2rotate(box_local_pos(box), grid_rotation(box_grid(box))));
}
float box_rotation(struct Box *box)
{
    return cpBodyGetAngle(cpShapeGetBody(box->shape));
}

#define WRITE_VARNAMES false // good for debugging
#include <string.h>
// assumes SerState *var defined
#define SER_VAR(var_pointer)                                                                                      \
    {                                                                                                             \
        const char *var_name = #var_pointer;                                                                      \
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

void ser_V2(SerState *ser, V2 *var)
{
    SER_VAR(&var->x);
    SER_VAR(&var->y);
}

void ser_box(SerState *ser, struct Box *var, struct GameState *gs, struct Grid *g)
{
    {
        V2 pos;
        if (ser->serializing)
        {
            pos = cp_to_v2(cpShapeGetCenterOfGravity(var->shape));
        }
        ser_V2(ser, &pos);
        if (!ser->serializing)
        {
            box_new(var, gs, g, pos);
        }
    }

    SER_VAR(&var->type); // @Rovarust separate enum serialization that checks for out of varounds enum
    SER_VAR(&var->compass_rotation);
    SER_VAR(&var->thrust);
    SER_VAR(&var->energy_used);
    SER_VAR(&var->damage);
}

void ser_grid(SerState *ser, struct GameState *gs, struct Grid *g)
{
    if (ser->serializing)
        assert(g->body != NULL);
    else
        assert(g->body == NULL);

    {
        V2 pos = {0};
        V2 vel = {0};
        float rot = 0.0f;
        float angular_vel = 0.0f;
        if (ser->serializing)
        {
            pos = grid_pos(g);
            vel = grid_vel(g);
            rot = grid_rotation(g);
            angular_vel = grid_angular_velocity(g);
        }
        SER_VAR(&pos);
        SER_VAR(&vel);
        SER_VAR(&rot);
        SER_VAR(&angular_vel);
        if (!ser->serializing)
        {
            grid_new(g, gs, pos);
            cpBodySetVelocity(g->body, v2_to_cp(vel));
            cpBodySetAngle(g->body, rot);
            cpBodySetAngularVelocity(g->body, angular_vel);
        }
    }

    SER_VAR(&g->total_energy_capacity);

    for (int i = 0; i < MAX_BOXES_PER_GRID; i++)
    {
        bool exists;
        if (ser->serializing)
            exists = g->boxes[i].shape != NULL;
        SER_VAR(&exists);
        if (exists)
        {
            ser_box(ser, &g->boxes[i], gs, g);
        }
    }
}

void ser_inputframe(SerState *ser, struct InputFrame *i)
{
    SER_VAR(&i->movement);
    SER_VAR(&i->inhabit);
    SER_VAR(&i->build);
    SER_VAR(&i->dobuild);
    SER_VAR(&i->build_type);
    SER_VAR(&i->build_rotation);
    SER_VAR(&i->grid_index);
}

void ser_player(SerState *ser, struct Player *p)
{
    SER_VAR(&p->connected);
    if (p->connected)
    {
        SER_VAR(&p->currently_inhabiting_index);
        ser_V2(ser, &p->pos);
        ser_V2(ser, &p->vel);
        SER_VAR(&p->spice_taken_away);
        SER_VAR(&p->goldness);
        ser_inputframe(ser, &p->input);
    }
}

void ser_server_to_client(SerState *ser, ServerToClient *s)
{
    struct GameState *gs = s->cur_gs;

    if (!ser->serializing)
    {
        destroy(gs);
        initialize(gs);
    }

    SER_VAR(&s->your_player);
    SER_VAR(&gs->tick);
    SER_VAR(&gs->time);

    ser_V2(ser, &gs->goldpos);

    for (int i = 0; i < MAX_PLAYERS; i++)
    {
        ser_player(ser, &gs->players[i]);
    }

    for (int i = 0; i < MAX_GRIDS; i++)
    {
        bool exists;
        if (ser->serializing)
            exists = gs->grids[i].body != NULL;
        SER_VAR(&exists);
        if (exists)
        {
            ser_grid(ser, gs, &gs->grids[i]);
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
    float dist = V2length(cp_to_v2(cpvsub(points->points[0].pointA, points->points[0].pointB)));
    // float dist = -points->points[0].distance;
    if (dist > closest_to_point_in_radius_result_largest_dist)
    {
        closest_to_point_in_radius_result_largest_dist = dist;
        closest_to_point_in_radius_result = shape;
    }
}

struct Grid *closest_to_point_in_radius(struct GameState *gs, V2 point, float radius)
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
        return (struct Grid *)cpBodyGetUserData(cpShapeGetBody(closest_to_point_in_radius_result));
    }

    return NULL;
}

V2 thruster_direction(struct Box *box)
{
    assert(box->type == BoxThruster);
    V2 to_return = (V2){.x = 1.0f, .y = 0.0f};

    to_return = V2rotate(to_return, rotangle(box->compass_rotation));
    to_return = V2rotate(to_return, box_rotation(box));

    return to_return;
}

V2 thruster_force(struct Box *box)
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
    gs->tick += 1;
    gs->time += dt;

    // process input
    for (int i = 0; i < MAX_PLAYERS; i++)
    {
        struct Player *p = &gs->players[i];
        if (!p->connected)
            continue;

        // update gold win condition
        if (V2length(V2sub(p->pos, gs->goldpos)) < GOLD_COLLECT_RADIUS)
        {
            p->goldness += 0.1;
            p->spice_taken_away = 0.0f;
            gs->goldpos = (V2){.x = hash11(gs->time) * 20.0f, .y = hash11(gs->time - 13.6f) * 20.0f};
        }

        if (gs->grids[p->currently_inhabiting_index].body == NULL)
        {
            p->currently_inhabiting_index = -1;
        }

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

        // process movement
        {
            // no cheating by making movement bigger than length 1
            if (V2length(p->input.movement) != 0.0f)
            {
                p->input.movement = V2scale(V2normalize(p->input.movement), clamp(V2length(p->input.movement), 0.0f, 1.0f));
            }
            if (p->currently_inhabiting_index == -1)
            {
                // @Robust make sure movement vector is normalized so player can't cheat
                p->vel = V2add(p->vel, V2scale(p->input.movement, dt * 0.5f));
                p->spice_taken_away += dt * 0.15f * V2length(p->input.movement);
            }
            else
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
            p->pos = V2add(p->pos, V2scale(p->vel, dt));
        }

        if (p->input.dobuild)
        {
            p->input.dobuild = false; // handle the input. if didn't do this, after destruction of hovered box, would try to build on its grid with grid_index...

            cpPointQueryInfo info = {0};
            // @Robust make sure to query only against boxes...
            V2 world_build = p->input.build;
            if (p->input.grid_index != -1)
            {
                world_build = grid_local_to_world(&gs->grids[p->input.grid_index], p->input.build);
            }
            cpShape *nearest = cpSpacePointQueryNearest(gs->space, v2_to_cp(world_build), 0.01f, cpShapeFilterNew(CP_NO_GROUP, CP_ALL_CATEGORIES, CP_ALL_CATEGORIES), &info);
            if (nearest != NULL)
            {
                struct Box *cur_box = (struct Box *)cpShapeGetUserData(nearest);
                struct Grid *cur_grid = (struct Grid *)cpBodyGetUserData(cpShapeGetBody(nearest));
                grid_remove_box(gs->space, cur_grid, cur_box);
                p->spice_taken_away -= 0.1f;
            }
            else if (p->input.grid_index == -1)
            {
                struct Grid *empty_grid = find_empty_grid(gs);
                p->spice_taken_away += 0.2f;
                grid_new(empty_grid, gs, world_build);
                box_new(&empty_grid->boxes[0], gs, empty_grid, (V2){0});
                empty_grid->boxes[0].type = p->input.build_type;
                empty_grid->boxes[0].compass_rotation = p->input.build_rotation;
                cpBodySetVelocity(empty_grid->body, v2_to_cp(p->vel));
            }
            else
            {
                struct Grid *g = &gs->grids[p->input.grid_index];

                struct Box *empty_box = NULL;
                for (int ii = 0; ii < MAX_BOXES_PER_GRID; ii++)
                {
                    if (g->boxes[ii].shape == NULL)
                    {
                        empty_box = &g->boxes[ii];
                        break;
                    }
                }
                // @Robust cleanly fail when not enough boxes
                assert(empty_box != NULL);
                p->spice_taken_away += 0.1f;
                box_new(empty_box, gs, g, grid_world_to_local(g, world_build));
                empty_box->type = p->input.build_type;
                empty_box->compass_rotation = p->input.build_rotation;
            }
        }

        if (p->spice_taken_away >= 1.0f)
        {
            reset_player(p);
            p->connected = true;
        }

        p->spice_taken_away = clamp01(p->spice_taken_away);
    }

    // add thrust from thruster blocks
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

    cpSpaceStep(gs->space, dt);
}