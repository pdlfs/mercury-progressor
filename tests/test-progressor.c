/*
 * Copyright (c) 2019, Carnegie Mellon University.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT
 * HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY
 * WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <mercury.h>
#include "mercury-progressor.h"

void checkstat(char *tag, progressor_handle_t *p,
               struct progressor_stats *psp, int rn, int nd, int rf) {
    if (mercury_progressor_getstats(p, psp) != HG_SUCCESS) {
        fprintf(stderr, "progressor stat %s failed\n", tag);
        exit(1);
    }
    if (psp->is_running != rn) {
        fprintf(stderr, "progressor stat %s run check failed\n", tag);
        exit(1);
    }
    if (psp->needed != nd) {
        fprintf(stderr, "progressor stat %s need check failed\n", tag);
        exit(1);
    }
    if (psp->progressor_refcnt != rf) {
        fprintf(stderr, "progressor stat %s ref check failed\n", tag);
        exit(1);
    }
}

int main(int argc, char **argv) {
    hg_class_t *cls;
    hg_context_t *ctx;
    progressor_handle_t *phand, *duphand;
    struct progressor_stats ps;

    cls = HG_Init("na+sm", HG_TRUE);
    if (!cls) {
        fprintf(stderr, "HG_Init failed\n");
        exit(1);
    }

    ctx = HG_Context_create(cls);
    if (!ctx) {
        fprintf(stderr, "HG_Context_create failed\n");
        exit(1);
    }

    phand = mercury_progressor_init(cls, ctx);
    if (!phand) {
        fprintf(stderr, "mercury_progressor_init failed\n");
        exit(1);
    }
    if (mercury_progressor_hgclass(phand) != cls ||
        mercury_progressor_hgcontext(phand) != ctx) {
        fprintf(stderr, "progressor access check failed\n");
        exit(1);
    }
    printf("my address: %s\n", mercury_progressor_addrstring(phand));

    checkstat("check 0", phand, &ps, 0, 0, 1);

    duphand = mercury_progressor_duphandle(phand);
    if (!duphand) {
        fprintf(stderr, "progressor dup failed\n");
        exit(1);
    }

    checkstat("dup check A0", phand, &ps, 0, 0, 2);
    checkstat("dup check A1", duphand, &ps, 0, 0, 2);

    if (mercury_progressor_needed(duphand) != HG_SUCCESS) {
        fprintf(stderr, "needed start failed\n");
        exit(1);
    }
    checkstat("dup check B0", phand, &ps, 1, 0, 2);
    checkstat("dup check B1", duphand, &ps, 1, 1, 2);

    if (mercury_progressor_needed(duphand) != HG_SUCCESS) {
        fprintf(stderr, "needed start failed\n");
        exit(1);
    }
    checkstat("dup check C0", phand, &ps, 1, 0, 2);
    checkstat("dup check C1", duphand, &ps, 1, 2, 2);

    if (mercury_progressor_needed(phand) != HG_SUCCESS) {
        fprintf(stderr, "needed start failed\n");
        exit(1);
    }
    checkstat("dup check D0", phand, &ps, 1, 1, 2);
    checkstat("dup check D1", duphand, &ps, 1, 2, 2);

    if (mercury_progressor_idle(duphand) != HG_SUCCESS) {
        fprintf(stderr, "idle failed\n");
        exit(1);
    }
    checkstat("dup check E0", phand, &ps, 1, 1, 2);
    checkstat("dup check E1", duphand, &ps, 1, 1, 2);

    if (mercury_progressor_needed(duphand) != HG_SUCCESS) {
        fprintf(stderr, "needed start failed\n");
        exit(1);
    }
    checkstat("dup check F0", phand, &ps, 1, 1, 2);
    checkstat("dup check F1", duphand, &ps, 1, 2, 2);

    if (mercury_progressor_freehandle(duphand) != HG_SUCCESS) {
        fprintf(stderr, "free dup failed\n");
        exit(1);
    }
    duphand = NULL;
    checkstat("dup check G0", phand, &ps, 1, 1, 1);

    if (mercury_progressor_idle(phand) != HG_SUCCESS) {
        fprintf(stderr, "idle failed\n");
        exit(1);
    }
    checkstat("dup check H0", phand, &ps, 0, 0, 1);

    if (mercury_progressor_needed(phand) != HG_SUCCESS) {
        fprintf(stderr, "needed start failed\n");
        exit(1);
    }
    checkstat("dup check I0", phand, &ps, 1, 1, 1);

    if (mercury_progressor_freehandle(phand) != HG_SUCCESS) {
        fprintf(stderr, "free phand failed\n");
        exit(1);
    }
    phand = NULL;

    HG_Context_destroy(ctx);
    HG_Finalize(cls);
    exit(0);
}
