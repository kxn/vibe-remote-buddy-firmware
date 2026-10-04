#include "host_os.h"
#include <string.h>
void buddy_host_reset(buddy_host_probe_t *p) { memset(p, 0, sizeof *p); }
void buddy_host_setup(buddy_host_probe_t *p, uint8_t type, uint8_t request, uint16_t value, uint16_t length, uint32_t now) {
  /* Only standard device-to-host STRING descriptors, never class/audio traffic. */
  if (p->frozen || type != 0x80 || request != 6 || (value >> 8) != 3) return;
  if (p->strings == UINT16_MAX) return;
  p->strings++; p->last = now;
  if (length == 2) p->short2++;
  if (length == 4) p->short4++;
  if (length == 255) p->full255++;
}
buddy_host_os_t buddy_host_classify(buddy_host_probe_t *p, uint32_t now, bool configured) {
  if (p->frozen) return p->result;
  if (!configured || !p->strings || (uint32_t)(now-p->last) < 300) return BUDDY_HOST_UNKNOWN;
  /* Independent conservative classifier of enumeration behavior documented by QMK.
   * Ambiguous sequences never select Mac. No enumeration or identity modification. */
  if (p->short4 && p->full255 >= 2) p->result = BUDDY_HOST_WINDOWS;
  /* macOS also enumerates this composite device using only header/size reads
   * (observed: 13 strings, six 2-byte reads, no 4/255-byte reads). The final
   * 255-byte probe is optional. Without it require at least three headers and
   * as many other reads, so headers alone or sparse evidence remain OTHER. */
  else if (p->strings >= 5 && p->short2 >= 2 && !p->short4 &&
           (p->full255 || (p->short2 >= 3 && p->strings >= 2u * p->short2)))
    p->result = BUDDY_HOST_MACOS;
  /* Linux usb_string_sub first requests 255 bytes. Require repeated, unmixed
   * full-length reads; ambiguous/quirked enumeration remains OTHER. */
  else if (p->strings >= 3 && p->strings == p->full255) p->result = BUDDY_HOST_LINUX;
  else p->result = BUDDY_HOST_OTHER;
  p->frozen = p->result == BUDDY_HOST_WINDOWS || p->result == BUDDY_HOST_MACOS || p->result == BUDDY_HOST_LINUX;
  return p->result;
}
