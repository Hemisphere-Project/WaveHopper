// Framework bug workaround, linked in via -Wl,--wrap=dns_clear_cache
// (platformio.ini).
//
// Arduino-ESP32's NetworkManager::hostByName() calls lwIP's dns_clear_cache()
// straight from the caller's task whenever the interface IP state changed
// (wifi dropped / came back) — without the TCPIP core lock. If another task
// has a DNS query in flight at that moment, the clear tears down its UDP pcb
// and lwIP's thread-safety check aborts:
//   assert failed: udp_remove udp.c:1199 (Required to lock TCPIP core
//   functionality!)  ← dns_call_found ← dns_clear_cache ← hostByName
// Found by scripts/fuzz.py (wifi drop + concurrent lookups). Every lookup in
// this firmware goes through hostByName (audio lib connects, HTTPClient, our
// prewarm), so the fix belongs at the one unsafe call, not at call sites.

#include <lwip/dns.h>
#include <lwip/tcpip.h>

extern "C" {

void __real_dns_clear_cache(void);

void __wrap_dns_clear_cache(void) {
  if (sys_thread_tcpip(LWIP_CORE_LOCK_QUERY_HOLDER)) {  // tcpip thread / lock held
    __real_dns_clear_cache();
    return;
  }
  LOCK_TCPIP_CORE();
  __real_dns_clear_cache();
  UNLOCK_TCPIP_CORE();
}

}  // extern "C"
