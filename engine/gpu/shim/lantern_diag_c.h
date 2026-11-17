// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LANTERN_DIAG_C_H
#define LANTERN_DIAG_C_H

#ifdef __cplusplus
extern "C" {
#endif

#define LANTERN_SW_LAUNCHER 1u
#define LANTERN_SW_LEGACY 2u
#define LANTERN_SW_SECRET 4u
#define LANTERN_SW_VERIFY_ALIAS 8u
#define LANTERN_SW_SHOW 16u
#define LANTERN_SW_COMPARE_ALIAS 32u

enum {
    LANTERN_KIND_Bool,
    LANTERN_KIND_Mode,
    LANTERN_KIND_Int,
    LANTERN_KIND_Enum,
    LANTERN_KIND_Text
};

enum { LANTERN_PARSE_UNSET, LANTERN_PARSE_OK, LANTERN_PARSE_ALIAS, LANTERN_PARSE_BAD };

enum {
#define X(id, name, kind, def, mn, mx, live, step, flags) LANTERN_SW_ID_##id,
#include "lantern_switches.inc"
#undef X
    LANTERN_SW_ID_COUNT
};

#if defined(LANTERN_MUTANTS) && defined(__cplusplus)
extern int lantern_mutant;
#define LANTERN_MUTANT(n) (!__builtin_is_constant_evaluated() && lantern_mutant == (n))
#else
#define LANTERN_MUTANT(n) 0
#endif

#ifdef __cplusplus
#define LANTERN_PURE static constexpr inline
#else
#define LANTERN_PURE static inline
#endif

enum { LANTERN_MAX_SWITCH_DIGITS = 9 };

LANTERN_PURE int lantern_parse_number(const char* text, long* out) {
    long value = 0;
    int negative = 0;
    int digits = 0;
    const char* p = text;
    if (*p == '-') {
        negative = 1;
        ++p;
    }
    for (; *p; ++p) {
        if (*p < '0' || *p > '9' || digits == LANTERN_MAX_SWITCH_DIGITS) return 0;
        value = value * 10 + (*p - '0');
        ++digits;
    }
    if (digits == 0) return 0;
    *out = negative ? -value : value;
    return 1;
}

LANTERN_PURE int lantern_text_equal(const char* a, const char* b) {
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

LANTERN_PURE int lantern_parse_switch(int kind, long min, long max, unsigned flags, const char* text, long* out) {
    long value = 0;
    if (!text || !text[0]) return LANTERN_PARSE_UNSET;
    if (kind == LANTERN_KIND_Text) return LANTERN_PARSE_OK;
    if (kind == LANTERN_KIND_Mode && (flags & LANTERN_SW_VERIFY_ALIAS) && lantern_text_equal(text, "verify")) {
        *out = 2;
        return LANTERN_PARSE_ALIAS;
    }
    if (kind == LANTERN_KIND_Mode && (flags & LANTERN_SW_COMPARE_ALIAS) && lantern_text_equal(text, "compare")) {
        *out = 2;
        return LANTERN_PARSE_ALIAS;
    }
    if (!lantern_parse_number(text, &value)) return LANTERN_PARSE_BAD;
    if (LANTERN_MUTANT(1) ? value > max : (value < min || value > max)) return LANTERN_PARSE_BAD;
    *out = value;
    return LANTERN_PARSE_OK;
}

#ifdef LANTERN_SWITCH_C_STUB
static inline long lantern_switch_c(int id) {
    static const struct {
        int kind;
        const char* def;
        long min, max;
        unsigned flags;
    } rows[] = {
#define X(id, name, kind, def, mn, mx, live, step, flags) {LANTERN_KIND_##kind, def, mn, mx, flags},
#include "lantern_switches.inc"
#undef X
    };
    long value = 0;
    if (id < 0 || id >= LANTERN_SW_ID_COUNT) return 0;
    lantern_parse_switch(rows[id].kind, rows[id].min, rows[id].max, rows[id].flags, rows[id].def, &value);
    return value;
}
#else
long lantern_switch_c(int id);
#endif

int lantern_report_claim(int reason);
void lantern_report_done(void);
int lantern_report_wait_ms(unsigned ms);

#ifdef __cplusplus
}
#endif

#endif
