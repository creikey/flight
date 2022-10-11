#NoEnv
#SingleInstance, Force
SendMode, Input
SetBatchLines, -1
SetWorkingDir, %A_ScriptDir%


^+b::
WinKill, Flight
WinActivate, flightbuild
If WinActive("flightbuild")
{
    Send, build_debug.bat && flight.exe{Enter}
}