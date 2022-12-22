@echo off

@REM what all the compile flags mean: https://learn.microsoft.com/en-us/cpp/build/reference/compiler-options-listed-by-category?view=msvc-170


set OPUSLIB=%~dp0thirdparty\opus\win32\VS2015\x64\Release\opus.lib

if not exist %OPUSLIB% (
  ECHO ERROR Couldn't find %OPUSLIB% compile opus by opening the visual studio project in win32\VS2015 and building the release setting
)

setlocal enabledelayedexpansion enableextensions
pushd thirdparty\Chipmunk2D\src
  set MUNKSRC=
  for %%x in (*.c) do set MUNKSRC=!MUNKSRC! thirdparty\Chipmunk2D\src\%%x
popd

@REM /DENET_DEBUG=1^
gcc -c^
  -I"thirdparty" -I"thirdparty\minilzo" -I"thirdparty\enet\include" -I"thirdparty\Chipmunk2D\include\chipmunk" -I"thirdparty\Chipmunk2D\include" -I"thirdparty\opus\include" -I"thirdparty\opus\src"^
  %MUNKSRC%

mkdir elf_objects
move *.o elf_objects

  @REM main.c gamestate.c server.c debugdraw.c^
  @REM thirdparty\minilzo\minilzo.c^
  @REM %OPUSLIB%
  @REM thirdparty\enet\callbacks.c thirdparty\enet\compress.c thirdparty\enet\host.c thirdparty\enet\list.c thirdparty\enet\packet.c thirdparty\enet\peer.c thirdparty\enet\protocol.c thirdparty\enet\win32.c Ws2_32.lib winmm.lib^
