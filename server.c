#include "types.h"
#include "sokol_time.h"
#include <enet/enet.h>
#include <stdio.h>
#include <inttypes.h> // int64 printing

#define TIMESTEP (1.0f / 60.0f)

// started in a thread from host
void server(void *data)
{
    (void)data;

    stm_setup();

    struct GameState gs = {0};

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

    if (enet_initialize() != 0)
    {
        fprintf(stderr, "An error occurred while initializing ENet.\n");
        exit(-1);
    }

    ENetAddress address;
    ENetHost *server;
    address.host = ENET_HOST_ANY;
    /* Bind the server to port 1234. */
    address.port = 8000;
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

    Log("Serving on port 8000...\n");
    ENetEvent event;
    uint64_t last_processed_time = stm_now();
    float total_time = 0.0f;
    while (true)
    {
        // @Speed handle enet messages and simulate gamestate in parallel, then sync... must clone gamestate for this
        while (true)
        {
            int ret = enet_host_service(server, &event, 16);
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
                        gs.players[player_slot] = (struct Player){ 0 };
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
                        gs.players[player_slot].input = received.input;
                    }

                    /* Clean up the packet now that we're done using it. */
                    enet_packet_destroy(event.packet);

                    break;

                case ENET_EVENT_TYPE_DISCONNECT:
                    Log("%" PRId64 " disconnected.\n", (int64_t)event.peer->data);
                    gs.players[(int64_t)event.peer->data].connected = false;
                    event.peer->data = NULL;
                }
            }
        }

        total_time += (float)stm_sec(stm_diff(stm_now(), last_processed_time));
        last_processed_time = stm_now();
        // @Robost if can't process quick enough will be stuck being lagged behind, think of a solution for this...
        while (total_time > TIMESTEP)
        {
            process(&gs, TIMESTEP);
            total_time -= TIMESTEP;
        }

        for (int i = 0; i < server->peerCount; i++)
        {
            // @Speed don't recreate the packet for every peer, gets expensive copying gamestate over and over again
            if (server->peers[i].state != ENET_PEER_STATE_CONNECTED)
            {
                continue;
            }
            struct ServerToClient to_send;
            to_send.cur_gs = gs;
            to_send.your_player = (int)(int64_t)server->peers[i].data;
            ENetPacket *gamestate_packet = enet_packet_create((void *)&to_send, sizeof(struct ServerToClient), ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT);
            enet_peer_send(&server->peers[i], 0, gamestate_packet);
        }
    }

    enet_host_destroy(server);
    enet_deinitialize();
}