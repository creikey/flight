git push || goto :error
call build_release.bat || goto :error 
call update_server.bat || goto :error
tar.exe -a -c -f releases\flight-nonumber.zip flight_release.exe loaded LICENSE.txt || goto :error
echo "Now test flight-nonumber and make sure it works. Once everything is confirmed to be working:"
echo "1. Increment the GIT_RELEASE_TAG in buildsettings.h"
echo "2. Add everything to git and commit"
echo "3. Tag the new commit the _exact same_ as the previously mentioned GIT_RELEASE_TAG"
echo "4. Push everything, then update all the servers (@TODO make this a script that works for multiple servers)"

goto :EOF

:error
echo Failed to release with error %errorlevel%
exit /b %errorlevel%