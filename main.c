//------------------------------------------------------------------------------
//  Take flight
//------------------------------------------------------------------------------

#define SOKOL_IMPL
#define SOKOL_D3D11
#include <enet/enet.h>
#include <process.h> // starting server thread

#pragma warning ( disable: 33010 ) // this warning is so broken, doesn't understand assert()
#pragma warning ( disable: 33010 ) // this warning is so broken, doesn't understand assert()
#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_glue.h"
#include "sokol_gp.h"
#include "sokol_time.h"
#pragma warning ( default: 33010 )
#pragma warning ( disable: 6262 ) // warning about using a lot of stack, lol that's how stb image is
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include "types.h"

#include <inttypes.h>
#include <string.h> // errno error message on file open

#include "minilzo.h"

static struct GameState gs = { 0 };
static int myplayer = -1;
static bool right_mouse_down = false;
static bool keydown[SAPP_KEYCODE_MENU] = { 0 };
typedef struct KeyPressed {
	bool pressed;
	uint64_t frame;
} KeyPressed;
static KeyPressed keypressed[SAPP_KEYCODE_MENU] = { 0 };
static V2 mouse_pos = { 0 };
static bool mouse_pressed = false;
static uint64_t mouse_pressed_frame = 0;
static bool mouse_frozen = false; // @BeforeShip make this debug only thing
static float funval = 0.0f; // easy to play with value controlled by left mouse button when held
// down @BeforeShip remove on release builds
static struct ClientToServer client_to_server = { 0 }; // buffer of inputs
static ENetHost* client;
static ENetPeer* peer;
static float zoom_target = 300.0f;
static float zoom = 300.0f;
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
static int cur_editing_boxtype = -1;
static int cur_editing_rotation = 0;

