#NoEnv
#SingleInstance, Force
SendMode, Input
SetBatchLines, -1
SetWorkingDir, %A_ScriptDir%

; Fuck windows for having this hardcoded
^Esc::return 

^b::
WinKill, Flight
Sleep, 20
WinKill, Flight
Sleep, 20
WinKill, Flight
WinActivate, flightbuild
If WinActive("flightbuild")
{
    Send, cd C:\Users\Cameron\Documents\flight{Enter} build_debug.bat && flight.exe --host{Enter}
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
    Send, cd C:\Users\Cameron\Documents\flight{Enter} build_debug.bat && START /B flight.exe && flight.exe --host{Enter}
}
return