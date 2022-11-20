#NoEnv
#SingleInstance, Force
SendMode, Input
SetBatchLines, -1
SetWorkingDir, %A_ScriptDir%

; Fuck windows for having this hardcoded
^Esc::return 

^b::
WinKill, Flight Hosting
WinActivate, flightbuild
If WinActive("flightbuild")
{
    Send, {Enter}
    Send, remedybg continue-execution && sleep 0.1 && remedybg.exe stop-debugging && msbuild && remedybg.exe start-debugging {Enter}
}
return

^+b::
WinKill, Flight
Sleep, 20
WinKill, Flight
Sleep, 20
WinKill, Flight
WinActivate, flightbuild
If WinActive("flightbuild")
{
    Send, cd C:\Users\Cameron\Documents\flight{Enter} build_debug.bat && START /B flight_debug.exe && flight_debug.exe --host{Enter}
}
return