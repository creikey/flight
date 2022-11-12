#include "types.h"

#define SOKOL_IMPL
#include "sokol_time.h"

int main(int argc, char **argv)
{
	stm_setup();
	server("world.bin");
	return 0;
}
