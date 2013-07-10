#include "../static_assert.h"

#include <stdint.h>

int main () {
    /* These first two should cause a compile-time error. */
    STATIC_ASSERT(sizeof(uint8_t) == 2, "uint8_t is not two bytes");
    STATIC_ASSERT2(sizeof(uint8_t) == 2);

    /* These should caues no error. */
    STATIC_ASSERT(sizeof(uint16_t) == 2, "uint16_t is not two bytes");
    STATIC_ASSERT2(sizeof(uint16_t) == 2);
}
