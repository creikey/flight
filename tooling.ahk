#NoEnv
#SingleInstance, Force
SendMode, Input
SetBatchLines, -1
SetWorkingDir, %A_ScriptDir%

^b::
WinKill, Flight
Sleep, 20
WinKill, Flight
Sleep, 20
WinKill, Flight
WinActivate, flightbuild
If WinActive("flightbuild")
{
    Send, build_debug.bat && flight.exe --host{Enter}
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
    Send, build_debug.bat && START /B flight.exe && flight.exe --host{Enter}
}
return