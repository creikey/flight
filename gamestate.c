#include "types.h"

// do not use any global variables to process gamestate

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

void process(struct GameState *gs, float dt)
{
    for (int i = 0; i < MAX_PLAYERS; i++)
    {
        struct Player *p = &gs->players[i];
        if (!p->connected)
            continue;
        p->body.acceleration = V2scale(p->input, 5.0f);
        p->body.angular_acceleration = p->input.x * 10.0f;
        integrate_acceleration(&p->body, dt);
    }

    for (int i = 0; i < gs->num_boxes; i++)
    {
        integrate_acceleration(&gs->boxes[i].body, dt);
    }

    
}