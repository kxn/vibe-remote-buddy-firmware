#pragma once
#include <stdbool.h>
#include <stdint.h>
/* Diagnostic headers only: no audio payload, allocation, or logging in RX. */
typedef struct {
  uint32_t ordinal, time_us, hash;
  uint16_t connection, attribute, sequence, part, length;
  uint8_t layer;
} hci_probe_entry_t;
#ifdef S3_HCI_PROBE
void hci_probe_app(uint16_t connection, uint16_t attribute,
                   const uint8_t *data, unsigned length);
void hci_probe_freeze(void);
void hci_probe_reset(void);
void hci_probe_status(uint32_t out[5]);
bool hci_probe_get(unsigned offset, hci_probe_entry_t *entry);
#else
static inline void hci_probe_app(uint16_t c, uint16_t a, const uint8_t *d, unsigned n) {
  (void)c; (void)a; (void)d; (void)n;
}
static inline void hci_probe_freeze(void) {}
static inline void hci_probe_reset(void) {}
static inline void hci_probe_status(uint32_t out[5]) {
  for (unsigned i=0;i<5;i++)out[i]=0;
}
static inline bool hci_probe_get(unsigned n, hci_probe_entry_t *e) {
  (void)n; (void)e; return false;
}
#endif
