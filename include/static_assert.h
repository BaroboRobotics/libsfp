#ifndef _STATIC_ASSERT_H_
#define _STATIC_ASSERT_H_

#if (__STDC_VERSION__ >= 201112L)
#include <static_assert.h>
#else

/* GCC 4.3.0 and up support error attribute directives, which we can use to
 * emulate C11's static_assert. */
#if defined(__GNUC__) && ( \
    (__GNUC__ > 4) || \
    (__GNUC__ == 4 && __GNUC__MINOR__ >= 3))
/*
 * Generate a compile-time error if condition X is not met. Uses the GNU
 * nested function and error attribute extensions.
 *
 * Note that since this is a compile-time check, it makes little sense to do
 * something like this:
 *
 * if (some_runtime_condition)
 *     static_assert(some_compiletime_condition, "some message");
 * else
 *     ...
 *
 * The do-while construct is included to prevent that construct from
 * generating a needless syntax error, anyway. If one compile-time condition
 * does depend on a previous compile-time condition, a better way of writing
 * it would be this:
 *
 * static_assert((condition1 ? condition2 : 1), "some message");
 */
#define static_assert(CONDITION, MESSAGE) \
    do { \
        /* Declare and define an erroneous function. */ \
        auto void _static_assert () __attribute__((error(MESSAGE))); \
        void _static_assert () { }; \
        /* And use dead-code elimination to NOT call it, if our condition is
         * met. */ \
        (CONDITION) ? 0 : _static_assert(); \
    } while (0)

#else /* if GCC 4.3.0+ */
/* TODO write static_asserts for other compilers, if I ever need them. */
#define static_assert(c,m)
#endif

#endif /* if C11 */

#endif
