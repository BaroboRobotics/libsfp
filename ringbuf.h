#ifndef _RINGBUF_H_
#define _RINGBUF_H_

/* A fixed-size ring buffer optimized for power-of-two buffer sizes. */

#include "static_assert.h"

#include <assert.h>

#define RINGBUF(TYPE, CAPACITY) \
    struct { \
        size_t begin; \
        size_t end; \
        TYPE data[CAPACITY]; \
    }

#define RINGBUF_CAPACITY(BUF) \
    (sizeof((BUF).data) / sizeof((BUF).data[0]))

#define RINGBUF_SIZE(BUF) \
  (RINGBUF_FULL(BUF) ? RINGBUF_CAPACITY(BUF) \
   : ((BUF).end - (BUF).begin) & (RINGBUF_CAPACITY(BUF) - 1))

#define RINGBUF_assert_pot(BUF) \

/* Kinda hacky, provided mostly just so I can use a ring buffer as the
 * underlying data structure for a queue, but still have the static assertion
 * error message make some kind of sense. */
#define RINGBUF_INIT_NAMED(BUF, NAME) \
    do { \
        static_assert(RINGBUF_CAPACITY(BUF), "zero-length " NAME " (don't do that)"); \
        static_assert(!(RINGBUF_CAPACITY(BUF) % 2), NAME " with non-power-of-two capacity"); \
        (BUF).begin = 0; \
        (BUF).end = 0; \
    } while (0)

#define RINGBUF_INIT(BUF) RINGBUF_INIT_NAMED(BUF, "ring buffer")

#define RINGBUF_EMPTY(BUF) \
    ((BUF).begin == (BUF).end)

#define RINGBUF_FULL(BUF) \
    (((BUF).begin ^ RINGBUF_CAPACITY(BUF)) == (BUF).end)

/* private */
#define RINGBUF_wrapped_access(BUF, INDEX) \
    ((BUF).data[(INDEX) & (RINGBUF_CAPACITY(BUF) - 1)])

/* Array-like access, counting forward from begin. */
#define RINGBUF_AT(BUF, INDEX) \
    RINGBUF_wrapped_access(BUF, ((BUF).begin + (INDEX)))

/* Array-like access, counting backward from end. */
#define RINGBUF_REVERSE_AT(BUF, INDEX) \
    RINGBUF_wrapped_access(BUF, ((BUF).end - (INDEX)))

/* Array-like access, where negative indices count backward from end. */
#define RINGBUF_COMBO_AT(BUF, INDEX) \
    RINGBUF_wrapped_access(BUF, (((INDEX) < 0 ? (BUF).end : (BUF).begin) + (INDEX)))

#define RINGBUF_FRONT(BUF) RINGBUF_AT(BUF, 0)

#define RINGBUF_BACK(BUF) RINGBUF_REVERSE_AT(BUF, 1)

/* private */
#define RINGBUF_add(BUF, INDEX, AMOUNT) \
    ((BUF).INDEX = ((BUF).INDEX + (AMOUNT)) & (2 * RINGBUF_CAPACITY(BUF) - 1))

/* private */
#define RINGBUF_incr(BUF, INDEX) RINGBUF_add(BUF, INDEX, 1)

/* private */
#define RINGBUF_decr(BUF, INDEX) RINGBUF_add(BUF, INDEX, -1)

#define RINGBUF_PUSH_BACK(BUF, ELEM) \
    do { \
        if (RINGBUF_FULL(BUF)) { \
            RINGBUF_incr(BUF, begin); \
        } \
        RINGBUF_incr(BUF, end); \
        RINGBUF_BACK(BUF) = (ELEM); \
    } while (0)

#define RINGBUF_PUSH_FRONT(BUF, ELEM) \
    do { \
        if (RINGBUF_FULL(BUF)) { \
            RINGBUF_decr(BUF, end); \
        } \
        RINGBUF_decr(BUF, begin); \
        RINGBUF_FRONT(BUF) = (ELEM); \
    } while (0)

#define RINGBUF_POP_FRONT(BUF) \
    do { \
        assert(!RINGBUF_EMPTY(BUF)); \
        RINGBUF_incr(BUF, begin); \
    } while (0)

#define RINGBUF_POP_BACK(BUF) \
    do { \
        assert(!RINGBUF_EMPTY(BUF)); \
        RINGBUF_decr(BUF, end); \
    } while (0)

#endif
