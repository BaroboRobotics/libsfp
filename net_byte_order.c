#include "net_byte_order.h"

#define BYTESWAP(X) \
    ({ \
        __typeof__(X) result; \
        uint8_t *presult = (uint8_t *)&result; \
        for (int i = sizeof(X) - 1; i >= 0; --i) { \
            presult[i] = (X) & 0xff; \
            (X) >>= 8; \
        } \
        result; \
     })

uint16_t netByteOrder16 (uint16_t value) {
    return BYTESWAP(value);
}

uint32_t netByteOrder32 (uint32_t value) {
    return BYTESWAP(value);
}

uint64_t netByteOrder64 (uint64_t value) {
    return BYTESWAP(value);
}
