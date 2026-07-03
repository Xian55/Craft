// WebSocket transport for the Emscripten build — the browser does the
// protocol; we just queue incoming text messages. Link with -lwebsocket.js.
#include "net.h"
#include <emscripten/websocket.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static EMSCRIPTEN_WEBSOCKET_T ws = 0;
static bool open_ = false;

#define QCAP 256
typedef struct QMsg { uint8_t *p; size_t n; } QMsg;
static QMsg queue[QCAP];
static int qhead = 0, qtail = 0;

static void q_push(uint8_t *p, size_t n) {
  int next = (qtail + 1) % QCAP;
  if (next == qhead) { free(p); return; }
  queue[qtail] = (QMsg){ p, n }; qtail = next;
}

const uint8_t *ws_poll(size_t *len) {
  static uint8_t *last = NULL;
  if (last) { free(last); last = NULL; }
  if (qhead == qtail) return NULL;
  last = queue[qhead].p; *len = queue[qhead].n;
  qhead = (qhead + 1) % QCAP;
  return last;
}

static EM_BOOL on_open(int t, const EmscriptenWebSocketOpenEvent *e, void *ud) {
  (void)t; (void)e; (void)ud;
  open_ = true;
  return EM_TRUE;
}
static EM_BOOL on_close(int t, const EmscriptenWebSocketCloseEvent *e, void *ud) {
  (void)t; (void)e; (void)ud;
  open_ = false;
  return EM_TRUE;
}
static EM_BOOL on_error(int t, const EmscriptenWebSocketErrorEvent *e, void *ud) {
  (void)t; (void)e; (void)ud;
  open_ = false;
  return EM_TRUE;
}
static EM_BOOL on_message(int t, const EmscriptenWebSocketMessageEvent *e, void *ud) {
  (void)t; (void)ud;
  if (!e->isText) {
    // binary: numBytes is the exact payload length (no NUL terminator)
    uint8_t *s = malloc(e->numBytes);
    memcpy(s, e->data, e->numBytes);
    q_push(s, e->numBytes);
  }
  return EM_TRUE;
}

bool ws_connect(const char *host, int port, const char *path) {
  if (!emscripten_websocket_is_supported()) return false;
  if (ws > 0) ws_close();          // drop any stale/CONNECTING handle first
  char url[256];
  snprintf(url, sizeof url, "ws://%s:%d%s", host, port, path);
  EmscriptenWebSocketCreateAttributes attr = { url, NULL, EM_TRUE };
  ws = emscripten_websocket_new(&attr);
  if (ws <= 0) return false;
  emscripten_websocket_set_onopen_callback(ws, NULL, on_open);
  emscripten_websocket_set_onclose_callback(ws, NULL, on_close);
  emscripten_websocket_set_onerror_callback(ws, NULL, on_error);
  emscripten_websocket_set_onmessage_callback(ws, NULL, on_message);
  return true;
}

bool ws_is_open(void) { return open_; }

void ws_close(void) {
  if (ws > 0) {
    emscripten_websocket_close(ws, 1000, "");
    emscripten_websocket_delete(ws);   // reconnect creates a fresh handle
    ws = 0;
  }
  open_ = false;
}

void ws_send_bin(const void *buf, size_t len) {
  if (open_ && ws > 0) emscripten_websocket_send_binary(ws, (void *)buf, (uint32_t)len);
}

void ws_pump(void) { /* browser event loop delivers callbacks */ }
