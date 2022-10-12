@echo off

@REM what all the flags mean: https://learn.microsoft.com/en-us/cpp/build/reference/compiler-options-listed-by-category?view=msvc-170

WHERE sokol-shdc.exe
IF %ERRORLEVEL% NEQ 0 ECHO ERROR download sokol-shdc from https://github.com/floooh/sokol-tools-bin/blob/master/bin/win32/sokol-shdc.exe and put it in this folder

@REM example of how to compile shaders: sokol-shdc.exe --input triangle.glsl --output triangle.gen.h --slang glsl330:hlsl5:metal_macos

cl /MP /Zi /Fd"flight.pdb" /I"thirdparty" /Fe"flight" main.c gamestate.c server.c