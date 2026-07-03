// Minimal WebSocket client over winsock/BSD sockets — speaks exactly what
// server.js expects: text frames, client->server MASKED, server->client
// unmasked. Mirrors the frame logic of server.js drain(), including the
// 16-bit and 64-bit length branches (join batches are ~180 KB).
#include "net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define CLOSESOCK closesocket
#else
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
typedef int sock_t;
#define INVALID_SOCKET (-1)
#define CLOSESOCK close
#endif

// Async connect state machine: everything runs on the main thread, so no call
// here may block — a synchronous connect() froze the game for seconds per
// reconnect attempt whenever the server was down.
typedef enum { WS_IDLE, WS_CONNECTING, WS_HANDSHAKE, WS_OPEN } WsState;
static WsState state = WS_IDLE;
static sock_t sock = INVALID_SOCKET;
static time_t conn_start = 0;              // connect/handshake timeout clock
#define CONNECT_TIMEOUT_S 5

// upgrade-request parameters, kept for the deferred handshake send
static char req_host[128], req_path[64];
static int req_port = 0;
static char hshake[2048]; static size_t hshake_len = 0;   // 101-response buffer

// getaddrinfo candidates: "localhost" resolves ::1 FIRST on Windows, but the
// server may be IPv4-only — walk the whole list instead of only result #0.
static struct addrinfo *addr_list = NULL, *addr_cur = NULL;

// receive buffer (grows to hold partial frames)
static uint8_t *rbuf = NULL;
static size_t rlen = 0, rcap = 0;

// queue of decoded binary messages (length-carrying — no NUL games)
#define QCAP 256
typedef struct QMsg { uint8_t *p; size_t n; } QMsg;
static QMsg queue[QCAP];
static int qhead = 0, qtail = 0;

