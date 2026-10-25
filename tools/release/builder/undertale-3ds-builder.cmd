@echo off
rem Starts the builder window. A release zip carries python.org's embeddable Python in runtime\; a source checkout
rem uses the installed Python launcher.
setlocal
set "HERE=%~dp0"
if exist "%HERE%runtime\pythonw.exe" (
    start "" "%HERE%runtime\pythonw.exe" "%HERE%builder.py" --game undertale %*
) else (
    start "" pyw -3 "%HERE%builder.py" --game undertale %*
)
