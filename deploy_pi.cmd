@echo off
rem Deploy the server (native binary, --server mode) + web client as a
rem docker container to the host in .env.
rem   deploy_pi.cmd [user@host]     optional override of PI_HOST
rem Config comes from .env (gitignored) — copy .env.example to .env first.
rem Ships the C sources + web build over ssh; the image builds on the
rem target (raylib is cached in a BuildKit mount, so redeploys are quick).
rem The world lives in the craft_world docker volume and survives redeploys.
setlocal enabledelayedexpansion
cd /d "%~dp0"

if exist ".env" goto :haveenv
echo No .env found. Create one:  copy .env.example .env  - then edit it.
exit /b 1
:haveenv
for /f "usebackq eol=# tokens=1,* delims==" %%a in (".env") do set "%%a=%%b"
if "%~1" neq "" set "PI_HOST=%~1"
if not defined PI_HOST ( echo PI_HOST missing from .env & exit /b 1 )
if not defined PI_DIR  set "PI_DIR=/root/craft"
if not defined PI_PORT set "PI_PORT=8080"

call "%~dp0build.cmd" web || exit /b 1

ssh %PI_HOST% "mkdir -p %PI_DIR%/web" || exit /b 1
scp -r CMakeLists.txt Dockerfile src tests assets %PI_HOST%:%PI_DIR%/ || exit /b 1
scp ..\craft_raylib_build\web\craft.html ..\craft_raylib_build\web\craft.js ^
    ..\craft_raylib_build\web\craft.wasm ..\craft_raylib_build\web\craft.data ^
    %PI_HOST%:%PI_DIR%/web/ || exit /b 1

ssh %PI_HOST% "cd %PI_DIR% && docker build -t craft-server . && (docker rm -f craft 2>/dev/null || true) && docker run -d --name craft --restart unless-stopped -p %PI_PORT%:8080 -v craft_world:/data craft-server" || exit /b 1

rem host part of user@host, for the URL hint
for /f "tokens=2 delims=@" %%h in ("%PI_HOST%") do set "PI_ADDR=%%h"
if not defined PI_ADDR set "PI_ADDR=%PI_HOST%"
echo.
echo Deployed. Game: http://%PI_ADDR%:%PI_PORT%/
exit /b 0
