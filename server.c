#include "chipmunk.h" // initializing bodies
#include "sokol_time.h"
#include "types.h"
#include <enet/enet.h>
#include <errno.h>
#include <inttypes.h> // int64 printing
#include <stdio.h>
#include <stdlib.h>
#include <string.h> // error string

#include "minilzo.h"

#include "opus.h"

#ifdef __unix
#define fopen_s(pFile, filename, mode) ((*(pFile)) = fopen((filename), (mode))) == NULL
#endif

#define CONNECTED_PEERS(host, cur)                                              \
  for (ENetPeer *cur = host->peers; cur < host->peers + host->peerCount; cur++) \
    if (cur->state == ENET_PEER_STATE_CONNECTED)

#include "profiling.h"

static void panicquit()
{
  flight_assert(false);
  exit(-1);
}

// started in a thread from host
void server(void *info_raw)
{
  init_profiling_mythread(1);
  ServerThreadInfo *info = (ServerThreadInfo *)info_raw;
  const char *world_save_name = info->world_save;
#ifdef PROFILING

#endif

  struct GameState gs = {0};
  size_t entities_size = (sizeof(Entity) * MAX_ENTITIES);
  Entity *entity_data = calloc(1, entities_size);
  initialize(&gs, entity_data, entities_size);
  gs.server_side_computing = true;
  Log("Allocated %zu bytes for entities\n", entities_size);

  create_initial_world(&gs);

  // inputs
  Queue player_input_queues[MAX_PLAYERS] = {0};
  size_t input_queue_data_size = QUEUE_SIZE_FOR_ELEMENTS(sizeof(InputFrame), INPUT_QUEUE_MAX);
  for (int i = 0; i < MAX_PLAYERS; i++)
    queue_init(&player_input_queues[i], sizeof(InputFrame), calloc(1, input_queue_data_size), input_queue_data_size);

  // voip
  Queue player_voip_buffers[MAX_PLAYERS] = {0};
  size_t player_voip_buffer_size = QUEUE_SIZE_FOR_ELEMENTS(sizeof(OpusPacket), VOIP_PACKET_BUFFER_SIZE);
  for (int i = 0; i < MAX_PLAYERS; i++)
    queue_init(&player_voip_buffers[i], sizeof(OpusPacket), calloc(1, player_voip_buffer_size), player_voip_buffer_size);
  OpusEncoder *player_encoders[MAX_PLAYERS] = {0};
  OpusDecoder *player_decoders[MAX_PLAYERS] = {0};

  if (world_save_name != NULL)
  {
    size_t read_game_data_buffer_size = entities_size;
    unsigned char *read_game_data = calloc(1, read_game_data_buffer_size);

    FILE *file = NULL;
    fopen_s(&file, (const char *)world_save_name, "rb");
    if (file == NULL)
    {
      Log("Could not read from data file %s: errno %d\n", (const char *)world_save_name, errno);
    }
    else
    {
      size_t actual_length = fread(read_game_data, sizeof(char), entities_size, file);
      if (actual_length <= 1)
      {
        Log("Could only read %zu bytes, error: errno %d\n", actual_length, errno);
        panicquit();
      }
      Log("Read %zu bytes from save file\n", actual_length);
      ServerToClient msg = (ServerToClient){
          .cur_gs = &gs,
      };
      SerState ser = init_deserializing(&gs, read_game_data, actual_length, true);
      SerMaybeFailure maybe_fail = ser_server_to_client(&ser, &msg);
      if (maybe_fail.failed)
      {
        Log("Failed to deserialize game world from save file: %d %s\n", maybe_fail.line, maybe_fail.expression);
      }
      fclose(file);
    }

    free(read_game_data);
  }

#define BOX_AT_TYPE(grid, pos, type) \
  {                                  \
    Entity *box = new_entity(&gs);   \
    create_box(&gs, box, grid, pos); \
    box->box_type = type;            \
  }
#define BOX_AT(grid, pos) BOX_AT_TYPE(grid, pos, BoxHullpiece)

  if (enet_initialize() != 0)
  {
    fprintf(stderr, "An error occurred while initializing ENet.\n");
    panicquit();
  }

  ENetAddress address;
  ENetHost *enet_host;
  int sethost = enet_address_set_host_ip(&address, "0.0.0.0");
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
    panicquit();
  }

  Log("Serving on port %d...\n", SERVER_PORT);
  ENetEvent event;
  uint64_t last_processed_time = stm_now();
  uint64_t last_saved_world_time = stm_now();
  uint64_t last_sent_audio_time = stm_now();
  uint64_t last_sent_gamestate_time = stm_now();
  double audio_time_to_send = 0.0;
  double total_time = 0.0;
  unsigned char *world_save_buffer = calloc(1, entities_size);
  PROFILE_SCOPE("Serving")
  {
    while (true)
    {
      ma_mutex_lock(&info->info_mutex);
      if (info->should_quit)
      {
        ma_mutex_unlock(&info->info_mutex);
        break;
      }
      ma_mutex_unlock(&info->info_mutex);

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
              event.peer->data = (void *)player_slot;
              gs.players[player_slot] = (struct Player){0};
              gs.players[player_slot].connected = true;
              create_player(&gs.players[player_slot]);

              int error;
              player_encoders[player_slot] = opus_encoder_create(VOIP_SAMPLE_RATE, 1, OPUS_APPLICATION_VOIP, &error);
              if (error != OPUS_OK)
                Log("Failed to create encoder: %d\n", error);
              player_decoders[player_slot] = opus_decoder_create(VOIP_SAMPLE_RATE, 1, &error);
              if (error != OPUS_OK)
                Log("Failed to create decoder: %d\n", error);
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
            else
            {
              int64_t player_slot = (int64_t)event.peer->data;
#define VOIP_QUEUE_DECL(queue_name, queue_data_name)                                                \
  Queue queue_name = {0};                                                                           \
  char queue_data_name[QUEUE_SIZE_FOR_ELEMENTS(sizeof(OpusPacket), VOIP_PACKET_BUFFER_SIZE)] = {0}; \
  queue_init(&queue_name, sizeof(OpusPacket), queue_data_name, QUEUE_SIZE_FOR_ELEMENTS(sizeof(OpusPacket), VOIP_PACKET_BUFFER_SIZE))
              VOIP_QUEUE_DECL(throwaway_buffer, throwaway_buffer_data);
              Queue *buffer_to_fill = &player_voip_buffers[player_slot];
              if (get_entity(&gs, gs.players[player_slot].entity) == NULL)
                buffer_to_fill = &throwaway_buffer;

              Queue new_inputs = {0};
              char new_inputs_data[QUEUE_SIZE_FOR_ELEMENTS(sizeof(InputFrame), INPUT_QUEUE_MAX)] = {0};
              queue_init(&new_inputs, sizeof(InputFrame), new_inputs_data, ARRLEN(new_inputs_data));

              struct ClientToServer received = {.mic_data = buffer_to_fill, .input_data = &new_inputs};
              unsigned char decompressed[MAX_CLIENT_TO_SERVER] = {0};
              size_t decompressed_max_len = MAX_CLIENT_TO_SERVER;
              flight_assert(LZO1X_MEM_DECOMPRESS == 0);

              int return_value = lzo1x_decompress_safe(event.packet->data, event.packet->dataLength, decompressed, &decompressed_max_len, NULL);

              if (return_value == LZO_E_OK)
              {
                SerState ser = init_deserializing(&gs, decompressed, decompressed_max_len, false);
                SerMaybeFailure maybe_fail = ser_client_to_server(&ser, &received);
                if (maybe_fail.failed)
                {
                  Log("Bad packet from client %d | %d %s\n", (int)player_slot, maybe_fail.line, maybe_fail.expression);
                }
                else
                {
                  QUEUE_ITER(&new_inputs, InputFrame, new_input)
                  {
                    QUEUE_ITER(&player_input_queues[player_slot], InputFrame, existing_input)
                    {
                      if (existing_input->tick == new_input->tick && existing_input->been_processed)
                      {
                        new_input->been_processed = true;
                      }
                    }
                  }
                  queue_clear(&player_input_queues[player_slot]);
                  QUEUE_ITER(&new_inputs, InputFrame, cur)
                  {
                    InputFrame *new_elem = queue_push_element(&player_input_queues[player_slot]);
                    flight_assert(new_elem != NULL);
                    *new_elem = *cur;
                  }
                }
              }
              else
              {
                Log("Couldn't decompress player packet, error code %d from lzo\n", return_value);
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
            Entity *player_body = get_entity(&gs, gs.players[player_index].entity);
            if (player_body != NULL)
            {
              entity_memory_free(&gs, player_body);
            }
            opus_encoder_destroy(player_encoders[player_index]);
            player_encoders[player_index] = NULL;
            opus_decoder_destroy(player_decoders[player_index]);
            player_decoders[player_index] = NULL;
            gs.players[player_index].connected = false;
            queue_clear(&player_voip_buffers[player_index]);
            event.peer->data = NULL;
          }
          break;

          case ENET_EVENT_TYPE_NONE:
          {
          }
          break;
          }
        }
      }
      total_time += stm_sec(stm_diff(stm_now(), last_processed_time));
      last_processed_time = stm_now();
      const double max_time = 5.0 * TIMESTEP;
      if (total_time > max_time)
      {
        Log("SERVER LAGGING Abnormally large total time %f, clamping\n", total_time);
        total_time = max_time;
      }

      while (total_time > TIMESTEP)
      {
        PROFILE_SCOPE("World Processing")
        {
          CONNECTED_PEERS(enet_host, cur_peer)
          {
            int this_player_index = (int)(int64_t)cur_peer->data;
            QUEUE_ITER(&player_input_queues[this_player_index], InputFrame, cur)
            {
              if (cur->tick == tick(&gs))
              {
                gs.players[this_player_index].input = *cur;
                cur->been_processed = true;
                break;
              }
              if (cur->tick < tick(&gs) && !cur->been_processed)
              {
                Log("Did not process input from client %d %llu ticks ago!\n", this_player_index,tick(&gs) - cur->tick);
              }
            }
          }

          process(&gs, TIMESTEP);
          total_time -= TIMESTEP;
        }
      }

      if (world_save_name != NULL && (stm_sec(stm_diff(stm_now(), last_saved_world_time))) > TIME_BETWEEN_WORLD_SAVE)
      {
        PROFILE_SCOPE("Save World")
        {
          last_saved_world_time = stm_now();
          ServerToClient msg = (ServerToClient){
              .cur_gs = &gs,
          };
          SerState ser = init_serializing(&gs, world_save_buffer, entities_size, NULL, true);
          SerMaybeFailure maybe_fail = ser_server_to_client(&ser, &msg);
          size_t out_len = ser_size(&ser);
          if (!maybe_fail.failed)
          {
            FILE *save_file = NULL;
            fopen_s(&save_file, (const char *)world_save_name, "wb");
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
                Log("Saved game world to %s\n", (const char *)world_save_name);
              }
              fclose(save_file);
            }
          }
          else
          {
            Log("URGENT: FAILED TO SAVE WORLD FILE! Failed at line %d expression %s\n", maybe_fail.line, maybe_fail.expression);
          }
        }
      }

      if (stm_sec(stm_diff(stm_now(), last_sent_gamestate_time)) > TIME_BETWEEN_SEND_GAMESTATE)
      {
        last_sent_gamestate_time = stm_now();
        PROFILE_SCOPE("send_data")
        {
          static char lzo_working_mem[LZO1X_1_MEM_COMPRESS] = {0};

          audio_time_to_send += (float)stm_sec(stm_diff(stm_now(), last_sent_audio_time));
          last_sent_audio_time = stm_now();
          int num_audio_packets = (int)floor(1.0 / (VOIP_TIME_PER_PACKET / audio_time_to_send));

#define MAX_AUDIO_PACKETS_TO_SEND 12
          if (num_audio_packets > MAX_AUDIO_PACKETS_TO_SEND)
          {
            Log("Wants %d, this is too many packets. Greater than the maximum %d\n", num_audio_packets, MAX_AUDIO_PACKETS_TO_SEND);
            num_audio_packets = MAX_AUDIO_PACKETS_TO_SEND;
          }

          opus_int16 decoded_audio_packets[MAX_PLAYERS][MAX_AUDIO_PACKETS_TO_SEND][VOIP_EXPECTED_FRAME_COUNT] = {0};

          audio_time_to_send -= num_audio_packets * VOIP_TIME_PER_PACKET;

          // decode what everybody said
          CONNECTED_PEERS(enet_host, cur)
          {
            int this_player_index = (int)(int64_t)cur->data;
            for (int packet_i = 0; packet_i < num_audio_packets; packet_i++)
            {
              opus_int16 *to_dump_to = decoded_audio_packets[this_player_index][packet_i];
              OpusPacket *cur_packet = (OpusPacket *)queue_pop_element(&player_voip_buffers[this_player_index]);
              if (cur_packet == NULL)
                opus_decode(player_decoders[this_player_index], NULL, 0, to_dump_to, VOIP_EXPECTED_FRAME_COUNT, 0);
              else
                opus_decode(player_decoders[this_player_index], cur_packet->data, cur_packet->length, to_dump_to, VOIP_EXPECTED_FRAME_COUNT, 0);
            }
          }

          // send gamestate to each player
          CONNECTED_PEERS(enet_host, cur)
          {
            int this_player_index = (int)(int64_t)cur->data;
            Entity *this_player_entity = get_entity(&gs, gs.players[this_player_index].entity);
            if (this_player_entity == NULL)
              continue;
            // @Speed don't recreate the packet for every peer, gets expensive copying gamestate over and over again
            unsigned char *bytes_buffer = calloc(1, sizeof *bytes_buffer * MAX_SERVER_TO_CLIENT);
            unsigned char *compressed_buffer = calloc(1, sizeof *compressed_buffer * MAX_SERVER_TO_CLIENT);

            // mix audio to be sent
            VOIP_QUEUE_DECL(buffer_to_play, buffer_to_play_data);
            {
              for (int packet_i = 0; packet_i < num_audio_packets; packet_i++)
              {
                opus_int16 to_send_to_cur[VOIP_EXPECTED_FRAME_COUNT] = {0}; // mix what other players said into this buffer
                CONNECTED_PEERS(enet_host, other_player)
                {
                  if (other_player != cur)
                  {
                    int other_player_index = (int)(int64_t)other_player->data;
                    Entity *other_player_entity = get_entity(&gs, gs.players[other_player_index].entity);
                    if (other_player_entity != NULL)
                    {
                      double dist = cpvdist(entity_pos(this_player_entity), entity_pos(other_player_entity));
                      double volume = lerp(1.0, 0.0, clamp01(dist / VOIP_DISTANCE_WHEN_CANT_HEAR));
                      if (volume > 0.01)
                      {
                        for (int frame_i = 0; frame_i < VOIP_EXPECTED_FRAME_COUNT; frame_i++)
                        {
                          to_send_to_cur[frame_i] += (opus_int16)((float)decoded_audio_packets[other_player_index][packet_i][frame_i] * volume);
                        }
                      }
                    }
                  }
                }
                OpusPacket *this_packet = (OpusPacket *)queue_push_element(&buffer_to_play);
                opus_int32 ret = opus_encode(player_encoders[this_player_index], to_send_to_cur, VOIP_EXPECTED_FRAME_COUNT, this_packet->data, VOIP_PACKET_MAX_SIZE);
                if (ret < 0)
                {
                  Log("Failed to encode audio packet for player %d: opus error code %d\n", this_player_index, ret);
                }
                else
                {
                  this_packet->length = ret;
                }
              }
            }
            ServerToClient to_send = (ServerToClient){
                .cur_gs = &gs,
                .your_player = this_player_index,
                .audio_playback_buffer = &buffer_to_play,
            };

            SerState ser = init_serializing(&gs, bytes_buffer, MAX_SERVER_TO_CLIENT, this_player_entity, false);
            SerMaybeFailure maybe_fail = ser_server_to_client(&ser, &to_send);
            size_t len = ser_size(&ser);
            if (!maybe_fail.failed)
            {
              if (len > MAX_SERVER_TO_CLIENT - 8)
              {
                Log("Too much data quitting!\n");
                panicquit();
              }

              size_t compressed_len = 0;
              lzo1x_1_compress(bytes_buffer, len, compressed_buffer, &compressed_len, (void *)lzo_working_mem);

#ifdef LOG_GAMESTATE_SIZE
              Log("Size of gamestate packet before comrpession: %zu | After: %zu\n", len, compressed_len);
#endif
              ENetPacket *gamestate_packet = enet_packet_create((void *)compressed_buffer, compressed_len, ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT);
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
  }
  for (int i = 0; i < MAX_PLAYERS; i++)
  {
    if (player_encoders[i] != NULL)
      opus_encoder_destroy(player_encoders[i]);
    if (player_decoders[i] != NULL)
      opus_decoder_destroy(player_decoders[i]);
  }
  for (int i = 0; i < MAX_PLAYERS; i++)
    free(player_voip_buffers[i].data);
  for (int i = 0; i < MAX_PLAYERS; i++)
    free(player_input_queues[i].data);
  free(world_save_buffer);
  destroy(&gs);
  free(entity_data);
  enet_host_destroy(enet_host);
  enet_deinitialize();

  end_profiling_mythread();
  printf("Cleanup\n");

#ifdef PROFILING
#endif
}