static void q_push(uint8_t *p, size_t n) {
  int next = (qtail + 1) % QCAP;
  if (next == qhead) { free(p); return; }   // full: drop (shouldn't happen)
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

static void set_nonblocking(sock_t s) {
#ifdef _WIN32
  u_long on = 1; ioctlsocket(s, FIONBIO, &on);
#else
  fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
}

static bool send_all(const uint8_t *p, size_t n) {
  while (n > 0) {
    int w = send(sock, (const char *)p, (int)n, 0);
    if (w <= 0) {
#ifdef _WIN32
      if (WSAGetLastError() == WSAEWOULDBLOCK) continue;
#else
      if (errno == EWOULDBLOCK || errno == EAGAIN) continue;
#endif
      return false;
    }
    p += w; n -= (size_t)w;
  }
  return true;
}

static void addrs_free(void) {
  if (addr_list) { freeaddrinfo(addr_list); addr_list = NULL; addr_cur = NULL; }
}

static void fail_socket(void) {
  if (sock != INVALID_SOCKET) { CLOSESOCK(sock); sock = INVALID_SOCKET; }
  addrs_free();
  state = WS_IDLE;
}

// start a non-blocking connect to addr_cur; false = candidate failed instantly
static bool try_current_addr(void) {
  sock = socket(addr_cur->ai_family, addr_cur->ai_socktype, addr_cur->ai_protocol);
  if (sock == INVALID_SOCKET) return false;
  set_nonblocking(sock);
  int rc = connect(sock, addr_cur->ai_addr, (int)addr_cur->ai_addrlen);
  if (rc != 0) {
#ifdef _WIN32
    if (WSAGetLastError() != WSAEWOULDBLOCK) { CLOSESOCK(sock); sock = INVALID_SOCKET; return false; }
#else
    if (errno != EINPROGRESS) { CLOSESOCK(sock); sock = INVALID_SOCKET; return false; }
#endif
  }
  conn_start = time(NULL);
  hshake_len = 0;
  state = (rc == 0) ? WS_HANDSHAKE : WS_CONNECTING;   // loopback may connect instantly
  return true;
}

// current candidate failed: move to the next one, or give up
static void next_addr_or_fail(void) {
  if (sock != INVALID_SOCKET) { CLOSESOCK(sock); sock = INVALID_SOCKET; }
  while (addr_cur && (addr_cur = addr_cur->ai_next) != NULL)
    if (try_current_addr()) return;
  addrs_free();
  state = WS_IDLE;
}

bool ws_connect(const char *host, int port, const char *path) {
  // an attempt is already in flight: keep it, never stack parallel sockets
  if (state == WS_CONNECTING || state == WS_HANDSHAKE || state == WS_OPEN) return true;
#ifdef _WIN32
  WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
  snprintf(req_host, sizeof req_host, "%s", host);
  snprintf(req_path, sizeof req_path, "%s", path);
  req_port = port;

  char portstr[16]; snprintf(portstr, sizeof portstr, "%d", port);
  struct addrinfo hints = {0};
  hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
  addrs_free();
  if (getaddrinfo(host, portstr, &hints, &addr_list) != 0 || !addr_list) { addr_list = NULL; return false; }
  addr_cur = addr_list;
  if (!try_current_addr()) {
    next_addr_or_fail();
    if (state == WS_IDLE) return false;
  }
  return true;
}

// HTTP upgrade request. Key: 16 random bytes base64 (server doesn't validate content).
static bool send_upgrade(void) {
  srand((unsigned)time(NULL));
  uint8_t kb[16]; for (int i = 0; i < 16; i++) kb[i] = (uint8_t)(rand() & 0xff);
  static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  char key[25]; int o = 0;
  for (int i = 0; i < 15; i += 3) {
    uint32_t v = (kb[i] << 16) | (kb[i+1] << 8) | kb[i+2];
    key[o++] = b64[(v >> 18) & 63]; key[o++] = b64[(v >> 12) & 63];
    key[o++] = b64[(v >> 6) & 63];  key[o++] = b64[v & 63];
  }
  uint32_t v = kb[15] << 16;
  key[o++] = b64[(v >> 18) & 63]; key[o++] = b64[(v >> 12) & 63];
  key[o++] = '='; key[o++] = '='; key[o] = 0;

  char req[512];
  int n = snprintf(req, sizeof req,
    "GET %s HTTP/1.1\r\nHost: %s:%d\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
    "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n",
    req_path, req_host, req_port, key);
  return send_all((const uint8_t *)req, (size_t)n);
}

// CONNECTING: poll the TCP handshake without blocking (zero-timeout select).
// A failed candidate falls through to the next resolved address (v6 -> v4).
static void pump_connecting(void) {
  fd_set wr, ex;
  FD_ZERO(&wr); FD_ZERO(&ex);
  FD_SET(sock, &wr); FD_SET(sock, &ex);
  struct timeval tv = { 0, 0 };
  int r = select((int)sock + 1, NULL, &wr, &ex, &tv);
  if (r < 0 || FD_ISSET(sock, &ex)) { next_addr_or_fail(); return; }
  if (r == 0 || !FD_ISSET(sock, &wr)) {
    if (time(NULL) - conn_start >= CONNECT_TIMEOUT_S) next_addr_or_fail();
    return;                              // still connecting
  }
  int err = 0; socklen_t el = sizeof err;
  if (getsockopt(sock, SOL_SOCKET, SO_ERROR, (char *)&err, &el) != 0 || err != 0) { next_addr_or_fail(); return; }
  addrs_free();                          // connected: candidates no longer needed
  if (!send_upgrade()) { fail_socket(); return; }
  state = WS_HANDSHAKE;
}

// HANDSHAKE: collect the 101 response headers, still without blocking.
static void pump_handshake(void) {
  for (;;) {
    int r = recv(sock, hshake + hshake_len, (int)(sizeof hshake - 1 - hshake_len), 0);
    if (r > 0) { hshake_len += (size_t)r; hshake[hshake_len] = 0; continue; }
    if (r == 0) { fail_socket(); return; }             // closed before 101
#ifdef _WIN32
    if (WSAGetLastError() != WSAEWOULDBLOCK) { fail_socket(); return; }
#else
    if (errno != EWOULDBLOCK && errno != EAGAIN) { fail_socket(); return; }
#endif
    break;                                             // no more data right now
  }
  char *end = strstr(hshake, "\r\n\r\n");
  if (!end) {
    if (hshake_len >= sizeof hshake - 1 || time(NULL) - conn_start >= CONNECT_TIMEOUT_S) fail_socket();
    return;
  }
  if (!strstr(hshake, " 101 ")) { fail_socket(); return; }
  // anything after the headers is already frame data
  size_t body = hshake_len - (size_t)(end + 4 - hshake);
  rlen = 0;
  if (body > 0) {
    if (rcap < body) { rcap = body < 4096 ? 4096 : body * 2; rbuf = realloc(rbuf, rcap); }
    memcpy(rbuf, end + 4, body); rlen = body;
  }
  addrs_free();
  state = WS_OPEN;
}

bool ws_is_open(void) { return state == WS_OPEN; }

void ws_close(void) {
  if (sock != INVALID_SOCKET) { CLOSESOCK(sock); sock = INVALID_SOCKET; }
  addrs_free();
  state = WS_IDLE;
}

void ws_send_bin(const void *buf, size_t len) {
  if (state != WS_OPEN) return;
  const uint8_t *src = buf;
  uint8_t hdr[14]; size_t hn = 0;
  hdr[0] = 0x82;                             // FIN + binary
  uint8_t mask[4];
  for (int i = 0; i < 4; i++) mask[i] = (uint8_t)(rand() & 0xff);
  if (len < 126) { hdr[1] = 0x80 | (uint8_t)len; hn = 2; }
  else if (len < 65536) {
    hdr[1] = 0x80 | 126; hdr[2] = (uint8_t)(len >> 8); hdr[3] = (uint8_t)len; hn = 4;
  } else {
    hdr[1] = 0x80 | 127; uint64_t l = len;
    for (int i = 0; i < 8; i++) hdr[2 + i] = (uint8_t)(l >> (56 - 8 * i));
    hn = 10;
  }
  memcpy(hdr + hn, mask, 4); hn += 4;
  uint8_t *body = malloc(len);
  for (size_t i = 0; i < len; i++) body[i] = src[i] ^ mask[i & 3];
  bool ok = send_all(hdr, hn) && send_all(body, len);
  free(body);
  if (!ok) ws_close();
}

static void send_pong(const uint8_t *payload, size_t len) {
  if (len > 125) len = 125;
  uint8_t hdr[6]; uint8_t mask[4];
  for (int i = 0; i < 4; i++) mask[i] = (uint8_t)(rand() & 0xff);
  hdr[0] = 0x8A; hdr[1] = 0x80 | (uint8_t)len;
  memcpy(hdr + 2, mask, 4);
  uint8_t body[125];
  for (size_t i = 0; i < len; i++) body[i] = payload[i] ^ mask[i & 3];
  if (!send_all(hdr, 6) || !send_all(body, len)) ws_close();
}

// Parse complete frames from rbuf — direct port of server.js drain().
static void parse_frames(void) {
  size_t off_total = 0;
  while (rlen - off_total >= 2) {
    const uint8_t *b = rbuf + off_total;
    size_t avail = rlen - off_total;
    uint8_t opcode = b[0] & 0x0f;
    bool masked = (b[1] & 0x80) != 0;        // server frames are unmasked
    uint64_t len = b[1] & 0x7f;
    size_t off = 2;
    if (len == 126) { if (avail < 4) break; len = ((uint64_t)b[2] << 8) | b[3]; off = 4; }
    else if (len == 127) {
      if (avail < 10) break;
      len = 0; for (int i = 0; i < 8; i++) len = (len << 8) | b[2 + i];
      off = 10;
    }
    size_t mask_len = masked ? 4 : 0;
    if (avail < off + mask_len + len) break;  // incomplete frame
    const uint8_t *payload = b + off + mask_len;
    if (opcode == 0x8) { ws_close(); return; }               // close
    if (opcode == 0x9) { send_pong(payload, (size_t)len); }  // ping -> pong
    if (opcode == 0x2) {                                      // binary (v2)
      uint8_t *s = malloc((size_t)len);
      if (masked) {
        const uint8_t *mask = b + off;
        for (uint64_t i = 0; i < len; i++) s[i] = (uint8_t)(payload[i] ^ mask[i & 3]);
      } else {
        memcpy(s, payload, (size_t)len);
      }
      q_push(s, (size_t)len);
    }
    off_total += off + mask_len + (size_t)len;
  }
  if (off_total > 0) { memmove(rbuf, rbuf + off_total, rlen - off_total); rlen -= off_total; }
}

void ws_pump(void) {
  if (state == WS_CONNECTING) { pump_connecting(); return; }
  if (state == WS_HANDSHAKE)  { pump_handshake(); if (state != WS_OPEN) return; }
  if (state != WS_OPEN) return;
  for (;;) {
    if (rcap - rlen < 65536) {
      rcap = rcap ? rcap * 2 : 262144;
      rbuf = realloc(rbuf, rcap);
    }
    int r = recv(sock, (char *)rbuf + rlen, (int)(rcap - rlen), 0);
    if (r > 0) { rlen += (size_t)r; continue; }
    if (r == 0) { ws_close(); break; }        // remote closed
#ifdef _WIN32
    if (WSAGetLastError() == WSAEWOULDBLOCK) break;
#else
    if (errno == EWOULDBLOCK || errno == EAGAIN) break;
#endif
    ws_close(); break;
  }
  if (rlen >= 2) parse_frames();
}
