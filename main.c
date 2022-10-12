//------------------------------------------------------------------------------
//  Take flight
//------------------------------------------------------------------------------

#define SOKOL_IMPL
#define SOKOL_D3D11
#include "sokol_gfx.h"
#include "sokol_gp.h"
#include "sokol_app.h"
#include "sokol_glue.h"
#include <winsock.h>

#include "types.h"

static struct GameState gs = {0};
static int myplayer = -1;
static bool mouse_down = false;
static bool keydown[SAPP_KEYCODE_MENU] = {0};
static float funval = 0.0f; // easy to play with value controlled by left mouse button when held down @BeforeShip remove on release builds
static bool is_host = false;

void init(void)
{
    // @BeforeShip make all fprintf into logging to file, warning dialog boxes on failure instead of exit(-1), replace the macros in sokol with this as well, like assert

    sg_desc sgdesc = {.context = sapp_sgcontext()};
    sg_setup(&sgdesc);
    if (!sg_isvalid())
    {
        fprintf(stderr, "Failed to create Sokol GFX context!\n");
        exit(-1);
    }

    gs.boxes[0] = (struct Box){
        .body = (struct Body){
            .position = (P2){.x = 0.75f, .y = 0.0}},
    };
    gs.boxes[0].body.old_position = gs.boxes[0].body.position;
    gs.boxes[1] = (struct Box){
        .body = (struct Body){
            .position = (P2){.x = 0.75f, .y = 0.5f}},
    };
    gs.boxes[1].body.old_position = gs.boxes[1].body.position;
    gs.num_boxes = 2;

    sgp_desc sgpdesc = {0};
    sgp_setup(&sgpdesc);
    if (!sgp_is_valid())
    {
        fprintf(stderr, "Failed to create Sokol GP context: %s\n", sgp_get_error_message(sgp_get_last_error()));
        exit(-1);
    }
}

void frame(void)
{
    int width = sapp_width(), height = sapp_height();
    float ratio = width / (float)height;
    float time = sapp_frame_count() * sapp_frame_duration();

    // gameplay
    struct ClientToServer curmsg = {0};
    {
        V2 input = (V2){
            .x = (float)keydown[SAPP_KEYCODE_D] - (float)keydown[SAPP_KEYCODE_A],
            .y = (float)keydown[SAPP_KEYCODE_S] - (float)keydown[SAPP_KEYCODE_W],
        };
        curmsg.input = input;

        process(&gs, (float)sapp_frame_duration());
    }

    // drawing
    {
        sgp_begin(width, height);
        sgp_viewport(0, 0, width, height);
        // sgp_project(-ratio, ratio, 1.0f, -1.0f);
        sgp_project(0.0f, width, 0.0f, height);

        // Draw background color
        sgp_set_color(0.1f, 0.1f, 0.1f, 1.0f);
        sgp_clear();

        // Drawing in world space now
        sgp_translate(width / 2, height / 2);
        sgp_scale_at(300.0f + funval, 300.0f + funval, 0.0f, 0.0f);
        if(myplayer != -1)
        {
            sgp_translate(-gs.players[myplayer].body.position.x, -gs.players[myplayer].body.position.y);
        }

        sgp_set_color(1.0f, 1.0f, 1.0f, 1.0f);
        sgp_draw_filled_rect(100.0f, 100.0f, 400.0f, 400.0f);

        // stars
        const int num = 50;
        for (int x = -num; x < num; x++)
        {
            for (int y = -num; y < num; y++)
            {
                sgp_draw_point((float)x * 0.1f, (float)y * 0.1f);
            }
        }

        float halfbox = BOX_SIZE / 2.0f;

        // player
        for(int i = 0; i < MAX_PLAYERS; i++){
            struct Player * p = &gs.players[i];
            if(!p->connected)
                continue;
            sgp_set_color(1.0f, 1.0f, 1.0f, 1.0f);
            sgp_draw_filled_rect(p->body.position.x - halfbox, p->body.position.y - halfbox, BOX_SIZE, BOX_SIZE);
        }

        // boxes
        {
            sgp_set_color(0.5f, 0.5f, 0.5f, 1.0f);
            for (int i = 0; i < gs.num_boxes; i++)
            {
                sgp_draw_filled_rect(gs.boxes[i].body.position.x - halfbox, gs.boxes[i].body.position.y - halfbox, BOX_SIZE, BOX_SIZE);
            }
        }

        // sgp_draw_line(5.0f, 5.0f, 5.0f, 10.0f);
        // sgp_draw_line()
        // sgp_rotate_at(time, 0.0f, 0.0f);

        // Begin a render pass.
        sg_pass_action pass_action = {0};
        sg_begin_default_pass(&pass_action, width, height);
        sgp_flush();
        sgp_end();
        sg_end_pass();
        sg_commit();
    }
}

void cleanup(void)
{
    sgp_shutdown();
    sg_shutdown();
}

void event(const sapp_event *e)
{
    switch (e->type)
    {
    case SAPP_EVENTTYPE_KEY_DOWN:
        keydown[e->key_code] = true;
        break;
    case SAPP_EVENTTYPE_KEY_UP:
        keydown[e->key_code] = false;
        break;
    case SAPP_EVENTTYPE_MOUSE_DOWN:
        if (e->mouse_button == SAPP_MOUSEBUTTON_LEFT)
            mouse_down = true;
        break;
    case SAPP_EVENTTYPE_MOUSE_UP:
        if (e->mouse_button == SAPP_MOUSEBUTTON_LEFT)
            mouse_down = false;
        break;
    case SAPP_EVENTTYPE_MOUSE_MOVE:
        if (mouse_down)
        {
            funval += e->mouse_dx;
            printf("Funval %f\n", funval);
        }
        break;
    }
}

sapp_desc sokol_main(int argc, char *argv[])
{
    is_host = argc >= 1;
    (void)argv;
    return (sapp_desc){
        .init_cb = init,
        .frame_cb = frame,
        .cleanup_cb = cleanup,
        .width = 640,
        .height = 480,
        .gl_force_gles2 = true,
        .window_title = "Flight",
        .icon.sokol_default = true,
        .event_cb = event,
        .win32_console_attach = true,
        .sample_count = 4, // anti aliasing
    };
}