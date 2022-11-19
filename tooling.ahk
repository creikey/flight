#NoEnv
#SingleInstance, Force
SendMode, Input
SetBatchLines, -1
SetWorkingDir, %A_ScriptDir%

; Fuck windows for having this hardcoded
^Esc::return 

^b::
WinKill, Flight Hosting
Sleep, 20
WinActivate flight.rdbg
Sleep 20
Send, {Shift down}{F5}{Shift up}
Send, {F5}
WinActivate, flightbuild
If WinActive("flightbuild")
{
    Send, {Enter}
    Send, msbuild{Enter}
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