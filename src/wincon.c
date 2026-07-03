// Windows console attachment for the GUI-subsystem build. Separate TU:
// windows.h clashes with raylib symbols (Rectangle, CloseWindow, DrawText),
// so this file must never include raylib headers.
#if defined(_WIN32) && defined(CRAFT_GUI_SUBSYSTEM)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

// GUI subsystem = no terminal window when double-clicked. But when launched
// FROM a terminal (start_server.cmd, CI, dev shells) attach to it so printf
// still lands somewhere; --console forces a fresh one for debugging.
void win_console(int argc, char **argv) {
  bool force = false;
  for (int i = 1; i < argc; i++)
    if (strcmp(argv[i], "--console") == 0) force = true;
  if (AttachConsole(ATTACH_PARENT_PROCESS) || (force && AllocConsole())) {
    // rebind only streams with no real handle — piped/redirected stdio
    // (e2e, CI, `craft --version | ...`) must keep flowing into the pipe
    if (GetFileType(GetStdHandle(STD_OUTPUT_HANDLE)) == FILE_TYPE_UNKNOWN) freopen("CONOUT$", "w", stdout);
    if (GetFileType(GetStdHandle(STD_ERROR_HANDLE)) == FILE_TYPE_UNKNOWN) freopen("CONOUT$", "w", stderr);
    if (GetFileType(GetStdHandle(STD_INPUT_HANDLE)) == FILE_TYPE_UNKNOWN) freopen("CONIN$", "r", stdin);
  }
}
#else
void win_console(int argc, char **argv) { (void)argc; (void)argv; }
#endif
