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
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "types.h"

#include <string.h> // errno error message on file open
#include <inttypes.h>

static struct GameState gs = { 0 };
static int myplayer = -1;
static bool right_mouse_down = false;
static bool keydown[SAPP_KEYCODE_MENU] = { 0 };
typedef struct KeyPressed
{
	bool pressed;
	uint64_t frame;
} KeyPressed;
static KeyPressed keypressed[SAPP_KEYCODE_MENU] = { 0 };
static V2 mouse_pos = { 0 };
static bool mouse_pressed = false;
static uint64_t mouse_pressed_frame = 0;
static bool mouse_frozen = false;                    // @BeforeShip make this debug only thing
static float funval = 0.0f;                          // easy to play with value controlled by left mouse button when held down @BeforeShip remove on release builds
static struct ClientToServer client_to_server = { 0 }; // buffer of inputs
static ENetHost* client;
static ENetPeer* peer;
static float zoom_target = 300.0f;
static float zoom = 300.0f;
static sg_image image_itemframe;
static sg_image image_itemframe_selected;
static sg_image image_thrusterburn;
static sg_image image_player;
static int cur_editing_boxtype = -1;
static int cur_editing_rotation = 0;

static struct BoxInfo
{
	enum BoxType type;
	const char* image_path;
	sg_image image;
} boxes[] = { // if added to here will show up in toolbar, is placeable
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
	} };
const int boxes_len = sizeof(boxes) / sizeof(*boxes);

struct BoxInfo boxinfo(enum BoxType type)
{
	for (int i = 0; i < boxes_len; i++)
	{
		if (boxes[i].type == type)
			return boxes[i];
	}
	Log("No box info found for type %d\n", type);
	return (struct BoxInfo) { 0 };
}

