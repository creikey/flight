#include <chipmunk.h>
#include "types.h"

#include <stdio.h> // assert logging

void __assert(bool cond, const char *file, int line, const char *cond_string)
{
    if (!cond)
    {
        fprintf(stderr, "%s:%d | Assertion %s failed\n", file, line, cond_string);
    }
}

#define assert(condition) __assert(condition, __FILE__, __LINE__, #condition)

// do not use any global variables to process gamestate

// super try not to depend on external libraries like enet or sokol to keep build process simple,
// gamestate its own portable submodule. If need to link to other stuff document here:
// - debug.c for debug drawing
// - chipmunk

void initialize(struct GameState *gs)
{
    gs->space = cpSpaceNew();
}
void destroy(struct GameState *gs)
{
    for (int i = 0; i < MAX_PLAYERS; i++)
    {
        box_destroy(&gs->players[i].box);
    }
    for (int i = 0; i < gs->num_boxes; i++)
    {
        box_destroy(&gs->boxes[i]);
    }
    gs->num_boxes = 0;
    cpSpaceDestroy(gs->space);
    gs->space = NULL;
}

static V2 cp_to_v2(cpVect v)
{
    return (V2){.x = v.x, .y = v.y};
}

static cpVect v2_to_cp(V2 v)
{
    return cpv(v.x, v.y);
}

struct Box box_new(struct GameState *gs, V2 pos)
{
    assert(gs->space != NULL);
    float halfbox = BOX_SIZE / 2.0f;
    cpBody *body = cpSpaceAddBody(gs->space, cpBodyNew(BOX_MASS, cpMomentForBox(BOX_MASS, BOX_SIZE, BOX_SIZE)));
    cpShape *shape = cpBoxShapeNew(body, BOX_SIZE, BOX_SIZE, 0.0f);
    cpSpaceAddShape(gs->space, shape);
    cpBodySetPosition(body, v2_to_cp(pos));

    return (struct Box){
        .body = body,
        .shape = shape,
    };
}

void box_destroy(struct Box *box)
{
    cpShapeFree(box->shape);
    cpBodyFree(box->body);
    box->shape = NULL;
    box->body = NULL;
}



V2 box_pos(struct Box box)
{
    return cp_to_v2(cpBodyGetPosition(box.body));
}
V2 box_vel(struct Box box)
{
    return cp_to_v2(cpBodyGetVelocity(box.body));
}
float box_rotation(struct Box box)
{
    return cpBodyGetAngle(box.body);
}
float box_angular_velocity(struct Box box)
{
    return cpBodyGetAngularVelocity(box.body);
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

void ser_box(char **out, struct Box *b)
{
    // box must not be null, dummy!
    assert(b->body != NULL);
    ser_V2(out, box_pos(*b));
    ser_V2(out, box_vel(*b));
    ser_float(out, box_rotation(*b));
    ser_float(out, box_angular_velocity(*b));
}

// takes gamestate as argument to place box in the gamestates space
void des_box(char **in, struct Box *b, struct GameState *gs)
{
    assert(b->body == NULL); // destroy the box before deserializing into it
    V2 pos = {0};
    V2 vel = {0};
    float rot = 0.0f;
    float angular_vel = 0.0f;

    des_V2(in, &pos);
    des_V2(in, &vel);
    des_float(in, &rot);
    des_float(in, &angular_vel);

    *b = box_new(gs, pos);
    cpBodySetVelocity(b->body, v2_to_cp(vel));
    cpBodySetAngle(b->body, rot);
    cpBodySetAngularVelocity(b->body, angular_vel);
}

void ser_player(char **out, struct Player *p)
{
    ser_bool(out, p->connected);
    if (p->connected)
    {
        ser_box(out, &p->box);
        ser_V2(out, p->input);
    }
}

void des_player(char **in, struct Player *p, struct GameState *gs)
{
    des_bool(in, &p->connected);
    if (p->connected)
    {
        des_box(in, &p->box, gs);
        des_V2(in, &p->input);
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

    for (int i = 0; i < MAX_PLAYERS; i++)
    {
        ser_player(&bytes, &gs->players[i]);
        LEN_CHECK();
    }

    // @Robust invalid message on num boxes bigger than max boxes
    ser_int(&bytes, gs->num_boxes);
    LEN_CHECK();

    for (int i = 0; i < gs->num_boxes; i++)
    {
        ser_box(&bytes, &gs->boxes[i]);
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

    des_int(&bytes, &gs->num_boxes);
    LEN_CHECK();

    for (int i = 0; i < gs->num_boxes; i++)
    {
        des_box(&bytes, &gs->boxes[i], gs);
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
        cpBodyApplyForceAtWorldPoint(p->box.body, v2_to_cp(V2scale(p->input, 5.0f)), v2_to_cp(box_pos(p->box)));
    }

    cpSpaceStep(gs->space, dt);
}