@echo off
setlocal
echo === PDF Viewer (C++) build ===

where g++ >nul 2>&1
if %ERRORLEVEL%==0 (
    echo [g++] found...
    g++ -std=c++17 -O2 -Wall -Wextra ^
        main.cpp pdf.cpp render.cpp ^
        -lgdi32 -luser32 -lcomdlg32 -lshell32 -lm ^
        -mwindows ^
        -o pdfview.exe
    if %ERRORLEVEL%==0 ( echo [OK] pdfview.exe & goto :done )
    echo [FAIL] g++ build failed.
)

where cl >nul 2>&1
if %ERRORLEVEL%==0 (
    echo [cl] found...
    cl /nologo /O2 /W3 /std:c++17 ^
        main.cpp pdf.cpp render.cpp ^
        /Fe:pdfview.exe ^
        /link user32.lib gdi32.lib comdlg32.lib shell32.lib ^
        /subsystem:windows /entry:WinMain
    if %ERRORLEVEL%==0 ( echo [OK] pdfview.exe & goto :done )
    echo [FAIL] cl build failed.
)

echo [ERROR] No compiler found (need g++ or cl with C++17).
exit /b 1

:done
echo.
echo Keys: Left/Right=page  +/-=zoom  F=fit  Ctrl+O=open  Ctrl+Wheel=zoom
