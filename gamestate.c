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

void initialize(struct GameState *gs)
{
    gs->space = cpSpaceNew();
    for (int i = 0; i < MAX_PLAYERS; i++)
    {
        reset_player(&gs->players[i]);
    }
}
void destroy(struct GameState *gs)
{
    for (int i = 0; i < gs->num_grids; i++)
    {
        grid_destroy(&gs->grids[i]);
    }
    gs->num_grids = 0;

    cpSpaceDestroy(gs->space);
    gs->space = NULL;
}

void reset_player(struct Player *p)
{
    *p = (struct Player){0};
    p->currently_inhabiting_index = -1;
}

struct Box box_new(struct GameState *gs, struct Grid *grid, V2 pos)
{

    float halfbox = BOX_SIZE / 2.0f;
    cpBB box = cpBBNew(-halfbox + pos.x, -halfbox + pos.y, halfbox + pos.x, halfbox + pos.y);
    cpVect verts[4] = {
        cpv(box.r, box.b),
        cpv(box.r, box.t),
        cpv(box.l, box.t),
        cpv(box.l, box.b),
    };

    struct Box to_return = (struct Box){
        .shape = (cpShape *)cpPolyShapeInitRaw(cpPolyShapeAlloc(), grid->body, 4, verts, 0.0f), // this cast is done in chipmunk, not sure why it works
    };

    // assumed to be grid in inhabit code as well
    cpShapeSetUserData(to_return.shape, (void *)grid);
    cpShapeSetMass(to_return.shape, BOX_MASS);
    cpSpaceAddShape(gs->space, to_return.shape);

    // update the center of mass (can't believe this isn't done for me...)
    // float total_mass = 0.0f;
    // float total_moment_of_inertia = 0.0f;
    // V2 total_pos = {0};
    // for (int i = 0; i < grid->num_boxes; i++)
    // {
    //     cpShape *cur_shape = grid->boxes[i].shape;
    //     total_mass += cpShapeGetMass(cur_shape);
    //     total_moment_of_inertia += cpShapeGetMoment(cur_shape);
    //     total_pos = V2add(total_pos, V2scale(cp_to_v2(cpShapeGetCenterOfGravity(cur_shape)), cpShapeGetMass(cur_shape)));
    // }
    // total_pos = V2scale(total_pos, 1.0f / total_mass);


    // @Robust I think moment of inertia calculation is wrong? https://chipmunk-physics.net/forum/viewtopic.php?t=2566

    return to_return;
}

struct Grid grid_new(struct GameState *gs, V2 pos)
{
    assert(gs->space != NULL);
    float halfbox = BOX_SIZE / 2.0f;

    cpBody *body = cpSpaceAddBody(gs->space, cpBodyNew(0.0, 0.0)); // zeros for mass/moment of inertia means automatically calculated from its collision shapes
    cpBodySetPosition(body, v2_to_cp(pos));

    struct Grid to_return = (struct Grid){
        .body = body,
    };

    // box_new(gs, &to_return, (V2){0});

    return to_return;
}

