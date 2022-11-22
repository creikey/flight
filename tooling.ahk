#NoEnv
#SingleInstance, Force
SendMode, Input
SetBatchLines, -1
SetWorkingDir, %A_ScriptDir%

; Fuck windows for having this hardcoded
^Esc::return 

^b::
WinKill, Flight Hosting
WinKill, Flight Not Hosting
WinActivate, flightbuild
If WinActive("flightbuild")
{
    Send, {Enter}
    Send, remedybg continue-execution && timeout 1 && remedybg.exe stop-debugging && msbuild && remedybg.exe start-debugging {Enter}
}
return

^+b::
WinKill, Flight Hosting
WinKill, Flight Not Hosting
WinActivate, flightbuild
If WinActive("flightbuild")
{
    Send, {Enter}
    Send, remedybg continue-execution && timeout 1 && remedybg.exe stop-debugging && msbuild && remedybg.exe start-debugging && sleep 0.2 && x64\Debug\Flight.exe {Enter}
}
return