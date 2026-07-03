// Localization: flat key=value tables loaded from assets/lang/<code>.lang.
// Plain stdio only (no raylib) so the test binaries can link it too.
// tr() falls back to the key itself, so a missing file or key is visible
// in-game instead of crashing.
#include "lang.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define MAX_ENTRIES 128
#define KEY_LEN 40
#define VAL_LEN 160

static char keys[MAX_ENTRIES][KEY_LEN];
static char vals[MAX_ENTRIES][VAL_LEN];
static int  n_entries = 0;
static char cur[8] = "en";

const char *lang_current(void) { return cur; }

bool lang_load(const char *code) {
  if (!code || !code[0] || strlen(code) >= sizeof(cur) || strchr(code, '/') || strchr(code, '\\') || strstr(code, ".."))
    return false;
  char path[128];
  snprintf(path, sizeof(path), "assets/lang/%s.lang", code);
  FILE *f = fopen(path, "r");
  if (!f) return false;
  n_entries = 0;
  char line[256];
  while (fgets(line, sizeof(line), f) && n_entries < MAX_ENTRIES) {
    char *nl = strpbrk(line, "\r\n");
    if (nl) *nl = 0;
    if (!line[0] || line[0] == '#') continue;
    char *eq = strchr(line, '=');
    if (!eq) continue;
    *eq = 0;
    snprintf(keys[n_entries], KEY_LEN, "%s", line);
    snprintf(vals[n_entries], VAL_LEN, "%s", eq + 1);
    n_entries++;
  }
  fclose(f);
  snprintf(cur, sizeof(cur), "%s", code);
  return true;
}

const char *tr(const char *key) {
  for (int i = 0; i < n_entries; i++)
    if (strcmp(keys[i], key) == 0) return vals[i];
  return key;
}

void lang_init(void) {
  const char *code = getenv("CRAFT_LANG");
  char saved[8] = "";
  if (!code) {
    FILE *f = fopen("lang.txt", "r");
    if (f) {
      if (fscanf(f, "%7s", saved) == 1) code = saved;
      fclose(f);
    }
  }
  if (!code || !lang_load(code)) lang_load("en");
}
