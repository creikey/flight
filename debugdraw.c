#include "sokol_gfx.h"
#include "sokol_gp.h"
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

#if defined(WIN32) || defined(_WIN32) || defined(__WIN32__) || defined(__NT__)
#define THREADLOCAL __declspec(thread)
#else
#define THREADLOCAL __thread
#endif

static THREADLOCAL Command commands[MAX_COMMANDS] = {0};
static THREADLOCAL int command_i = 0;

void dbg_drawall()
{
    // return;
    sgp_set_color(0.4f, 0.8f, 0.2f, 0.8f);
    for (int i = 0; i < command_i; i++)
    {
        const float size = 0.05f;
        switch (commands[i].type)
        {
        case rect:
        {
            V2 center = commands[i].center;
            V2 upper_left = V2add(center, (V2){.x = -size / 2.0f, .y = -size / 2.0f});
            sgp_draw_filled_rect(upper_left.x, upper_left.y, size, size);
            break;
        }
        case line:
        {
            V2 from = commands[i].from;
            V2 to = commands[i].to;
            sgp_draw_line(from.x, from.y, to.x, to.y);
            break;
        }
        }
    }
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