@echo off
rem Package the game for sharing: ..\craft_raylib_build\craft_publish.zip
rem One exe does everything: play native, or run start_server.cmd to host
rem (craft.exe --server serves the web client + world sync on port 8080).
rem No bun/node needed anywhere.
setlocal
cd /d "%~dp0"

set "BUILD=%~dp0..\craft_raylib_build"
set "STAGE=%BUILD%\publish"
set "ZIP=%BUILD%\craft_publish.zip"

rem Always rebuild both targets so the zip can't ship a stale build.
call "%~dp0build.cmd" || exit /b 1
call "%~dp0build.cmd" web || exit /b 1

if exist "%STAGE%" rmdir /s /q "%STAGE%" 2>nul
if exist "%STAGE%\craft.exe" (
  echo Cannot clean %STAGE% - close the running craft.exe first.
  exit /b 1
)
mkdir "%STAGE%\web" || exit /b 1

xcopy /e /i /q "%BUILD%\native\assets" "%STAGE%\assets" >nul || exit /b 1
copy /y "%BUILD%\native\craft.exe" "%STAGE%\" >nul || exit /b 1
copy /y "%BUILD%\web\craft.html" "%STAGE%\web\" >nul || exit /b 1
copy /y "%BUILD%\web\craft.js"   "%STAGE%\web\" >nul || exit /b 1
copy /y "%BUILD%\web\craft.wasm" "%STAGE%\web\" >nul || exit /b 1
copy /y "%BUILD%\web\craft.data" "%STAGE%\web\" >nul || exit /b 1

(
  echo @echo off
  echo cd /d "%%~dp0"
  echo set "STATIC=%%~dp0web"
  echo craft.exe --server
) > "%STAGE%\start_server.cmd"

(
  echo Craft Survival - game + server in one exe
  echo.
  echo PLAY:  run craft.exe          ^(joins localhost, or: craft.exe --host SERVER-IP^)
  echo HOST:  run start_server.cmd   ^(allow it through the Windows firewall when asked^)
  echo        then everyone opens http://YOUR-IP:8080/ in a browser
  echo        ^(find YOUR-IP with: ipconfig^)
  echo BOTH:  craft.exe --serve      ^(play and host at the same time^)
  echo.
  echo Must be plain http, not https - the game uses ws:// on the same port.
  echo The world saves itself next to the exe every 10 seconds
  echo ^(world.edits, players.json, world.meta.json^).
) > "%STAGE%\README.txt"

if exist "%ZIP%" del "%ZIP%" 2>nul
if exist "%ZIP%" (
  echo Cannot overwrite %ZIP% - close whatever has it open.
  exit /b 1
)
tar -a -c -f "%ZIP%" -C "%STAGE%" . || exit /b 1

echo.
echo Published: %ZIP%
for %%A in ("%ZIP%") do echo Size: %%~zA bytes
exit /b 0
