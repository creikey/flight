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
    Send, remedybg continue-execution && timeout 1 && remedybg.exe stop-debugging && shadergen.bat && msbuild && remedybg.exe start-debugging {Enter}
}
Send, {Blind} ; So it doesn't hold down ctrl after running! WTF
return