static struct BoxInfo {
	enum BoxType type;
	const char* image_path;
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
const int boxes_len = sizeof(boxes) / sizeof(*boxes);

struct BoxInfo
	boxinfo(enum BoxType type)
{
	for (int i = 0; i < boxes_len; i++) {
		if (boxes[i].type == type)
			return boxes[i];
	}
	Log("No box info found for type %d\n", type);
	return (struct BoxInfo) { 0 };
}

static sg_image
load_image(const char* path)
{
	sg_image to_return = sg_alloc_image();

	int x = 0;
	int y = 0;
	int comp = 0;
	const int desired_channels = 4;
	stbi_set_flip_vertically_on_load(true);
	stbi_uc* image_data = stbi_load(path, &x, &y, &comp, desired_channels);
	if (!image_data) {
		fprintf(stderr, "Failed to load image: %s\n", stbi_failure_reason());
		exit(-1);
	}
	sg_init_image(to_return,
		&(sg_image_desc) {.width = x,
		.height = y,
		.pixel_format = SG_PIXELFORMAT_RGBA8,
		.min_filter = SG_FILTER_NEAREST,
		.mag_filter = SG_FILTER_NEAREST,
		.data.subimage[0][0] = {
			.ptr = image_data,
			.size = (size_t)(x * y * desired_channels),
		} });

	stbi_image_free(image_data);

	return to_return;
}
static void
init(void)
{
	// @BeforeShip make all fprintf into logging to file, warning dialog grids on
	// failure instead of exit(-1), replace the macros in sokol with this as well,
	// like assert

	Entity* entity_data = malloc(sizeof * entity_data * MAX_ENTITIES);
	initialize(&gs, entity_data, sizeof * entity_data * MAX_ENTITIES);

	sg_desc sgdesc = { .context = sapp_sgcontext() };
	sg_setup(&sgdesc);
	if (!sg_isvalid()) {
		fprintf(stderr, "Failed to create Sokol GFX context!\n");
		exit(-1);
	}

	sgp_desc sgpdesc = { 0 };
	sgp_setup(&sgpdesc);
	if (!sgp_is_valid()) {
		fprintf(stderr,
			"Failed to create Sokol GP context: %s\n",
			sgp_get_error_message(sgp_get_last_error()));
		exit(-1);
	}

	// image loading
	{
		for (int i = 0; i < boxes_len; i++) {
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
	}

	// socket initialization
	{
		if (enet_initialize() != 0) {
			fprintf(stderr, "An error occurred while initializing ENet.\n");
			exit(-1);
		}
		client = enet_host_create(NULL /* create a client host */,
			1 /* only allow 1 outgoing connection */,
			2 /* allow up 2 channels to be used, 0 and 1 */,
			0 /* assume any amount of incoming bandwidth */,
			0 /* assume any amount of outgoing bandwidth */);
		if (client == NULL) {
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
		if (peer == NULL) {
			fprintf(stderr,
				"No available peers for initiating an ENet connection.\n");
			exit(-1);
		}
		// the timeout is the third parameter here
		if (enet_host_service(client, &event, 5000) > 0 && event.type == ENET_EVENT_TYPE_CONNECT) {
			Log("Connected\n");
		}
		else {
			/* Either the 5 seconds are up or a disconnect event was */
			/* received. Reset the peer in the event the 5 seconds   */
			/* had run out without any significant event.            */
			enet_peer_reset(peer);
			fprintf(stderr, "Connection to server failed.");
			exit(-1);
		}
	}
}

#define DeferLoop(start, end) \
    for (int _i_ = ((start), 0); _i_ == 0; _i_ += 1, (end))
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
	draw_texture_rectangle_centered(center, (V2) { size, size });
}

static void
draw_circle(V2 point, float radius)
{
#define POINTS 64
	sgp_line lines[POINTS];
	for (int i = 0; i < POINTS; i++) {
		float progress = (float)i / (float)POINTS;
		float next_progress = (float)(i + 1) / (float)POINTS;
		lines[i].a = (V2){ .x = cosf(progress * 2.0f * PI) * radius,
			.y = sinf(progress * 2.0f * PI) * radius };
		lines[i].b = (V2){ .x = cosf(next_progress * 2.0f * PI) * radius,
			.y = sinf(next_progress * 2.0f * PI) * radius };
		lines[i].a = V2add(lines[i].a, point);
		lines[i].b = V2add(lines[i].b, point);
	}
	sgp_draw_lines(lines, POINTS);
}

static Entity*
myentity()
{
	if (myplayer == -1)
		return NULL;
	Entity* to_return = get_entity(&gs, gs.players[myplayer].entity);
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
	if (cur_opacity <= 0.01f) {
		return;
	}

	if (draw)
		sgp_push_transform();

	// if(draw) sgp_scale(1.0f, -1.0f);
	//  draw spice bar
	if (draw) {
		static float damage = 0.5f;

		if (myentity() != NULL) {
			damage = myentity()->damage;
		}

		sgp_set_color(0.5f, 0.5f, 0.5f, cur_opacity);
		float margin = width * 0.1f;
		float bar_width = width - margin * 2.0f;
		sgp_draw_filled_rect(margin, 80.0f, bar_width, 30.0f);
		sgp_set_color(1.0f, 1.0f, 1.0f, cur_opacity);
		sgp_draw_filled_rect(
			margin, 80.0f, bar_width * (1.0f - damage), 30.0f);
	}

	// draw item toolbar
	{
		float itemframe_width = (float)sg_query_image_info(image_itemframe).width * 2.0f;
		float itemframe_height = (float)sg_query_image_info(image_itemframe).height * 2.0f;
		float total_width = itemframe_width * boxes_len;
		float item_width = itemframe_width * 0.75f;
		float item_height = itemframe_height * 0.75f;
		float item_offset_x = (itemframe_width - item_width) / 2.0f;
		float item_offset_y = (itemframe_height - item_height) / 2.0f;

		float x = width / 2.0f - total_width / 2.0f;
		float y = height - itemframe_height * 1.5f;
		for (int i = 0; i < boxes_len; i++) {
			if (has_point(
				(AABB) {
				.x = x,
					.y = y,
					.width = itemframe_width,
					.height = itemframe_height,
			},
				mouse_pos)
				&& mouse_pressed) {
				// "handle" mouse pressed
				attempt_to_build(i);
				mouse_pressed = false;
			}

			if (draw) {
				sgp_set_color(1.0f, 1.0f, 1.0f, cur_opacity);
				if (cur_editing_boxtype == i) {
					sgp_set_image(0, image_itemframe_selected);
				}
				else {
					sgp_set_image(0, image_itemframe);
				}
				sgp_draw_textured_rect(x, y, itemframe_width, itemframe_height);
				struct BoxInfo info = boxinfo((enum BoxType)i);
				if (can_build(i))
				{
					sgp_set_image(0, info.image);
				}
				else {
					sgp_set_image(0, image_mystery);
				}
				transform_scope
				{
					float item_x = x + item_offset_x;
					float item_y = y + item_offset_y;
					sgp_scale_at(1.0f, -1.0f, item_x + item_width / 2.0f, item_y + item_height / 2.0f);
					//sgp_scale(1.0f, -1.0f);
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
	for (int x = -num; x < num; x++) {
		for (int y = -num; y < num; y++) {
			V2 star = (V2){ (float)x * gap, (float)y * gap };
			if(V2lengthsqr(V2sub(star, camera_pos)) > VISION_RADIUS*VISION_RADIUS)
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
		for (int i = 0; i < SAPP_KEYCODE_MENU; i++) {
			if (keypressed[i].frame < sapp_frame_count()) {
				keypressed[i].pressed = false;
			}
		}
		if (mouse_pressed_frame < sapp_frame_count()) {
			mouse_pressed = false;
		}
	}

	// networking
	{
		ENetEvent event;
		while (true) {
			int enet_status = enet_host_service(client, &event, 0);
			if (enet_status > 0) {
				switch (event.type) {
				case ENET_EVENT_TYPE_CONNECT: {
					Log("New client from host %x\n", event.peer->address.host);
					break;
				}

				case ENET_EVENT_TYPE_RECEIVE: {
					// @Robust @BeforeShip use some kind of serialization strategy that
					// checks for out of bounds and other validation instead of just
					// casting to a struct "Alignment of structure members can be
					// different even among different compilers on the same platform,
					// let alone different platforms."
					// ^^ need serialization strategy that accounts for this if multiple
					// platforms is happening
					// https://stackoverflow.com/questions/28455163/how-can-i-portably-send-a-c-struct-through-a-network-socket
					ServerToClient msg = (ServerToClient){
						.cur_gs = &gs,
					};
					// @Robust @BeforeShip maximum acceptable message size?
					char* decompressed = malloc(sizeof * decompressed * MAX_BYTES_SIZE);
					size_t decompressed_max_len = MAX_BYTES_SIZE;
					assert(LZO1X_MEM_DECOMPRESS == 0);
					int return_value = lzo1x_decompress_safe(event.packet->data, event.packet->dataLength, decompressed, &decompressed_max_len, NULL);
					// @Robust not sure what return_value is, error test on it somehow
					if (return_value == LZO_E_OK)
					{
						from_bytes(&msg, decompressed, decompressed_max_len, false, false);
						myplayer = msg.your_player;
					}
					else {
						Log("Couldn't decompress gamestate packet, error code %d from lzo\n", return_value);
					}
					enet_packet_destroy(event.packet);
					free(decompressed);
					break;
				}

				case ENET_EVENT_TYPE_DISCONNECT: {
					fprintf(stderr, "Disconnected from server\n");
					exit(-1);
					break;
				}
				}
			}
			else if (enet_status == 0) {
				break;
			}
			else if (enet_status < 0) {
				fprintf(stderr, "Error receiving enet events: %d\n", enet_status);
				break;
			}
		}
	}

	// gameplay
	ui(false, dt, width, height); // handle events
	V2 build_target_pos = { 0 };
	float build_target_rotation = 0.0f;
	static V2 camera_pos = {
		0
	}; // keeps camera at same position after player death
	V2 world_mouse_pos = mouse_pos; // processed later in scope
	struct BuildPreviewInfo {
		V2 grid_pos;
		float grid_rotation;
	} build_preview = { 0 };
	V2 hand_pos = { 0 }; // in local space of grid when hovering over a grid
	bool hand_at_arms_length = false;
	{
		// interpolate zoom
		zoom = lerp(zoom, zoom_target, dt * 12.0f);

		// calculate world position and camera
		{
			if (myentity() != NULL) {
				camera_pos = entity_pos(myentity());
			}
			world_mouse_pos = V2sub(world_mouse_pos, (V2) { .x = width / 2.0f, .y = height / 2.0f });
			world_mouse_pos.x /= zoom;
			world_mouse_pos.y /= -zoom;
			world_mouse_pos = V2add(world_mouse_pos, (V2) { .x = camera_pos.x, .y = camera_pos.y });
		}

		// calculate build preview stuff
		EntityID grid_to_build_on = (EntityID){ 0 };
		V2 possibly_local_hand_pos = (V2){ 0 };
		if (myentity() != NULL) {
			hand_pos = V2sub(world_mouse_pos, entity_pos(myentity()));
			float hand_len = V2length(hand_pos);
			if (hand_len > MAX_HAND_REACH) {
				hand_at_arms_length = true;
				hand_len = MAX_HAND_REACH;
			}
			else {
				hand_at_arms_length = false;
			}
			hand_pos = V2scale(V2normalize(hand_pos), hand_len);
			hand_pos = V2add(hand_pos, entity_pos(myentity()));

			possibly_local_hand_pos = V2sub(hand_pos, entity_pos(myentity()));
			Entity* placing_grid = closest_to_point_in_radius(&gs, hand_pos, BUILD_BOX_SNAP_DIST_TO_SHIP);
			if (placing_grid == NULL) {
				build_preview = (struct BuildPreviewInfo){
					.grid_pos = hand_pos,
					.grid_rotation = 0.0f,
				};
			}
			else {
				grid_to_build_on = get_id(&gs, placing_grid);
				hand_pos = grid_snapped_box_pos(placing_grid, hand_pos);
				possibly_local_hand_pos = grid_world_to_local(placing_grid, hand_pos);
				build_preview = (struct BuildPreviewInfo){ .grid_pos = entity_pos(placing_grid),
					.grid_rotation = entity_rotation(placing_grid),
				};
			}
		}

		// Create and send input packet
		{
			// @Robust accumulate total time and send input at rate like 20 hz, not
			// every frame

			static size_t last_frame_id = 0;
			InputFrame cur_input_frame = { 0 };
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

			if (mouse_pressed && cur_editing_boxtype != -1) {
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

			if (input_differs) {
				InputFrame last_frame = client_to_server.inputs[0];
				for (int i = 0; i < INPUT_BUFFER - 1; i++) {
					InputFrame last_last_frame = last_frame;
					last_frame = client_to_server.inputs[i + 1];
					client_to_server.inputs[i + 1] = last_last_frame;
				}
				cur_input_frame.tick = tick(&gs);
				client_to_server.inputs[0] = cur_input_frame;
				last_frame_id += 1;
			}

			static double last_input_sent_time = 0.0;
			if (fabs(last_input_sent_time - time) > TIME_BETWEEN_INPUT_PACKETS) {
				ENetPacket* packet = enet_packet_create((void*)&client_to_server,
					sizeof(client_to_server),
					ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT);
				enet_peer_send(peer, 0, packet); // @Robust error check this
				last_input_sent_time = time;
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
				//sgp_draw_textured_rect(0, 0, stars_width, stars_height);
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
				//sgp_draw_textured_rect(0, 0, stars_width, stars_height);
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
				if (myentity() != NULL) {
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
				if (mouse_frozen) {
					sgp_set_color(1.0f, 0.0f, 0.0f, 0.5f);
					sgp_draw_filled_rect(world_mouse_pos.x, world_mouse_pos.y, 0.1f, 0.1f);
				}

				// building preview
				if (cur_editing_boxtype != -1) {
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
				for (size_t i = 0; i < gs.cur_next_entity; i++) {
					Entity* e = &gs.entities[i];
					if (!e->exists)
						continue;
					// draw grid
					if (e->is_grid)
					{
						Entity* g = e;
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
								if (b->box_type == BoxBattery) {
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

									if (b->box_type == BoxThruster) {
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
					if (e->is_player && get_entity(&gs, e->currently_inside_of_box) == NULL) {
						transform_scope
						{
							sgp_rotate_at(entity_rotation(e), entity_pos(e).x, entity_pos(e).y);
							sgp_set_color(1.0f, 1.0f, 1.0f, 1.0f);
							sgp_set_image(0, image_player);
							draw_texture_rectangle_centered(entity_pos(e), V2scale(PLAYER_SIZE, player_scaling));
							sgp_reset_image(0);
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
					draw_texture_centered((V2) { 0 }, SUN_RADIUS * 2.0f);
					sgp_reset_image(0);

					// sun DEATH RADIUS
					set_color(RED);
					draw_circle((V2) { 0 }, INSTANT_DEATH_DISTANCE_FROM_SUN);
				}

				sgp_set_color(1.0f, 1.0f, 1.0f, 1.0f);
				dbg_drawall();
		} // world space transform end

		// low health
		if (myentity() != NULL)
		{
			sgp_set_color(1.0f, 1.0f, 1.0f, myentity()->damage);
			sgp_set_image(0, image_low_health);
			draw_texture_rectangle_centered((V2) { width / 2.0f, height / 2.0f }, (V2) { width, height });
			sgp_reset_image(0);
		}

		// UI drawn in screen space
		ui(true, dt, width, height);
	}

	sg_pass_action pass_action = { 0 };
	sg_begin_default_pass(&pass_action, (int)width, (int)height);
	sgp_flush();
	sgp_end();
	sg_end_pass();
	sg_commit();
}

void cleanup(void)
{
	destroy(&gs);
	free(gs.entities);
	sgp_shutdown();
	sg_shutdown();
	enet_deinitialize();
}

void event(const sapp_event* e)
{
	switch (e->type) {
	case SAPP_EVENTTYPE_KEY_DOWN:
		if (e->key_code == SAPP_KEYCODE_T) {
			mouse_frozen = !mouse_frozen;
		}
		if (e->key_code == SAPP_KEYCODE_R) {
			cur_editing_rotation += 1;
			cur_editing_rotation %= RotationLast;
		}
		if (e->key_code == SAPP_KEYCODE_F11)
		{
			sapp_toggle_fullscreen();
		}
		int key_num = e->key_code - SAPP_KEYCODE_0;
		int target_box = key_num - 1;
		if (target_box < BoxLast) {
			attempt_to_build(target_box);
		}

		if (!mouse_frozen) {
			keydown[e->key_code] = true;
			if (keypressed[e->key_code].frame == 0) {
				keypressed[e->key_code].pressed = true;
				keypressed[e->key_code].frame = e->frame_count;
			}
		}
		break;
	case SAPP_EVENTTYPE_KEY_UP:
		if (!mouse_frozen) {
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
		if (e->mouse_button == SAPP_MOUSEBUTTON_LEFT) {
			mouse_pressed = true;
			mouse_pressed_frame = e->frame_count;
		}
		if (e->mouse_button == SAPP_MOUSEBUTTON_RIGHT) {
			right_mouse_down = true;
		}
		break;
	case SAPP_EVENTTYPE_MOUSE_UP:
		if (e->mouse_button == SAPP_MOUSEBUTTON_LEFT) {
			mouse_pressed = false;
			mouse_pressed_frame = 0;
		}
		if (e->mouse_button == SAPP_MOUSEBUTTON_RIGHT) {
			right_mouse_down = false;
		}
		break;
	case SAPP_EVENTTYPE_MOUSE_MOVE:
		if (!mouse_frozen) {
			mouse_pos = (V2){ .x = e->mouse_x, .y = e->mouse_y };
		}
		if (right_mouse_down) {
			funval += e->mouse_dx;
			Log("Funval %f\n", funval);
		}
		break;
	}
}

sapp_desc
sokol_main(int argc, char* argv[])
{
	bool hosting = false;
	if (argc > 1) {
		_beginthread(server, 0, "debug_world.bin");
		hosting = true;
	}
	(void)argv;
	return (sapp_desc) {
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