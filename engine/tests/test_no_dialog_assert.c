// SPDX-License-Identifier: GPL-3.0-or-later
#undef NDEBUG
#include <assert.h>
#include <stdio.h>

int main(int argc, char** argv) {
    (void)argv;
    fprintf(stderr, "no-dialog-assert: asserting\n");
    fflush(stderr);
    assert(argc == 0);
    return 0;
}
