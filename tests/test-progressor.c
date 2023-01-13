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
#include <assert.h>

#include <mercury.h>
#include <mercury_macros.h>
#include "mercury-progressor.h"
#include "config.h"

void checkstat(char *tag, progressor_handle_t *p,
               struct progressor_stats *psp, int rn, int nd, int rf) {
    if (mercury_progressor_getstats(p, psp) != HG_SUCCESS) {
        fprintf(stderr, "progressor stat %s failed\n", tag);
        exit(1);
    }
#ifndef ENABLE_MARGO
    if (psp->is_running != rn) {
        fprintf(stderr, "progressor stat %s run check failed\n", tag);
        exit(1);
    }
#else
    if (psp->is_running != 1) {
        fprintf(stderr, "progressor stat %s run check failed\n", tag);
        exit(1);
    }
#endif
    if (psp->needed != nd) {
        fprintf(stderr, "progressor stat %s need check failed\n", tag);
        exit(1);
    }
    if (psp->progressor_refcnt != rf) {
        fprintf(stderr, "progressor stat %s ref check failed\n", tag);
        exit(1);
    }
}

MERCURY_GEN_PROC(op_in_t,
        ((int32_t)(x))\
        ((int32_t)(y)))

MERCURY_GEN_PROC(op_out_t, ((int32_t)(ret)))

hg_return_t sum(hg_handle_t handle)
{
    hg_return_t ret;
    op_in_t in;
    op_out_t out;

    const struct hg_info* info = HG_Get_info(handle);

    ret = HG_Get_input(handle, &in);
    assert(ret == HG_SUCCESS);

    out.ret = in.x + in.y;
    printf("%d + %d = %d\n", in.x, in.y, in.x+in.y);

    ret = HG_Respond(handle, NULL, NULL, &out);
    assert(ret == HG_SUCCESS);

    ret = HG_Free_input(handle, &in);
    assert(ret == HG_SUCCESS);

    ret = HG_Destroy(handle);
    assert(ret == HG_SUCCESS);

    return HG_SUCCESS;
}

hg_return_t sum_completed(const struct hg_cb_info *info) {
    volatile int *completed = info->arg;
    *completed = 1;
    return HG_SUCCESS;
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

    // register RPC
    hg_id_t sum_id = HG_Register_name(cls, "sum", hg_proc_op_in_t, hg_proc_op_out_t, sum);

    // get self address
    hg_addr_t self_addr = HG_ADDR_NULL;
    hg_return_t ret = HG_Addr_self(cls, &self_addr);
    if (ret != HG_SUCCESS) {
        fprintf(stderr, "HG_Addr_self failed (%d)\n", ret);
        exit(1);
    }

    // create RPC handle
    hg_handle_t handle;
    ret = HG_Create(ctx, self_addr, sum_id, &handle);
    if (ret != HG_SUCCESS) {
        fprintf(stderr, "HG_Create failed (%d)\n", ret);
        exit(1);
    }

    // make sure progress loop is runnning
    mercury_progressor_needed(phand);

    // forward RPC
    op_in_t in;
    in.x = 42;
    in.y = 23;
    volatile int completed = 0;
    ret = HG_Forward(handle, sum_completed, (void*)&completed, &in);
    if (ret != HG_SUCCESS) {
        fprintf(stderr, "HG_Forward failed (%d)\n", ret);
        exit(1);
    }

    // ugly active loop
    while(!completed) { usleep(100);}

    // get output
    op_out_t out;
    ret = HG_Get_output(handle, &out);
    if (ret != HG_SUCCESS) {
        fprintf(stderr, "HG_Get_output failed (%d)\n", ret);
        exit(1);
    }

    if (out.ret != in.x + in.y) {
        fprintf(stderr, "Output is incorrect\n");
        exit(1);
    }

    // free output
    ret = HG_Free_output(handle, &out);
    if (ret != HG_SUCCESS) {
        fprintf(stderr, "HG_Destroy failed (%d)\n", ret);
        exit(1);
    }

    // free handle
    ret = HG_Destroy(handle);
    if (ret != HG_SUCCESS) {
        fprintf(stderr, "HG_Destroy failed (%d)\n", ret);
        exit(1);
    }

    // free self address
    ret = HG_Addr_free(cls, self_addr);
    if (ret != HG_SUCCESS) {
        fprintf(stderr, "HG_Addr_free failed (%d)\n", ret);
        exit(1);
    }

    // stop the progress loop
    mercury_progressor_idle(phand);

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
