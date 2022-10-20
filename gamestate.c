#include "types.h"

#include <stdlib.h> // malloc
#include <stdio.h>

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
// - debug

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

struct ProcessBody
{
    V2 vertices[4];
    struct Body *body;
};

struct ProcessBody make_process_body(struct Body *from)
{
    float halfbox = BOX_SIZE / 2.0f;
    struct ProcessBody to_return =
        {
            .vertices = {
                // important that the first one is the upper right, used to deduce rotation from vertex position
                // @Robust instead of array of vertices have type? like struct with upper_right upper_left etc
                V2add(from->position, V2rotate((V2){.x = halfbox, .y = -halfbox}, from->rotation)),  // upper right
                V2add(from->position, V2rotate((V2){.x = halfbox, .y = halfbox}, from->rotation)),   // bottom right
                V2add(from->position, V2rotate((V2){.x = -halfbox, .y = halfbox}, from->rotation)),  // lower left
                V2add(from->position, V2rotate((V2){.x = -halfbox, .y = -halfbox}, from->rotation)), // upper left
            },
            .body = from,
        };
    return to_return;
}

static void project(struct ProcessBody *from, V2 axis, float *min, float *max)
{
    float DotP = V2dot(axis, from->vertices[0]);

    // Set the minimum and maximum values to the projection of the first vertex
    *min = DotP;
    *max = DotP;

    for (int I = 1; I < 4; I++)
    {
        // Project the rest of the vertices onto the axis and extend
        // the interval to the left/right if necessary
        DotP = V2dot(axis, from->vertices[I]);

        *min = fmin(DotP, *min);
        *max = fmax(DotP, *max);
    }
}

static float interval_distance(float min_a, float max_a, float min_b, float max_b)
{
    if (min_a < min_b)
        return min_b - max_a;
    else
        return min_a - max_b;
}

static void move_vertices(V2 *vertices, int num, V2 shift)
{
    for (int i = 0; i < num; i++)
    {
        vertices[i] = V2add(vertices[i], shift);
    }
}

void process(struct GameState *gs, float dt)
{
    // process input
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

    struct ProcessBody *bodies = malloc(sizeof *bodies * num_bodies);
    int cur_body_index = 0;

    for (int i = 0; i < MAX_PLAYERS; i++)
    {
        struct Player *p = &gs->players[i];
        if (!p->connected)
            continue;
        integrate_acceleration(&p->body, dt);
        bodies[cur_body_index] = make_process_body(&p->body);
        cur_body_index++;
    }

    for (int i = 0; i < gs->num_boxes; i++)
    {
        integrate_acceleration(&gs->boxes[i].body, dt);
        bodies[cur_body_index] = make_process_body(&gs->boxes[i].body);
        cur_body_index++;
    }

    assert(cur_body_index == num_bodies);

    // Collision
    // @Robust handle when bodies are overlapping (even perfectly)
    for (int i = 0; i < num_bodies; i++)
    {
        for (int ii = 0; ii < num_bodies; ii++)
        {
            if (ii == i)
                continue;
            struct ProcessBody *from = &bodies[i];
            struct ProcessBody *to = &bodies[ii];
            dbg_line(from->body->position, to->body->position);

            float MinDistance = 10000.0f;

            struct Edge
            {
                struct ProcessBody *parent;
                V2 *from;
                V2 *to;
            };

            struct ProcessBody *bodies[2] = {from, to};
            bool was_collision = false;
            V2 normal = {0};
            struct Edge edge = {0};
            for (int body_i = 0; body_i < 2; body_i++)
            {
                struct ProcessBody *body = bodies[body_i];
                for (int edge_from_i = 0; edge_from_i < 3; edge_from_i++)
                {
                    int edge_to_i = edge_from_i + 1;
                    V2 *edge_from = &body->vertices[edge_from_i];
                    V2 *edge_to = &body->vertices[edge_to_i];

                    // normal vector of edge
                    V2 axis = (V2){
                        .x = edge_from->y - edge_to->y,
                        .y = edge_to->x - edge_from->x,
                    };
                    axis = V2normalize(axis);

                    float min_from, min_to, max_from, max_to = 0.0f;
                    project(from, axis, &min_from, &max_from);
                    project(to, axis, &min_to, &max_to);

                    float distance = interval_distance(min_from, min_to, max_from, max_to);

                    if (distance > 0.0f)
                        break;
                    else if (fabsf(distance) < MinDistance)
                    {
                        MinDistance = fabsf(distance);
                        was_collision = true;
                        normal = axis;
                        edge = (struct Edge){
                            .parent = &body,
                            .from = edge_from,
                            .to = edge_to,
                        };
                    }
                }
            }
            float depth = MinDistance;
            if (was_collision)
            {
                float intersection_depth = from_interval[1] - to_interval[0];

                move_vertices(from->vertices, 4, V2scale(axis, intersection_depth * -0.5f));
                move_vertices(to->vertices, 4, V2scale(axis, intersection_depth * 0.5f));
            }
        }
    }

    // Wall
    if (true)
    {
        for (int i = 0; i < num_bodies; i++)
        {
            for (int v_i = 0; v_i < 4; v_i++)
            {
                V2 *vert = &bodies[i].vertices[v_i];
                if (vert->x > 2.0f)
                {
                    vert->x = 2.0f;
                }
            }
        }
    }

    // Correct for differences in vertex position
    const int edge_update_iters = 3;
    for (int iter = 0; iter < edge_update_iters; iter++)
    {
        for (int i = 0; i < num_bodies; i++)
        {
            for (int v_i = 0; v_i < 3; v_i++)
            {
                int other_v_i = v_i + 1;
                V2 *from = &bodies[i].vertices[v_i];
                V2 *to = &bodies[i].vertices[other_v_i];

                V2 line = V2sub(*to, *from);
                float len = V2length(line);
                float diff = len - BOX_SIZE;

                line = V2normalize(line);

                *from = V2add(*from, V2scale(line, diff * 0.5f));
                *to = V2sub(*to, V2scale(line, diff * 0.5f));
            }
        }
    }

    // Reupdate the positions of the bodies based on how the vertices changed
    for (int i = 0; i < num_bodies; i++)
    {
        float upper_right_angle = V2angle(V2sub(bodies[i].vertices[0], bodies[i].body->position));
        bodies[i].body->rotation = upper_right_angle - (PI / 4.0f);

        V2 avg = {0};
        for (int v_i = 0; v_i < 4; v_i++)
        {
            avg = V2add(avg, bodies[i].vertices[v_i]);
        }
        avg = V2scale(avg, 1.0f / 4.0f);
        bodies[i].body->position = avg;
    }

    free(bodies);
}