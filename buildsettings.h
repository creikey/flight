#pragma once

#ifdef SERVER_ADDRESS
#error Dont define server address from the build system, use the build settings header
#endif

#define SERVER_PORT 2551
// must be unsigned integer
#define GIT_RELEASE_TAG 24

#ifdef DEBUG

#define SERVER_ADDRESS "127.0.0.1"
#define ASSERT_DO_POPUP_AND_CRASH
// #define SERVER_ADDRESS "207.246.80.160"

// #define PROFILING
// #define DEBUG_RENDERING
// #define DEBUG_WORLD
#define UNLOCK_ALL
#define TIME_BETWEEN_WORLD_SAVE 1000000.0f
#define INFINITE_RESOURCES
#define DEBUG_TOOLS
#define CHIPMUNK_INTEGRITY_CHECK
// #define FAT_THRUSTERS
#define NO_GRAVITY
// #define NO_SUNS

#else

#ifdef RELEASE

// DANGER modifying these, make sure to change them back before releasing

// #define PROFILING
// #define SERVER_ADDRESS "127.0.0.1"
#define SERVER_ADDRESS "207.246.80.160"
#define ASSERT_DO_POPUP_AND_CRASH

#else
#error Define either DEBUG or RELEASE
#endif

#endif
