#include <chipmunk.h> // initializing bodies
#include "types.h"
#include "sokol_time.h"
#include <enet/enet.h>
#include <stdio.h>
#include <inttypes.h> // int64 printing
#include <stdlib.h>


// started in a thread from host
void server(void* data)
{
	(void)data;

	stm_setup();

	struct GameState gs = { 0 };
	size_t entities_size = (sizeof(Entity) * MAX_ENTITIES);
	Entity* entity_data = malloc(entities_size);
	initialize(&gs, entity_data, entities_size);
	Log("Allocated %zu bytes for entities\n", entities_size);

	// one box policy
	if (true)
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
		
#define BOX_AT(pos) { Entity* box = new_entity(&gs); box_create(&gs, box, grid, pos); }
		BOX_AT(((V2) { 0 }));
		BOX_AT(((V2) { BOX_SIZE, 0 }));
		BOX_AT(((V2) { 2.0*BOX_SIZE, 0 }));
		BOX_AT(((V2) { 2.0*BOX_SIZE, BOX_SIZE }));
		BOX_AT(((V2) { 0.0*BOX_SIZE, -BOX_SIZE }));
	}

	if (enet_initialize() != 0)
	{
		fprintf(stderr, "An error occurred while initializing ENet.\n");
		exit(-1);
	}

	ENetAddress address;
	ENetHost* server;
	int sethost = enet_address_set_host_ip(&address, LOCAL_SERVER_ADDRESS);
	if (sethost != 0)
	{
		Log("Fishy return value from set host: %d\n", sethost);
	}
	/* Bind the server to port 1234. */
	address.port = SERVER_PORT;
	server = enet_host_create(&address /* the address to bind the server host to */,
		32 /* allow up to 32 clients and/or outgoing connections */,
		2 /* allow up to 2 channels to be used, 0 and 1 */,
		0 /* assume any amount of incoming bandwidth */,
		0 /* assume any amount of outgoing bandwidth */);
	if (server == NULL)
	{
		fprintf(stderr,
			"An error occurred while trying to create an ENet server host.\n");
		exit(-1);
	}

	Log("Serving on port %d...\n", SERVER_PORT);
	ENetEvent event;
	uint64_t last_processed_time = stm_now();
	float total_time = 0.0f;
	size_t player_to_latest_id_processed[MAX_PLAYERS] = { 0 };
	while (true)
	{
		// @Speed handle enet messages and simulate gamestate in parallel, then sync... must clone gamestate for this
		while (true)
		{
			int ret = enet_host_service(server, &event, 0);
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
					}

					break;
				}
					
				case ENET_EVENT_TYPE_RECEIVE:
				{
					// Log("A packet of length %zu was received on channel %u.\n",
					//        event.packet->dataLength,
					//        event.channelID);

					size_t length = event.packet->dataLength;
					if (length != sizeof(struct ClientToServer))
					{
						Log("Length did not match up...\n");
					}
					else
					{
						struct ClientToServer received = { 0 };
						memcpy(&received, event.packet->data, length);
						int64_t player_slot = (int64_t)event.peer->data;
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

					/* Clean up the packet now that we're done using it. */
					enet_packet_destroy(event.packet);

					break;
				}


				case ENET_EVENT_TYPE_DISCONNECT:
				{
					int player_index = (int)(int64_t)event.peer->data;
					Log("%" PRId64 " disconnected player index %d.\n", (int64_t)event.peer->data, player_index);
					gs.players[player_index].connected = false;
					// box_destroy(&gs.players[player_index].box);
					event.peer->data = NULL;
					break;
				}

				}
			}
		}

		total_time += (float)stm_sec(stm_diff(stm_now(), last_processed_time));
		last_processed_time = stm_now();
		// @Robost @BeforeShip if can't process quick enough will be stuck being lagged behind, think of a solution for this...
		bool processed = false;
		while (total_time > TIMESTEP)
		{
			processed = true;
			process(&gs, TIMESTEP);
			total_time -= TIMESTEP;
		}

		if (processed)
		{
#define MAX_BYTES_SIZE 2048 * 2
			static char bytes_buffer[MAX_BYTES_SIZE] = { 0 };
			for (int i = 0; i < server->peerCount; i++)
			{
				// @Speed don't recreate the packet for every peer, gets expensive copying gamestate over and over again
				if (server->peers[i].state != ENET_PEER_STATE_CONNECTED)
				{
					continue;
				}
				struct ServerToClient to_send;
				to_send.cur_gs = &gs;
				to_send.your_player = (int)(int64_t)server->peers[i].data;

				size_t len = 0;
				into_bytes(&to_send, bytes_buffer, &len, MAX_BYTES_SIZE);

				ENetPacket* gamestate_packet = enet_packet_create((void*)bytes_buffer, len, ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT);
				enet_peer_send(&server->peers[i], 0, gamestate_packet);
			}
		}
	}

	destroy(&gs);
	free(entity_data);
	enet_host_destroy(server);
	enet_deinitialize();
}