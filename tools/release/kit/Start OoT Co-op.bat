@echo off
rem The old name of "Start Squadala.bat" (the start file up to v0.11.2), kept so a desktop shortcut, a Steam
rem entry or the updater of an older kit still finds it. It only starts "Start Squadala.bat": use that one.
rem All on one line: an update may replace this file while it runs.
if exist "%~dp0Start Squadala.bat" (call "%~dp0Start Squadala.bat" %* & exit /b) else (echo "Start Squadala.bat" is missing beside this file: unzip the kit again.& pause & exit /b 1)
