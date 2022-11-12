#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#include "types.h"

#define SOKOL_IMPL
#include "sokol_time.h"

#include <signal.h>
#include <unistd.h>

ServerThreadInfo server_info = {
	.world_save = "world.bin",
};

void term(int signum)
{
	ma_mutex_lock(&server_info.info_mutex);
	server_info.running = false;
	ma_mutex_unlock(&server_info.info_mutex);
}


int main(int argc, char **argv)
{
    struct sigaction action;
    memset(&action, 0, sizeof(struct sigaction));
    action.sa_handler = term;
    sigaction(SIGTERM, &action, NULL);

	stm_setup();
	ma_mutex_init(&server_info.info_mutex);
	server(&server_info);
	ma_mutex_uninit(&server_info.info_mutex);
	return 0;
}
