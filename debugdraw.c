#include "types.h"
#include "buildsettings.h"

#ifdef DEBUG_RENDERING
#include "sokol_gfx.h"
#include "sokol_gp.h"
#endif

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
        cpVect center;

        // line
        struct
        {
            cpVect from;
            cpVect to;
        };
    };
} Command;

// thread local variables so debug drawing in server thread
// doesn't fuck up main thread


static THREADLOCAL Command commands[MAX_COMMANDS] = {0};
static THREADLOCAL int command_i = 0;

void dbg_drawall()
{
#ifdef DEBUG_RENDERING
    sgp_set_color(0.4f, 0.8f, 0.2f, 0.8f);
    for (int i = 0; i < command_i; i++)
    {
        const double size = 0.05;
        switch (commands[i].type)
        {
        case rect:
        {
            cpVect center = commands[i].center;
            cpVect upper_left = cpvadd(center, (cpVect){.x = -size / 2.0, .y = -size / 2.0});
            sgp_draw_filled_rect((float)upper_left.x, (float)upper_left.y, (float)size, (float)size);
            break;
        }
        case line:
        {
            cpVect from = commands[i].from;
            cpVect to = commands[i].to;
            sgp_draw_line((float)from.x, (float)from.y, (float)to.x, (float)to.y);
            break;
        }
        }
    }
#endif
    command_i = 0;
}

void dbg_line(cpVect from, cpVect to)
{
    commands[command_i] = (Command){
        .type = line,
        .from = from,
        .to = to,
    };
    command_i++;
    command_i %= MAX_COMMANDS;
}

void dbg_rect(cpVect center)
{
    commands[command_i] = (Command){
        .type = rect,
        .center = center,
    };
    command_i++;
    command_i %= MAX_COMMANDS;
}
