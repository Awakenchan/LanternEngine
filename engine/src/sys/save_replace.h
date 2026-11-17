// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LANTERN_SAVE_REPLACE_H
#define LANTERN_SAVE_REPLACE_H
#include <errno.h>

enum { SAVE_REPLACE_RETRIES = 5 };
enum { SAVE_REPLACE_ACCESS_DENIED = 5, SAVE_REPLACE_SHARING_VIOLATION = 32, SAVE_REPLACE_LOCK_VIOLATION = 33 };
enum { SAVE_REPLACE_MUTANT_ANY_ERROR = 4, SAVE_REPLACE_MUTANT_LATCH_KEPT = 5 };

#ifdef LANTERN_SAVE_TEST_HOOKS
extern int save_replace_mutant;
void sys_save_data_test_replace_errors(const unsigned long *errors, int count);
int sys_save_data_test_replace_attempts(void);
#define SAVE_REPLACE_MUTANT(number) (save_replace_mutant == (number))
#else
#define SAVE_REPLACE_MUTANT(number) 0
#endif

static inline int save_replace_may_pass_later(unsigned long error) {
    if (SAVE_REPLACE_MUTANT(SAVE_REPLACE_MUTANT_ANY_ERROR)) return error != 0;
#ifdef _WIN32
    return error == SAVE_REPLACE_ACCESS_DENIED || error == SAVE_REPLACE_SHARING_VIOLATION ||
           error == SAVE_REPLACE_LOCK_VIOLATION;
#else
    return error == EBUSY || error == ETXTBSY;
#endif
}

static inline int save_replace_retries(int spent) {
    return spent ? 0 : SAVE_REPLACE_RETRIES;
}

static inline int save_replace_spent_after(int spent, unsigned long error, int retries) {
    if (!error) return SAVE_REPLACE_MUTANT(SAVE_REPLACE_MUTANT_LATCH_KEPT) ? spent : 0;
    return spent || (retries > 0 && save_replace_may_pass_later(error));
}

#endif
