#ifdef DEBUG_RENDERING
#include "sokol_gfx.h"
#include "sokol_gp.h"
#endif
#include "types.h"

#define MAX_COMMANDS 64

typedef struct Command
{
    enum
    {
        rect,
        line
    } type;
    union
    {
        // rect
        V2 center;

        // line
        struct
        {
            V2 from;
            V2 to;
        };
    };
} Command;

// thread local variables so debug drawing in server thread
// doesn't fuck up main thread


static THREADLOCAL Command commands[MAX_COMMANDS] = {0};
static THREADLOCAL int command_i = 0;

void dbg_drawall()
{
    // return;

#ifdef DEBUG_RENDERING
    sgp_set_color(0.4f, 0.8f, 0.2f, 0.8f);
    for (int i = 0; i < command_i; i++)
    {
        const double size = 0.05;
        switch (commands[i].type)
        {
        case rect:
        {
            V2 center = commands[i].center;
            V2 upper_left = cpvadd(center, (V2){.x = -size / 2.0, .y = -size / 2.0});
            sgp_draw_filled_rect((float)upper_left.x, (float)upper_left.y, (float)size, (float)size);
            break;
        }
        case line:
        {
            V2 from = commands[i].from;
            V2 to = commands[i].to;
            sgp_draw_line((float)from.x, (float)from.y, (float)to.x, (float)to.y);
            break;
        }
        }
    }
#endif
    command_i = 0;
}

void dbg_line(V2 from, V2 to)
{
    commands[command_i] = (Command){
        .type = line,
        .from = from,
        .to = to,
    };
    command_i++;
    command_i %= MAX_COMMANDS;
}

void dbg_rect(V2 center)
{
    commands[command_i] = (Command){
        .type = rect,
        .center = center,
    };
    command_i++;
    command_i %= MAX_COMMANDS;
}
