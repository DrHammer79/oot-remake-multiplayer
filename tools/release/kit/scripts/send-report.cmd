@echo off
rem Sending reports by themselves (docs\problem-reports.md): "send-report.cmd yes" sends the kits crash and problem
rem reports to the Squadala! developers Discord channel through the relay in report-relay.txt, "no" never (the default
rem asks once), "ask" asks again. The same as send_reports in coop-options.ini.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0send-report.ps1" %*
pause
