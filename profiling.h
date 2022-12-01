#include "types.h"
#include <stdlib.h> // malloc the profiling buffer

#ifdef PROFILING_H
#error only include profiling.h once
#endif
#define PROFILING_H

#ifdef PROFILING
#define PROFILING_BUFFER_SIZE (1 * 1024 * 1024)

#ifdef PROFILING_IMPL
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
SpallProfile spall_ctx;
THREADLOCAL SpallBuffer spall_buffer;
THREADLOCAL unsigned char *buffer_data = NULL;
THREADLOCAL uint32_t my_thread_id = 0;

void init_profiling(const char *filename)
{
  spall_ctx = SpallInit(filename, 1);
}

void init_profiling_mythread(uint32_t id)
{
  my_thread_id = id;
  if (buffer_data != NULL)
  {
    *(int *)0 = 0;
  }
  buffer_data = malloc(PROFILING_BUFFER_SIZE);
  spall_buffer = (SpallBuffer){
      .length = PROFILING_BUFFER_SIZE,
      .data = buffer_data,
  };
  SpallBufferInit(&spall_ctx, &spall_buffer);
}

void end_profiling_mythread()
{
  SpallBufferFlush(&spall_ctx, &spall_buffer);
  SpallBufferQuit(&spall_ctx, &spall_buffer);
  free(buffer_data);
}

void end_profiling()
{
  SpallQuit(&spall_ctx);
}

#endif // PROFILING_IMPL

#include "spall.h"

extern SpallProfile spall_ctx;
extern THREADLOCAL SpallBuffer spall_buffer;
extern THREADLOCAL uint32_t my_thread_id;

double get_time_in_micros();
void init_profiling(const char *filename);
// you can pass anything to id as long as it's different from other threads
void init_profiling_mythread(uint32_t id);
void end_profiling();
void end_profiling_mythread();

#define PROFILE_SCOPE(name) DeferLoop(SpallTraceBeginLenTidPid(&spall_ctx, &spall_buffer, name, sizeof(name) - 1, my_thread_id, 0, get_time_in_micros()), SpallTraceEndTidPid(&spall_ctx, &spall_buffer, my_thread_id, 0, get_time_in_micros()))

#else // PROFILING

void inline init_profiling(const char *filename) { (void)filename; }
// you can pass anything to id as long as it's different from other threads
void inline init_profiling_mythread(uint32_t id) { (void)id; }
void inline end_profiling() {}
void inline end_profiling_mythread() {}

#define PROFILE_SCOPE(name)
#endif
