call shadergen.bat
set compileopts=/Fe"flight_debug" /Zi /FS /Fd"flight.pdb" /DSERVER_ADDRESS="\"127.0.0.1\"" /DDEBUG_RENDERING
call build_msvc.bat