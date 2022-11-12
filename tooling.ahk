#NoEnv
#SingleInstance, Force
SendMode, Input
SetBatchLines, -1
SetWorkingDir, %A_ScriptDir%

; Fuck windows for having this hardcoded
^Esc::return 

^b::
WinKill, "Flight Not Hosting"
Sleep, 20
WinActivate, flightbuild
If WinActive("flightbuild")
{
    Send, cd C:\Users\Cameron\Documents\flight{Enter} build_debug.bat && flight_debug.exe{Enter}
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