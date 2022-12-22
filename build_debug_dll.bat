@REM for whitebox
call shadergen.bat
set compileopts=/Zi /DDEBUG /DLL /OUT:flight_dll /LD
call build_msvc.bat