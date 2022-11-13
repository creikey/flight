//------------------------------------------------------------------------------
//  Take flight
//------------------------------------------------------------------------------

#define SOKOL_IMPL
#define SOKOL_D3D11
#include <enet/enet.h>
#include <process.h> // starting server thread

#pragma warning(disable : 33010) // this warning is so broken, doesn't understand assert()
#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_glue.h"
#include "sokol_gp.h"
#include "sokol_time.h"
#pragma warning(default : 33010)
#pragma warning(disable : 6262) // warning about using a lot of stack, lol that's how stb image is
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include "types.h"

#include "opus.h"

#include <inttypes.h>
#include <string.h> // errno error message on file open

#include "minilzo.h"

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

// shaders
#include "hueshift.gen.h"
static sg_pipeline pip;

static struct GameState gs = {0};
static int myplayer = -1;
static bool right_mouse_down = false;
static bool keydown[SAPP_KEYCODE_MENU] = {0};
typedef struct KeyPressed
{
	bool pressed;
	uint64_t frame;
} KeyPressed;
static KeyPressed keypressed[SAPP_KEYCODE_MENU] = {0};
static V2 mouse_pos = {0};
static bool fullscreened = false;
static bool mouse_pressed = false;
static uint64_t mouse_pressed_frame = 0;
static bool mouse_frozen = false; // @BeforeShip make this debug only thing
static float funval = 0.0f;		  // easy to play with value controlled by left mouse button when held
// down @BeforeShip remove on release builds
static struct ClientToServer client_to_server = {0}; // buffer of inputs
static ENetHost *client;
static ENetPeer *peer;
static float zoom_target = 300.0f;
static float zoom = 300.0f;
static enum Squad take_over_squad = (enum Squad) - 1; // -1 means not taking over any squad

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
static sg_image image_flag_available;
static sg_image image_flag_taken;

static int cur_editing_boxtype = -1;
static int cur_editing_rotation = 0;

// audio
static bool muted = false;
static ma_device microphone_device;
static ma_device speaker_device;
OpusEncoder *enc;
OpusDecoder *dec;
OpusBuffer packets_to_send = {0};
OpusBuffer packets_to_play = {0};
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
	bool needs_tobe_unlocked;
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
		.needs_tobe_unlocked = true,
	},
};
#define ARRLEN(arr) (sizeof(arr) / sizeof(*arr))

static struct SquadMeta
{
	enum Squad squad;
	float hue;
	bool is_colorless;
} squad_metas[] = {
	{
		.squad = SquadNone,
		.is_colorless = true,
	},
	{
		.squad = SquadRed,
		.hue = 21.0f / 360.0f,
	},
	{
		.squad = SquadGreen,
		.hue = 111.0f / 360.0f,
	},
	{
		.squad = SquadBlue,
		.hue = 201.0f / 360.0f,
	},
	{
		.squad = SquadPurple,
		.hue = 291.0f / 360.0f,
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

struct BoxInfo
boxinfo(enum BoxType type)
{
	for (int i = 0; i < ARRLEN(boxes); i++)
	{
		if (boxes[i].type == type)
			return boxes[i];
	}
	Log("No box info found for type %d\n", type);
	return (struct BoxInfo){0};
}

static sg_image
load_image(const char *path)
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
		fprintf(stderr, "Failed to load image: %s\n", stbi_failure_reason());
		exit(-1);
	}
	sg_init_image(to_return,
				  &(sg_image_desc){.width = x,
								   .height = y,
								   .pixel_format = SG_PIXELFORMAT_RGBA8,
								   .min_filter = SG_FILTER_NEAREST,
								   .mag_filter = SG_FILTER_NEAREST,
								   .data.subimage[0][0] = {
									   .ptr = image_data,
									   .size = (size_t)(x * y * desired_channels),
								   }});

	stbi_image_free(image_data);

	return to_return;
}

void microphone_data_callback(ma_device *pDevice, void *pOutput, const void *pInput, ma_uint32 frameCount)
{
	assert(frameCount == VOIP_EXPECTED_FRAME_COUNT);
	if (peer != NULL)
	{
		ma_mutex_lock(&send_packets_mutex);
		OpusPacket *packet = push_packet(&packets_to_send);
		if (packet != NULL)
		{
			opus_int16 muted_audio[VOIP_EXPECTED_FRAME_COUNT] = {0};
			const opus_int16 *audio_buffer = (const opus_int16 *)pInput;
			if (muted)
				audio_buffer = muted_audio;
			opus_int32 written = opus_encode(enc, audio_buffer, VOIP_EXPECTED_FRAME_COUNT, packet->data, VOIP_PACKET_MAX_SIZE);
			packet->length = written;
		}
		ma_mutex_unlock(&send_packets_mutex);
	}
	(void)pOutput;
}