void grid_destroy(struct Grid *grid)
{
    for (int ii = 0; ii < grid->num_boxes; ii++)
    {
        cpShapeFree(grid->boxes[ii].shape);
        grid->boxes[ii].shape = NULL;
    }
    grid->num_boxes = 0;

    cpBodyFree(grid->body);
    grid->body = NULL;
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
    struct Grid *g = (struct Grid *)cpShapeGetUserData(box->shape);
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

void ser_int(char **out, int i)
{
    memwrite(out, i);
}

void des_int(char **in, int *i)
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

    ser_int(out, g->num_boxes);
    for (int i = 0; i < g->num_boxes; i++)
    {
        ser_V2(out, cp_to_v2(cpShapeGetCenterOfGravity(g->boxes[i].shape)));
        ser_float(out, g->boxes[i].damage);
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

    *g = grid_new(gs, pos);
    cpBodySetVelocity(g->body, v2_to_cp(vel));
    cpBodySetAngle(g->body, rot);
    cpBodySetAngularVelocity(g->body, angular_vel);

    des_int(in, &g->num_boxes);

    for (int i = 0; i < g->num_boxes; i++)
    {
        V2 pos = {0};
        des_V2(in, &pos);
        g->boxes[i] = box_new(gs, g, pos);
        des_float(in, &g->boxes[i].damage);
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
        ser_V2(out, p->movement);
        ser_bool(out, p->inhabit);
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
        des_V2(in, &p->movement);
        des_bool(in, &p->inhabit);
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

    for (int i = 0; i < MAX_PLAYERS; i++)
    {
        ser_player(&bytes, &gs->players[i]);
        LEN_CHECK();
    }

    // @Robust invalid message on num boxes bigger than max boxes
    ser_int(&bytes, gs->num_grids);
    LEN_CHECK();

    for (int i = 0; i < gs->num_grids; i++)
    {
        ser_grid(&bytes, &gs->grids[i]);
        LEN_CHECK();
    }

    *out_len = bytes - original_bytes;
}

void from_bytes(struct ServerToClient *msg, char *bytes, int max_len)
{
    struct GameState *gs = msg->cur_gs;

    char *original_bytes = bytes;
    // destroy and free all chipmunk

    destroy(gs);
    initialize(gs);

    des_int(&bytes, &msg->your_player);
    LEN_CHECK();

    for (int i = 0; i < MAX_PLAYERS; i++)
    {
        des_player(&bytes, &gs->players[i], gs);
        LEN_CHECK();
    }

    des_int(&bytes, &gs->num_grids);
    LEN_CHECK();

    for (int i = 0; i < gs->num_grids; i++)
    {
        des_grid(&bytes, &gs->grids[i], gs);
        LEN_CHECK();
    }
}

void process(struct GameState *gs, float dt)
{
    assert(gs->space != NULL);

    // process input
    for (int i = 0; i < MAX_PLAYERS; i++)
    {
        struct Player *p = &gs->players[i];
        if (!p->connected)
            continue;

        if (p->inhabit)
        {
            p->inhabit = false; // "handle" the input
            if (p->currently_inhabiting_index == -1)
            {

                // @Robust mask to only ship boxes of things the player can inhabit
                cpPointQueryInfo query_info = {0};
                cpShape *result = cpSpacePointQueryNearest(gs->space, v2_to_cp(p->pos), 0.1, cpShapeFilterNew(CP_NO_GROUP, CP_ALL_CATEGORIES, CP_ALL_CATEGORIES), &query_info);
                if (result != NULL)
                {
                    struct Grid *g = (struct Grid *)cpShapeGetUserData(result);
                    for (int ii = 0; ii < gs->num_grids; ii++)
                    {
                        if (&gs->grids[ii] == g)
                        {
                            p->currently_inhabiting_index = ii;
                            break;
                        }
                    }
                    if (p->currently_inhabiting_index == -1)
                    {
                        Log("Couldn't find ship to inhabit even though point collision returned something\n");
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

        if (p->currently_inhabiting_index == -1)
        {
            p->vel = V2lerp(p->vel, p->movement, dt * 5.0f);
            p->pos = V2add(p->pos, V2scale(p->vel, dt));
        }
        else
        {
            struct Grid *g = &gs->grids[p->currently_inhabiting_index];
            p->pos = V2lerp(p->pos, grid_com(g), dt * 20.0f);
            cpBodyApplyForceAtWorldPoint(g->body, v2_to_cp(V2scale(p->movement, 5.0f)), v2_to_cp(grid_com(g)));
        }
        // cpBodyApplyForceAtWorldPoint(p->box.body, v2_to_cp(V2scale(p->input, 5.0f)), v2_to_cp(box_pos(p->box)));
    }

    cpSpaceStep(gs->space, dt);
}