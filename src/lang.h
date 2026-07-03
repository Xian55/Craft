#ifndef LANG_H
#define LANG_H

#include <stdbool.h>

// Localization: key=value files in assets/lang/<code>.lang.
// Default language is English; CRAFT_LANG env or the /lang chat command
// (persisted to lang.txt next to the exe) selects another one.
void        lang_init(void);
bool        lang_load(const char *code);
const char *lang_current(void);
const char *tr(const char *key);   // translated string, or the key if missing

#endif