void speaker_data_callback(ma_device *pDevice, void *pOutput, const void *pInput, ma_uint32 frameCount)
{
	assert(frameCount == VOIP_EXPECTED_FRAME_COUNT);
	ma_mutex_lock(&play_packets_mutex);
	OpusPacket *cur_packet = pop_packet(&packets_to_play);
	if (cur_packet != NULL && cur_packet->length > 0) // length of 0 means skipped packet
	{
		opus_decode(dec, cur_packet->data, cur_packet->length, (opus_int16 *)pOutput, frameCount, 0);
	}
	else
	{
		opus_decode(dec, NULL, 0, (opus_int16 *)pOutput, frameCount, 0); // I think opus makes it sound good if packets are skipped with null
	}
	ma_mutex_unlock(&play_packets_mutex);
	(void)pInput;
}

static void
init(void)
{

	// audio
	{
		// opus
		{
			int error;
			enc = opus_encoder_create(VOIP_SAMPLE_RATE, 1, OPUS_APPLICATION_VOIP, &error);
			assert(error == OPUS_OK);
			dec = opus_decoder_create(VOIP_SAMPLE_RATE, 1, &error);
			assert(error == OPUS_OK);
		}

		ma_device_config microphone_config = ma_device_config_init(ma_device_type_capture);

		microphone_config.capture.format = ma_format_s16;
		microphone_config.capture.channels = 1;
		microphone_config.sampleRate = VOIP_SAMPLE_RATE;
		microphone_config.dataCallback = microphone_data_callback;

		ma_device_config speaker_config = ma_device_config_init(ma_device_type_playback);
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
		fprintf(stderr,
				"Failed to create Sokol GP context: %s\n",
				sgp_get_error_message(sgp_get_last_error()));
		exit(-1);
	}

	// shaders
	{
		// initialize shader
		sgp_pipeline_desc pip_desc = {
			.shader = *hueshift_program_shader_desc(sg_query_backend()),
			.blend_mode = SGP_BLENDMODE_BLEND,
		};
		pip = sgp_make_pipeline(&pip_desc);
		if (sg_query_pipeline_state(pip) != SG_RESOURCESTATE_VALID)
		{
			fprintf(stderr, "failed to make custom pipeline\n");
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
		image_flag_available = load_image("loaded/flag_available.png");
		image_flag_taken = load_image("loaded/flag_ripped.png");
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
		address.port = SERVER_PORT;
		peer = enet_host_connect(client, &address, 2, 0);
		if (peer == NULL)
		{
			fprintf(stderr,
					"No available peers for initiating an ENet connection.\n");
			exit(-1);
		}
		// the timeout is the third parameter here
		if (enet_host_service(client, &event, 5000) > 0 && event.type == ENET_EVENT_TYPE_CONNECT)
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

#define transform_scope DeferLoop(sgp_push_transform(), sgp_pop_transform())

static void
draw_color_rect_centered(V2 center, float size)
{
	float halfbox = size / 2.0f;

	sgp_draw_filled_rect(center.x - halfbox, center.y - halfbox, size, size);
}

static void
draw_texture_rectangle_centered(V2 center, V2 width_height)
{
	V2 halfsize = V2scale(width_height, 0.5f);
	sgp_draw_textured_rect(center.x - halfsize.x,
						   center.y - halfsize.y,
						   width_height.x,
						   width_height.y);
}

static void
draw_texture_centered(V2 center, float size)
{
	draw_texture_rectangle_centered(center, (V2){size, size});
}

static void
draw_circle(V2 point, float radius)
{
#define POINTS 64
	sgp_line lines[POINTS];
	for (int i = 0; i < POINTS; i++)
	{
		float progress = (float)i / (float)POINTS;
		float next_progress = (float)(i + 1) / (float)POINTS;
		lines[i].a = (V2){.x = cosf(progress * 2.0f * PI) * radius,
						  .y = sinf(progress * 2.0f * PI) * radius};
		lines[i].b = (V2){.x = cosf(next_progress * 2.0f * PI) * radius,
						  .y = sinf(next_progress * 2.0f * PI) * radius};
		lines[i].a = V2add(lines[i].a, point);
		lines[i].b = V2add(lines[i].b, point);
	}
	sgp_draw_lines(lines, POINTS);
}

static Entity *
myentity()
{
	if (myplayer == -1)
		return NULL;
	Entity *to_return = get_entity(&gs, gs.players[myplayer].entity);
	if (to_return != NULL)
		assert(to_return->is_player);
	return to_return;
}

bool can_build(int i)
{
	bool allow_building = true;
	if (boxinfo((enum BoxType)i).needs_tobe_unlocked)
	{
		allow_building = gs.players[myplayer].unlocked_bombs;
	}
	return allow_building;
}

void attempt_to_build(int i)
{
	if (can_build(i))
		cur_editing_boxtype = i;
}

static void
ui(bool draw, float dt, float width, float height)
{
	static float cur_opacity = 1.0f;
	cur_opacity = lerp(cur_opacity, myentity() != NULL ? 1.0f : 0.0f, dt * 5.0f);
	if (cur_opacity <= 0.01f)
	{
		return;
	}

	if (draw)
		sgp_push_transform();

	// draw flags
	static V2 flag_pos[SquadLast] = {0};
	static float flag_rot[SquadLast] = {0};
	static float flag_scaling_increase[SquadLast] = {0};
	static bool choosing_flags = false;
	const float flag_padding = 70.0f;
	const float center_panel_height = 200.0f;
	static float center_panel_width = 0.0f;
	const float target_center_panel_width = ((SquadLast) + 2) * flag_padding;
#define FLAG_ITER(i) for (int i = 0; i < SquadLast; i++)
	{
		FLAG_ITER(i)
		{
			V2 target_pos = {0};
			float target_rot = 0.0f;
			float flag_progress = (float)i / (float)(SquadLast - 1.0f);
			if (choosing_flags)
			{
				target_pos.x = width / 2.0f + lerp(-center_panel_width / 2.0f + flag_padding, center_panel_width / 2.0f - flag_padding, flag_progress);
				target_pos.y = height * 0.5f;
				target_rot = 0.0f;
			}
			else
			{
				target_pos.x = 25.0f;
				target_pos.y = 200.0f;
				target_rot = lerp(-PI / 3.0f, PI / 3.0f, flag_progress) + PI / 2.0f;
			}
			flag_pos[i] = V2lerp(flag_pos[i], target_pos, dt * 5.0f);
			flag_rot[i] = lerp_angle(flag_rot[i], target_rot, dt * 5.0f);
		}

		center_panel_width = lerp(center_panel_width, choosing_flags ? target_center_panel_width : 0.0f, 6.0f * dt);

		// center panel
		{
			AABB panel_rect = (AABB){
				.x = width / 2.0f - center_panel_width / 2.0f,
				.y = height / 2.0f - center_panel_height / 2.0f,
				.width = center_panel_width,
				.height = center_panel_height,
			};

			if (choosing_flags && mouse_pressed && !has_point(panel_rect, mouse_pos))
			{
				mouse_pressed = false;
				choosing_flags = false;
			}
			if (draw)
			{
				sgp_set_color(0.7f, 0.7f, 0.7f, 0.5f);
				sgp_draw_filled_rect(panel_rect.x, panel_rect.y, panel_rect.width, panel_rect.height);
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

			float size = 128.0f;
			bool hovering = V2dist(mouse_pos, flag_pos[i]) < size * 0.25f && this_squad_available;

			if (!choosing_flags && hovering && mouse_pressed)
			{
				choosing_flags = true;
				mouse_pressed = false;
			}

			if (this_squad_available && choosing_flags && hovering && mouse_pressed)
			{
				take_over_squad = this_squad;
				mouse_pressed = false;
			}

			flag_scaling_increase[i] = lerp(flag_scaling_increase[i], hovering ? 0.2f : 0.0f, dt * 9.0f);

			size *= 1.0f + flag_scaling_increase[i];

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

					sgp_set_pipeline(pip);
					struct SquadMeta meta = squad_meta(this_squad);
					hueshift_uniforms_t uniform = {0};
					uniform.is_colorless = meta.is_colorless;
					uniform.target_hue = meta.hue;
					sgp_set_uniform(&uniform, sizeof(hueshift_uniforms_t));

					sgp_rotate_at(flag_rot[i], flag_pos[i].x, flag_pos[i].y);
					sgp_scale_at(1.0f, -1.0f, flag_pos[i].x, flag_pos[i].y); // images upside down by default :(
					draw_texture_centered(flag_pos[i], size);

					sgp_reset_image(0);
					sgp_reset_pipeline();
				}
			}
		}

		if (choosing_flags) mouse_pressed = false; // no more inputs beyond flags when the flag choice modal is open
	}
#undef FLAG_ITER

	//  draw spice bar
	if (draw)
	{
		static float damage = 0.5f;

		if (myentity() != NULL)
		{
			damage = myentity()->damage;
		}

		sgp_set_color(0.5f, 0.5f, 0.5f, cur_opacity);
		float margin = width * 0.2f;
		float bar_width = width - margin * 2.0f;
		float y = height - 150.0f;
		sgp_draw_filled_rect(margin, y, bar_width, 30.0f);
		sgp_set_color(1.0f, 1.0f, 1.0f, cur_opacity);
		sgp_draw_filled_rect(
			margin, y, bar_width * (1.0f - damage), 30.0f);
	}

	// draw muted
	static float muted_opacity = 0.0f;
	if (draw)
	{
		muted_opacity = lerp(muted_opacity, muted ? 1.0f : 0.0f, 8.0f * dt);
		sgp_set_color(1.0f, 1.0f, 1.0f, muted_opacity);
		float size_x = 150.0f;
		float size_y = 150.0f;
		sgp_set_image(0, image_mic_muted);

		float x = width - size_x - 40.0f;
		float y = height - size_y - 40.0f;
		transform_scope
		{
			sgp_scale_at(1.0f, -1.0f, x + size_x / 2.0f, y + size_y / 2.0f);
			sgp_draw_textured_rect(x, y, size_x, size_y);
			sgp_reset_image(0);
		}
	}

	// draw item toolbar
	{
		float itemframe_width = (float)sg_query_image_info(image_itemframe).width * 2.0f;
		float itemframe_height = (float)sg_query_image_info(image_itemframe).height * 2.0f;
		float total_width = itemframe_width * ARRLEN(boxes);
		float item_width = itemframe_width * 0.75f;
		float item_height = itemframe_height * 0.75f;
		float item_offset_x = (itemframe_width - item_width) / 2.0f;
		float item_offset_y = (itemframe_height - item_height) / 2.0f;

		float x = width / 2.0f - total_width / 2.0f;
		float y = height - itemframe_height * 1.5f;
		for (int i = 0; i < ARRLEN(boxes); i++)
		{
			if (has_point(
					(AABB){
						.x = x,
						.y = y,
						.width = itemframe_width,
						.height = itemframe_height,
					},
					mouse_pos) &&
				mouse_pressed)
			{
				// "handle" mouse pressed
				attempt_to_build(i);
				mouse_pressed = false;
			}

			if (draw)
			{
				sgp_set_color(1.0f, 1.0f, 1.0f, cur_opacity);
				if (cur_editing_boxtype == i)
				{
					sgp_set_image(0, image_itemframe_selected);
				}
				else
				{
					sgp_set_image(0, image_itemframe);
				}
				sgp_draw_textured_rect(x, y, itemframe_width, itemframe_height);
				struct BoxInfo info = boxinfo((enum BoxType)i);
				if (can_build(i))
				{
					sgp_set_image(0, info.image);
				}
				else
				{
					sgp_set_image(0, image_mystery);
				}
				transform_scope
				{
					float item_x = x + item_offset_x;
					float item_y = y + item_offset_y;
					sgp_scale_at(1.0f, -1.0f, item_x + item_width / 2.0f, item_y + item_height / 2.0f);
					// sgp_scale(1.0f, -1.0f);
					sgp_draw_textured_rect(item_x, item_y, item_width, item_height);
				}
				sgp_reset_image(0);
			}
			x += itemframe_width;
		}
	}

	if (draw)
		sgp_pop_transform();
}

static void draw_dots(V2 camera_pos, float gap)
{
	sgp_set_color(1.0f, 1.0f, 1.0f, 1.0f);
	const int num = 100;
	for (int x = -num; x < num; x++)
	{
		for (int y = -num; y < num; y++)
		{
			V2 star = (V2){(float)x * gap, (float)y * gap};
			if (V2lengthsqr(V2sub(star, camera_pos)) > VISION_RADIUS * VISION_RADIUS)
				continue;

			star.x += hash11(star.x * 100.0f + star.y * 67.0f) * gap;
			star.y += hash11(star.y * 93.0f + star.x * 53.0f) * gap;
			sgp_draw_point(star.x, star.y);
		}
	}
}

static void
frame(void)
{
	float width = (float)sapp_width(), height = (float)sapp_height();
	float ratio = width / height;
	double time = sapp_frame_count() * sapp_frame_duration();
	float dt = (float)sapp_frame_duration();

	// pressed input management
	{
		for (int i = 0; i < SAPP_KEYCODE_MENU; i++)
		{
			if (keypressed[i].frame < sapp_frame_count())
			{
				keypressed[i].pressed = false;
			}
		}
		if (mouse_pressed_frame < sapp_frame_count())
		{
			mouse_pressed = false;
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
				{
					Log("New client from host %x\n", event.peer->address.host);
					break;
				}

				case ENET_EVENT_TYPE_RECEIVE:
				{
					char *decompressed = malloc(sizeof *decompressed * MAX_SERVER_TO_CLIENT); // @Robust no malloc
					size_t decompressed_max_len = MAX_SERVER_TO_CLIENT;
					assert(LZO1X_MEM_DECOMPRESS == 0);

					ma_mutex_lock(&play_packets_mutex);
					ServerToClient msg = (ServerToClient){
						.cur_gs = &gs,
						.playback_buffer = &packets_to_play,
					};
					int return_value = lzo1x_decompress_safe(event.packet->data, event.packet->dataLength, decompressed, &decompressed_max_len, NULL);
					if (return_value == LZO_E_OK)
					{
						server_to_client_deserialize(&msg, decompressed, decompressed_max_len, false);
						myplayer = msg.your_player;
					}
					else
					{
						Log("Couldn't decompress gamestate packet, error code %d from lzo\n", return_value);
					}
					ma_mutex_unlock(&play_packets_mutex);
					free(decompressed);
					enet_packet_destroy(event.packet);
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
	ui(false, dt, width, height); // handle events
	V2 build_target_pos = {0};
	float build_target_rotation = 0.0f;
	static V2 camera_pos = {
		0};							// keeps camera at same position after player death
	V2 world_mouse_pos = mouse_pos; // processed later in scope
	struct BuildPreviewInfo
	{
		V2 grid_pos;
		float grid_rotation;
	} build_preview = {0};
	V2 hand_pos = {0}; // in local space of grid when hovering over a grid
	bool hand_at_arms_length = false;
	{
		// interpolate zoom
		zoom = lerp(zoom, zoom_target, dt * 12.0f);

		// calculate world position and camera
		{
			if (myentity() != NULL)
			{
				camera_pos = entity_pos(myentity());
			}
			world_mouse_pos = V2sub(world_mouse_pos, (V2){.x = width / 2.0f, .y = height / 2.0f});
			world_mouse_pos.x /= zoom;
			world_mouse_pos.y /= -zoom;
			world_mouse_pos = V2add(world_mouse_pos, (V2){.x = camera_pos.x, .y = camera_pos.y});
		}

		// calculate build preview stuff
		EntityID grid_to_build_on = (EntityID){0};
		V2 possibly_local_hand_pos = (V2){0};
		if (myentity() != NULL)
		{
			hand_pos = V2sub(world_mouse_pos, entity_pos(myentity()));
			float hand_len = V2length(hand_pos);
			if (hand_len > MAX_HAND_REACH)
			{
				hand_at_arms_length = true;
				hand_len = MAX_HAND_REACH;
			}
			else
			{
				hand_at_arms_length = false;
			}
			hand_pos = V2scale(V2normalize(hand_pos), hand_len);
			hand_pos = V2add(hand_pos, entity_pos(myentity()));

			possibly_local_hand_pos = V2sub(hand_pos, entity_pos(myentity()));
			Entity *placing_grid = closest_to_point_in_radius(&gs, hand_pos, BUILD_BOX_SNAP_DIST_TO_SHIP);
			if (placing_grid == NULL)
			{
				build_preview = (struct BuildPreviewInfo){
					.grid_pos = hand_pos,
					.grid_rotation = 0.0f,
				};
			}
			else
			{
				grid_to_build_on = get_id(&gs, placing_grid);
				hand_pos = grid_snapped_box_pos(placing_grid, hand_pos);
				possibly_local_hand_pos = grid_world_to_local(placing_grid, hand_pos);
				build_preview = (struct BuildPreviewInfo){
					.grid_pos = entity_pos(placing_grid),
					.grid_rotation = entity_rotation(placing_grid),
				};
			}
		}

		// Create and send input packet
		{

			static size_t last_frame_id = 0;
			InputFrame cur_input_frame = {0};
			cur_input_frame.id = last_frame_id;
			V2 input = (V2){
				.x = (float)keydown[SAPP_KEYCODE_D] - (float)keydown[SAPP_KEYCODE_A],
				.y = (float)keydown[SAPP_KEYCODE_W] - (float)keydown[SAPP_KEYCODE_S],
			};
			if (V2length(input) > 0.0)
				input = V2normalize(input);
			cur_input_frame.movement = input;
			cur_input_frame.seat_action = keypressed[SAPP_KEYCODE_G].pressed;
			cur_input_frame.grid_hand_pos_local_to = grid_to_build_on;
			cur_input_frame.hand_pos = possibly_local_hand_pos;
			cur_input_frame.take_over_squad = take_over_squad;

			if (mouse_pressed && cur_editing_boxtype != -1)
			{
				cur_input_frame.dobuild = mouse_pressed;
				cur_input_frame.build_type = cur_editing_boxtype;
				cur_input_frame.build_rotation = cur_editing_rotation;
			}

			InputFrame latest = client_to_server.inputs[0];
			// @Robust split this into separate lines and be very careful about testing for inequality

			bool input_differs = false;
			input_differs = input_differs || !V2equal(cur_input_frame.movement, latest.movement, 0.01f);

			input_differs = input_differs || cur_input_frame.seat_action != latest.seat_action;
			input_differs = input_differs || !entityids_same(cur_input_frame.seat_to_inhabit, latest.seat_to_inhabit);
			input_differs = input_differs || !V2equal(cur_input_frame.hand_pos, latest.hand_pos, 0.01f);

			input_differs = input_differs || cur_input_frame.dobuild != latest.dobuild;
			input_differs = input_differs || cur_input_frame.build_type != latest.build_type;
			input_differs = input_differs || cur_input_frame.build_rotation != latest.build_rotation;
			input_differs = input_differs || !entityids_same(cur_input_frame.grid_hand_pos_local_to, latest.grid_hand_pos_local_to);

			input_differs = input_differs || cur_input_frame.take_over_squad != latest.take_over_squad;

			if (input_differs)
			{
				InputFrame last_frame = client_to_server.inputs[0];
				for (int i = 0; i < INPUT_BUFFER - 1; i++)
				{
					InputFrame last_last_frame = last_frame;
					last_frame = client_to_server.inputs[i + 1];
					client_to_server.inputs[i + 1] = last_last_frame;

					// these references, in old input frames, may have been deleted by the time we
					// want to send them.
					client_to_server.inputs[i + 1].seat_to_inhabit = cur_input_frame.seat_to_inhabit;
					client_to_server.inputs[i + 1].grid_hand_pos_local_to = cur_input_frame.grid_hand_pos_local_to;
				}
				cur_input_frame.tick = tick(&gs);
				client_to_server.inputs[0] = cur_input_frame;
				last_frame_id += 1;
			}

			static int64_t last_sent_input_time = 0;
			if (stm_sec(stm_diff(stm_now(), last_sent_input_time)) > TIME_BETWEEN_INPUT_PACKETS)
			{
				ma_mutex_lock(&send_packets_mutex);
				client_to_server.mic_data = &packets_to_send;
				char serialized[MAX_CLIENT_TO_SERVER] = {0};
				size_t out_len = 0;
				if (client_to_server_serialize(&gs, &client_to_server, serialized, &out_len, MAX_CLIENT_TO_SERVER))
				{
					ENetPacket *packet = enet_packet_create((void *)serialized,
															out_len,
															ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT);
					enet_peer_send(peer, 0, packet); // @Robust error check this
					last_sent_input_time = stm_now();
				}
				else
				{
					Log("Failed to serialize client to server!\n");
				}
				client_to_server.mic_data = NULL;
				ma_mutex_unlock(&send_packets_mutex);
			}
		}

		// @BeforeShip client side prediction and rollback to previous server
		// authoritative state, then replay inputs no need to store copies of game
		// state, just player input frame to frame. Then know how many frames ago
		// the server game state arrived, it's that easy! process(&gs,
		// (float)sapp_frame_duration());
	}

	// drawing
	{
		sgp_begin((int)width, (int)height);
		sgp_viewport(0, 0, (int)width, (int)height);
		sgp_project(0.0f, width, 0.0f, height);
		sgp_set_blend_mode(SGP_BLENDMODE_BLEND);

		// Draw background color
		set_color(colhexcode(0x000000));
		// sgp_set_color(0.1f, 0.1f, 0.1f, 1.0f);
		sgp_clear();

		// sokol drawing library draw in world space
		// world space coordinates are +Y up, -Y down. Like normal cartesian coords
		transform_scope
		{
			sgp_translate(width / 2, height / 2);
			sgp_scale_at(zoom, -zoom, 0.0f, 0.0f);

			// parllax layers, just the zooming, but not 100% of the camera panning
#if 1 // space background
			transform_scope
			{
				V2 scaled_camera_pos = V2scale(camera_pos, 0.05f); // this is how strong/weak the parallax is
				sgp_translate(-scaled_camera_pos.x, -scaled_camera_pos.y);
				set_color(WHITE);
				sgp_set_image(0, image_stars);
				float stars_height_over_width = (float)sg_query_image_info(image_stars).height / (float)sg_query_image_info(image_stars).width;
				const float stars_width = 35.0f;
				float stars_height = stars_width * stars_height_over_width;
				sgp_draw_textured_rect(-stars_width / 2.0f, -stars_height / 2.0f, stars_width, stars_height);
				// sgp_draw_textured_rect(0, 0, stars_width, stars_height);
				sgp_reset_image(0);
			}
			transform_scope
			{
				V2 scaled_camera_pos = V2scale(camera_pos, 0.1f); // this is how strong/weak the parallax is
				sgp_translate(-scaled_camera_pos.x, -scaled_camera_pos.y);
				set_color(WHITE);
				sgp_set_image(0, image_stars2);
				float stars_height_over_width = (float)sg_query_image_info(image_stars).height / (float)sg_query_image_info(image_stars).width;
				const float stars_width = 35.0f;
				float stars_height = stars_width * stars_height_over_width;
				sgp_draw_textured_rect(-stars_width / 2.0f, -stars_height / 2.0f, stars_width, stars_height);
				// sgp_draw_textured_rect(0, 0, stars_width, stars_height);
				sgp_reset_image(0);
			}
#endif

#if 1 // parallaxed dots
			transform_scope
			{
				V2 scaled_camera_pos = V2scale(camera_pos, 0.25f);
				sgp_translate(-scaled_camera_pos.x, -scaled_camera_pos.y);
				set_color(WHITE);
				draw_dots(scaled_camera_pos, 3.0f);
			}
			transform_scope
			{
				V2 scaled_camera_pos = V2scale(camera_pos, 0.5f);
				sgp_translate(-scaled_camera_pos.x, -scaled_camera_pos.y);
				set_color(WHITE);
				draw_dots(scaled_camera_pos, 2.0f);
			}
#endif

			// camera go to player
			sgp_translate(-camera_pos.x, -camera_pos.y);

			draw_dots(camera_pos, 1.5f); // in plane dots

			// hand reached limit circle
			if (myentity() != NULL)
			{
				static float hand_reach_alpha = 1.0f;
				hand_reach_alpha = lerp(hand_reach_alpha, hand_at_arms_length ? 1.0f : 0.0f, dt * 5.0f);
				sgp_set_color(1.0f, 1.0f, 1.0f, hand_reach_alpha);
				draw_circle(entity_pos(myentity()), MAX_HAND_REACH);
			}

			// vision circle, what player can see
			if (myentity() != NULL)
			{
				set_color(colhexcode(0x4685e3));
				draw_circle(entity_pos(myentity()), VISION_RADIUS);
			}

			float halfbox = BOX_SIZE / 2.0f;

			// mouse frozen, debugging tool
			if (mouse_frozen)
			{
				sgp_set_color(1.0f, 0.0f, 0.0f, 0.5f);
				sgp_draw_filled_rect(world_mouse_pos.x, world_mouse_pos.y, 0.1f, 0.1f);
			}

			// building preview
			if (cur_editing_boxtype != -1)
			{
				sgp_set_color(0.5f, 0.5f, 0.5f, (sinf((float)time * 9.0f) + 1.0f) / 3.0f + 0.2f);

				transform_scope
				{
					sgp_set_image(0, boxinfo(cur_editing_boxtype).image);
					sgp_rotate_at(build_preview.grid_rotation + rotangle(cur_editing_rotation),
								  hand_pos.x,
								  hand_pos.y);
					draw_texture_centered(hand_pos, BOX_SIZE);
					// drawbox(hand_pos, build_preview.grid_rotation, 0.0f,
					// cur_editing_boxtype, cur_editing_rotation);
					sgp_reset_image(0);
				}
			}

			static float player_scaling = 1.0f;
			player_scaling = lerp(player_scaling, zoom < 6.5f ? 100.0f : 1.0f, dt * 7.0f);
			for (size_t i = 0; i < gs.cur_next_entity; i++)
			{
				Entity *e = &gs.entities[i];
				if (!e->exists)
					continue;
				// draw grid
				if (e->is_grid)
				{
					Entity *g = e;
					BOXES_ITER(&gs, b, g)
					{
						if (b->is_explosion_unlock)
						{
							set_color(colhexcode(0xfcba03));
							draw_circle(entity_pos(b), GOLD_UNLOCK_RADIUS);
						}
						sgp_set_color(1.0f, 1.0f, 1.0f, 1.0f);
// debug draw force vectors for thrusters
#if 0
																			{
																				if (b->type == BoxThruster)
																				{
																					dbg_rect(entity_pos(b));
																					dbg_line(entity_pos(b), V2add(entity_pos(b), V2scale(thruster_force(b), -1.0f)));
																				}
																			}
#endif
						if (b->box_type == BoxBattery)
						{
							float cur_alpha = sgp_get_color().a;
							Color from = WHITE;
							Color to = colhex(255, 0, 0);
							Color result = Collerp(from, to, b->energy_used / BATTERY_CAPACITY);
							sgp_set_color(result.r, result.g, result.b, cur_alpha);
						}
						transform_scope
						{
							sgp_rotate_at(entity_rotation(g) + rotangle(b->compass_rotation),
										  entity_pos(b).x,
										  entity_pos(b).y);

							if (b->box_type == BoxThruster)
							{
								transform_scope
								{
									sgp_set_color(1.0f, 1.0f, 1.0f, 1.0f);
									sgp_set_image(0, image_thrusterburn);
									// float scaling = 1.0 + (hash11(time*3.0)/2.0)*lerp(0.0,
									// 0.07, b->thrust); printf("%f\n", b->thrust);
									float scaling = 0.95f + lerp(0.0f, 0.3f, b->thrust);
									// float scaling = 1.1;
									// sgp_translate(-(scaling*BOX_SIZE - BOX_SIZE), 0.0);
									// sgp_scale(scaling, 1.0);
									sgp_scale_at(scaling, 1.0f, entity_pos(b).x, entity_pos(b).y);
									draw_texture_centered(entity_pos(b), BOX_SIZE);
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
							sgp_set_image(0, img);
							if (b->indestructible)
							{
								sgp_set_color(0.2f, 0.2f, 0.2f, 1.0f);
							}
							draw_texture_centered(entity_pos(b), BOX_SIZE);
							sgp_reset_image(0);

							if (b->box_type == BoxSolarPanel)
							{
								Color to_set = colhexcode(0xeb9834);
								to_set.a = b->sun_amount * 0.5f;
								set_color(to_set);
								draw_color_rect_centered(entity_pos(b), BOX_SIZE);
							}

							sgp_set_color(0.5f, 0.1f, 0.1f, b->damage);
							draw_color_rect_centered(entity_pos(b), BOX_SIZE);
						}
					}

					// draw the velocity
#if 0
						sgp_set_color(1.0f, 0.0f, 0.0f, 1.0f);
						V2 vel = grid_vel(g);
						V2 to = V2add(grid_com(g), vel);
						sgp_draw_line(grid_com(g).x, grid_com(g).y, to.x, to.y);
#endif
				}

				// draw player
				if (e->is_player && get_entity(&gs, e->currently_inside_of_box) == NULL)
				{
					transform_scope
					{
						sgp_rotate_at(entity_rotation(e), entity_pos(e).x, entity_pos(e).y);
						sgp_set_color(1.0f, 1.0f, 1.0f, 1.0f);

						sgp_set_pipeline(pip);
						struct SquadMeta meta = squad_meta(e->presenting_squad);
						hueshift_uniforms_t uniform = {0};
						uniform.is_colorless = meta.is_colorless;
						uniform.target_hue = meta.hue;
						sgp_set_uniform(&uniform, sizeof(hueshift_uniforms_t));
						sgp_set_image(0, image_player);
						draw_texture_rectangle_centered(entity_pos(e), V2scale(PLAYER_SIZE, player_scaling));
						sgp_reset_image(0);
						sgp_reset_pipeline();
					}
				}
				if (e->is_explosion)
				{
					sgp_set_image(0, image_explosion);
					sgp_set_color(1.0f, 1.0f, 1.0f, 1.0f - (e->explosion_progresss / EXPLOSION_TIME));
					draw_texture_centered(e->explosion_pos, EXPLOSION_RADIUS * 2.0f);
					sgp_reset_image(0);
				}
			}

			// gold target
			set_color(GOLD);
			sgp_draw_filled_rect(gs.goldpos.x, gs.goldpos.y, 0.1f, 0.1f);

			// the SUN
			transform_scope
			{
				sgp_translate(SUN_POS.x, SUN_POS.y);
				set_color(WHITE);
				sgp_set_image(0, image_sun);
				draw_texture_centered((V2){0}, SUN_RADIUS * 2.0f);
				sgp_reset_image(0);

				// sun DEATH RADIUS
				set_color(RED);
				draw_circle((V2){0}, INSTANT_DEATH_DISTANCE_FROM_SUN);
			}

			sgp_set_color(1.0f, 1.0f, 1.0f, 1.0f);
			dbg_drawall();
		} // world space transform end

		// low health
		if (myentity() != NULL)
		{
			sgp_set_color(1.0f, 1.0f, 1.0f, myentity()->damage);
			sgp_set_image(0, image_low_health);
			draw_texture_rectangle_centered((V2){width / 2.0f, height / 2.0f}, (V2){width, height});
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

void cleanup(void)
{
	sg_destroy_pipeline(pip);

	ma_mutex_lock(&server_info.info_mutex);
	server_info.should_quit = true;
	ma_mutex_unlock(&server_info.info_mutex);
	WaitForSingleObject(server_thread_handle, INFINITE);

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
		if (e->key_code == SAPP_KEYCODE_M)
		{
			muted = !muted;
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
		int target_box = key_num - 1;
		if (target_box < BoxLast && target_box >= 0)
		{
			attempt_to_build(target_box);
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
		zoom_target *= 1.0f + (e->scroll_y / 4.0f) * 0.1f;
		zoom_target = clamp(zoom_target, 0.5f, 900.0f);
		break;
	case SAPP_EVENTTYPE_MOUSE_DOWN:
		if (e->mouse_button == SAPP_MOUSEBUTTON_LEFT)
		{
			mouse_pressed = true;
			mouse_pressed_frame = e->frame_count;
		}
		if (e->mouse_button == SAPP_MOUSEBUTTON_RIGHT)
		{
			right_mouse_down = true;
		}
		break;
	case SAPP_EVENTTYPE_MOUSE_UP:
		if (e->mouse_button == SAPP_MOUSEBUTTON_LEFT)
		{
			mouse_pressed = false;
			mouse_pressed_frame = 0;
		}
		if (e->mouse_button == SAPP_MOUSEBUTTON_RIGHT)
		{
			right_mouse_down = false;
		}
		break;
	case SAPP_EVENTTYPE_MOUSE_MOVE:
		if (!mouse_frozen)
		{
			mouse_pos = (V2){.x = e->mouse_x, .y = e->mouse_y};
		}
		if (right_mouse_down)
		{
			funval += e->mouse_dx;
			Log("Funval %f\n", funval);
		}
		break;
	}
}

sapp_desc
sokol_main(int argc, char *argv[])
{
	bool hosting = false;
	stm_setup();
	ma_mutex_init(&server_info.info_mutex);
	server_info.world_save = "debug_world.bin";
	if (argc > 1)
	{
		server_thread_handle = (void *)_beginthread(server, 0, (void *)&server_info);
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