static sg_image load_image(const char* path)
{
	sg_image to_return = sg_alloc_image();

	int x = 0;
	int y = 0;
	int comp = 0;
	const int desired_channels = 4;
	stbi_set_flip_vertically_on_load(true);
	stbi_uc* image_data = stbi_load(path, &x, &y, &comp, desired_channels);
	if (!image_data)
	{
		fprintf(stderr, "Failed to load image: %s\n", stbi_failure_reason());
		exit(-1);
	}
	sg_init_image(to_return, &(sg_image_desc){
		.width = x,
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
static void init(void)
{
	// @BeforeShip make all fprintf into logging to file, warning dialog grids on failure instead of exit(-1), replace the macros in sokol with this as well, like assert

	Entity* entity_data = malloc(sizeof * entity_data * MAX_ENTITIES);
	initialize(&gs, entity_data, sizeof * entity_data * MAX_ENTITIES);

	sg_desc sgdesc = { .context = sapp_sgcontext() };
	sg_setup(&sgdesc);
	if (!sg_isvalid())
	{
		fprintf(stderr, "Failed to create Sokol GFX context!\n");
		exit(-1);
	}

	sgp_desc sgpdesc = { 0 };
	sgp_setup(&sgpdesc);
	if (!sgp_is_valid())
	{
		fprintf(stderr, "Failed to create Sokol GP context: %s\n", sgp_get_error_message(sgp_get_last_error()));
		exit(-1);
	}

	// image loading
	{
		for (int i = 0; i < boxes_len; i++)
		{
			boxes[i].image = load_image(boxes[i].image_path);
		}
		image_thrusterburn = load_image("loaded/thrusterburn.png");
		image_itemframe = load_image("loaded/itemframe.png");
		image_itemframe_selected = load_image("loaded/itemframe_selected.png");
		image_player = load_image("loaded/player.png");
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

#define DeferLoop(start, end) for (int _i_ = ((start), 0); _i_ == 0; _i_ += 1, (end))
#define transform_scope DeferLoop(sgp_push_transform(), sgp_pop_transform())

static void draw_color_rect_centered(V2 center, float size)
{
	float halfbox = size / 2.0f;

	sgp_draw_filled_rect(center.x - halfbox, center.y - halfbox, size, size);
}

static void draw_texture_rectangle_centered(V2 center, V2 width_height)
{
	V2 halfsize = V2scale(width_height, 0.5f);
	sgp_draw_textured_rect(center.x - halfsize.x, center.y - halfsize.y, width_height.x, width_height.y);
}

static void draw_texture_centered(V2 center, float size)
{
	draw_texture_rectangle_centered(center, (V2) { size, size });
}

static void draw_circle(V2 point, float radius)
{
#define POINTS 64
	sgp_line lines[POINTS];
	for (int i = 0; i < POINTS; i++)
	{
		float progress = (float)i / (float)POINTS;
		float next_progress = (float)(i + 1) / (float)POINTS;
		lines[i].a = (V2){ .x = cos(progress * 2.0f * PI) * radius, .y = sin(progress * 2.0f * PI) * radius };
		lines[i].b = (V2){ .x = cos(next_progress * 2.0f * PI) * radius, .y = sin(next_progress * 2.0f * PI) * radius };
		lines[i].a = V2add(lines[i].a, point);
		lines[i].b = V2add(lines[i].b, point);
	}
	sgp_draw_lines(lines, POINTS);
}

static Entity* myentity()
{
	if (myplayer == -1)
		return NULL;
	Entity* to_return = get_entity(&gs, gs.players[myplayer].entity);
	if (to_return != NULL)
		assert(to_return->is_player);
	return to_return;
}

static void ui(bool draw, float dt, float width, float height)
{
	static float cur_opacity = 1.0f;
	cur_opacity = lerp(cur_opacity, myentity() != NULL ? 1.0f : 0.0f, dt * 5.0f);
	if (cur_opacity <= 0.01f)
	{
		return;
	}

	// draw spice bar
	if (draw)
	{
		static float spice_taken_away = 0.5f;

		if (myentity() != NULL)
		{
			spice_taken_away = myentity()->spice_taken_away;
		}

		sgp_set_color(0.5f, 0.5f, 0.5f, cur_opacity);
		float margin = width * 0.1;
		float bar_width = width - margin * 2.0f;
		sgp_draw_filled_rect(margin, 80.0f, bar_width, 30.0f);
		sgp_set_color(1.0f, 1.0f, 1.0f, cur_opacity);
		sgp_draw_filled_rect(margin, 80.0f, bar_width * (1.0f - spice_taken_away), 30.0f);
	}

	// draw item toolbar
	{
		int itemframe_width = sg_query_image_info(image_itemframe).width * 2.0f;
		int itemframe_height = sg_query_image_info(image_itemframe).height * 2.0f;
		int total_width = itemframe_width * boxes_len;
		float item_width = itemframe_width * 0.75;
		float item_height = itemframe_height * 0.75;
		float item_offset_x = (itemframe_width - item_width) / 2.0f;
		float item_offset_y = (itemframe_height - item_height) / 2.0f;

		float x = width / 2.0 - total_width / 2.0;
		float y = height - itemframe_height * 1.5;
		for (int i = 0; i < boxes_len; i++)
		{
			if (has_point((AABB) {
				.x = x,
					.y = y,
					.width = itemframe_width,
					.height = itemframe_height,
			},
				mouse_pos) &&
				mouse_pressed)
			{
				// "handle" mouse pressed
				mouse_pressed = false;
				cur_editing_boxtype = i;
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
					sgp_set_image(0, boxinfo((enum BoxType)i).image);
					sgp_draw_textured_rect(x + item_offset_x, y + item_offset_y, item_width, item_height);
					sgp_reset_image(0);
				}

				x += itemframe_width;
		}
	}
}

static void frame(void)
{
	int width = sapp_width(), height = sapp_height();
	float ratio = width / (float)height;
	double time = sapp_frame_count() * sapp_frame_duration();
	float dt = sapp_frame_duration();

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
					// @Robust @BeforeShip use some kind of serialization strategy that checks for out of bounds
					// and other validation instead of just casting to a struct
					// "Alignment of structure members can be different even among different compilers on the same platform, let alone different platforms."
					// ^^ need serialization strategy that accounts for this if multiple platforms is happening https://stackoverflow.com/questions/28455163/how-can-i-portably-send-a-c-struct-through-a-network-socket
					ServerToClient msg = (ServerToClient){
						.cur_gs = &gs,
					};
					// @Robust @BeforeShip maximum acceptable message size?
					from_bytes(&msg, event.packet->data, event.packet->dataLength);
					myplayer = msg.your_player;
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
	V2 build_target_pos = { 0 };
	float build_target_rotation = 0.0f;
	static V2 camera_pos = { 0 }; // keeps camera at same position after player death
	V2 world_mouse_pos = mouse_pos;
	struct BuildPreviewInfo
	{
		V2 grid_pos;
		float grid_rotation;
		V2 pos;
	} build_preview = { 0 };
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
			world_mouse_pos = V2sub(world_mouse_pos, (V2) { .x = width / 2.0f, .y = height / 2.0f });
			world_mouse_pos.x /= zoom;
			world_mouse_pos.y /= -zoom;
			world_mouse_pos = V2add(world_mouse_pos, (V2) { .x = camera_pos.x, .y = camera_pos.y });
		}

		// calculate build preview stuff
		EntityID grid_to_build_on = (EntityID){ 0 };
		if (myentity() != NULL)
		{
			V2 hand_pos = V2sub(world_mouse_pos, entity_pos(myentity()));
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

			Entity* placing_grid = closest_to_point_in_radius(&gs, hand_pos, BUILD_BOX_SNAP_DIST_TO_SHIP);
			if (placing_grid == NULL)
			{
				build_preview = (struct BuildPreviewInfo){
					.grid_pos = hand_pos,
					.grid_rotation = 0.0f,
					.pos = hand_pos,
				};
			}
			else
			{
				grid_to_build_on = get_id(&gs, placing_grid);
				V2 pos = grid_snapped_box_pos(placing_grid, hand_pos);
				build_preview = (struct BuildPreviewInfo){
					.grid_pos = entity_pos(placing_grid),
					.grid_rotation = entity_rotation(placing_grid),
					.pos = pos };
			}
		}

		// Create and send input packet
		{
			// @Robust accumulate total time and send input at rate like 20 hz, not every frame

			struct InputFrame cur_input_frame = { 0 };
			V2 input = (V2){
				.x = (float)keydown[SAPP_KEYCODE_D] - (float)keydown[SAPP_KEYCODE_A],
				.y = (float)keydown[SAPP_KEYCODE_W] - (float)keydown[SAPP_KEYCODE_S],
			};
			cur_input_frame.movement = input;
			cur_input_frame.inhabit = keypressed[SAPP_KEYCODE_G].pressed;
			if (mouse_pressed && cur_editing_boxtype != -1)
			{
				cur_input_frame.dobuild = mouse_pressed;
				cur_input_frame.build_type = cur_editing_boxtype;
				cur_input_frame.build_rotation = cur_editing_rotation;
				cur_input_frame.grid_to_build_on = grid_to_build_on;
				Entity* grid = get_entity(&gs, grid_to_build_on);
				if (grid != NULL)
				{
					cur_input_frame.build = grid_world_to_local(grid, build_preview.pos);
				}
				else
				{
					cur_input_frame.build = build_preview.pos;
				}
			}

			struct InputFrame latest = client_to_server.inputs[0];
			// if they're not the same
			if (
				!V2cmp(cur_input_frame.movement, latest.movement, 0.01f) ||
				cur_input_frame.inhabit != latest.inhabit ||
				cur_input_frame.dobuild != latest.dobuild ||
				cur_input_frame.grid_to_build_on.generation != latest.grid_to_build_on.generation ||
				cur_input_frame.grid_to_build_on.index != latest.grid_to_build_on.index ||
				!V2cmp(cur_input_frame.build, latest.build, 0.01f))
			{
				for (int i = 0; i < INPUT_BUFFER - 1; i++)
				{
					client_to_server.inputs[i + 1] = client_to_server.inputs[i];
				}
				cur_input_frame.tick = tick(&gs);
				client_to_server.inputs[0] = cur_input_frame;
			}

			static double last_input_sent_time = 0.0;
			if (fabs(last_input_sent_time - time) > TIME_BETWEEN_INPUT_PACKETS)
			{
				ENetPacket* packet = enet_packet_create((void*)&client_to_server, sizeof(client_to_server), ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT);
				enet_peer_send(peer, 0, packet);
				last_input_sent_time = time;
			}
		}

		// @BeforeShip client side prediction and rollback to previous server authoritative state, then replay inputs
		// no need to store copies of game state, just player input frame to frame. Then know how many frames ago the server game state arrived, it's that easy!
		// process(&gs, (float)sapp_frame_duration());
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

		// sokol drawing library draw in world space
		// world space coordinates are +Y up, -Y down. Like normal cartesian coords
		transform_scope{
			sgp_translate(width / 2, height / 2);
			sgp_scale_at(zoom, -zoom, 0.0f, 0.0f);

			// camera go to player
			sgp_translate(-camera_pos.x, -camera_pos.y);

			// hand reached limit circle
			if (myentity() != NULL)
			{
				static float hand_reach_alpha = 1.0f;
				hand_reach_alpha = lerp(hand_reach_alpha, hand_at_arms_length ? 1.0f : 0.0f, dt * 5.0);
				sgp_set_color(1.0f, 1.0f, 1.0f, hand_reach_alpha);
				draw_circle(entity_pos(myentity()), MAX_HAND_REACH);
			}

			// stars
			sgp_set_color(1.0f, 1.0f, 1.0f, 1.0f);
			const int num = 30;
			for (int x = -num; x < num; x++)
			{
				for (int y = -num; y < num; y++)
				{
					sgp_draw_point((float)x * 0.1f, (float)y * 0.1f);
				}
			}

			float halfbox = BOX_SIZE / 2.0f;

			// mouse
			if (mouse_frozen)
			{
				sgp_set_color(1.0f, 0.0f, 0.0f, 0.5f);
				sgp_draw_filled_rect(world_mouse_pos.x, world_mouse_pos.y, 0.1f, 0.1f);
			}

			// building preview
			if (cur_editing_boxtype != -1)
			{
				sgp_set_color(0.5f, 0.5f, 0.5f, (sin(time * 9.0f) + 1.0) / 3.0f + 0.2);

				transform_scope
				{

					sgp_set_image(0, boxinfo(cur_editing_boxtype).image);
					sgp_rotate_at(build_preview.grid_rotation + rotangle(cur_editing_rotation), build_preview.pos.x, build_preview.pos.y);
					draw_texture_centered(build_preview.pos, BOX_SIZE);
					// drawbox(build_preview.pos, build_preview.grid_rotation, 0.0f, cur_editing_boxtype, cur_editing_rotation);
					sgp_reset_image(0);
				}
			}

			for (int i = 0; i < gs.cur_next_entity; i++)
			{
				Entity* e = &gs.entities[i];
				if (!e->exists)
					continue;
				if (e->is_grid) // grid drawing
				{
					Entity* g = e;
					BOXES_ITER(&gs, b, g)
					{
						sgp_set_color(1.0f, 1.0f, 1.0f, 1.0f);
						// debug draw force vectors for thrusters
						#if 0
												{
													if (b->type == BoxThruster)
													{
														dbg_rect(box_pos(b));
														dbg_line(box_pos(b), V2add(box_pos(b), V2scale(thruster_force(b), -1.0f)));
													}
												}
						#endif
												if (b->box_type == BoxBattery)
												{
													float cur_alpha = sgp_get_color().a;
													Color from = WHITE;
													Color to = colhex(255, 0, 0);
													Color result = Collerp(from, to, b->energy_used);
													sgp_set_color(result.r, result.g, result.b, cur_alpha);
												}
												transform_scope {

													sgp_rotate_at(entity_rotation(g) + rotangle(b->compass_rotation), box_pos(b).x, box_pos(b).y);

													if (b->box_type == BoxThruster)
													{
														transform_scope
														{
															sgp_set_color(1.0f, 1.0f, 1.0f, 1.0f);
															sgp_set_image(0, image_thrusterburn);
															// float scaling = 1.0 + (hash11(time*3.0)/2.0)*lerp(0.0, 0.07, b->thrust);
															// printf("%f\n", b->thrust);
															float scaling = 0.95 + lerp(0.0, 0.3, b->thrust);
															// float scaling = 1.1;
															// sgp_translate(-(scaling*BOX_SIZE - BOX_SIZE), 0.0);
															// sgp_scale(scaling, 1.0);
															sgp_scale_at(scaling, 1.0, box_pos(b).x, box_pos(b).y);
															draw_texture_centered(box_pos(b), BOX_SIZE);
															sgp_reset_image(0);
														}
													}

													sgp_set_image(0, boxinfo(b->box_type).image);
													draw_texture_centered(box_pos(b), BOX_SIZE);
													sgp_reset_image(0);

													sgp_set_color(0.5f, 0.1f, 0.1f, b->damage);
													draw_color_rect_centered(box_pos(b), BOX_SIZE);
												}
											}
											sgp_set_color(1.0f, 0.0f, 0.0f, 1.0f);
											V2 vel = grid_vel(g);
											V2 to = V2add(grid_com(g), vel);
											sgp_draw_line(grid_com(g).x, grid_com(g).y, to.x, to.y);
										}
										if (e->is_player)
										{
											transform_scope
											{
												sgp_rotate_at(entity_rotation(e), entity_pos(e).x, entity_pos(e).y);
												sgp_set_color(1.0f, 0.5f, 0.5f, 1.0f);
												sgp_set_image(0, image_player);
												draw_texture_rectangle_centered(entity_pos(e), PLAYER_SIZE);
												sgp_reset_image(0);
											}
										}
									}
		}

			// player
		for (int i = 0; i < MAX_PLAYERS; i++)
		{
			struct Player* player = &gs.players[i];
			if (!player->connected)
				continue;
			Entity* p = get_entity(&gs, player->entity);
			if (p == NULL)
				continue;
			static float opacities[MAX_PLAYERS] = { 1.0f };
			static V2 positions[MAX_PLAYERS] = { 0 };
			opacities[i] = lerp(opacities[i], p != NULL ? 1.0f : 0.1f, dt * 7.0f);
			Color col_to_draw = Collerp(WHITE, GOLD, p->goldness);
			col_to_draw.a = opacities[i];

			if (p != NULL)
			{
				positions[i] = entity_pos(p);
			}

			set_color(col_to_draw);
			transform_scope
			{
				float psize = 0.1f;
				sgp_draw_filled_rect(positions[i].x - psize / 2.0f, positions[i].y - psize / 2.0f, psize, psize);
			}
		}

		// gold target
		set_color(GOLD);
		sgp_draw_filled_rect(gs.goldpos.x, gs.goldpos.y, 0.1f, 0.1f);

		sgp_set_color(1.0f, 1.0f, 1.0f, 1.0f);
		dbg_drawall();
	}

	// UI drawn in screen space
	ui(true, dt, width, height);

	sg_pass_action pass_action = { 0 };
	sg_begin_default_pass(&pass_action, width, height);
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
	switch (e->type)
	{
	case SAPP_EVENTTYPE_KEY_DOWN:
		if (e->key_code == SAPP_KEYCODE_T)
		{
			mouse_frozen = !mouse_frozen;
		}
		if (e->key_code == SAPP_KEYCODE_R)
		{
			cur_editing_rotation += 1;
			cur_editing_rotation %= RotationLast;
		}
		int key_num = e->key_code - SAPP_KEYCODE_0;
		int target_box = key_num - 1;
		if (target_box < BoxLast)
		{
			cur_editing_boxtype = target_box;
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
			mouse_pos = (V2){ .x = e->mouse_x, .y = e->mouse_y };
		}
		if (right_mouse_down)
		{
			funval += e->mouse_dx;
			Log("Funval %f\n", funval);
		}
		break;
	}
}

sapp_desc sokol_main(int argc, char* argv[])
{
	if (argc > 1)
	{
		_beginthread(server, 0, NULL);
	}
	(void)argv;
	return (sapp_desc) {
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