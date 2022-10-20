@echo off

@REM what all the compile flags mean: https://learn.microsoft.com/en-us/cpp/build/reference/compiler-options-listed-by-category?view=msvc-170

WHERE sokol-shdc.exe
IF %ERRORLEVEL% NEQ 0 ECHO ERROR download sokol-shdc from https://github.com/floooh/sokol-tools-bin/blob/master/bin/win32/sokol-shdc.exe and put it in this folder

@REM example of how to compile shaders: sokol-shdc.exe --input triangle.glsl --output triangle.gen.h --slang glsl330:hlsl5:metal_macos

setlocal enabledelayedexpansion enableextensions
pushd thirdparty\Chipmunk2D\src
  set MUNKSRC=
  for %%x in (*.c) do set MUNKSRC=!MUNKSRC! thirdparty\Chipmunk2D\src\%%x
popd

cl /MP /Zi /Fd"flight.pdb"  /Fe"flight"^
  /I"thirdparty" /I"thirdparty\enet\include" /I"thirdparty\Chipmunk2D\include\chipmunk" /I"thirdparty\Chipmunk2D\include"^
  main.c gamestate.c server.c debugdraw.c^
  thirdparty\enet\callbacks.c thirdparty\enet\compress.c thirdparty\enet\host.c thirdparty\enet\list.c thirdparty\enet\packet.c thirdparty\enet\peer.c thirdparty\enet\protocol.c thirdparty\enet\win32.c Ws2_32.lib winmm.lib^
  %MUNKSRC%