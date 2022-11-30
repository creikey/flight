#include "types.h"
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#define SOKOL_IMPL
#include "sokol_time.h"

#include <signal.h>
#include <unistd.h>

ServerThreadInfo server_info = {
	.world_save = "world.bin",
};

void term(int signum)
{
	server_info.should_quit = true;
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
