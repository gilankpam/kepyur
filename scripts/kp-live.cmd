@echo off
rem kepyur on this PC: scan, watch the strongest pilot, show the picture. Extra args go to kp_live
rem (e.g. --watch 5800 to skip the scan). Status lines go to build\windows\kp_live.log.
cd /d "%~dp0..\build\windows"
"%~dp0..\build\windows\kp_live.exe" --frames %* 2> kp_live.log | ffplay -hide_banner -loglevel error -window_title kepyur -f rawvideo -pixel_format rgb0 -video_size 720x480 -framerate 60 -fflags nobuffer -flags low_delay -i -
