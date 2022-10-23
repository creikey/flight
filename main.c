//------------------------------------------------------------------------------
//  Take flight
//------------------------------------------------------------------------------

#define SOKOL_IMPL
#define SOKOL_D3D11
#include "sokol_gfx.h"
#include "sokol_gp.h"
#include "sokol_app.h"
#include "sokol_glue.h"
#include "sokol_time.h"
#include <enet/enet.h>
#include <process.h> // starting server thread

#include "types.h"

static struct GameState gs = {0};
static int myplayer = -1;
static bool mouse_down = false;
static bool keydown[SAPP_KEYCODE_MENU] = {0};
typedef struct KeyPressed
{
    bool pressed;
    uint64_t frame;
} KeyPressed;
static KeyPressed keypressed[SAPP_KEYCODE_MENU] = {0};
static float funval = 0.0f; // easy to play with value controlled by left mouse button when held down @BeforeShip remove on release builds
static ENetHost *client;
static ENetPeer *peer;

void init(void)
{
    // @BeforeShip make all fprintf into logging to file, warning dialog grids on failure instead of exit(-1), replace the macros in sokol with this as well, like assert

    initialize(&gs);

    sg_desc sgdesc = {.context = sapp_sgcontext()};
    sg_setup(&sgdesc);
    if (!sg_isvalid())
    {
        fprintf(stderr, "Failed to create Sokol GFX context!\n");
        exit(-1);
    }

    sgp_desc sgpdesc = {0};
    sgp_setup(&sgpdesc);
    if (!sgp_is_valid())
    {
        fprintf(stderr, "Failed to create Sokol GP context: %s\n", sgp_get_error_message(sgp_get_last_error()));
        exit(-1);
    }

    // socket initialization
    {
        if (enet_initialize() != 0)
        {
            fprintf(stderr, "An error occurred while initializing ENet.\n");
            exit(-1);
        }
        client = enet_host_create(NULL /* create a client host */,
                                  1 /* only allow 1 outgoing connection */,
                                  2 /* allow up 2 channels to be used, 0 and 1 */,
                                  0 /* assume any amount of incoming bandwidth */,
                                  0 /* assume any amount of outgoing bandwidth */);
        if (client == NULL)
        {
            fprintf(stderr,
                    "An error occurred while trying to create an ENet client host.\n");
            exit(-1);
        }
        ENetAddress address;
        ENetEvent event;

        enet_address_set_host(&address, "127.0.0.1");
        address.port = 8000;
        peer = enet_host_connect(client, &address, 2, 0);
        if (peer == NULL)
        {
            fprintf(stderr,
                    "No available peers for initiating an ENet connection.\n");
            exit(-1);
        }
        /* Wait up to 5 seconds for the connection attempt to succeed. */
        if (enet_host_service(client, &event, 1000) > 0 &&
            event.type == ENET_EVENT_TYPE_CONNECT)
        {
            Log("Connected\n");
        }
        else
        {
            /* Either the 5 seconds are up or a disconnect event was */
            /* received. Reset the peer in the event the 5 seconds   */
            /* had run out without any significant event.            */
            enet_peer_reset(peer);
            fprintf(stderr, "Connection to server failed.");
            exit(-1);
        }
    }
}

