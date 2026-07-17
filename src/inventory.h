// Inventory, items, crafting, chests — port of game.js:1697-1810.
#ifndef INVENTORY_H
#define INVENTORY_H

#include <stdbool.h>
#include <stdint.h>

// Item ids (tools/items live above the block range, game.js:36-43)
#define I_SHOVEL       20
#define I_PICKAXE      21
#define I_SWORD        22
#define I_BUCKET       23
#define I_WATER_BUCKET 24
#define I_PORK         25
#define I_STICK        26
#define I_LAVA_BUCKET  27
#define I_COAL         28
#define I_IRON_INGOT   29
#define I_COOKED_PORK  30
#define I_IRON_PICKAXE 31
#define I_IRON_SWORD   32
#define I_IRON_SHOVEL  33

#define HOTBAR_SIZE 9
#define INV_SIZE    36
#define STACK_MAX   64
#define CHEST_SIZE  27

typedef struct Stack { int id, count; } Stack;   // id 0 = empty slot

extern Stack slots[INV_SIZE];
extern int   sel_slot;
extern Stack craft_grid[9];
extern Stack cursor_stack;                        // carried by the mouse in panels

bool is_tool(int id);
bool is_block(int id);
int  stack_max(int id);
int  item_count(int id);
void add_item(int id, int n);
bool remove_item(int id, int n);
int  drop_of(int block);                          // what a broken block drops

const char *item_name(int id);                    // ASCII-folded Hungarian name
void item_color(int id, uint8_t rgb[3]);          // block swatch color
int  item_tile(int id);                           // tile in items.png, -1 if none

// crafting
typedef struct Recipe {
  int shape[3][3];       // 0 = empty; used when h>0
  int h, w;              // shape size; 0,0 = shapeless
  int shapeless[4], n_shapeless;
  int out_id, out_n;
} Recipe;
extern const Recipe RECIPES[];
extern const int N_RECIPES;

bool match_recipe(int *out_id, int *out_n);       // against craft_grid
void take_result(void);                           // consume grid -> cursor_stack

// chests: world cell -> 27 slots
Stack *chest_array_at(int x, int y, int z);       // creates if missing
Stack *chest_array_peek(int x, int y, int z);     // NULL if none
void   chest_delete(int x, int y, int z);

#endif
