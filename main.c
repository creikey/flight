//------------------------------------------------------------------------------

//  Take flight
//------------------------------------------------------------------------------

#define SOKOL_IMPL
#define SOKOL_D3D11
#include <enet/enet.h>
#include <process.h> // starting server thread

#define TOOLBAR_SLOTS 9

#pragma warning(disable : 33010) // this warning is so broken, doesn't
                                 // understand assert()
#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_glue.h"
#include "sokol_gp.h"
#include "sokol_time.h"
#pragma warning(default : 33010)
#pragma warning(disable : 6262) // warning about using a lot of stack, lol
                                // that's how stb image is
#define STB_IMAGE_IMPLEMENTATION
#include <inttypes.h>
#include <string.h> // errno error message on file open

#include "minilzo.h"
#include "opus.h"
#include "queue.h"
#include "stb_image.h"
#include "types.h"

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include "buildsettings.h"
#include "profiling.h"

// shaders
#include "goodpixel.gen.h"
#include "hueshift.gen.h"
static sg_pipeline hueshift_pipeline;
static sg_pipeline goodpixel_pipeline;

static struct GameState gs = {0};
static int my_player_index = -1;
static bool right_mouse_down = false;
#define MAX_KEYDOWN SAPP_KEYCODE_MENU
static bool keydown[MAX_KEYDOWN] = {0};
typedef struct KeyPressed
{
  bool pressed;
  uint64_t frame;
} KeyPressed;
static KeyPressed keypressed[MAX_KEYDOWN] = {0};
static cpVect mouse_pos = {0};
static bool fullscreened = false;
static bool picking_new_boxtype = false;

static bool build_pressed = false;
static bool interact_pressed = false;
#define MAX_MOUSEBUTTON (SAPP_MOUSEBUTTON_MIDDLE + 1)
static bool mousedown[MAX_MOUSEBUTTON] = {0};
typedef struct MousePressed
{
  bool pressed;
  uint64_t frame;
} MousePressed;
static MousePressed mousepressed[MAX_MOUSEBUTTON] = {0};
static EntityID maybe_inviting_this_player = {0};
bool confirm_invite_this_player = false;
bool accept_invite = false;
bool reject_invite = false;
static cpVect camera_pos = {0}; // it being a global variable keeps camera at same
// position after player death
static double player_scaling = 1.0;

static bool mouse_frozen = false; // @BeforeShip make this debug only thing
static double funval =
    0.0; // easy to play with value controlled by left mouse button when held
// down @BeforeShip remove on release builds
static Queue input_queue = {0};
char input_queue_data[QUEUE_SIZE_FOR_ELEMENTS(sizeof(InputFrame), LOCAL_INPUT_QUEUE_MAX)] = {0};
static ENetHost *client;
static ENetPeer *peer;
static double zoom_target = 300.0;
static double zoom = 300.0;
static enum Squad take_over_squad = (enum Squad) - 1; // -1 means not taking over any squad
static double target_prediction_time_factor = 1.0;
static double current_time_ahead_of_server = 0.0;

// images
static sg_image image_itemframe;
static sg_image image_itemframe_selected;
static sg_image image_thrusterburn;
static sg_image image_player;
static sg_image image_cockpit_used;
static sg_image image_stars;
static sg_image image_stars2;
static sg_image image_sun;
static sg_image image_medbay_used;
static sg_image image_mystery;
static sg_image image_explosion;
static sg_image image_low_health;
static sg_image image_mic_muted;
static sg_image image_mic_unmuted;
static sg_image image_flag_available;
static sg_image image_flag_taken;
static sg_image image_squad_invite;
static sg_image image_check;
static sg_image image_no;
static sg_image image_solarpanel_charging;
static sg_image image_scanner_head;
static sg_image image_itemswitch;
static sg_image image_cloaking_panel;
static sg_image image_missile;
static sg_image image_missile_burning;

static enum BoxType toolbar[TOOLBAR_SLOTS] = {
    BoxHullpiece,
    BoxThruster,
    BoxBattery,
    BoxCockpit,
    BoxMedbay,
    BoxSolarPanel,
    BoxScanner,
};
static int cur_toolbar_slot = 0;
static int cur_editing_rotation = Right;

// audio
static bool muted = false;
static ma_device microphone_device;
static ma_device speaker_device;
OpusEncoder *enc;
OpusDecoder *dec;
Queue packets_to_send = {0};
char packets_to_send_data[QUEUE_SIZE_FOR_ELEMENTS(sizeof(OpusPacket),
                                                  VOIP_PACKET_BUFFER_SIZE)];
Queue packets_to_play = {0};
char packets_to_play_data[QUEUE_SIZE_FOR_ELEMENTS(sizeof(OpusPacket),
                                                  VOIP_PACKET_BUFFER_SIZE)];
ma_mutex send_packets_mutex = {0};
ma_mutex play_packets_mutex = {0};

// server thread
void *server_thread_handle = 0;
ServerThreadInfo server_info = {0};

static struct BoxInfo
{
  enum BoxType type;
  const char *image_path;
  sg_image image;
} boxes[] = {
    // if added to here will show up in toolbar, is placeable
    {
        .type = BoxHullpiece,
        .image_path = "loaded/hullpiece.png",
    },
    {
        .type = BoxThruster,
        .image_path = "loaded/thruster.png",
    },
    {
        .type = BoxBattery,
        .image_path = "loaded/battery.png",
    },
    {
        .type = BoxCockpit,
        .image_path = "loaded/cockpit.png",
    },
    {
        .type = BoxMedbay,
        .image_path = "loaded/medbay.png",
    },
    {
        .type = BoxSolarPanel,
        .image_path = "loaded/solarpanel.png",
    },
    {
        .type = BoxExplosive,
        .image_path = "loaded/explosive.png",
    },
    {
        .type = BoxScanner,
        .image_path = "loaded/scanner_base.png",
    },
    {
        .type = BoxGyroscope,
        .image_path = "loaded/gyroscope.png",

    },
    {
        .type = BoxCloaking,
        .image_path = "loaded/cloaking_device.png",
    },
    {
        .type = BoxMissileLauncher,
        .image_path = "loaded/missile_launcher.png",
    },
    {
        .type = BoxMerge,
        .image_path = "loaded/merge.png",
    },
};
#define ENTITIES_ITER(cur)                                                \
  for (Entity *cur = gs.entities; cur < gs.entities + gs.cur_next_entity; \
       cur++)                                                             \
    if (cur->exists)
// suppress compiler warning about ^^ above used in floating point context
#define ARRLENF(arr) ((float)sizeof(arr) / sizeof(*arr))
static struct SquadMeta
{
  enum Squad squad;
  double hue;
  bool is_colorless;
} squad_metas[] = {
    {
        .squad = SquadNone,
        .is_colorless = true,
    },
    {
        .squad = SquadRed,
        .hue = 21.0 / 360.0,
    },
    {
        .squad = SquadGreen,
        .hue = 111.0 / 360.0,
    },
    {
        .squad = SquadBlue,
        .hue = 201.0 / 360.0,
    },
    {
        .squad = SquadPurple,
        .hue = 291.0 / 360.0,
    },
};

struct SquadMeta squad_meta(enum Squad squad)
{
  for (int i = 0; i < ARRLEN(squad_metas); i++)
  {
    if (squad_metas[i].squad == squad)
      return squad_metas[i];
  }
  Log("Could not find squad %d!\n", squad);
  return (struct SquadMeta){0};
}

static enum BoxType currently_building()
{
  assert(cur_toolbar_slot >= 0);
  assert(cur_toolbar_slot < TOOLBAR_SLOTS);
  return toolbar[cur_toolbar_slot];
}

struct BoxInfo boxinfo(enum BoxType type)
{
  for (int i = 0; i < ARRLEN(boxes); i++)
  {
    if (boxes[i].type == type)
      return boxes[i];
  }
  Log("No box info found for type %d\n", type);
  return (struct BoxInfo){0};
}

static sg_image load_image(const char *path)
{
  sg_image to_return = sg_alloc_image();

  int x = 0;
  int y = 0;
  int comp = 0;
  const int desired_channels = 4;
  stbi_set_flip_vertically_on_load(true);
  stbi_uc *image_data = stbi_load(path, &x, &y, &comp, desired_channels);
  if (!image_data)
  {
    fprintf(stderr, "Failed to load %s image: %s\n", path,
            stbi_failure_reason());
    exit(-1);
  }
  sg_init_image(to_return,
                &(sg_image_desc){.width = x,
                                 .height = y,
                                 .pixel_format = SG_PIXELFORMAT_RGBA8,
                                 .min_filter = SG_FILTER_LINEAR,
                                 .mag_filter = SG_FILTER_LINEAR,
                                 .wrap_u = SG_WRAP_CLAMP_TO_EDGE,
                                 .data.subimage[0][0] = {
                                     .ptr = image_data,
                                     .size = (size_t)(x * y * desired_channels),
                                 }});

  stbi_image_free(image_data);

  return to_return;
}