static void frame(void)
{
    int width = sapp_width(), height = sapp_height();
    float ratio = width / (float)height;
    float time = sapp_frame_count() * sapp_frame_duration();
    float dt = sapp_frame_duration();

    for (int i = 0; i < SAPP_KEYCODE_MENU; i++)
    {
        if (keypressed[i].frame < sapp_frame_count())
        {
            keypressed[i].pressed = false;
        }
    }

    // networking
    {
        ENetEvent event;
        while (true)
        {
            int enet_status = enet_host_service(client, &event, 0);
            if (enet_status > 0)
            {
                switch (event.type)
                {
                case ENET_EVENT_TYPE_CONNECT:
                    Log("New client from host %x\n", event.peer->address.host);
                    break;
                case ENET_EVENT_TYPE_RECEIVE:
                    // @Robust @BeforeShip use some kind of serialization strategy that checks for out of bounds
                    // and other validation instead of just casting to a struct
                    // "Alignment of structure members can be different even among different compilers on the same platform, let alone different platforms."
                    // ^^ need serialization strategy that accounts for this if multiple platforms is happening https://stackoverflow.com/questions/28455163/how-can-i-portably-send-a-c-struct-through-a-network-socket
                    struct ServerToClient msg = {
                        .cur_gs = &gs,
                    };
                    // @Robust @BeforeShip maximum acceptable message size?
                    from_bytes(&msg, event.packet->data, event.packet->dataLength);
                    myplayer = msg.your_player;
                    enet_packet_destroy(event.packet);
                    break;
                case ENET_EVENT_TYPE_DISCONNECT:
                    fprintf(stderr, "Disconnected from server\n");
                    exit(-1);
                    break;
                }
            }
            else if (enet_status == 0)
            {
                break;
            }
            else if (enet_status < 0)
            {
                fprintf(stderr, "Error receiving enet events: %d\n", enet_status);
                break;
            }
        }
    }

    // gameplay
    {
        // @Robust accumulate total time and send input at rate like 20 hz, not every frame
        struct ClientToServer curmsg = {0};
        V2 input = (V2){
            .x = (float)keydown[SAPP_KEYCODE_D] - (float)keydown[SAPP_KEYCODE_A],
            .y = (float)keydown[SAPP_KEYCODE_S] - (float)keydown[SAPP_KEYCODE_W],
        };
        curmsg.movement = input;
        curmsg.inhabit = keypressed[SAPP_KEYCODE_G].pressed;

        ENetPacket *packet = enet_packet_create((void *)&curmsg, sizeof(curmsg), ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT);
        enet_peer_send(peer, 0, packet);

        // @BeforeShip client side prediction and rollback to previous server authoritative state, then replay inputs
        // no need to store copies of game state, just player input frame to frame. Then know how many frames ago the server game state arrived, it's that easy!
        process(&gs, (float)sapp_frame_duration());
    }

    // drawing
    {
        sgp_begin(width, height);
        sgp_viewport(0, 0, width, height);
        sgp_project(0.0f, width, 0.0f, height);
        sgp_set_blend_mode(SGP_BLENDMODE_BLEND);

        // Draw background color
        sgp_set_color(0.1f, 0.1f, 0.1f, 1.0f);
        sgp_clear();

        // Drawing in world space now
        sgp_translate(width / 2, height / 2);
        sgp_scale_at(300.0f + funval, 300.0f + funval, 0.0f, 0.0f);

        // camera go to player
        if (myplayer != -1)
        {
            V2 pos = gs.players[myplayer].pos;
            sgp_translate(-pos.x, -pos.y);
        }

        sgp_set_color(1.0f, 1.0f, 1.0f, 1.0f);

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
        for (int i = 0; i < MAX_PLAYERS; i++)
        {
            struct Player *p = &gs.players[i];
            if (!p->connected)
                continue;
            static float opacities[MAX_PLAYERS] = {1.0f};
            opacities[i] = lerp(opacities[i], p->currently_inhabiting_index == -1 ? 1.0f : 0.1f, dt * 7.0f);
            sgp_set_color(1.0f, 1.0f, 1.0f, opacities[i]);
            sgp_push_transform();
            float psize = 0.1f;
            sgp_draw_filled_rect(p->pos.x - psize / 2.0f, p->pos.y - psize / 2.0f, psize, psize);
            sgp_pop_transform();
            // sgp_rotate_at(grid_rotation(p->grid), grid_pos(p->grid).x, grid_pos(p->grid).y);
            // V2 bpos = grid_pos(p->grid);
            // sgp_draw_filled_rect(grid_pos(p->grid).x - halfbox, grid_pos(p->grid).y - halfbox, BOX_SIZE, BOX_SIZE);
            // sgp_pop_transform();

            // sgp_set_color(1.0f, 0.0f, 0.0f, 1.0f);
            // V2 vel = grid_vel(p->grid);
            // V2 to = V2add(grid_pos(p->grid), vel);
            // sgp_draw_line(grid_pos(p->grid).x, grid_pos(p->grid).y, to.x, to.y);
        }

        // grids
        {
            for (int i = 0; i < MAX_GRIDS; i++)
            {
                SKIPNULL(gs.grids[i].body);
                struct Grid *g = &gs.grids[i];
                for (int ii = 0; ii < MAX_BOXES_PER_GRID; ii++)
                {
                    SKIPNULL(g->boxes[ii].shape);
                    struct Box *b = &g->boxes[ii];
                    sgp_set_color(0.5f, 0.5f, 0.5f, 1.0f);
                    sgp_push_transform();
                    sgp_rotate_at(box_rotation(b), grid_pos(g).x, grid_pos(g).y);
                    V2 bpos = box_pos(b);
                    sgp_draw_line(bpos.x - halfbox, bpos.y - halfbox, bpos.x - halfbox, bpos.y + halfbox); // left
                    sgp_draw_line(bpos.x - halfbox, bpos.y - halfbox, bpos.x + halfbox, bpos.y - halfbox); // top
                    sgp_draw_line(bpos.x + halfbox, bpos.y - halfbox, bpos.x + halfbox, bpos.y + halfbox); // right
                    sgp_draw_line(bpos.x - halfbox, bpos.y + halfbox, bpos.x + halfbox, bpos.y + halfbox); // bottom
                    sgp_draw_line(bpos.x - halfbox, bpos.y - halfbox, bpos.x + halfbox, bpos.y + halfbox); // diagonal

                    if (b->damage > 0.01f)
                    {
                        Log("Damage: %f\n", b->damage);
                    }
                    sgp_set_color(0.5f, 0.1f, 0.1f, b->damage);
                    sgp_draw_filled_rect(box_pos(b).x - halfbox, box_pos(b).y - halfbox, BOX_SIZE, BOX_SIZE);
                    sgp_pop_transform();
                }
                sgp_set_color(1.0f, 0.0f, 0.0f, 1.0f);
                V2 vel = grid_vel(&gs.grids[i]);
                V2 to = V2add(grid_com(g), vel);
                sgp_draw_line(grid_com(g).x, grid_com(g).y, to.x, to.y);
            }
        }

        sgp_set_color(1.0f, 1.0f, 1.0f, 1.0f);
        dbg_drawall();

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
    destroy(&gs);
    sgp_shutdown();
    sg_shutdown();
    enet_deinitialize();
}

void event(const sapp_event *e)
{
    switch (e->type)
    {
    case SAPP_EVENTTYPE_KEY_DOWN:
        keydown[e->key_code] = true;
        if (keypressed[e->key_code].frame == 0)
        {
            keypressed[e->key_code].pressed = true;
            keypressed[e->key_code].frame = e->frame_count;
        }
        break;
    case SAPP_EVENTTYPE_KEY_UP:
        keydown[e->key_code] = false;
        keypressed[e->key_code].pressed = false;
        keypressed[e->key_code].frame = 0;
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
            Log("Funval %f\n", funval);
        }
        break;
    }
}

sapp_desc sokol_main(int argc, char *argv[])
{
    if (argc > 1)
    {
        _beginthread(server, 0, NULL);
    }
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