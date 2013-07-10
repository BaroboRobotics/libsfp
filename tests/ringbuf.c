#include "../ringbuf.h"

#include <stdio.h>

int main () {
    int i = 0;

    // The following should cause a compile-time error 
#if 0
    RINGBUF(char, 5) fux;
    RINGBUF_INIT(fux);
#endif

    // another compile-time error
#if 0
    RINGBUF(int, 0) r;
    RINGBUF_INIT(r);
    RINGBUF_PUSH_BACK(r, 10);
#endif

    RINGBUF(int, (1<<4)) buf;
    RINGBUF_INIT(buf);

    while (!RINGBUF_FULL(buf)) {
        printf("(%d) pushing back %d\n", RINGBUF_SIZE(buf), i);
        RINGBUF_PUSH_BACK(buf, i++);
    }

    printf("ringbuffer full, pushing one more to test overwrite\n");
    printf("(%d) pushing back %d\n", RINGBUF_SIZE(buf), i);
    RINGBUF_PUSH_BACK(buf, i++);

    printf("RINGBUF_REVERSE_AT(3) == %d\n", RINGBUF_REVERSE_AT(buf, 3));
    printf("RINGBUF_REVERSE_AT(2) == %d\n", RINGBUF_REVERSE_AT(buf, 2));
    printf("RINGBUF_BACK  == %d\n", RINGBUF_BACK(buf));
    printf("RINGBUF_FRONT == %d\n", RINGBUF_FRONT(buf));
    printf("RINGBUF_AT(1) == %d\n", RINGBUF_AT(buf, 1));
    printf("RINGBUF_AT(2) == %d\n", RINGBUF_AT(buf, 2));

    printf("RINGBUF_COMBO_AT(-3) == %d\n", RINGBUF_COMBO_AT(buf, -3));
    printf("RINGBUF_COMBO_AT(-2) == %d\n", RINGBUF_COMBO_AT(buf, -2));
    printf("RINGBUF_COMBO_AT(-1) == %d\n", RINGBUF_COMBO_AT(buf, -1));
    printf("RINGBUF_COMBO_AT(0) == %d\n", RINGBUF_COMBO_AT(buf, 0));
    printf("RINGBUF_COMBO_AT(1) == %d\n", RINGBUF_COMBO_AT(buf, 1));
    printf("RINGBUF_COMBO_AT(2) == %d\n", RINGBUF_COMBO_AT(buf, 2));

    while (!RINGBUF_EMPTY(buf)) {
        printf("(%d) popping front %d\n", RINGBUF_SIZE(buf), RINGBUF_FRONT(buf));
        RINGBUF_POP_FRONT(buf);
    }

    printf("Test use in opposite direction\n");
    while (!RINGBUF_FULL(buf)) {
        printf("(%d) pushing front %d\n", RINGBUF_SIZE(buf), i);
        RINGBUF_PUSH_FRONT(buf, i++);
    }

    printf("ringbuffer full, pushing one more to test overwrite\n");
    printf("(%d) pushing back %d\n", RINGBUF_SIZE(buf), i);

    while (!RINGBUF_EMPTY(buf)) {
        printf("(%d) popping back %d\n", RINGBUF_SIZE(buf), RINGBUF_BACK(buf));
        RINGBUF_POP_BACK(buf);
    }
}
