#include "types.h"

// do not use any global variables to process gamestate


static void process_body(struct Body *body, float dt)
{
    V2 current = body->position;
    body->position = V2add(body->position, V2sub(current, body->old_position));
    body->position = V2add(body->position, V2scale(body->acceleration, dt*dt));
    body->old_position = current;
}


void process(struct GameState * gs, float dt) {
    for(int i = 0; i < MAX_PLAYERS; i++)
    {
        struct Player * p = &gs->players[i];
        if(!p->connected)
            continue;
        p->body.acceleration = V2scale(p->input, 5.0f);
        process_body(&p->body, dt);
    }

    for(int i = 0; i < gs->num_boxes; i++)
    {
        process_body(&gs->boxes[i].body, dt);
    }
}