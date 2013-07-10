#include "../queue.h"

#include <stdio.h>

typedef struct {
    int a;
    int b;
    int c;
} elem_t;

int main () {
    int i = 0;
    elem_t elem;

    // the following should cause a compile-time error
#if 0
    QUEUE(elem_t, 5) fux;
    QUEUE_INIT(fux);
#endif

    QUEUE(elem_t, (1<<4)) queue;
    QUEUE_INIT(queue);

    for (i = 0; i < 5*3; i += 3) {
        elem_t elem = { i, i+1, i+2 };
        printf("pushing { %d %d %d }\n", elem.a, elem.b, elem.c);
        QUEUE_PUSH(queue, elem);
    }

    printf("\n");

    {
        // pointer play :)
        elem_t *pelem = &QUEUE_FRONT(queue);

        elem.a = pelem->a + 20;
        elem.b = pelem->b + 20;
        elem.c = pelem->c + 20;
        printf("pushing { %d %d %d }\n", elem.a, elem.b, elem.c);
        QUEUE_PUSH(queue, elem);

        printf("popping { %d %d %d }\n", pelem->a, pelem->b, pelem->c);
        QUEUE_POP(queue);
        // pelem is now in a bad state: it points to valid memory, but the
        // data in that memory can be used for a different element
    }

    printf("\n");

    while (!QUEUE_EMPTY(queue)) {
        printf("popping { %d %d %d }\n", 
                QUEUE_FRONT(queue).a,
                QUEUE_FRONT(queue).b,
                QUEUE_FRONT(queue).c
                );
        QUEUE_POP(queue);
    }
}
