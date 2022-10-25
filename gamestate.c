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

static void grid_remove_box(cpSpace *space, struct Grid *grid, struct Box *box)
{
    box_destroy(space, box);

    if (grid_num_boxes(grid) == 0)
    {
        grid_destroy(space, grid);
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
V2 box_pos(struct Box *box)
{
    struct Grid *g = (struct Grid *)cpBodyGetUserData(cpShapeGetBody(box->shape));
    return V2add(grid_pos(g), cp_to_v2(cpShapeGetCenterOfGravity(box->shape)));
}
float box_rotation(struct Box *box)
{
    return cpBodyGetAngle(cpShapeGetBody(box->shape));
}

#define memwrite(out, variable)                 \
    for (char b = 0; b < sizeof(variable); b++) \
    {                                           \
        **out = ((char *)&variable)[b];         \
        *out += 1;                              \
    }

#define memread(in, variable_pointer)                    \
    for (char b = 0; b < sizeof(*variable_pointer); b++) \
    {                                                    \
        ((char *)variable_pointer)[b] = **in;            \
        *in += 1;                                        \
    }

void ser_float(char **out, float f)
{
    memwrite(out, f);
}

void des_float(char **in, float *f)
{
    memread(in, f);
}

void ser_double(char **out, double d)
{
    memwrite(out, d);
}

void des_double(char **in, double *d)
{
    memread(in, d);
}

void ser_int(char **out, int i)
{
    memwrite(out, i);
}

void des_int(char **in, int *i)
{
    memread(in, i);
}

void ser_uint64(char **out, uint64_t i)
{
    memwrite(out, i);
}

void des_uint64(char **in, uint64_t *i)
{
    memread(in, i);
}

void ser_bool(char **out, bool b)
{
    **out = (char)b;
    *out += 1;
}

void des_bool(char **in, bool *b)
{
    *b = (bool)**in;
    *in += 1;
}

void ser_V2(char **out, V2 v)
{
    ser_float(out, v.x);
    ser_float(out, v.y);
}

void des_V2(char **in, V2 *v)
{
    des_float(in, &v->x);
    des_float(in, &v->y);
}

void ser_grid(char **out, struct Grid *g)
{
    // grid must not be null, dummy!
    assert(g->body != NULL);

    ser_V2(out, grid_pos(g));
    ser_V2(out, grid_vel(g));
    ser_float(out, grid_rotation(g));
    ser_float(out, grid_angular_velocity(g));

    for (int i = 0; i < MAX_BOXES_PER_GRID; i++)
    {
        bool exists = g->boxes[i].shape != NULL;
        ser_bool(out, exists);
        if (exists)
        {
            ser_V2(out, cp_to_v2(cpShapeGetCenterOfGravity(g->boxes[i].shape)));
            ser_float(out, g->boxes[i].damage);
        }
    }
}

// takes gamestate as argument to place box in the gamestates space
void des_grid(char **in, struct Grid *g, struct GameState *gs)
{
    assert(g->body == NULL); // destroy the grid before deserializing into it

    V2 pos = {0};
    V2 vel = {0};
    float rot = 0.0f;
    float angular_vel = 0.0f;

    des_V2(in, &pos);
    des_V2(in, &vel);
    des_float(in, &rot);
    des_float(in, &angular_vel);

    grid_new(g, gs, pos);
    cpBodySetVelocity(g->body, v2_to_cp(vel));
    cpBodySetAngle(g->body, rot);
    cpBodySetAngularVelocity(g->body, angular_vel);

    for (int i = 0; i < MAX_BOXES_PER_GRID; i++)
    {
        bool exists = false;
        des_bool(in, &exists);
        if (exists)
        {
            V2 pos = {0};
            des_V2(in, &pos);
            box_new(&g->boxes[i], gs, g, pos);
            des_float(in, &g->boxes[i].damage);
        }
    }
}

void ser_player(char **out, struct Player *p)
{
    ser_bool(out, p->connected);
    if (p->connected)
    {
        ser_int(out, p->currently_inhabiting_index);
        ser_V2(out, p->pos);
        ser_V2(out, p->vel);
        ser_float(out, p->spice_taken_away);
        ser_float(out, p->goldness);

        // input
        ser_V2(out, p->movement);
        ser_bool(out, p->inhabit);

        ser_V2(out, p->build);
        ser_bool(out, p->dobuild);
        ser_int(out, p->grid_index);
    }
}

void des_player(char **in, struct Player *p, struct GameState *gs)
{
    des_bool(in, &p->connected);
    if (p->connected)
    {
        des_int(in, &p->currently_inhabiting_index);
        des_V2(in, &p->pos);
        des_V2(in, &p->vel);
        des_float(in, &p->spice_taken_away);
        des_float(in, &p->goldness);

        // input
        des_V2(in, &p->movement);
        des_bool(in, &p->inhabit);

        des_V2(in, &p->build);
        des_bool(in, &p->dobuild);
        des_int(in, &p->grid_index);
    }
}

// @Robust really think about if <= makes more sense than < here...
#define LEN_CHECK() assert(bytes - original_bytes <= max_len)

void into_bytes(struct ServerToClient *msg, char *bytes, int *out_len, int max_len)
{
    assert(msg->cur_gs != NULL);
    assert(msg != NULL);
    struct GameState *gs = msg->cur_gs;
    char *original_bytes = bytes;

    ser_int(&bytes, msg->your_player);
    LEN_CHECK();

    ser_uint64(&bytes, gs->tick);

    ser_double(&bytes, gs->time);
    LEN_CHECK();

    ser_V2(&bytes, gs->goldpos);
    LEN_CHECK();

    for (int i = 0; i < MAX_PLAYERS; i++)
    {
        ser_player(&bytes, &gs->players[i]);
        LEN_CHECK();
    }

    // @Robust invalid message on num boxes bigger than max boxes

    for (int i = 0; i < MAX_GRIDS; i++)
    {
        bool exists = gs->grids[i].body != NULL;
        ser_bool(&bytes, exists);
        LEN_CHECK();
        if (exists)
        {
            ser_grid(&bytes, &gs->grids[i]);
            LEN_CHECK();
        }
    }

    *out_len = bytes - original_bytes;
}

void from_bytes(struct ServerToClient *msg, char *bytes, int max_len)
{
    struct GameState *gs = msg->cur_gs;

    char *original_bytes = bytes;

    destroy(gs);
    initialize(gs);

    des_int(&bytes, &msg->your_player);
    LEN_CHECK();

    des_uint64(&bytes, &gs->tick);
    LEN_CHECK();

    des_double(&bytes, &gs->time);
    LEN_CHECK();

    des_V2(&bytes, &gs->goldpos);
    LEN_CHECK();

    for (int i = 0; i < MAX_PLAYERS; i++)
    {
        des_player(&bytes, &gs->players[i], gs);
        LEN_CHECK();
    }

    for (int i = 0; i < MAX_GRIDS; i++)
    {
        bool exists = false;
        des_bool(&bytes, &exists);
        LEN_CHECK();
        if (exists)
        {
            des_grid(&bytes, &gs->grids[i], gs);
            LEN_CHECK();
        }
    }
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

// for random generation
static float hash11(float p)
{
    p = fract(p * .1031);
    p *= p + 33.33;
    p *= p + p;
    return fract(p);
}

uint64_t tick(struct GameState *gs)
{
    return (uint64_t)floor(gs->time / ((double)TIMESTEP));
}

void process(struct GameState *gs, float dt)
{
    assert(gs->space != NULL);

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
            p->goldness += 0.2;
            gs->goldpos = (V2){.x = hash11(gs->time) * 20.0f, .y = hash11(gs->time - 13.6f) * 20.0f};
        }

        if (gs->grids[p->currently_inhabiting_index].body == NULL)
        {
            p->currently_inhabiting_index = -1;
        }

        if (p->inhabit)
        {
            p->inhabit = false; // "handle" the input
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
            if (p->currently_inhabiting_index == -1)
            {
                // @Robust make sure movement vector is normalized so player can't cheat
                p->vel = V2add(p->vel, V2scale(p->movement, dt * 0.5f));
                p->spice_taken_away += dt * 0.15f * V2length(p->movement);
            }
            else
            {
                struct Grid *g = &gs->grids[p->currently_inhabiting_index];
                V2 target_new_pos = V2lerp(p->pos, grid_com(g), dt * 20.0f);
                p->vel = V2scale(V2sub(target_new_pos, p->pos), 1.0f / dt);
                cpBodyApplyForceAtWorldPoint(g->body, v2_to_cp(V2scale(p->movement, 5.0f)), v2_to_cp(grid_com(g)));
                // bigger the ship, the more efficient the spice usage
                p->spice_taken_away += dt * 0.15f / (cpBodyGetMass(g->body) * 2.0f) * V2length(p->movement);
            }
            p->pos = V2add(p->pos, V2scale(p->vel, dt));
        }

        if (p->dobuild)
        {
            p->dobuild = false; // handle the input. if didn't do this, after destruction of hovered box, would try to build on its grid with grid_index...

            cpPointQueryInfo info = {0};
            // @Robust make sure to query only against boxes...
            V2 world_build = p->build;
            if (p->grid_index != -1)
            {
                world_build = grid_local_to_world(&gs->grids[p->grid_index], p->build);
            }
            cpShape *nearest = cpSpacePointQueryNearest(gs->space, v2_to_cp(world_build), 0.01f, cpShapeFilterNew(CP_NO_GROUP, CP_ALL_CATEGORIES, CP_ALL_CATEGORIES), &info);
            if (nearest != NULL)
            {
                struct Box *cur_box = (struct Box *)cpShapeGetUserData(nearest);
                struct Grid *cur_grid = (struct Grid *)cpBodyGetUserData(cpShapeGetBody(nearest));
                grid_remove_box(gs->space, cur_grid, cur_box);
                p->spice_taken_away -= 0.1f;
            }
            else if (p->grid_index == -1)
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
                assert(empty_grid != NULL);
                p->spice_taken_away += 0.2f;
                grid_new(empty_grid, gs, world_build);
                box_new(&empty_grid->boxes[0], gs, empty_grid, (V2){0});
                cpBodySetVelocity(empty_grid->body, v2_to_cp(p->vel));
            }
            else
            {
                struct Grid *g = &gs->grids[p->grid_index];

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
            }
        }

        if (p->spice_taken_away >= 1.0f)
        {
            reset_player(p);
            p->connected = true;
        }

        p->spice_taken_away = clamp01(p->spice_taken_away);
    }

    cpSpaceStep(gs->space, dt);
}