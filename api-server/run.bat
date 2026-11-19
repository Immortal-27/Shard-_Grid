@echo off
title ShardGrid — Network Display Server
echo.
echo  ShardGrid Network Display (OLED Replacement)
echo  =============================================
echo.

REM List available serial ports first
echo  Available serial ports:
python -c "import serial.tools.list_ports; [print(f'    {p.device:12s}  {p.description}') for p in serial.tools.list_ports.comports()] or print('    (none detected)')"
echo.

REM If a port is passed as argument, use it; otherwise auto-detect
if "%1"=="" (
    echo  No port specified — will auto-detect.
    echo  Usage: run.bat COM5
    echo.
    python server.py
) else (
    python server.py --port %1
)

pause
