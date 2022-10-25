#include "types.h"
#include "sokol_time.h"
#include <enet/enet.h>
#include <stdio.h>
#include <inttypes.h> // int64 printing

// started in a thread from host
void server(void *data)
{
    (void)data;

    stm_setup();

    struct GameState gs = {0};
    initialize(&gs);

    // box haven
    if (true)
    {
        grid_new(&gs.grids[0], &gs, (V2){.x = 0.75f, .y = 0.0});
        box_new(&gs.grids[0].boxes[0], &gs, &gs.grids[0], (V2){0});
        box_new(&gs.grids[0].boxes[1], &gs, &gs.grids[0], (V2){0, 0.5f});
        box_new(&gs.grids[0].boxes[2], &gs, &gs.grids[0], (V2){0, 1.0f});
        box_new(&gs.grids[0].boxes[3], &gs, &gs.grids[0], (V2){0.5f, 1.0f});

        grid_new(&gs.grids[1], &gs, (V2){.x = -0.75f, .y = 0.0});
        box_new(&gs.grids[1].boxes[0], &gs, &gs.grids[1], (V2){0});

        grid_new(&gs.grids[2], &gs, (V2){.x = -0.75f, .y = 0.5});
        box_new(&gs.grids[2].boxes[0], &gs, &gs.grids[2], (V2){0});
    }

    // two boxes
    if (false)
    {
        grid_new(&gs.grids[0], &gs, (V2){.x = 0.75f, .y = 0.0});
        box_new(&gs.grids[0].boxes[0], &gs, &gs.grids[0], (V2){0});

        grid_new(&gs.grids[1], &gs, (V2){.x = -1.75f, .y = 0.0});
        box_new(&gs.grids[1].boxes[1], &gs, &gs.grids[1], (V2){1});
    }

    // one box policy
    if (false)
    {
        grid_new(&gs.grids[0], &gs, (V2){.x = 0.75f, .y = 0.0});
        box_new(&gs.grids[0].boxes[0], &gs, &gs.grids[0], (V2){0});
    }

    if (enet_initialize() != 0)
    {
        fprintf(stderr, "An error occurred while initializing ENet.\n");
        exit(-1);
    }

    ENetAddress address;
    ENetHost *server;
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
    uint64_t player_to_latest_tick_processed[MAX_PLAYERS] = {0};
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
                        event.peer->data = (void *)player_slot;
                        reset_player(&gs.players[player_slot]);
                        // gs.players[player_slot].box = box_new(&gs, (V2){
                        //                                                .x = 0.0f,
                        //                                                .y = 1.0f * (float)player_slot,
                        //                                            });
                        gs.players[player_slot].connected = true;
                    }

                    break;
                case ENET_EVENT_TYPE_RECEIVE:
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
                        struct ClientToServer received = {0};
                        memcpy(&received, event.packet->data, length);
                        int64_t player_slot = (int64_t)event.peer->data;
                        uint64_t latest_tick = player_to_latest_tick_processed[player_slot];

                        if (received.inputs[0].tick > latest_tick)
                        {
                            for (int i = INPUT_BUFFER - 1; i >= 0; i--)
                            {
                                if (received.inputs[i].tick == 0) // empty input
                                    continue;
                                if(received.inputs[i].tick <= latest_tick)
                                    continue; // don't reprocess inputs already processed
                                struct InputFrame cur_input = received.inputs[i];
                                gs.players[player_slot].movement = cur_input.movement;
                                gs.players[player_slot].grid_index = cur_input.grid_index;

                                // for these "event" inputs, only modify the game state if the event is true.
                                // while processing the gamestate, will mark it as false once processed. This
                                // prevents setting the event input to false before it's been processed.
                                if (cur_input.inhabit)
                                {
                                    gs.players[player_slot].inhabit = cur_input.inhabit;
                                }
                                if (cur_input.dobuild)
                                {
                                    gs.players[player_slot].build = cur_input.build;
                                    gs.players[player_slot].dobuild = cur_input.dobuild;
                                }
                            }
                            player_to_latest_tick_processed[player_slot] = received.inputs[0].tick;
                        }
                    }

                    /* Clean up the packet now that we're done using it. */
                    enet_packet_destroy(event.packet);

                    break;

                case ENET_EVENT_TYPE_DISCONNECT:
                    int player_index = (int64_t)event.peer->data;
                    Log("%" PRId64 " disconnected player index %d.\n", (int64_t)event.peer->data, player_index);
                    gs.players[player_index].connected = false;
                    // box_destroy(&gs.players[player_index].box);
                    event.peer->data = NULL;
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
            static char bytes_buffer[MAX_BYTES_SIZE] = {0};
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

                int len = 0;
                into_bytes(&to_send, bytes_buffer, &len, MAX_BYTES_SIZE);

                ENetPacket *gamestate_packet = enet_packet_create((void *)bytes_buffer, len, ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT);
                enet_peer_send(&server->peers[i], 0, gamestate_packet);
            }
        }
    }

    destroy(&gs);
    enet_host_destroy(server);
    enet_deinitialize();
}