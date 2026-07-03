@echo off
rem One-shot build from a fresh checkout.
rem   build.cmd          native client + test exes -> ..\craft_raylib_build\native
rem   build.cmd web      wasm build (needs emsdk)  -> ..\craft_raylib_build\web
rem First configure downloads raylib 5.5 via CMake FetchContent (needs git + network).
setlocal enabledelayedexpansion
cd /d "%~dp0"

rem --- locate gcc/mingw32-make: PATH, then winget Links, then the WinLibs package dir ---
where gcc >nul 2>nul
if errorlevel 1 (
  if exist "%LOCALAPPDATA%\Microsoft\WinGet\Links\gcc.exe" (
    set "PATH=%LOCALAPPDATA%\Microsoft\WinGet\Links;!PATH!"
  ) else (
    for /d %%i in ("%LOCALAPPDATA%\Microsoft\WinGet\Packages\BrechtSanders.WinLibs*") do (
      if exist "%%i\mingw64\bin\gcc.exe" set "PATH=%%i\mingw64\bin;!PATH!"
    )
  )
)
where gcc >nul 2>nul
if errorlevel 1 (
  echo gcc not found. Install the toolchain first:
  echo   winget install BrechtSanders.WinLibs.POSIX.UCRT
  echo   winget install Kitware.CMake
  exit /b 1
)

if /i "%~1"=="web" goto :web

rem configure only once: cmake --build re-runs it automatically when
rem CMakeLists.txt changes, so skipping it here costs nothing
if not exist "..\craft_raylib_build\native\CMakeCache.txt" (
  cmake -S . -B ..\craft_raylib_build\native -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release || exit /b 1
)
cmake --build ..\craft_raylib_build\native -j || exit /b 1
echo.
echo Done. Run ..\craft_raylib_build\native\craft.exe  (server: bun server.js)
exit /b 0

:web
rem emsdk: EMSDK env var if set, otherwise the ..\emsdk sibling checkout
if defined EMSDK (
  call "%EMSDK%\emsdk_env.bat" >nul 2>nul
) else if exist "..\emsdk\emsdk_env.bat" (
  call "..\emsdk\emsdk_env.bat" >nul 2>nul
)
where emcmake >nul 2>nul
if errorlevel 1 (
  echo emsdk not found. Install it next to this project:
  echo   git clone https://github.com/emscripten-core/emsdk ..\emsdk
  echo   ..\emsdk\emsdk install latest ^&^& ..\emsdk\emsdk activate latest
  exit /b 1
)
if not exist "..\craft_raylib_build\web\CMakeCache.txt" (
  call emcmake cmake -S . -B ..\craft_raylib_build\web -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release -DPLATFORM=Web || exit /b 1
)
cmake --build ..\craft_raylib_build\web -j || exit /b 1
echo.
echo Done. Run serve.cmd and open http://localhost:8080/
exit /b 0
