@echo off

WHERE sokol-shdc.exe
IF %ERRORLEVEL% NEQ 0 ECHO ERROR download sokol-shdc from https://github.com/floooh/sokol-tools-bin/blob/master/bin/win32/sokol-shdc.exe and put it in this folder

@REM example of how to compile shaders: sokol-shdc.exe --input triangle.glsl --output triangle.gen.h --slang glsl330:hlsl5:metal_macos
sokol-shdc.exe --format sokol --input hueshift.glsl --output hueshift.gen.h --slang glsl330:hlsl5:metal_macos
sokol-shdc.exe --format sokol --input lightning.glsl --output lightning.gen.h --slang glsl330:hlsl5:metal_macos
sokol-shdc.exe --format sokol --input goodpixel.glsl --output goodpixel.gen.h --slang glsl330:hlsl5:metal_macos