void microphone_data_callback(ma_device *pDevice, void *pOutput,
                              const void *pInput, ma_uint32 frameCount)
{
  assert(frameCount == VOIP_EXPECTED_FRAME_COUNT);
#if 0 // print audio data
  Log("Mic data: ");
  for (ma_uint32 i = 0; i < VOIP_EXPECTED_FRAME_COUNT; i++)
  {
    printf("%d ", ((const opus_int16 *)pInput)[i]);
  }
  printf("\n");
#endif
  if (peer != NULL)
  {
    ma_mutex_lock(&send_packets_mutex);
    OpusPacket *packet = queue_push_element(&packets_to_send);
    if (packet == NULL)
    {
      queue_clear(&packets_to_send);
      packet = queue_push_element(&packets_to_send);
    }
    assert(packet != NULL);
    {
      opus_int16 muted_audio[VOIP_EXPECTED_FRAME_COUNT] = {0};
      const opus_int16 *audio_buffer = (const opus_int16 *)pInput;
      if (muted)
        audio_buffer = muted_audio;
      opus_int32 written =
          opus_encode(enc, audio_buffer, VOIP_EXPECTED_FRAME_COUNT,
                      packet->data, VOIP_PACKET_MAX_SIZE);
      packet->length = written;
    }
    ma_mutex_unlock(&send_packets_mutex);
  }
  (void)pOutput;
}

void speaker_data_callback(ma_device *pDevice, void *pOutput,
                           const void *pInput, ma_uint32 frameCount)
{
  assert(frameCount == VOIP_EXPECTED_FRAME_COUNT);
  ma_mutex_lock(&play_packets_mutex);
  OpusPacket *cur_packet = (OpusPacket *)queue_pop_element(&packets_to_play);
  if (cur_packet != NULL)
  {
    opus_decode(dec, cur_packet->data, cur_packet->length,
                (opus_int16 *)pOutput, frameCount, 0);
  }
  else
  {
    opus_decode(dec, NULL, 0, (opus_int16 *)pOutput, frameCount,
                0); // I think opus makes it sound good if packets are skipped
                    // with null
  }
  ma_mutex_unlock(&play_packets_mutex);
  (void)pInput;
}

static Player *myplayer()
{
  if (my_player_index == -1)
    return NULL;
  return &gs.players[my_player_index];
}

static Entity *myentity()
{
  if (myplayer() == NULL)
    return NULL;
  Entity *to_return = get_entity(&gs, myplayer()->entity);
  if (to_return != NULL)
    assert(to_return->is_player);
  return to_return;
}

void recalculate_camera_pos()
{
  if (myentity() != NULL)
  {
    camera_pos = entity_pos(myentity());
  }
}

// drawing

#define WHITE                                  \
  (Color)                                      \
  {                                            \
    .r = 1.0f, .g = 1.0f, .b = 1.0f, .a = 1.0f \
  }
#define RED                                    \
  (Color)                                      \
  {                                            \
    .r = 1.0f, .g = 0.0f, .b = 0.0f, .a = 1.0f \
  }
#define BLUE                                   \
  (Color)                                      \
  {                                            \
    .r = 0.0f, .g = 0.0f, .b = 1.0f, .a = 1.0f \
  }
#define GOLD colhex(255, 215, 0)

typedef struct Color
{
  float r, g, b, a;
} Color;

static Color colhex(int r, int g, int b)
{
  return (Color){
      .r = (float)r / 255.0f,
      .g = (float)g / 255.0f,
      .b = (float)b / 255.0f,
      .a = 1.0f,
  };
}

static Color colhexcode(int hexcode)
{
  // 0x020509;
  int r = (hexcode >> 16) & 0xFF;
  int g = (hexcode >> 8) & 0xFF;
  int b = (hexcode >> 0) & 0xFF;
  return colhex(r, g, b);
}

static Color Collerp(Color a, Color b, double factor)
{
  Color to_return = {0};
  to_return.r = (float)lerp(a.r, b.r, factor);
  to_return.g = (float)lerp(a.g, b.g, factor);
  to_return.b = (float)lerp(a.b, b.b, factor);
  to_return.a = (float)lerp(a.a, b.a, factor);

  return to_return;
}

static void set_color(Color c)
{
  sgp_set_color(c.r, c.g, c.b, c.a);
}

// sokol_gp uses floats, gameplay is in doubles. This is my destiny!

void set_color_values(double r, double g, double b, double a)
{
  sgp_set_color((float)r, (float)g, (float)b, (float)a);
}

// @Robust make the transform stack actually use double precision logic, and
// fix the debug drawing after that as well
void translate(double x, double y)
{
  sgp_translate((float)x, (float)y);
}
void rotate_at(double theta, double x, double y)
{
  sgp_rotate_at((float)theta, (float)x, (float)y);
}
void scale_at(double sx, double sy, double x, double y)
{
  sgp_scale_at((float)sx, (float)sy, (float)x, (float)y);
}

void draw_filled_rect(double x, double y, double w, double h)
{
  sgp_draw_filled_rect((float)x, (float)y, (float)w, (float)h);
}

void draw_line(double ax, double ay, double bx, double by)
{
  sgp_draw_line((float)ax, (float)ay, (float)bx, (float)by);
}
void draw_textured_rect(double x, double y, double w, double h)
{
  sgp_draw_textured_rect((float)x, (float)y, (float)w, (float)h);
}

