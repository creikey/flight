#include "sokol_time.h"
#include <chipmunk.h> // initializing bodies
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

#ifdef PROFILING

#define SPALL_IMPLEMENTATION
#pragma warning(disable : 4996) // spall uses fopen
#include "spall.h"

#define WIN32_LEAN_AND_MEAN
#define VC_EXTRALEAN
#define NOMINMAX
#include <Windows.h>
// This is slow, if you can use RDTSC and set the multiplier in SpallInit, you'll have far better timing accuracy
double get_time_in_micros()
{
  static double invfreq;
  if (!invfreq)
  {
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    invfreq = 1000000.0 / frequency.QuadPart;
  }
  LARGE_INTEGER counter;
  QueryPerformanceCounter(&counter);
  return counter.QuadPart * invfreq;
}

static SpallProfile spall_ctx;
static SpallBuffer spall_buffer;

#define PROFILE_SCOPE(name) DeferLoop(SpallTraceBeginLenTidPid(&spall_ctx, &spall_buffer, name, sizeof(name) - 1, 0, 0, get_time_in_micros()), SpallTraceEndTidPid(&spall_ctx, &spall_buffer, 0, 0, get_time_in_micros()))
#else // PROFILING

#define PROFILE_SCOPE(name)

#endif

// started in a thread from host
void server(void *info_raw)
{
  ServerThreadInfo *info = (ServerThreadInfo *)info_raw;
  const char *world_save_name = info->world_save;
#ifdef PROFILING
#define BUFFER_SIZE (1 * 1024 * 1024)
  spall_ctx = SpallInit("server.spall", 1);
  unsigned char *buffer = malloc(BUFFER_SIZE);
  spall_buffer = (SpallBuffer){
      .length = BUFFER_SIZE,
      .data = buffer,
  };
  SpallBufferInit(&spall_ctx, &spall_buffer);

#endif

  struct GameState gs = {0};
  size_t entities_size = (sizeof(Entity) * MAX_ENTITIES);
  Entity *entity_data = malloc(entities_size);
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
    unsigned char *read_game_data = malloc(read_game_data_buffer_size);

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

#define BOX_AT_TYPE(grid, pos, type) \
  {                                  \
    Entity *box = new_entity(&gs);   \
    box_create(&gs, box, grid, pos); \
    box->box_type = type;            \
  }
#define BOX_AT(grid, pos) BOX_AT_TYPE(grid, pos, BoxHullpiece)

  // one box policy
  if (false)
  {
    Entity *grid = new_entity(&gs);
    grid_create(&gs, grid);
    entity_set_pos(grid, (V2){-BOX_SIZE * 2, 0.0f});
    Entity *box = new_entity(&gs);
    box_create(&gs, box, grid, (V2){0});
  }

  // rotation test
  if (false)
  {
    Entity *grid = new_entity(&gs);
    grid_create(&gs, grid);
    entity_set_pos(grid, (V2){-BOX_SIZE * 2, 0.0f});
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
  ENetHost *enet_host;
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
  uint64_t last_sent_audio_time = stm_now();
  uint64_t last_sent_gamestate_time = stm_now();
  float audio_time_to_send = 0.0f;
  float total_time = 0.0f;
  unsigned char *world_save_buffer = malloc(entities_size);
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

#ifdef UNLOCK_ALL
            gs.players[player_slot].unlocked_bombs = true;
#endif
            gs.players[player_slot].squad = SquadPurple;
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
            size_t length = event.packet->dataLength;
#define VOIP_QUEUE_DECL(queue_name, queue_data_name)                                                \
  Queue queue_name = {0};                                                                           \
  char queue_data_name[QUEUE_SIZE_FOR_ELEMENTS(sizeof(OpusPacket), VOIP_PACKET_BUFFER_SIZE)] = {0}; \
  queue_init(&queue_name, sizeof(OpusPacket), queue_data_name, QUEUE_SIZE_FOR_ELEMENTS(sizeof(OpusPacket), VOIP_PACKET_BUFFER_SIZE))
            VOIP_QUEUE_DECL(throwaway_buffer, throwaway_buffer_data);
            Queue *buffer_to_fill = &player_voip_buffers[player_slot];
            if (get_entity(&gs, gs.players[player_slot].entity) == NULL)
              buffer_to_fill = &throwaway_buffer;

            queue_clear(&player_input_queues[player_slot]);
            struct ClientToServer received = {.mic_data = buffer_to_fill, .input_data = &player_input_queues[player_slot]};
            unsigned char decompressed[MAX_CLIENT_TO_SERVER] = {0};
            size_t decompressed_max_len = MAX_CLIENT_TO_SERVER;
            assert(LZO1X_MEM_DECOMPRESS == 0);

            int return_value = lzo1x_decompress_safe(event.packet->data, event.packet->dataLength, decompressed, &decompressed_max_len, NULL);

            if (return_value == LZO_E_OK)
            {
              if (!client_to_server_deserialize(&gs, &received, decompressed, decompressed_max_len))
              {
                Log("Bad packet from client %d\n", (int)player_slot);
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
            entity_destroy(&gs, player_body);
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
    total_time += (float)stm_sec(stm_diff(stm_now(), last_processed_time));
    last_processed_time = stm_now();
    // @Robost @BeforeShip if can't process quick enough will be stuck being lagged behind, think of a solution for this...
    const float max_time = 5.0f * TIMESTEP;
    if (total_time > max_time)
    {
      Log("Abnormally large total time %f, clamping\n", total_time);
      total_time = max_time;
    }

    PROFILE_SCOPE("World Processing")
    {
      while (total_time > TIMESTEP)
      {
        CONNECTED_PEERS(enet_host, cur)
        {
          int this_player_index = (int)(int64_t)cur->data;
          QUEUE_ITER(&player_input_queues[this_player_index], cur_header)
          {
            InputFrame *cur = (InputFrame *)cur_header->data;
            if (cur->tick == tick(&gs))
            {
              gs.players[this_player_index].input = *cur;
              break;
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
        size_t out_len = 0;
        if (server_to_client_serialize(&msg, world_save_buffer, &out_len, entities_size, NULL, true))
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
          Log("URGENT: FAILED TO SAVE WORLD FILE!\n");
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
        int num_audio_packets = (int)floor(1.0f / (VOIP_TIME_PER_PACKET / audio_time_to_send));

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
          unsigned char *bytes_buffer = malloc(sizeof *bytes_buffer * MAX_SERVER_TO_CLIENT);
          unsigned char *compressed_buffer = malloc(sizeof *compressed_buffer * MAX_SERVER_TO_CLIENT);

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
                    float dist = V2dist(entity_pos(this_player_entity), entity_pos(other_player_entity));
                    float volume = lerp(1.0f, 0.0f, clamp01(dist / VOIP_DISTANCE_WHEN_CANT_HEAR));
                    if (volume > 0.01f)
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

          size_t len = 0;
          if (server_to_client_serialize(&to_send, bytes_buffer, &len, MAX_SERVER_TO_CLIENT, this_player_entity, false))
          {
            if (len > MAX_SERVER_TO_CLIENT - 8)
            {
              Log("Too much data quitting!\n");
              exit(-1);
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

  printf("Cleanup\n");

#ifdef PROFILING
  SpallBufferQuit(&spall_ctx, &spall_buffer);
  SpallQuit(&spall_ctx);
#endif
}