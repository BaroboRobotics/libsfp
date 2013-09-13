#include "net_byte_order.h"

/* 
 * Functions that swap integers between host byte order and network byte
 * order.
 *
 * If the host byte order is little-endian, a call to netByteOrderXX reorders
 * the bytes of an integer like so:
 *
 * [ A B ... Z ] -> [ Z Y ... A]
 *
 * If the host byte order is big-endian, a call to netByteOrderXX is a no-op.
 */

#define BYTESWAP(OUT, IN) \
    do { \
        uint8_t *presult = (uint8_t *)&(OUT); \
        int i; \
        for (i = sizeof(IN) - 1; i >= 0; --i) { \
            presult[i] = (IN) & 0xff; \
            (IN) >>= 8; \
        } \
     } while (0)

uint16_t netByteOrder16 (uint16_t value) {
  uint16_t ret;
  BYTESWAP(ret, value);
  return ret;
}

uint32_t netByteOrder32 (uint32_t value) {
  uint32_t ret;
  BYTESWAP(ret, value);
  return ret;
}

uint64_t netByteOrder64 (uint64_t value) {
  uint64_t ret;
  BYTESWAP(ret, value);
  return ret;
}