static void init(void)
{
  queue_init(&packets_to_play, sizeof(OpusPacket), packets_to_play_data,
             ARRLEN(packets_to_play_data));
  queue_init(&packets_to_send, sizeof(OpusPacket), packets_to_send_data,
             ARRLEN(packets_to_send_data));
  queue_init(&input_queue, sizeof(InputFrame), input_queue_data,
             ARRLEN(input_queue_data));

  // audio
  {
    // opus
    {
      int error;
      enc = opus_encoder_create(VOIP_SAMPLE_RATE, 1, OPUS_APPLICATION_VOIP,
                                &error);
      assert(error == OPUS_OK);
      dec = opus_decoder_create(VOIP_SAMPLE_RATE, 1, &error);
      assert(error == OPUS_OK);
    }

    ma_device_config microphone_config =
        ma_device_config_init(ma_device_type_capture);

    microphone_config.capture.format = ma_format_s16;
    microphone_config.capture.channels = 1;
    microphone_config.sampleRate = VOIP_SAMPLE_RATE;
    microphone_config.dataCallback = microphone_data_callback;

    ma_device_config speaker_config =
        ma_device_config_init(ma_device_type_playback);
    speaker_config.playback.format = ma_format_s16;
    speaker_config.playback.channels = 1;
    speaker_config.sampleRate = VOIP_SAMPLE_RATE;
    speaker_config.dataCallback = speaker_data_callback;

    ma_result result;

    result = ma_device_init(NULL, &microphone_config, &microphone_device);
    if (result != MA_SUCCESS)
    {
      Log("Failed to initialize capture device.\n");
      exit(-1);
    }

    result = ma_device_init(NULL, &speaker_config, &speaker_device);
    if (result != MA_SUCCESS)
    {
      ma_device_uninit(&microphone_device);
      Log("Failed to init speaker\n");
      exit(-1);
    }

    if (ma_mutex_init(&send_packets_mutex) != MA_SUCCESS)
      Log("Failed to init send mutex\n");
    if (ma_mutex_init(&play_packets_mutex) != MA_SUCCESS)
      Log("Failed to init play mutex\n");

    result = ma_device_start(&microphone_device);
    if (result != MA_SUCCESS)
    {
      ma_device_uninit(&microphone_device);
      Log("Failed to start device.\n");
      exit(-1);
    }

    result = ma_device_start(&speaker_device);
    if (result != MA_SUCCESS)
    {
      ma_device_uninit(&microphone_device);
      ma_device_uninit(&speaker_device);
      Log("Failed to start speaker\n");
      exit(-1);
    }

    Log("Initialized audio\n");
  }

  // @BeforeShip make all fprintf into logging to file, warning dialog grids on
  // failure instead of exit(-1), replace the macros in sokol with this as well,
  // like assert

  Entity *entity_data = malloc(sizeof *entity_data * MAX_ENTITIES);
  initialize(&gs, entity_data, sizeof *entity_data * MAX_ENTITIES);

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
    fprintf(stderr, "Failed to create Sokol GP context: %s\n",
            sgp_get_error_message(sgp_get_last_error()));
    exit(-1);
  }

  // shaders
  {
    // initialize shader
    {
      {
        sgp_pipeline_desc pip_desc = {
            .shader = *hueshift_program_shader_desc(sg_query_backend()),
            .blend_mode = SGP_BLENDMODE_BLEND,
        };
        hueshift_pipeline = sgp_make_pipeline(&pip_desc);
        if (sg_query_pipeline_state(hueshift_pipeline) != SG_RESOURCESTATE_VALID)
        {
          fprintf(stderr, "failed to make hueshift pipeline\n");
          exit(-1);
        }
      }

      {
        sgp_pipeline_desc pip_desc = {
            .shader = *goodpixel_program_shader_desc(sg_query_backend()),
            .blend_mode = SGP_BLENDMODE_BLEND,
        };

        goodpixel_pipeline = sgp_make_pipeline(&pip_desc);
        if (sg_query_pipeline_state(goodpixel_pipeline) != SG_RESOURCESTATE_VALID)
        {
          fprintf(stderr, "failed to make goodpixel pipeline\n");
          exit(-1);
        }
      }

      // images loading
      {
        for (int i = 0; i < ARRLEN(boxes); i++)
        {
          boxes[i].image = load_image(boxes[i].image_path);
        }
        image_thrusterburn = load_image("loaded/thrusterburn.png");
        image_itemframe = load_image("loaded/itemframe.png");
        image_itemframe_selected = load_image("loaded/itemframe_selected.png");
        image_player = load_image("loaded/player.png");
        image_cockpit_used = load_image("loaded/cockpit_used.png");
        image_stars = load_image("loaded/stars.png");
        image_stars2 = load_image("loaded/stars2.png");
        image_sun = load_image("loaded/sun.png");
        image_medbay_used = load_image("loaded/medbay_used.png");
        image_mystery = load_image("loaded/mystery.png");
        image_explosion = load_image("loaded/explosion.png");
        image_low_health = load_image("loaded/low_health.png");
        image_mic_muted = load_image("loaded/mic_muted.png");
        image_mic_unmuted = load_image("loaded/mic_unmuted.png");
        image_flag_available = load_image("loaded/flag_available.png");
        image_flag_taken = load_image("loaded/flag_ripped.png");
        image_squad_invite = load_image("loaded/squad_invite.png");
        image_check = load_image("loaded/check.png");
        image_no = load_image("loaded/no.png");
        image_solarpanel_charging = load_image("loaded/solarpanel_charging.png");
        image_scanner_head = load_image("loaded/scanner_head.png");
        image_itemswitch = load_image("loaded/itemswitch.png");
        image_cloaking_panel = load_image("loaded/cloaking_panel.png");
        image_missile_burning = load_image("loaded/missile_burning.png");
        image_missile = load_image("loaded/missile.png");
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
          fprintf(
              stderr,
              "An error occurred while trying to create an ENet client host.\n");
          exit(-1);
        }
        ENetAddress address;
        ENetEvent event;

        enet_address_set_host(&address, SERVER_ADDRESS);
        Log("Connecting to %s:%d\n", SERVER_ADDRESS, SERVER_PORT);
        address.port = SERVER_PORT;
        peer = enet_host_connect(client, &address, 2, 0);
        if (peer == NULL)
        {
          fprintf(stderr,
                  "No available peers for initiating an ENet connection.\n");
          exit(-1);
        }
        // the timeout is the third parameter here
        if (enet_host_service(client, &event, 5000) > 0 &&
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
  }
}

#define transform_scope DeferLoop(sgp_push_transform(), sgp_pop_transform())

static void set_pipeline_and_pull_color(sg_pipeline pip)
{
  sgp_set_pipeline(pip);
  sgp_set_uniform(&sgp_query_state()->color, sizeof(sgp_query_state()->color));
}

#define pipeline_scope(pipeline) DeferLoop(set_pipeline_and_pull_color(pipeline), sgp_reset_pipeline())

static void draw_color_rect_centered(cpVect center, double size)
{
  double halfbox = size / 2.0;
  draw_filled_rect(center.x - halfbox, center.y - halfbox, size, size);
}

static void draw_texture_rectangle_centered(cpVect center, cpVect width_height)
{
  cpVect halfsize = cpvmult(width_height, 0.5);
  draw_textured_rect(center.x - halfsize.x, center.y - halfsize.y, width_height.x, width_height.y);
}
static void draw_texture_centered(cpVect center, double size)
{
  draw_texture_rectangle_centered(center, (cpVect){size, size});
}

sgp_point V2point(cpVect v)
{
  return (sgp_point){.x = (float)v.x, .y = (float)v.y};
}

cpVect pointV2(sgp_point p)
{
  return (cpVect){.x = p.x, .y = p.y};
}

static void draw_circle(cpVect point, double radius)
{
#define POINTS 64
  sgp_line lines[POINTS];
  for (int i = 0; i < POINTS; i++)
  {
    double progress = (float)i / (float)POINTS;
    double next_progress = (float)(i + 1) / (float)POINTS;
    lines[i].a = V2point((cpVect){.x = cos(progress * 2.0 * PI) * radius,
                                  .y = sin(progress * 2.0 * PI) * radius});
    lines[i].b = V2point((cpVect){.x = cos(next_progress * 2.0 * PI) * radius,
                                  .y = sin(next_progress * 2.0 * PI) * radius});
    lines[i].a = V2point(cpvadd(pointV2(lines[i].a), point));
    lines[i].b = V2point(cpvadd(pointV2(lines[i].b), point));
  }
  sgp_draw_lines(lines, POINTS);
}

bool can_build(int i)
{
  bool allow_building = true;
  enum BoxType box_type = (enum BoxType)i;

  allow_building = false;
  if (myplayer() != NULL)
    allow_building = box_unlocked(myplayer(), box_type);
  return allow_building;
}
static void setup_hueshift(enum Squad squad)
{
  struct SquadMeta meta = squad_meta(squad);
  hueshift_uniforms_t uniform = {
      .is_colorless = meta.is_colorless,
      .target_hue = (float)meta.hue,
      .alpha = sgp_get_color().a,
  };
  sgp_set_uniform(&uniform, sizeof(hueshift_uniforms_t));
}

static cpVect screen_to_world(double width, double height, cpVect screen)
{
  cpVect world = screen;
  world = cpvsub(world, (cpVect){.x = width / 2.0, .y = height / 2.0});
  world.x /= zoom;
  world.y /= -zoom;
  world = cpvadd(world, camera_pos);
  return world;
}

static cpVect world_to_screen(double width, double height, cpVect world)
{
  cpVect screen = world;
  screen = cpvsub(screen, camera_pos);
  screen.x *= zoom;
  screen.y *= -zoom;
  screen = cpvadd(screen, (cpVect){.x = width / 2.0, .y = height / 2.0});
  return screen;
}

static void ui(bool draw, double dt, double width, double height)
{
  static double cur_opacity = 1.0;
  cur_opacity = lerp(cur_opacity, myentity() != NULL ? 1.0 : 0.0, dt * 4.0);
  if (cur_opacity <= 0.01)
  {
    return;
  }

  if (draw)
    sgp_push_transform();

  // draw pick new box type menu
  static double pick_opacity = 0.0;
  {
    if (keypressed[SAPP_KEYCODE_ESCAPE].pressed)
      picking_new_boxtype = false;
    AABB pick_modal = (AABB){
        .x = width * 0.25,
        .y = height * 0.25,
        .width = width * 0.5,
        .height = height * 0.5,
    };
    pick_opacity = lerp(pick_opacity, picking_new_boxtype ? 1.0 : 0.0, dt * 7.0);
    if (picking_new_boxtype)
    {
      if (build_pressed)
      {
        if (has_point(pick_modal, mouse_pos))
        {
        }
        else
        {
          build_pressed = false;
          picking_new_boxtype = false;
        }
      }
    }
    static double item_scaling[ARRLEN(boxes)] = {1.0};
    {
      double alpha = pick_opacity * 0.8;
      if (draw)
      {
        set_color_values(0.4, 0.4, 0.4, alpha);
        draw_filled_rect(pick_modal.x, pick_modal.y, pick_modal.width, pick_modal.height);

        set_color_values(1.0, 1.0, 1.0, 1.0 * pick_opacity);
      }
      int boxes_per_row = (int)floor(pick_modal.width / 128.0);
      boxes_per_row = boxes_per_row < 4 ? 4 : boxes_per_row;
      double cell_width = pick_modal.width / (float)boxes_per_row;
      double cell_height = cell_width;
      double padding = 0.2 * cell_width;
      int cur_row = 0;
      int cur_column = 0;
      for (int i = 0; i < ARRLEN(boxes); i++)
      {
        if (cur_column >= boxes_per_row)
        {
          cur_column = 0;
          cur_row++;
        }
        double item_width = cell_width - padding * 2.0;
        double item_height = cell_height - padding * 2.0;

        item_width *= item_scaling[i];
        item_height *= item_scaling[i];

        double cell_y = pick_modal.y + (float)cur_row * cell_height;
        double cell_x = pick_modal.x + (float)cur_column * cell_width;
        double item_x = cell_x + (cell_width - item_width) / 2.0;
        double item_y = cell_y + (cell_height - item_height) / 2.0;

        bool item_being_hovered = has_point((AABB){
                                                .x = item_x,
                                                .y = item_y,
                                                .width = item_width,
                                                .height = item_height,
                                            },
                                            mouse_pos);

        item_scaling[i] = lerp(item_scaling[i], item_being_hovered ? 1.3 : 1.0, dt * 4.0);

        struct BoxInfo info = boxes[i];
        if (item_being_hovered && build_pressed && picking_new_boxtype)
        {
          toolbar[cur_toolbar_slot] = info.type;
          picking_new_boxtype = false;
          build_pressed = false;
        }
        if (draw)
        {
          if (can_build(info.type))
          {
            sgp_set_image(0, info.image);
          }
          else
          {
            sgp_set_image(0, image_mystery);
          }
          transform_scope
          {
            scale_at(1.0, -1.0, item_x + item_width / 2.0, item_y + item_height / 2.0);
            pipeline_scope(goodpixel_pipeline)
                draw_textured_rect(item_x, item_y, item_width, item_height);
            sgp_reset_image(0);
          }
        }

        cur_column++;
      }
    }
  }

  // draw squad invite
  static double invite_y = -200.0;
  static enum Squad draw_as_squad = SquadNone;
  static double yes_size = 50.0;
  static double no_size = 50.0;
  {
    bool invited =
        myentity() != NULL && myentity()->squad_invited_to != SquadNone;
    double size = 200.0;
    double yes_no_size = 50.0;
    double x_center = 0.75 * width;
    double x = x_center - size / 2.0;
    // AABB box = (AABB){ .x = x, .y = invite_y, .width = size, .height = size
    // };
    double yes_x = x - size / 4.0;
    double no_x = x + size / 4.0;
    double buttons_y = invite_y + size / 2.0;

    bool yes_hovered =
        invited && cpvdist(mouse_pos, (cpVect){yes_x, buttons_y}) < yes_size / 2.0;
    bool no_hovered =
        invited && cpvdist(mouse_pos, (cpVect){no_x, buttons_y}) < no_size / 2.0;

    yes_size = lerp(yes_size, yes_hovered ? 75.0 : 50.0, dt * 9.0);
    no_size = lerp(no_size, no_hovered ? 75.0 : 50.0, dt * 9.0);

    if (invited && build_pressed && yes_hovered)
    {
      accept_invite = true;
      build_pressed = false;
    }
    if (invited && build_pressed && no_hovered)
    {
      reject_invite = true;
      build_pressed = false;
    }

    if (draw)
    {
      invite_y = lerp(invite_y, invited ? 50.0 : -200.0, dt * 5.0);

      if (invited)
        draw_as_squad = myentity()->squad_invited_to;

      transform_scope
      {
        pipeline_scope(hueshift_pipeline)
        {
          setup_hueshift(draw_as_squad);
          scale_at(1.0, -1.0, x,
                   invite_y); // images upside down by default :(
          sgp_set_image(0, image_squad_invite);
          draw_texture_centered((cpVect){x, invite_y}, size);
          sgp_reset_image(0);
        }
      }

      // yes
      transform_scope
      {
        set_color_values(1.0, 1.0, 1.0, 1.0);
        scale_at(1.0, -1.0, yes_x, buttons_y);
        sgp_set_image(0, image_check);
        pipeline_scope(goodpixel_pipeline)
        {
          draw_texture_centered((cpVect){yes_x, buttons_y}, yes_size);
        }
        sgp_reset_image(0);
      }

      // no
      transform_scope
      {
        set_color_values(1.0, 1.0, 1.0, 1.0);
        scale_at(1.0, -1.0, no_x, buttons_y);
        sgp_set_image(0, image_no);
        pipeline_scope(goodpixel_pipeline)
        {
          draw_texture_centered((cpVect){no_x, buttons_y}, no_size);
        }
        sgp_reset_image(0);
      }
    }
  }

  // draw maybe inviting
  {
    Entity *inviting = get_entity(&gs, maybe_inviting_this_player);
    if (inviting != NULL && myplayer() != NULL)
    {
      cpVect top_of_head = world_to_screen(
          width, height,
          cpvadd(entity_pos(inviting),
                 (cpVect){.y = player_scaling * PLAYER_SIZE.y / 2.0}));
      cpVect pos = cpvadd(top_of_head, (cpVect){.y = -30.0});
      cpVect to_mouse = cpvsub(mouse_pos,
                               world_to_screen(width, height, entity_pos(inviting)));
      bool selecting_to_invite =
          cpvdot(cpvnormalize(to_mouse), (cpVect){0.0, -1.0}) > 0.5 &&
          cpvlength(to_mouse) > 15.0;
      if (!mousedown[SAPP_MOUSEBUTTON_RIGHT])
      {
        if (selecting_to_invite)
          confirm_invite_this_player = true;
      }
      if (draw)
        transform_scope
        {
          const double size = 64.0;

          if (selecting_to_invite)
          {
            set_color_values(0.5, 0.5, 0.5, 0.4);
            draw_filled_rect(pos.x - size / 2.0, pos.y - size / 2.0, size,
                             size);
            set_color_values(1.0, 1.0, 1.0, 1.0);
          }
          pipeline_scope(hueshift_pipeline)
          {
            setup_hueshift(myplayer()->squad);

            scale_at(1.0, -1.0, pos.x,
                     pos.y); // images upside down by default :(
            sgp_set_image(0, image_squad_invite);
            draw_texture_centered(pos, size);
            sgp_reset_image(0);
          }
        }
    }
  }

  // draw flags
  static cpVect flag_pos[SquadLast] = {0};
  static double flag_rot[SquadLast] = {0};
  static double flag_scaling_increase[SquadLast] = {0};
  static bool choosing_flags = false;
  const double flag_padding = 70.0;
  const double center_panel_height = 200.0;
  static double center_panel_width = 0.0;
  const double target_center_panel_width = ((SquadLast) + 2) * flag_padding;
#define FLAG_ITER(i) for (int i = 0; i < SquadLast; i++)
  {
    FLAG_ITER(i)
    {
      cpVect target_pos = {0};
      double target_rot = 0.0;
      double flag_progress = (float)i / (float)(SquadLast - 1.0);
      if (choosing_flags)
      {
        target_pos.x =
            width / 2.0 + lerp(-center_panel_width / 2.0 + flag_padding,
                               center_panel_width / 2.0 - flag_padding,
                               flag_progress);
        target_pos.y = height * 0.5;
        target_rot = 0.0;
      }
      else
      {
        target_pos.x = 25.0;
        target_pos.y = 200.0;
        target_rot = lerp(-PI / 3.0, PI / 3.0, flag_progress) + PI / 2.0;
      }
      flag_pos[i] = cpvlerp(flag_pos[i], target_pos, dt * 5.0);
      flag_rot[i] = lerp_angle(flag_rot[i], target_rot, dt * 5.0);
    }

    center_panel_width =
        lerp(center_panel_width,
             choosing_flags ? target_center_panel_width : 0.0, 6.0 * dt);

    // center panel
    {
      AABB panel_rect = (AABB){
          .x = width / 2.0 - center_panel_width / 2.0,
          .y = height / 2.0 - center_panel_height / 2.0,
          .width = center_panel_width,
          .height = center_panel_height,
      };

      if (choosing_flags && build_pressed &&
          !has_point(panel_rect, mouse_pos))
      {
        build_pressed = false;
        choosing_flags = false;
      }
      if (draw)
      {
        set_color_values(0.7, 0.7, 0.7, 0.5);
        draw_filled_rect(panel_rect.x, panel_rect.y, panel_rect.width,
                         panel_rect.height);
      }
    }

    FLAG_ITER(i)
    {
      enum Squad this_squad = (enum Squad)i;
      bool this_squad_available = true;
      if (this_squad != SquadNone)
        PLAYERS_ITER(gs.players, other_player)
        {
          if (other_player->squad == this_squad)
          {
            this_squad_available = false;
            break;
          }
        }

      double size = 128.0;
      bool hovering =
          cpvdist(mouse_pos, flag_pos[i]) < size * 0.25 && this_squad_available;

      if (!choosing_flags && hovering && build_pressed)
      {
        choosing_flags = true;
        build_pressed = false;
      }

      if (this_squad_available && choosing_flags && hovering && build_pressed)
      {
        take_over_squad = this_squad;
        build_pressed = false;
      }

      flag_scaling_increase[i] =
          lerp(flag_scaling_increase[i], hovering ? 0.2 : 0.0, dt * 9.0);

      size *= 1.0 + flag_scaling_increase[i];

      if (draw)
      {
        transform_scope
        {
          if (this_squad_available)
          {
            sgp_set_image(0, image_flag_available);
          }
          else
          {
            sgp_set_image(0, image_flag_taken);
          }

          pipeline_scope(hueshift_pipeline)
          {
            set_color(WHITE);
            setup_hueshift(this_squad);

            rotate_at(flag_rot[i], flag_pos[i].x, flag_pos[i].y);
            scale_at(1.0, -1.0, flag_pos[i].x,
                     flag_pos[i].y); // images upside down by default :(
            draw_texture_centered(flag_pos[i], size);

            sgp_reset_image(0);
          }
        }
      }
    }

    if (choosing_flags)
      build_pressed = false; // no more inputs beyond flags when the flag
                             // choice modal is open
  }
#undef FLAG_ITER

  //  draw spice bar
  if (draw)
  {
    static double damage = 0.5;

    if (myentity() != NULL)
    {
      damage = myentity()->damage;
    }

    set_color_values(0.5, 0.5, 0.5, cur_opacity);
    double margin = width * 0.2;
    double bar_width = width - margin * 2.0;
    double y = height - 150.0;
    draw_filled_rect(margin, y, bar_width, 30.0);
    set_color_values(1.0, 1.0, 1.0, cur_opacity);
    draw_filled_rect(margin, y, bar_width * (1.0 - damage), 30.0);
  }

  // draw muted
  static double toggle_mute_opacity = 0.2;
  const double size = 150.0;
  AABB button = (AABB){
      .x = width - size - 40.0,
      .y = height - size - 40.0,
      .width = size,
      .height = size,
  };
  bool hovered = has_point(button, mouse_pos);
  if (build_pressed && hovered)
  {
    muted = !muted;
    build_pressed = false;
  }
  if (draw)
  {
    toggle_mute_opacity =
        lerp(toggle_mute_opacity, hovered ? 1.0 : 0.2, 6.0 * dt);
    set_color_values(1.0, 1.0, 1.0, toggle_mute_opacity);
    if (muted)
      sgp_set_image(0, image_mic_muted);
    else
      sgp_set_image(0, image_mic_unmuted);
    transform_scope
    {
      scale_at(1.0, -1.0, button.x + button.width / 2.0,
               button.y + button.height / 2.0);
      draw_textured_rect(button.x, button.y, button.width, button.height);
      sgp_reset_image(0);
    }
  }

  // draw item toolbar
  {
    double itemframe_width =
        (float)sg_query_image_info(image_itemframe).width * 2.0;
    double itemframe_height =
        (float)sg_query_image_info(image_itemframe).height * 2.0;
    double total_width = itemframe_width * (float)TOOLBAR_SLOTS;
    double item_width = itemframe_width * 0.75;
    double item_height = itemframe_height * 0.75;
    double item_offset_x = (itemframe_width - item_width) / 2.0;
    double item_offset_y = (itemframe_height - item_height) / 2.0;

    double x = width / 2.0 - total_width / 2.0;
    double y = height - itemframe_height * 1.5;
    for (int i = 0; i < TOOLBAR_SLOTS; i++)
    {
      // mouse over the item frame box
      if (has_point(
              (AABB){
                  .x = x,
                  .y = y,
                  .width = itemframe_width,
                  .height = itemframe_height,
              },
              mouse_pos) &&
          build_pressed)
      {
        // "handle" mouse pressed
        cur_toolbar_slot = i;
        build_pressed = false;
      }

      // mouse over the item switch button
      bool switch_hovered = false;
      if (has_point(
              (AABB){
                  .x = x,
                  .y = y - 20.0,
                  .width = itemframe_width,
                  .height = itemframe_height * 0.2,
              },
              mouse_pos))
      {
        switch_hovered = true;
      }

      if (switch_hovered && build_pressed)
      {
        picking_new_boxtype = true;
        build_pressed = false;
      }

      if (draw)
      {
        set_color_values(1.0, 1.0, 1.0, cur_opacity);

        bool is_current = cur_toolbar_slot == i;
        static double switch_scaling = 1.0;
        switch_scaling = lerp(switch_scaling, switch_hovered ? 1.8 : 1.2, dt * 3.0);
        if (is_current)
        {
          sgp_set_image(0, image_itemframe_selected);
        }
        else
        {
          sgp_set_image(0, image_itemframe);
        }
        pipeline_scope(goodpixel_pipeline)
            draw_textured_rect(x, y, itemframe_width, itemframe_height);
        sgp_reset_image(0);
        transform_scope
        {
          double item_x = x + item_offset_x;
          double item_y = y + item_offset_y;
          scale_at(1.0, -1.0, item_x + item_width / 2.0,
                   item_y + item_height / 2.0);

          pipeline_scope(goodpixel_pipeline)
          {
            if (toolbar[i] != BoxInvalid)
            {
              struct BoxInfo info = boxinfo(toolbar[i]);
              if (can_build(info.type))
                sgp_set_image(0, info.image);
              else
                sgp_set_image(0, image_mystery);
              draw_textured_rect(item_x, item_y, item_width, item_height);

              sgp_reset_image(0);
            }
            if (is_current)
            {
              sgp_set_image(0, image_itemswitch);
              double switch_item_width = item_width * switch_scaling;
              double switch_item_height = item_height * switch_scaling;
              item_x -= (switch_item_width - item_width) / 2.0;
              item_y -= (switch_item_height - item_height) / 2.0;
              draw_textured_rect(item_x, item_y + 20.0, switch_item_width, switch_item_height);
              sgp_reset_image(0);
            }
          }
        }
      }
      x += itemframe_width;
    }
  }

  if (draw)
    sgp_pop_transform();
}

static void draw_dots(cpVect camera_pos, double gap)
{
  set_color_values(1.0, 1.0, 1.0, 1.0);
  // const int num = 100;

  // initial_x * gap = camera_pos.x - VISION_RADIUS
  // initial_x = (camera_pos.x - VISION_RADIUS) / gap

  // int initial_x = (int)floor((camera_pos.x - VISION_RADIUS) / gap);
  // int final_x  = (int)floor((camera_pos.x + VISION_RADIUS) / gap);

  // -VISION_RADIUS < x * gap - camera_pos.x < VISION_RADIUS
  // -VISION_RADIUS + camera_pos.x < x * gap < VISION_RADIUS + camera_pos.x
  // (-VISION_RADIUS + camera_pos.x)/gap < x < (VISION_RADIUS + camera_pos.x)/gap
  int initial_x = (int)floor((-VISION_RADIUS * 2 + camera_pos.x) / gap);
  int final_x = (int)ceil((VISION_RADIUS * 2 + camera_pos.x) / gap);

  int initial_y = (int)floor((-VISION_RADIUS * 2 + camera_pos.y) / gap);
  int final_y = (int)ceil((VISION_RADIUS * 2 + camera_pos.y) / gap);

  // initial_x = -num;
  // final_x = num;

  for (int x = initial_x; x < final_x; x++)
  {
    for (int y = initial_y; y < final_y; y++)
    {
      cpVect star = (cpVect){(float)x * gap, (float)y * gap};
      star.x += hash11(star.x * 100.0 + star.y * 67.0) * gap;
      star.y += hash11(star.y * 93.0 + star.x * 53.0) * gap;
      if (cpvlengthsq(cpvsub(star, camera_pos)) > VISION_RADIUS * VISION_RADIUS)
        continue;

      sgp_draw_point((float)star.x, (float)star.y);
    }
  }
}

static cpVect get_global_hand_pos(cpVect world_mouse_pos, bool *hand_at_arms_length)
{
  if (myentity() == NULL)
    return (cpVect){0};

  cpVect global_hand_pos = cpvsub(world_mouse_pos, entity_pos(myentity()));
  double hand_len = cpvlength(global_hand_pos);
  if (hand_len > MAX_HAND_REACH)
  {
    *hand_at_arms_length = true;
    hand_len = MAX_HAND_REACH;
  }
  else
  {
    *hand_at_arms_length = false;
  }
  global_hand_pos = cpvmult(cpvnormalize(global_hand_pos), hand_len);
  global_hand_pos = cpvadd(global_hand_pos, entity_pos(myentity()));
  return global_hand_pos;
}
static void frame(void)
{
  PROFILE_SCOPE("frame")
  {
    double width = (float)sapp_width(), height = (float)sapp_height();
    double ratio = width / height;
    double exec_time = sapp_frame_count() * sapp_frame_duration();
    double dt = (float)sapp_frame_duration();

    // pressed input management
    {
      for (int i = 0; i < MAX_KEYDOWN; i++)
      {
        if (keypressed[i].frame < sapp_frame_count())
        {
          keypressed[i].pressed = false;
        }
      }
      for (int i = 0; i < MAX_MOUSEBUTTON; i++)
      {
        if (mousepressed[i].frame < sapp_frame_count())
        {
          mousepressed[i].pressed = false;
        }
      }
    }

    build_pressed = mousepressed[SAPP_MOUSEBUTTON_LEFT].pressed;
    interact_pressed = mousepressed[SAPP_MOUSEBUTTON_RIGHT].pressed;

    // networking
    static cpVect before_reprediction = {0};
    PROFILE_SCOPE("networking")
    {
      ENetEvent event;
      while (true)
      {
        int enet_status = enet_host_service(client, &event, 0);
        if (enet_status > 0)
        {
          switch (event.type)
          {
          case ENET_EVENT_TYPE_NONE:
          {
            Log("Wtf none event type?\n");
            break;
          }
          case ENET_EVENT_TYPE_CONNECT:
          {
            Log("New client from host %x\n", event.peer->address.host);
            break;
          }

          case ENET_EVENT_TYPE_RECEIVE:
          {
            unsigned char *decompressed = malloc(
                sizeof *decompressed * MAX_SERVER_TO_CLIENT); // @Robust no malloc
            size_t decompressed_max_len = MAX_SERVER_TO_CLIENT;
            assert(LZO1X_MEM_DECOMPRESS == 0);

            ma_mutex_lock(&play_packets_mutex);
            double predicted_to_time = time(&gs);
            ServerToClient msg = (ServerToClient){
                .cur_gs = &gs,
                .audio_playback_buffer = &packets_to_play,
            };
            int return_value = lzo1x_decompress_safe(
                event.packet->data, event.packet->dataLength, decompressed,
                &decompressed_max_len, NULL);
            if (return_value == LZO_E_OK)
            {
              if (myentity() != NULL)
                before_reprediction = entity_pos(myentity());
              PROFILE_SCOPE("Deserializing data")
              {
                server_to_client_deserialize(&msg, decompressed,
                                             decompressed_max_len, false);
              }
              my_player_index = msg.your_player;
            }
            else
            {
              Log("Couldn't decompress gamestate packet, error code %d from "
                  "lzo\n",
                  return_value);
            }
            ma_mutex_unlock(&play_packets_mutex);
            free(decompressed);
            enet_packet_destroy(event.packet);

            PROFILE_SCOPE("Repredicting inputs")
            {

              double server_current_time = time(&gs);
              double difference = predicted_to_time - server_current_time;
              double target_prediction_time =
                  (((double)peer->roundTripTime) / 1000.0) + TIMESTEP * 6.0;

              // keeps it stable even though causes jumps occasionally
              difference = fmax(difference, target_prediction_time);

              double eps = TIMESTEP * 0.1;
              if (predicted_to_time - time(&gs) < target_prediction_time - eps)
              {
                target_prediction_time_factor = 1.1;
              }
              else if (predicted_to_time - time(&gs) >
                       target_prediction_time + eps * 2.0)
              {
                target_prediction_time_factor = 0.9;
              }
              else
              {
                target_prediction_time_factor = 1.0;
              }

              // re-predict the inputs
              double time_to_repredict = (float)difference;
              Log("Repredicting %f\n", time_to_repredict);

              uint64_t start_prediction_time = stm_now();
              if (time_to_repredict > 0.0)
              {
                while (time_to_repredict > TIMESTEP)
                {
                  if (stm_ms(stm_diff(stm_now(), start_prediction_time)) > MAX_MS_SPENT_REPREDICTING)
                  {
                    Log("Reprediction took longer than %f milliseconds, could only predict %f\n", MAX_MS_SPENT_REPREDICTING, time_to_repredict);
                    break;
                  }
                  QUEUE_ITER(&input_queue, cur_header)
                  {
                    InputFrame *cur = (InputFrame *)cur_header->data;
                    if (cur->tick == tick(&gs))
                    {
                      myplayer()->input = *cur;
                      break;
                    }
                  }
                  process(&gs, TIMESTEP, false);
                  time_to_repredict -= TIMESTEP;
                }
                process(&gs, time_to_repredict, true);
                time_to_repredict = 0.0;
              }

              current_time_ahead_of_server = time(&gs) - server_current_time;
            }
            break;
          }

          case ENET_EVENT_TYPE_DISCONNECT:
          {
            fprintf(stderr, "Disconnected from server\n");
            exit(-1);
            break;
          }
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
    ui(false, dt, width, height); // if ui button is pressed before game logic, set the pressed to
    // false so it doesn't propagate from the UI modal/button
    cpVect build_target_pos = {0};
    double build_target_rotation = 0.0;
    struct BuildPreviewInfo
    {
      cpVect grid_pos;
      double grid_rotation;
    } build_preview = {0};
    cpVect global_hand_pos = {0}; // world coords! world star!
    bool hand_at_arms_length = false;
    recalculate_camera_pos();
    cpVect world_mouse_pos = screen_to_world(width, height, mouse_pos);
    PROFILE_SCOPE("gameplay and prediction")
    {
      // interpolate zoom
      zoom = lerp(zoom, zoom_target, dt * 12.0);

      // calculate build preview stuff
      cpVect local_hand_pos = {0};
      global_hand_pos =
          get_global_hand_pos(world_mouse_pos, &hand_at_arms_length);

      if (myentity() != NULL)
      {
        local_hand_pos = cpvsub(global_hand_pos, entity_pos(myentity()));
      }

      // process player interaction (squad invites)
      if (interact_pressed && myplayer() != NULL && myplayer()->squad != SquadNone)
        ENTITIES_ITER(cur)
        {
          if (cur != myentity() && cur->is_player &&
              has_point(centered_at(entity_pos(cur), cpvmult(PLAYER_SIZE, player_scaling)), world_mouse_pos))
          {
            maybe_inviting_this_player = get_id(&gs, cur);
            interact_pressed = false;
          }
        }

      // Create and send input packet, and predict a frame of gamestate
      static InputFrame cur_input_frame = {
          0}; // keep across frames for high refresh rate screens
      static size_t last_input_committed_tick = 0;
      {
        // prepare the current input frame, such that when processed next,
        // every button/action the player has pressed will be handled
        // without frustration by the server. Resulting in authoritative game
        // state that looks and feels good.

        cpVect input = (cpVect){
            .x = (float)keydown[SAPP_KEYCODE_D] - (float)keydown[SAPP_KEYCODE_A],
            .y = (float)keydown[SAPP_KEYCODE_W] - (float)keydown[SAPP_KEYCODE_S],
        };
        if (cpvlength(input) > 0.0)
          input = cpvnormalize(input);
        cur_input_frame.movement = input;
        cur_input_frame.rotation = (float)keydown[SAPP_KEYCODE_E] - (float)keydown[SAPP_KEYCODE_Q];

        if (interact_pressed)
          cur_input_frame.seat_action = interact_pressed;

        cur_input_frame.hand_pos = local_hand_pos;
        if (take_over_squad >= 0)
        {
          cur_input_frame.take_over_squad = take_over_squad;
          take_over_squad = -1;
        }
        if (confirm_invite_this_player)
        {
          cur_input_frame.invite_this_player = maybe_inviting_this_player;
          maybe_inviting_this_player = (EntityID){0};
          confirm_invite_this_player = false;
        }
        if (accept_invite)
        {
          cur_input_frame.accept_cur_squad_invite = true;
          accept_invite = false;
        }
        if (reject_invite)
        {
          cur_input_frame.reject_cur_squad_invite = true;
          reject_invite = false;
        }

        if (build_pressed && currently_building() != BoxInvalid)
        {
          cur_input_frame.dobuild = build_pressed;
          cur_input_frame.build_type = currently_building();
          cur_input_frame.build_rotation = cur_editing_rotation;
        }

        // "commit" the input. each input must be on a successive tick.
        if (tick(&gs) > last_input_committed_tick)
        {
          cur_input_frame.tick = tick(&gs);
          last_input_committed_tick = tick(&gs);

          InputFrame *to_push_to = queue_push_element(&input_queue);
          if (to_push_to == NULL)
          {
            InputFrame *to_discard = queue_pop_element(&input_queue);
            (void)to_discard;
            to_push_to = queue_push_element(&input_queue);
            assert(to_push_to != NULL);
          }

          *to_push_to = cur_input_frame;

          if (myplayer() != NULL)
            myplayer()->input =
                cur_input_frame; // for the client side prediction!

          cur_input_frame = (InputFrame){0};
          cur_input_frame.take_over_squad =
              -1; // @Robust make this zero initialized
        }

        // in client side prediction, only process the latest in the queue, not
        // the one currently constructing.
        static double prediction_time_factor = 1.0;
        prediction_time_factor = lerp(prediction_time_factor,
                                      target_prediction_time_factor, dt * 3.0);
        process(&gs, dt * prediction_time_factor, true);

        static int64_t last_sent_input_time = 0;
        if (stm_sec(stm_diff(stm_now(), last_sent_input_time)) >
            TIME_BETWEEN_INPUT_PACKETS)
        {
          ma_mutex_lock(&send_packets_mutex);
          ClientToServer to_send = {
              .mic_data = &packets_to_send,
              .input_data = &input_queue,
          };
          unsigned char serialized[MAX_CLIENT_TO_SERVER] = {0};
          size_t out_len = 0;
          if (client_to_server_serialize(&gs, &to_send, serialized, &out_len,
                                         MAX_CLIENT_TO_SERVER))
          {
            unsigned char compressed[MAX_CLIENT_TO_SERVER] = {0};
            char lzo_working_mem[LZO1X_1_MEM_COMPRESS] = {0};
            size_t compressed_len = 0;

            lzo1x_1_compress(serialized, out_len, compressed, &compressed_len,
                             (void *)lzo_working_mem);

            ENetPacket *packet =
                enet_packet_create((void *)compressed, compressed_len,
                                   ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT);
            enet_peer_send(peer, 0, packet); // @Robust error check this
            last_sent_input_time = stm_now();
          }
          else
          {
            Log("Failed to serialize client to server!\n");
          }
          ma_mutex_unlock(&send_packets_mutex);
        }
      }

      // calculate world position and camera
      recalculate_camera_pos();
      world_mouse_pos = screen_to_world(width, height, mouse_pos);
      global_hand_pos =
          get_global_hand_pos(world_mouse_pos, &hand_at_arms_length);

      Entity *placing_grid = box_grid(closest_box_to_point_in_radius(
          &gs, global_hand_pos, BUILD_BOX_SNAP_DIST_TO_SHIP, NULL));
      if (placing_grid == NULL)
      {
        build_preview = (struct BuildPreviewInfo){
            .grid_pos = global_hand_pos,
            .grid_rotation = 0.0,
        };
      }
      else
      {
        global_hand_pos = grid_snapped_box_pos(placing_grid, global_hand_pos);
        build_preview = (struct BuildPreviewInfo){
            .grid_pos = entity_pos(placing_grid),
            .grid_rotation = entity_rotation(placing_grid),
        };
      }
    }

    // drawing
    PROFILE_SCOPE("drawing")
    {
      sgp_begin((int)width, (int)height);
      sgp_viewport(0, 0, (int)width, (int)height);
      sgp_project(0.0f, (float)width, 0.0f, (float)height);
      sgp_set_blend_mode(SGP_BLENDMODE_BLEND);

      // Draw background color
      set_color(colhexcode(0x000000));
      // set_color_values(0.1, 0.1, 0.1, 1.0);
      sgp_clear();

      // WORLD SPACE
      // world space coordinates are +Y up, -Y down. Like normal cartesian coords
      transform_scope
      {
        translate(width / 2, height / 2);
        scale_at(zoom, -zoom, 0.0, 0.0);

        // parllax layers, just the zooming, but not 100% of the camera panning
#if 1 // space background
        transform_scope
        {
          cpVect scaled_camera_pos = cpvmult(
              camera_pos, 0.0005); // this is how strong/weak the parallax is
          translate(-scaled_camera_pos.x, -scaled_camera_pos.y);
          set_color(WHITE);
          sgp_set_image(0, image_stars);
          double stars_height_over_width =
              (float)sg_query_image_info(image_stars).height /
              (float)sg_query_image_info(image_stars).width;
          const double stars_width = 35.0;
          double stars_height = stars_width * stars_height_over_width;
          pipeline_scope(goodpixel_pipeline)
              draw_textured_rect(-stars_width / 2.0, -stars_height / 2.0, stars_width, stars_height);
          // draw_textured_rect(0, 0, stars_width, stars_height);
          sgp_reset_image(0);
        }
        transform_scope
        {
          cpVect scaled_camera_pos = cpvmult(
              camera_pos, 0.005); // this is how strong/weak the parallax is
          translate(-scaled_camera_pos.x, -scaled_camera_pos.y);
          set_color(WHITE);
          sgp_set_image(0, image_stars2);
          double stars_height_over_width =
              (float)sg_query_image_info(image_stars).height /
              (float)sg_query_image_info(image_stars).width;
          const double stars_width = 35.0;
          double stars_height = stars_width * stars_height_over_width;
          pipeline_scope(goodpixel_pipeline)
              draw_textured_rect(-stars_width / 2.0, -stars_height / 2.0, stars_width, stars_height);
          // draw_textured_rect(0, 0, stars_width, stars_height);
          sgp_reset_image(0);
        }
#endif

#if 1 // parallaxed dots
        transform_scope
        {
          cpVect scaled_camera_pos = cpvmult(camera_pos, 0.25);
          translate(-scaled_camera_pos.x, -scaled_camera_pos.y);
          set_color(WHITE);
          draw_dots(scaled_camera_pos, 3.0);
        }
        transform_scope
        {
          cpVect scaled_camera_pos = cpvmult(camera_pos, 0.5);
          translate(-scaled_camera_pos.x, -scaled_camera_pos.y);
          set_color(WHITE);
          draw_dots(scaled_camera_pos, 2.0);
        }
#endif

        // camera go to player
        translate(-camera_pos.x, -camera_pos.y);

        draw_dots(camera_pos, 1.5); // in plane dots

        // hand reached limit circle
        if (myentity() != NULL)
        {
          static double hand_reach_alpha = 1.0;
          hand_reach_alpha = lerp(hand_reach_alpha,
                                  hand_at_arms_length ? 1.0 : 0.0, dt * 5.0);
          set_color_values(1.0, 1.0, 1.0, hand_reach_alpha);
          draw_circle(entity_pos(myentity()), MAX_HAND_REACH);
        }

        // vision circle, what player can see
        if (myentity() != NULL)
        {
          set_color(colhexcode(0x4685e3));
          draw_circle(entity_pos(myentity()), VISION_RADIUS);
        }

        double halfbox = BOX_SIZE / 2.0;

        // mouse frozen, debugging tool
        if (mouse_frozen)
        {
          set_color_values(1.0, 0.0, 0.0, 0.5);
          draw_filled_rect(world_mouse_pos.x, world_mouse_pos.y, 0.1, 0.1);
        }

        // building preview
        if (currently_building() != BoxInvalid && can_build(currently_building()))
        {
          set_color_values(0.5, 0.5, 0.5,
                           (sin((float)exec_time * 9.0) + 1.0) / 3.0 + 0.2);

          transform_scope
          {
            sgp_set_image(0, boxinfo(currently_building()).image);
            rotate_at(build_preview.grid_rotation +
                          rotangle(cur_editing_rotation),
                      global_hand_pos.x, global_hand_pos.y);
            pipeline_scope(goodpixel_pipeline)
                draw_texture_centered(global_hand_pos, BOX_SIZE);
            // drawbox(hand_pos, build_preview.grid_rotation, 0.0,
            // cur_editing_boxtype, cur_editing_rotation);
            sgp_reset_image(0);
          }
        }

        player_scaling =
            lerp(player_scaling, zoom < 6.5 ? 100.0 : 1.0, dt * 7.0);
        ENTITIES_ITER(e)
        {
          // draw grid
          if (e->is_grid)
          {
            Entity *g = e;
            BOXES_ITER(&gs, b, g)
            {
              set_color_values(1.0, 1.0, 1.0, 1.0);
              if (b->box_type == BoxBattery)
              {
                double cur_alpha = sgp_get_color().a;
                Color from = WHITE;
                Color to = colhex(255, 0, 0);
                Color result =
                    Collerp(from, to, b->energy_used / BATTERY_CAPACITY);
                set_color_values(result.r, result.g, result.b, cur_alpha);
              }
              transform_scope
              {
                rotate_at(entity_rotation(g) + rotangle(b->compass_rotation),
                          entity_pos(b).x, entity_pos(b).y);

                if (b->box_type == BoxThruster)
                {
                  transform_scope
                  {
                    set_color_values(1.0, 1.0, 1.0, 1.0);
                    sgp_set_image(0, image_thrusterburn);
                    double scaling = 0.95 + lerp(0.0, 0.3, b->thrust);
                    scale_at(scaling, 1.0, entity_pos(b).x, entity_pos(b).y);
                    pipeline_scope(goodpixel_pipeline)
                    {
                      draw_texture_centered(entity_pos(b), BOX_SIZE);
                    }
                    sgp_reset_image(0);
                  }
                }
                sg_image img = boxinfo(b->box_type).image;
                if (b->box_type == BoxCockpit)
                {
                  if (get_entity(&gs, b->player_who_is_inside_of_me) != NULL)
                    img = image_cockpit_used;
                }
                if (b->box_type == BoxMedbay)
                {
                  if (get_entity(&gs, b->player_who_is_inside_of_me) != NULL)
                    img = image_medbay_used;
                }
                if (b->box_type == BoxSolarPanel)
                {
                  sgp_set_image(0, image_solarpanel_charging);
                  set_color_values(1.0, 1.0, 1.0, b->sun_amount);
                  pipeline_scope(goodpixel_pipeline)
                      draw_texture_centered(entity_pos(b), BOX_SIZE);
                  sgp_reset_image(0);
                  set_color_values(1.0, 1.0, 1.0, 1.0 - b->sun_amount);
                  /* Color to_set = colhexcode(0xeb9834);
                  to_set.a = b->sun_amount * 0.5;
                  set_color(to_set);
                  draw_color_rect_centered(entity_pos(b), BOX_SIZE);
                  */
                }

                sgp_set_image(0, img);
                if (b->indestructible)
                {
                  set_color_values(0.2, 0.2, 0.2, 1.0);
                }
                else if (b->is_platonic)
                {
                  set_color(GOLD);
                }

                // all of these box types show team colors so are drawn with the hue shifting shader
                // used with the player
                if (b->box_type == BoxCloaking || b->box_type == BoxMissileLauncher)
                {
                  pipeline_scope(hueshift_pipeline)
                  {
                    setup_hueshift(b->owning_squad);
                    draw_texture_centered(entity_pos(b), BOX_SIZE);
                  }
                }
                else
                {
                  pipeline_scope(goodpixel_pipeline)
                      draw_texture_centered(entity_pos(b), BOX_SIZE);
                }
                sgp_reset_image(0);

                if (b->box_type == BoxScanner)
                {
                  sgp_set_image(0, image_scanner_head);
                  transform_scope
                  {
                    pipeline_scope(goodpixel_pipeline)
                    {
                      rotate_at(b->scanner_head_rotate, entity_pos(b).x, entity_pos(b).y);
                      draw_texture_centered(entity_pos(b), BOX_SIZE);
                    }
                  }
                  sgp_reset_image(0);
                  set_color(WHITE);
                }

                // scanner range, visualizes what scanner can scan
                if (b->box_type == BoxScanner)
                {
                  set_color(BLUE);
                  draw_circle(entity_pos(b), SCANNER_RADIUS);
                  set_color(WHITE);
                }
                set_color_values(0.5, 0.1, 0.1, b->damage);
                draw_color_rect_centered(entity_pos(b), BOX_SIZE);

                if (b->box_type == BoxCloaking)
                {
                  set_color_values(1.0, 1.0, 1.0, b->cloaking_power);
                  sgp_set_image(0, image_cloaking_panel);
                  pipeline_scope(goodpixel_pipeline)
                      draw_texture_centered(entity_pos(b), CLOAKING_PANEL_SIZE);
                  sgp_reset_image(0);
                }
              }

              // outside of the transform scope
              if (b->box_type == BoxScanner)
              {
                if (b->platonic_detection_strength > 0.0)
                {
                  set_color(colhexcode(0xf2d75c));
                  cpVect to = cpvadd(entity_pos(b), cpvmult(b->platonic_nearest_direction, b->platonic_detection_strength));
                  dbg_rect(to);
                  dbg_rect(entity_pos(b));
                  draw_line(entity_pos(b).x, entity_pos(b).y, to.x, to.y);
                }
              }
              if (b->box_type == BoxMissileLauncher)
              {
                set_color(RED);
                draw_circle(entity_pos(b), MISSILE_RANGE);

                // draw the charging missile
                transform_scope
                {
                  rotate_at(missile_launcher_target(&gs, b).facing_angle, entity_pos(b).x, entity_pos(b).y);
                  sgp_set_image(0, image_missile);
                  pipeline_scope(hueshift_pipeline)
                  {
                    set_color_values(1.0, 1.0, 1.0, b->missile_construction_charge);
                    setup_hueshift(b->owning_squad);
                    draw_texture_centered(entity_pos(b), BOX_SIZE);
                  }
                  sgp_reset_image(0);
                }
              }
            }
          }

          // draw missile
          if (e->is_missile)
          {
            transform_scope
            {
              rotate_at(entity_rotation(e), entity_pos(e).x, entity_pos(e).y);
              set_color_values(1.0, 1.0, 1.0, 1.0);

              if (is_burning(e))
              {
                sgp_set_image(0, image_missile_burning);
              }
              else
              {
                sgp_set_image(0, image_missile);
              }

              pipeline_scope(hueshift_pipeline)
              {
                setup_hueshift(e->owning_squad);
                draw_texture_rectangle_centered(entity_pos(e), MISSILE_SPRITE_SIZE);
              }

              sgp_reset_image(0);
            }
          }

          // draw player
          if (e->is_player &&
              get_entity(&gs, e->currently_inside_of_box) == NULL)
          {
            transform_scope
            {
              rotate_at(entity_rotation(e), entity_pos(e).x, entity_pos(e).y);
              set_color_values(1.0, 1.0, 1.0, 1.0);

              pipeline_scope(hueshift_pipeline)
              {
                setup_hueshift(e->owning_squad);
                sgp_set_image(0, image_player);
                draw_texture_rectangle_centered(
                    entity_pos(e), cpvmult(PLAYER_SIZE, player_scaling));
                sgp_reset_image(0);
              }
            }
          }
          if (e->is_explosion)
          {
            sgp_set_image(0, image_explosion);
            set_color_values(1.0, 1.0, 1.0,
                             1.0 - (e->explosion_progress / EXPLOSION_TIME));
            draw_texture_centered(e->explosion_pos, e->explosion_radius * 2.0);
            sgp_reset_image(0);
          }
        }

        // gold target
        set_color(GOLD);
        draw_filled_rect(gs.goldpos.x, gs.goldpos.y, 0.1, 0.1);

        // instant death
        set_color(RED);
        draw_circle((cpVect){0}, INSTANT_DEATH_DISTANCE_FROM_CENTER);

        // the SUN
        SUNS_ITER(&gs)
        {
          transform_scope
          {
            translate(entity_pos(i.sun).x, entity_pos(i.sun).y);
            set_color(WHITE);
            sgp_set_image(0, image_sun);
            draw_texture_centered((cpVect){0}, i.sun->sun_radius * 2.0);
            sgp_reset_image(0);

            // can draw at 0,0 because everything relative to sun now!

            // sun DEATH RADIUS

            set_color(BLUE);
            draw_circle((cpVect){0}, sun_dist_no_gravity(i.sun));
          }
        }

        set_color_values(1.0, 1.0, 1.0, 1.0);
        dbg_drawall();
      } // world space transform end

      // low health
      if (myentity() != NULL)
      {
        set_color_values(1.0, 1.0, 1.0, myentity()->damage);
        sgp_set_image(0, image_low_health);
        draw_texture_rectangle_centered((cpVect){width / 2.0, height / 2.0},
                                        (cpVect){width, height});
        sgp_reset_image(0);
      }

      // UI drawn in screen space
      ui(true, dt, width, height);
    }

    sg_pass_action pass_action = {0};
    sg_begin_default_pass(&pass_action, (int)width, (int)height);
    sgp_flush();
    sgp_end();
    sg_end_pass();
    sg_commit();
  }
}

void cleanup(void)
{
  sg_destroy_pipeline(hueshift_pipeline);

  ma_mutex_lock(&server_info.info_mutex);
  server_info.should_quit = true;
  ma_mutex_unlock(&server_info.info_mutex);
  WaitForSingleObject(server_thread_handle, INFINITE);

  end_profiling_mythread();
  end_profiling();

  ma_mutex_uninit(&send_packets_mutex);
  ma_mutex_uninit(&play_packets_mutex);

  ma_device_uninit(&microphone_device);
  ma_device_uninit(&speaker_device);

  opus_encoder_destroy(enc);
  opus_decoder_destroy(dec);

  destroy(&gs);
  free(gs.entities);
  sgp_shutdown();
  sg_shutdown();
  enet_deinitialize();

  ma_mutex_uninit(&server_info.info_mutex);
}

void event(const sapp_event *e)
{
  switch (e->type)
  {
  case SAPP_EVENTTYPE_KEY_DOWN:
#ifdef DEBUG_TOOLS
    if (e->key_code == SAPP_KEYCODE_T)
    {
      mouse_frozen = !mouse_frozen;
    }
#endif
    if (e->key_code == SAPP_KEYCODE_R)
    {
      cur_editing_rotation += 1;
      cur_editing_rotation %= RotationLast;
    }
    if (e->key_code == SAPP_KEYCODE_F11)
    {
      fullscreened = !fullscreened;
      sapp_toggle_fullscreen();
    }
    if (e->key_code == SAPP_KEYCODE_ESCAPE && fullscreened)
    {
      sapp_toggle_fullscreen();
      fullscreened = false;
    }
    int key_num = e->key_code - SAPP_KEYCODE_0;
    int target_slot = key_num - 1;
    if (target_slot <= TOOLBAR_SLOTS && target_slot >= 0)
    {
      if (target_slot == cur_toolbar_slot)
      {
        picking_new_boxtype = !picking_new_boxtype;
      }
      else
      {
        picking_new_boxtype = false;
      }
      cur_toolbar_slot = target_slot;
    }

    if (!mouse_frozen)
    {
      keydown[e->key_code] = true;
      if (keypressed[e->key_code].frame == 0)
      {
        keypressed[e->key_code].pressed = true;
        keypressed[e->key_code].frame = e->frame_count;
      }
    }
    break;
  case SAPP_EVENTTYPE_KEY_UP:
    if (!mouse_frozen)
    {
      keydown[e->key_code] = false;
      keypressed[e->key_code].pressed = false;

      keypressed[e->key_code].frame = 0;
    }
    break;
  case SAPP_EVENTTYPE_MOUSE_SCROLL:
    zoom_target *= 1.0 + (e->scroll_y / 4.0) * 0.1;
    zoom_target = clamp(zoom_target, ZOOM_MIN, ZOOM_MAX);
    break;
  case SAPP_EVENTTYPE_MOUSE_DOWN:
    mousedown[e->mouse_button] = true;
    if (mousepressed[e->mouse_button].frame == 0)
    {
      mousepressed[e->mouse_button].pressed = true;
      mousepressed[e->mouse_button].frame = e->frame_count;
    }
    break;
  case SAPP_EVENTTYPE_MOUSE_UP:
    mousedown[e->mouse_button] = false;
    mousepressed[e->mouse_button].pressed = false;
    mousepressed[e->mouse_button].frame = 0;
    break;
  case SAPP_EVENTTYPE_MOUSE_MOVE:
    if (!mouse_frozen)
    {
      mouse_pos = (cpVect){.x = e->mouse_x, .y = e->mouse_y};
    }
    if (right_mouse_down)
    {
      funval += e->mouse_dx;
      Log("Funval %f\n", funval);
    }
    break;
  default:
  {
  }
  }
}

sapp_desc sokol_main(int argc, char *argv[])
{
  bool hosting = false;
  stm_setup();
  ma_mutex_init(&server_info.info_mutex);
  server_info.world_save = "debug_world.bin";
  init_profiling("astris.spall");
  init_profiling_mythread(0);
  if (argc > 1)
  {
    server_thread_handle =
        (void *)_beginthread(server, 0, (void *)&server_info);
    hosting = true;
  }
  (void)argv;
  return (sapp_desc){
      .init_cb = init,
      .frame_cb = frame,
      .cleanup_cb = cleanup,
      .width = 640,
      .height = 480,
      .gl_force_gles2 = true,
      .window_title = hosting ? "Flight Hosting" : "Flight Not Hosting",
      .icon.sokol_default = true,
      .event_cb = event,
      .win32_console_attach = true,
      .sample_count = 4, // anti aliasing
  };
}