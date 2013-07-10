#ifndef _STATIC_ASSERT_H_
#define _STATIC_ASSERT_H_

/*
 * Generate a compile-time error if condition X is not met. Uses the GNU
 * nested function and error attribute extensions.
 *
 * Note that since this is a compile-time check, it makes little sense to do
 * something like this:
 *
 * if (some_runtime_condition)
 *     STATIC_ASSERT(some_compiletime_condition, "some message");
 * else
 *     ...
 *
 * The do-while construct is included to prevent that construct from
 * generating a needless syntax error, anyway. If one compile-time condition
 * does depend on a previous compile-time condition, a better way of writing
 * it would be this:
 *
 * STATIC_ASSERT((condition1 ? condition2 : 1), "some message");
 */
#define STATIC_ASSERT(CONDITION, MESSAGE) \
    do { \
        /* Declare and define an erroneous function. */ \
        auto void _static_assert () __attribute__((error(MESSAGE))); \
        void _static_assert () { }; \
        /* And use dead-code elimination to NOT call it, if our condition is
         * met. */ \
        (CONDITION) ? 0 : _static_assert(); \
    } while (0)

#endif
