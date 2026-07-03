@echo off
rem Starts the game server AND serves the wasm build on one port — no bun
rem needed, the native client binary doubles as the server (--server).
rem Open http://<this-machine>:8080/ in a browser to play.
cd /d "%~dp0"
set "STATIC=%~dp0..\craft_raylib_build\web"
"..\craft_raylib_build\native\craft.exe" --server
