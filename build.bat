@echo off
set SDK=.\steamworks\sdk

if not exist "%SDK%\public\steam" (
    echo Steamworks SDK not found.
    echo Please extract it to:
    echo %SDK%
    exit /b 1
)

echo 480 > .\build\steam_appid.txt

if not exist build mkdir build

echo Copying steam_api64.lib and steam_api64.dll
copy %SDK%\redistributable_bin\win64\steam_api64.lib .\build
copy %SDK%\redistributable_bin\win64\steam_api64.dll .\build

if not exist "./build/steam_api64.lib" (
    echo Error: steam_api64.lib not found.
    echo Please copy it from SDK folder to: ./build
    exit /b 1
)

if not exist "./build/steam_api64.dll" (
    echo Error: steam_api64.dll not found.
    echo Please copy it from SDK folder to: ./build
    exit /b 1
)

if not exist "./build/steamwebrtc64.dll" (
    echo Error: steamwebrtc64.dll not found.
    echo Please copy it from steam folder to: ./build
    exit /b 1
)

cl.exe src\main.cpp /EHsc /std:c++17 /O2 /DNDEBUG /I "%SDK%\public" /utf-8 /nologo .\build\steam_api64.lib /Fe.\build\app.exe

if errorlevel 1 (
    echo Error: Build failed.
    exit /b 1
)

echo Build successful.
echo Result: .\build\app.exe
pause