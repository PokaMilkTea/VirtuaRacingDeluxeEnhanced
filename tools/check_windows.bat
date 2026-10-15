@echo off
rem The front end's checks, as on the Mac: from power-on into a Big Forest race and through it
rem (race_inputs.txt), each frame's composite against the core's, and the HLE against the SH2s.
rem Run in the folder with vrde.exe, SDL2.dll and race_inputs.txt:
rem   check_windows.bat "path\to\Virtua Racing Deluxe (USA).32x"
set /p INPUTS=<race_inputs.txt
vrde.exe -d 9000 -t 1250 %INPUTS% %1
