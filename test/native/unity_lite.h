#ifndef UNITY_LITE_H
#define UNITY_LITE_H

#include <stdio.h>

/* Deliberately tiny assertion framework — pulling in a full Unity/CMock
 * install just to check state transitions and JSON strings would be more
 * setup than the tests themselves. Defined once in main.c. */
extern int g_tests_run;
extern int g_tests_failed;

#define UL_CHECK(cond, msg)                                                       \
    do {                                                                          \
        g_tests_run++;                                                           \
        if (!(cond)) {                                                           \
            g_tests_failed++;                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);                 \
        }                                                                        \
    } while (0)

#endif
