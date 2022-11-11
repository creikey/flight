#include <chipmunk.h> // initializing bodies
#include "types.h"
#include "sokol_time.h"
#include <enet/enet.h>
#include <stdio.h>
#include <inttypes.h> // int64 printing
#include <stdlib.h>
#include <string.h> // error string
#include <errno.h>

#include "minilzo.h"

#include "opus.h"

#ifdef __unix
#define fopen_s(pFile, filename, mode) ((*(pFile)) = fopen((filename), (mode))) == NULL
#endif

#define CONNECTED_PEERS(host, cur)                                                \
	for (ENetPeer *cur = host->peers; cur < host->peers + host->peerCount; cur++) \
		if (cur->state == ENET_PEER_STATE_CONNECTED)

// started in a thread from host
void server(void* world_save_name)
{

	stm_setup();

	struct GameState gs = { 0 };
	size_t entities_size = (sizeof(Entity) * MAX_ENTITIES);
	Entity* entity_data = malloc(entities_size);
	initialize(&gs, entity_data, entities_size);
	Log("Allocated %zu bytes for entities\n", entities_size);

	OpusBuffer* player_voip_buffers[MAX_PLAYERS] = { 0 };
	for (int i = 0; i < MAX_PLAYERS; i++) player_voip_buffers[i] = calloc(1, sizeof * player_voip_buffers[i]);
	OpusEncoder* player_encoders[MAX_PLAYERS] = { 0 };
	OpusDecoder* player_decoders[MAX_PLAYERS] = { 0 };

	// for (int i = 0; i < MAX_PLAYERS; i++)
	//{
	//	int error = 0;
	//	player_encoders[i] = opus_encoder_create(VOIP_SAMPLE_RATE, 1, OPUS_APPLICATION_VOIP, &error);
	//	if (error != OPUS_OK) Log("Failed to create encoder\n");
	//	player_decoders[i] = opus_decoder_create(VOIP_SAMPLE_RATE, 1, &error);
	//	if (error != OPUS_OK) Log("Failed to create decoder\n");
	// }

	if (world_save_name != NULL)
	{
		size_t read_game_data_buffer_size = entities_size;
		char* read_game_data = malloc(read_game_data_buffer_size);

		FILE* file = NULL;
		fopen_s(&file, (const char*)world_save_name, "rb");
		if (file == NULL)
		{
			Log("Could not read from data file %s: errno %d\n", (const char*)world_save_name, errno);
		}
		else
		{
			size_t actual_length = fread(read_game_data, sizeof(char), entities_size, file);
			if (actual_length <= 1)
			{
				Log("Could only read %zu bytes, error: errno %d\n", actual_length, errno);
				exit(-1);
			}
			Log("Read %zu bytes from save file\n", actual_length);
			ServerToClient msg = (ServerToClient){
				.cur_gs = &gs,
			};
			server_to_client_deserialize(&msg, read_game_data, actual_length, true);
			fclose(file);
		}

		free(read_game_data);
	}

#define BOX_AT_TYPE(grid, pos, type)     \
	{                                    \
		Entity *box = new_entity(&gs);   \
		box_create(&gs, box, grid, pos); \
		box->box_type = type;            \
	}
#define BOX_AT(grid, pos) BOX_AT_TYPE(grid, pos, BoxHullpiece)

	// one box policy
	if (false)
	{
		Entity* grid = new_entity(&gs);
		grid_create(&gs, grid);
		entity_set_pos(grid, (V2) { -BOX_SIZE * 2, 0.0f });
		Entity* box = new_entity(&gs);
		box_create(&gs, box, grid, (V2) { 0 });
	}

	// rotation test
	if (false)
	{
		Entity* grid = new_entity(&gs);
		grid_create(&gs, grid);
		entity_set_pos(grid, (V2) { -BOX_SIZE * 2, 0.0f });
		entity_set_rotation(grid, PI / 1.7f);
		cpBodySetVelocity(grid->body, cpv(-0.1, 0.0));
		cpBodySetAngularVelocity(grid->body, 1.0f);

		BOX_AT(grid, ((V2){0}));
		BOX_AT(grid, ((V2){BOX_SIZE, 0}));
		BOX_AT(grid, ((V2){2.0 * BOX_SIZE, 0}));
		BOX_AT(grid, ((V2){2.0 * BOX_SIZE, BOX_SIZE}));
		BOX_AT(grid, ((V2){0.0 * BOX_SIZE, -BOX_SIZE}));
	}

	if (enet_initialize() != 0)
	{
		fprintf(stderr, "An error occurred while initializing ENet.\n");
		exit(-1);
	}

	ENetAddress address;
	ENetHost* enet_host;
	int sethost = enet_address_set_host_ip(&address, LOCAL_SERVER_ADDRESS);
	if (sethost != 0)
	{
		Log("Fishy return value from set host: %d\n", sethost);
	}
	/* Bind the server to port 1234. */
	address.port = SERVER_PORT;
	enet_host = enet_host_create(&address /* the address to bind the server host to */,
		MAX_PLAYERS /* allow up to MAX_PLAYERS clients and/or outgoing connections */,
		2 /* allow up to 2 channels to be used, 0 and 1 */,
		0 /* assume any amount of incoming bandwidth */,
		0 /* assume any amount of outgoing bandwidth */);
	if (enet_host == NULL)
	{
		fprintf(stderr,
			"An error occurred while trying to create an ENet server host.\n");
		exit(-1);
	}

	Log("Serving on port %d...\n", SERVER_PORT);
	ENetEvent event;
	uint64_t last_processed_time = stm_now();
	uint64_t last_saved_world_time = stm_now();
	float total_time = 0.0f;
	size_t player_to_latest_id_processed[MAX_PLAYERS] = { 0 };
	char* world_save_buffer = malloc(entities_size);
	while (true)
	{
		// @Speed handle enet messages and simulate gamestate in parallel, then sync... must clone gamestate for this
		while (true)
		{
			int ret = enet_host_service(enet_host, &event, 0);
			if (ret == 0)
				break;
			if (ret < 0)
			{
				fprintf(stderr, "Enet host service error %d\n", ret);
			}
			if (ret > 0)
			{
				switch (event.type)
				{
				case ENET_EVENT_TYPE_CONNECT:
				{
					Log("A new client connected from %x:%u.\n",
						event.peer->address.host,
						event.peer->address.port);

					int64_t player_slot = -1;
					for (int i = 0; i < MAX_PLAYERS; i++)
					{
						if (!gs.players[i].connected)
						{
							player_slot = i;
							break;
						}
					}

					if (player_slot == -1)
					{
						enet_peer_disconnect_now(event.peer, 69);
					}
					else
					{
						event.peer->data = (void*)player_slot;
						gs.players[player_slot] = (struct Player){ 0 };
						gs.players[player_slot].connected = true;
						player_to_latest_id_processed[player_slot] = 0;

						int error;
						player_encoders[player_slot] = opus_encoder_create(VOIP_SAMPLE_RATE, 1, OPUS_APPLICATION_VOIP, &error);
						if (error != OPUS_OK)
							Log("Failed to create encoder: %d\n", error);
						player_decoders[player_slot] = opus_decoder_create(VOIP_SAMPLE_RATE, 1, &error);
						if (error != OPUS_OK)
							Log("Failed to create decoder: %d\n", error);

#ifdef UNLOCK_ALL
						gs.players[player_slot].unlocked_bombs = true;
#endif
					}
				}
				break;

				case ENET_EVENT_TYPE_RECEIVE:
				{
					// Log("A packet of length %zu was received on channel %u.\n",
					//        event.packet->dataLength,
					// event.channelID);
					if (event.packet->dataLength == 0)
					{
						Log("Wtf an empty packet from enet?\n");
					}
					else {
						int64_t player_slot = (int64_t)event.peer->data;
						size_t length = event.packet->dataLength;
						struct ClientToServer received = { .mic_data = player_voip_buffers[player_slot] };
						if (!client_to_server_deserialize(&gs, &received, event.packet->data, event.packet->dataLength))
						{
							Log("Bad packet from client %d\n", (int)player_slot);
						}
						else
						{
							size_t latest_id = player_to_latest_id_processed[player_slot];

							if (received.inputs[0].id > latest_id)
							{
								for (int i = INPUT_BUFFER - 1; i >= 0; i--)
								{
									if (received.inputs[i].tick == 0) // empty input
										continue;
									if (received.inputs[i].id <= latest_id)
										continue; // don't reprocess inputs already processed
									InputFrame cur_input = received.inputs[i];
									gs.players[player_slot].input.movement = cur_input.movement;
									gs.players[player_slot].input.hand_pos = cur_input.hand_pos;

									// for these "event" inputs, only modify the current input if the event is true.
									// while processing the gamestate, will mark it as false once processed. This
									// prevents setting the event input to false before it's been processed.
									if (cur_input.seat_action)
									{
										gs.players[player_slot].input.seat_action = cur_input.seat_action;
										gs.players[player_slot].input.grid_hand_pos_local_to = cur_input.grid_hand_pos_local_to;
									}
									if (cur_input.dobuild)
									{
										gs.players[player_slot].input.grid_hand_pos_local_to = cur_input.grid_hand_pos_local_to;
										gs.players[player_slot].input.dobuild = cur_input.dobuild;
										gs.players[player_slot].input.build_type = cur_input.build_type;
										gs.players[player_slot].input.build_rotation = cur_input.build_rotation;
									}
								}
								player_to_latest_id_processed[player_slot] = received.inputs[0].id;
							}
						}
					}
					/* Clean up the packet now that we're done using it. */
					enet_packet_destroy(event.packet);
				}
				break;

				case ENET_EVENT_TYPE_DISCONNECT:
				{
					int player_index = (int)(int64_t)event.peer->data;
					Log("%" PRId64 " disconnected player index %d.\n", (int64_t)event.peer->data, player_index);
					Entity* player_body = get_entity(&gs, gs.players[player_index].entity);
					if (player_body != NULL)
					{
						entity_destroy(&gs, player_body);
					}
					opus_encoder_destroy(player_encoders[player_index]);
					opus_decoder_destroy(player_decoders[player_index]);
					gs.players[player_index].connected = false;
					clear_buffer(player_voip_buffers[player_index]);
					event.peer->data = NULL;
				}
				break;

				case ENET_EVENT_TYPE_NONE:
				{
				}
				break;
				}
			}

			total_time += (float)stm_sec(stm_diff(stm_now(), last_processed_time));
			last_processed_time = stm_now();
			// @Robost @BeforeShip if can't process quick enough will be stuck being lagged behind, think of a solution for this...
			const float max_time = 5.0f * TIMESTEP;
			if (total_time > max_time)
			{
				Log("Abnormally large total time %f, clamping\n", total_time);
				total_time = max_time;
			}
			bool processed = false;
			while (total_time > TIMESTEP)
			{
				processed = true;
				process(&gs, TIMESTEP);
				total_time -= TIMESTEP;
			}

			if (world_save_name != NULL && (stm_sec(stm_diff(stm_now(), last_saved_world_time))) > TIME_BETWEEN_WORLD_SAVE)
			{
				last_saved_world_time = stm_now();
				ServerToClient msg = (ServerToClient){
					.cur_gs = &gs,
				};
				size_t out_len = 0;
				if (server_to_client_serialize(&msg, world_save_buffer, &out_len, entities_size, NULL, true))
				{
					FILE* save_file = NULL;
					fopen_s(&save_file, (const char*)world_save_name, "wb");
					if (save_file == NULL)
					{
						Log("Could not open save file: errno %d\n", errno);
					}
					else
					{
						size_t data_written = fwrite(world_save_buffer, sizeof(*world_save_buffer), out_len, save_file);
						if (data_written != out_len)
						{
							Log("Failed to save world data, wanted to write %zu but could only write %zu\n", out_len, data_written);
						}
						else
						{
							Log("Saved game world to %s\n", (const char*)world_save_name);
						}
						fclose(save_file);
					}
				}
				else
				{
					Log("URGENT: FAILED TO SAVE WORLD FILE!\n");
				}
			}

			if (processed)
			{
				static char lzo_working_mem[LZO1X_1_MEM_COMPRESS] = { 0 };
				CONNECTED_PEERS(enet_host, cur)
				{
					int this_player_index = (int)(int64_t)cur->data;
					Entity* this_player_entity = get_entity(&gs, gs.players[this_player_index].entity);
					if (this_player_entity == NULL)
						continue;
					// @Speed don't recreate the packet for every peer, gets expensive copying gamestate over and over again
					char* bytes_buffer = malloc(sizeof * bytes_buffer * MAX_SERVER_TO_CLIENT);
					char* compressed_buffer = malloc(sizeof * compressed_buffer * MAX_SERVER_TO_CLIENT);
					ServerToClient to_send = (ServerToClient){
						.cur_gs = &gs,
						.your_player = this_player_index,
						.playback_buffer = player_voip_buffers[this_player_index],
					};

					size_t len = 0;
					if (server_to_client_serialize(&to_send, bytes_buffer, &len, MAX_SERVER_TO_CLIENT, this_player_entity, false))
					{
						if (len > MAX_SERVER_TO_CLIENT - 8)
						{
							Log("Too much data quitting!\n");
							exit(-1);
						}

						size_t compressed_len = 0;
						lzo1x_1_compress(bytes_buffer, len, compressed_buffer, &compressed_len, (void*)lzo_working_mem);

#ifdef LOG_GAMESTATE_SIZE
						Log("Size of gamestate packet before comrpession: %zu | After: %zu\n", len, compressed_len);
#endif
						ENetPacket* gamestate_packet = enet_packet_create((void*)compressed_buffer, compressed_len, ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT);
						int err = enet_peer_send(cur, 0, gamestate_packet);
						if (err < 0)
						{
							Log("Enet failed to send packet error %d\n", err);
							enet_packet_destroy(gamestate_packet);
						}
					}
					else
					{
						Log("Failed to serialize data for client %d\n", this_player_index);
					}
					free(bytes_buffer);
					free(compressed_buffer);
				}
			}
		}
	}
	for (int i = 0; i < MAX_PLAYERS; i++)
	{
		if (player_encoders[i] != NULL)
			opus_encoder_destroy(player_encoders[i]);
		if (player_decoders[i] != NULL)
			opus_decoder_destroy(player_decoders[i]);
	}
	for (int i = 0; i < MAX_PLAYERS; i++) free(player_voip_buffers[i]);
	free(world_save_buffer);
	destroy(&gs);
	free(entity_data);
	enet_host_destroy(enet_host);
	enet_deinitialize();
}