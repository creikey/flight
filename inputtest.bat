set EXECUTABLE=x64\Debug\Flight.exe

START /b %EXECUTABLE% host=true replay_inputs_from=input_go_up.bin
%EXECUTABLE% replay_inputs_from=input_go_down.bin