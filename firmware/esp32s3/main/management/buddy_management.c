#include "buddy_management.h"
#include "esp_random.h"
#include "faults.h"
#include "s3_runtime.h"
#include <math.h>
#include <string.h>
extern size_t s3_tx_write(const uint8_t *, size_t);
#define QUEUE 12
#define LEASE_MS 10000u
static rbp_rxparser_t parser;
static rbp_txseq_t txseq;
static uint32_t session, last_rx, rxseq, last_request;
static struct {
  uint8_t bytes[RBP_MAX_ENCODED];
  size_t len, offset;
} queue[QUEUE];
static unsigned first, count;
static rbp_header_t cached_header;
static uint8_t cached_body[512], last_body[512];
static size_t cached_len, last_len;
static uint16_t last_opcode;
static bool cached, closing;
bool buddy_u32(const cJSON *obj, const char *key, uint32_t *out) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
  if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || v->valuedouble < 0 ||
      v->valuedouble > 4294967295.0 || floor(v->valuedouble) != v->valuedouble)
    return false;
  *out = (uint32_t)v->valuedouble;
  return true;
}
bool buddy_management_active(void) { return session && !closing; }
void buddy_management_close(void) {
  session = rxseq = last_request = 0;
  cached = closing = false;
  first = count = 0;
  s3_management(false);
}
void buddy_management_init(void) {
  buddy_management_close();
  rbp_rxparser_init(&parser, 0);
  rbp_txseq_init(&txseq);
}
static bool send(rbp_header_t *h, const uint8_t *body, size_t n) {
  if (count == QUEUE) {
    rbp_fault_record(RBP_FAULT_STANDALONE, 0x300, QUEUE, session);
    buddy_management_close();
    return false;
  }
  unsigned pos = (first + count) % QUEUE;
  size_t len = rbp_frame_encode(h, &txseq, body, n, queue[pos].bytes);
  if (!len)
    return false;
  /* rbp_frame_encode includes the zero terminator. */
  queue[pos].len = len;
  queue[pos].offset = 0;
  count++;
  return true;
}
void buddy_management_event(uint16_t opcode, cJSON *body) {
  if (!body)
    return;
  char text[513];
  bool ok = cJSON_PrintPreallocated(body, text, sizeof text, false);
  cJSON_Delete(body);
  if (!buddy_management_active() || !ok)
    return;
  rbp_header_t h = {
      .kind = RBP_KIND_EVENT, .session_id = session, .opcode = opcode};
  send(&h, (uint8_t *)text, strlen(text));
}
bool buddy_management_audio(cJSON *body) {
  char text[513];
  bool ok = body && cJSON_PrintPreallocated(body, text, sizeof text, false);
  cJSON_Delete(body);
  if (!ok || !buddy_management_active() || count >= QUEUE - 4) return false;
  rbp_header_t h = {.kind=RBP_KIND_EVENT,.session_id=session,.opcode=BUDDY_PROBE_AUDIO};
  return send(&h,(uint8_t *)text,strlen(text));
}
static void incoming(rbp_rx_event_ctx_t *e, void *user) {
  uint32_t now = *(uint32_t *)user;
  if (e->event != RBP_RX_FRAME)
    return;
  const rbp_header_t *q = &e->header;
  if (q->major != RBP_MAJOR || q->minor != RBP_MINOR || q->status ||
      q->kind != RBP_KIND_REQUEST || q->flags || q->connection_id ||
      !q->request_id)
    return;
  if (q->opcode == BUDDY_HELLO) {
    /* HELLO cannot steal a live lease. After loss the client opens a fresh
     * session. */
    if (session) {
      if (closing || !cached || last_opcode != BUDDY_HELLO ||
          q->request_id != last_request || q->session_id ||
          q->tx_seq != rxseq + 1 || e->payload_len != last_len ||
          memcmp(e->payload, last_body, last_len))
        return;
    } else {
      if (q->session_id || q->tx_seq != 1)
        return;
      rxseq = last_request = 0;
      cached = false;
    }
  } else if (!buddy_management_active() || q->session_id != session)
    return;
  if (q->tx_seq != rxseq + 1)
    return;
  rxseq = q->tx_seq;
  last_rx = now;
  if (q->request_id == last_request && cached) {
    if (q->opcode == last_opcode && e->payload_len == last_len &&
        !memcmp(e->payload, last_body, last_len))
      send(&cached_header, cached_body, cached_len);
    else
      buddy_management_close();
    return;
  }
  if (q->request_id <= last_request)
    return;
  cJSON *body = NULL;
  const char *end = NULL;
  if (e->payload_len && e->payload_len <= 512) {
    char raw[513];
    memcpy(raw, e->payload, e->payload_len);
    raw[e->payload_len] = 0;
    bool quoted = false, escaped = false, shallow = true;
    unsigned depth = 0;
    for (unsigned n = 0; n < e->payload_len; n++) {
      char c = raw[n];
      if (quoted) {
        if (escaped)
          escaped = false;
        else if (c == '\\')
          escaped = true;
        else if (c == '"')
          quoted = false;
      } else if (c == '"')
        quoted = true;
      else if (c == '{' || c == '[') {
        if (++depth > 4)
          shallow = false;
      } else if ((c == '}' || c == ']') && depth)
        depth--;
    }
    if (shallow)
      body = cJSON_ParseWithLengthOpts(raw, e->payload_len + 1, &end, 1);
    if (end != raw + e->payload_len) {
      cJSON_Delete(body);
      body = NULL;
    }
  }
  if (cJSON_IsObject(body)) {
    for (cJSON *a = body->child; a; a = a->next)
      for (cJSON *b = a->next; b; b = b->next)
        if (a->string && b->string && !strcmp(a->string, b->string)) {
          cJSON_Delete(body);
          body = NULL;
          goto validated;
        }
  }
validated:;
  cJSON *response = cJSON_CreateObject();
  if (!response) {
    buddy_management_close();
    return;
  }
  uint16_t status = RBP_STATUS_OK;
  if (!cJSON_IsObject(body))
    status = RBP_STATUS_INVALID_ARGUMENT;
  else if (q->opcode == BUDDY_HELLO) {
    uint32_t api;
    if (!buddy_u32(body, "api", &api) || api != 1)
      status = RBP_STATUS_VERSION_MISMATCH;
    else {
      session = esp_random() | 1;
      rbp_txseq_init(&txseq);
      s3_management(true);
      cJSON_AddNumberToObject(response, "api", 1);
      cJSON_AddNumberToObject(response, "lease_ms", LEASE_MS);
      cJSON_AddNumberToObject(response, "slots", 4);
    }
  } else if (q->opcode == BUDDY_PING) {
  } else if (q->opcode == BUDDY_CLOSE) {
    closing = true;
    s3_management(false);
  } else
    status = buddy_command(q->opcode, body, response);
  cJSON_Delete(body);
  char encoded[513];
  if (!cJSON_PrintPreallocated(response, encoded, sizeof encoded, false)) {
    strcpy(encoded, "{}");
    status = RBP_STATUS_RESOURCE_LIMIT;
  }
  cJSON_Delete(response);
  rbp_header_t h = {.kind = RBP_KIND_RESPONSE,
                    .session_id = session,
                    .request_id = q->request_id,
                    .opcode = q->opcode,
                    .status = status};
  last_request = q->request_id;
  last_opcode = q->opcode;
  last_len = e->payload_len;
  memcpy(last_body, e->payload, last_len);
  cached_header = h;
  cached_len = strlen(encoded);
  memcpy(cached_body, encoded, cached_len);
  cached = true;
  send(&h, cached_body, cached_len);
}
void buddy_management_rx(const uint8_t *p, size_t n, uint32_t now) {
  if (session && now - last_rx >= LEASE_MS)
    buddy_management_close();
  rbp_rx_event_ctx_t e;
  rbp_rx_drain(&parser, p, n, now, &e, incoming, &now);
}
void buddy_management_tick(uint32_t now) {
  if (session && now - last_rx >= LEASE_MS) {
    buddy_management_close();
    return;
  }
  /* Drain by bytes, not one frame per callback. Keep BLE task work bounded
   * and preserve FIFO ordering, including a partially written frame. */
  size_t budget = 2048;
  while (count && budget) {
    unsigned pos = first;
    size_t remaining = queue[pos].len - queue[pos].offset;
    if (remaining > budget) remaining = budget;
    size_t n = s3_tx_write(queue[pos].bytes + queue[pos].offset,
                           remaining);
    if (!n) break;
    budget -= n;
    queue[pos].offset += n;
    if (queue[pos].offset == queue[pos].len) {
      first = (first + 1) % QUEUE;
      count--;
    }
  }
  if (closing && !count)
    buddy_management_close();
}
