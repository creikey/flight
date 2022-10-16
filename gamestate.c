#include "types.h"

#include <stdlib.h> // malloc
#include <stdio.h>

void __assert(bool cond, const char * file, int line, const char * cond_string)
{
    if(!cond)
    {
        fprintf(stderr, "%s:%d | Assertion %s failed\n", file, line, cond_string);
    }
}

#define assert(condition) __assert(condition, __FILE__, __LINE__, #condition)

// do not use any global variables to process gamestate

// super try not to depend on external libraries like enet or sokol to keep build process simple,
// gamestate its own portable submodule. If need to link to other stuff document here:

static void integrate_acceleration(struct Body *body, float dt)
{
    // position
    {
        V2 current = body->position;
        body->position = V2add(body->position, V2sub(current, body->old_position));
        body->position = V2add(body->position, V2scale(body->acceleration, dt * dt));
        body->old_position = current;
    }

    // rotation
    {
        float current = body->rotation;
        body->rotation = body->rotation + (current - body->old_rotation);
        body->rotation = body->rotation + body->angular_acceleration * dt * dt;
        body->old_rotation = current;
    }
}

static void modify_interval(struct Body *from, float *from_interval, V2 center, V2 axis)
{
    float halfbox = BOX_SIZE/2.0f;
    V2 points[4] = {
        V2add(from->position, V2rotate((V2){.x = halfbox, .y = -halfbox}, from->rotation)),  // upper right
        V2add(from->position, V2rotate((V2){.x = halfbox, .y = halfbox}, from->rotation)),   // bottom right
        V2add(from->position, V2rotate((V2){.x = -halfbox, .y = halfbox}, from->rotation)),  // lower left
        V2add(from->position, V2rotate((V2){.x = -halfbox, .y = -halfbox}, from->rotation)), // upper left
    };
    for (int point_i = 0; point_i < 4; point_i++)
    {
        float value = V2projectvalue(V2sub(points[point_i], center), axis);
        if (value > from_interval[1])
        {
            from_interval[1] = value;
        }
        if (value < from_interval[0])
        {
            from_interval[0] = value;
        }
    }
}


void process(struct GameState *gs, float dt)
{
    int num_bodies = gs->num_boxes;
    for (int i = 0; i < MAX_PLAYERS; i++)
    {
        struct Player *p = &gs->players[i];
        if (!p->connected)
            continue;
        p->body.acceleration = V2scale(p->input, 5.0f);
        p->body.angular_acceleration = p->input.x * 10.0f;
        num_bodies += 1;
    }

    // @Robust do this without malloc
    struct Body **bodies = malloc(sizeof *bodies * num_bodies);
    int cur_body_index = 0;

    for (int i = 0; i < MAX_PLAYERS; i++)
    {
        struct Player *p = &gs->players[i];
        if (!p->connected)
            continue;
        bodies[cur_body_index] = &p->body;
        cur_body_index++;
    }

    for (int i = 0; i < gs->num_boxes; i++)
    {
        bodies[cur_body_index] = &gs->boxes[i].body;
        cur_body_index++;
    }

    assert(cur_body_index == num_bodies);

    for (int i = 0; i < num_bodies; i++)
    {
        struct Body *body = bodies[i];
        integrate_acceleration(body, dt);
    }

    // @Robust handle when bodies are overlapping (even perfectly)
    for (int i = 0; i < num_bodies; i++)
    {
        for (int ii = 0; ii < num_bodies; ii++)
        {
            if (ii == i)
                continue;
            struct Body *from = bodies[i];
            struct Body *to = bodies[ii];

            V2 axis = V2normalize(V2sub(to->position, from->position));
            V2 center = V2scale(V2add(to->position, from->position), 0.5f);

            dbg_line(from->position, to->position);
            
            dbg_rect(center);
            
            float from_interval[2] = {1000.0f, -1000.0f};
            float to_interval[2] = {1000.0f, -1000.0f};
            modify_interval(from, from_interval, center, axis);
            modify_interval(to, to_interval, center, axis);
            assert(from_interval[0] < from_interval[1]);
            assert(to_interval[0] < to_interval[1]);

            // @BeforeShip debug compile time flag in preprocessor

            if (from_interval[1] > to_interval[0]) // intersecting
            {
                float intersection_depth = from_interval[1] - to_interval[0];
                

                from->position = V2add(from->position, V2scale(axis, intersection_depth*-0.5f));
                to->position = V2add(to->position, V2scale(axis, intersection_depth*0.5f));
            }
        }
    }

    free(bodies);
}