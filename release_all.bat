call vcvars
git push
call build_release.bat
call update_server.bat
tar.exe -a -c -f releases\flight-nonumber.zip flight_release.exe loaded