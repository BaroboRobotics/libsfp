#include "../net_byte_order.h"

#include <stdio.h>

int main () {
    uint16_t x16 = 0x9123;
    uint32_t x32 = 0x91234567;
    uint64_t x64 = 0x9123456789abcdef;

    printf("%x -> %x\n", x16, netByteOrder16(x16));
    printf("%x -> %x\n", x32, netByteOrder32(x32));
    printf("%lx -> %lx\n", x64, netByteOrder64(x64));
}
