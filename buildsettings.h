#pragma once

#ifdef SERVER_ADDRESS
#error Dont define server address from the build system, use the build settings header
#endif

#define SERVER_PORT 2551

#ifdef DEBUG

#define SERVER_ADDRESS "127.0.0.1"
// #define SERVER_ADDRESS "207.246.80.160"

//#define PROFILING
#define DEBUG_RENDERING
//#define DEBUG_WORLD
//#define UNLOCK_ALL
//#define INFINITE_RESOURCES
//#define NO_GRAVITY
//#define NO_SUNS

#else

#ifdef RELEASE

// #define PROFILING
// #define SERVER_ADDRESS "127.0.0.1"
#define SERVER_ADDRESS "207.246.80.160"

#else
#error Define either DEBUG or RELEASE
#endif

#endif
