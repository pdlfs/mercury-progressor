/*
 * Copyright (c) 2019-2023, Carnegie Mellon University.
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

/*
 * margo-progressor.c  mercury progressor component implemented with margo
 * 13-Jan-2023  mdorier@anl.gov
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <margo.h>
#include <margo-util.h>

#include "mercury-progressor.h"

struct margo_progressor {
    margo_instance_id mid;
    _Atomic unsigned  refcount;
    int               owns_mid;
    char              self_addr[128];
};

struct progressor_handle {
    struct margo_progressor* pg;
    _Atomic unsigned         needed;
};

/*
 * API functions!
 */

progressor_handle_t *mercury_progressor_init_from_margo(margo_instance_id mid) {
    if(!mid) return NULL;

    hg_addr_t addr = HG_ADDR_NULL;
    hg_return_t ret = margo_addr_self(mid, &addr);
    if(ret != HG_SUCCESS) return NULL;
    char self_addr[128];
    hg_size_t addr_size = 128;
    ret = margo_addr_to_string(mid, self_addr, &addr_size, addr);
    if(ret != HG_SUCCESS) return NULL;
    ret = margo_addr_free(mid, addr);
    if(ret != HG_SUCCESS) return NULL;

    progressor_handle_t* pgh = calloc(1, sizeof(*pgh));
    pgh->pg = calloc(1, sizeof(*(pgh->pg)));
    pgh->pg->mid = mid;
    pgh->pg->refcount = 1;
    strcpy(pgh->pg->self_addr, self_addr);

    return pgh;
}

margo_instance_id mercury_progressor_mid(progressor_handle_t *hand) {
    if(!hand) return NULL;
    return hand->pg->mid;
}

/*
 * mercury_progressor_init: allocate and init progressor
 */
progressor_handle_t *mercury_progressor_init(hg_class_t *hgclass,
                                             hg_context_t *hgcontext) {
    struct margo_init_info init_info = {0};
    init_info.hg_class = hgclass;
    init_info.hg_context = hgcontext;
    init_info.json_config = "{\"use_progress_thread\":true,\"rpc_thread_count\":1}";
    margo_instance_id mid = margo_init_ext(
            HG_Class_get_protocol(hgclass),
            HG_Class_is_listening(hgclass),
            &init_info);
    if(!mid) return NULL;
    progressor_handle_t* pgh = mercury_progressor_init_from_margo(mid);
    if(!pgh) return NULL;
    pgh->pg->owns_mid = 1;
    return pgh;
}

/*
 * mercury_progressor_get_progtimeout: get progtimeout (msec)
 */
unsigned int mercury_progressor_get_progtimeout(progressor_handle_t *hand) {
    if(!hand) return 0;
    unsigned int timeout = 0;
    margo_get_progress_timeout_ub_msec(hand->pg->mid, &timeout);
    return timeout;
}

/*
 * mercury_progressor_set_progtimeout: set progtimeout (msec)
 */
void mercury_progressor_set_progtimeout(progressor_handle_t *hand,
                                        unsigned int timeout) {
    if(!hand) return;
    margo_set_progress_timeout_ub_msec(hand->pg->mid, timeout);
}

/*
 * mercury_progressor_duphandle: create a duplicate handle.
 */
progressor_handle_t *mercury_progressor_duphandle(progressor_handle_t *hand) {
    if(!hand) return NULL;
    progressor_handle_t* new_pgh = calloc(1, sizeof(*new_pgh));
    new_pgh->pg = hand->pg;
    new_pgh->pg->refcount++;
    return new_pgh;
}

/*
 * mercury_progressor_freehandle: free a progressor handle.   the
 * caller should be holding the last reference to the handle.
 */
hg_return_t mercury_progressor_freehandle(progressor_handle_t *hand) {
    if(!hand) return HG_SUCCESS;
    if(--hand->pg->refcount == 0) {
        if(hand->pg->owns_mid)
            margo_finalize(hand->pg->mid);
        free(hand->pg);
    }
    free(hand);
    return HG_SUCCESS;
}

/*
 * mercury_progressor_getstats: get stats from progress thread
 */
hg_return_t mercury_progressor_getstats(progressor_handle_t *hand,
                                        struct progressor_stats *ps) {
    if(!hand) return HG_INVALID_ARG;
    if(!ps) return HG_SUCCESS;

    margo_instance_id mid = hand->pg->mid;

    memset(ps, 0, sizeof(*ps));
    ps->is_running        = 1; /* margo is always running */
    ps->needed            = hand->needed;
    ps->progressor_refcnt = hand->pg->refcount;
    ps->nprogress         = margo_get_num_progress_calls(mid);
    ps->ntrigger          = margo_get_num_trigger_calls(mid);
    // note: can't fill in runtime and runusage

    return HG_SUCCESS;
}

/*
 * mercury_progressor_nprogress: extract nprogress from progressor
 */
uint64_t mercury_progressor_nprogress(progressor_handle_t *hand) {
    return hand ? margo_get_num_progress_calls(hand->pg->mid) : 0;
}

/*
 * mercury_progressor_ntrigger: extract ntrigger from progressor
 */
uint64_t mercury_progressor_ntrigger(progressor_handle_t *hand) {
    return hand ? margo_get_num_trigger_calls(hand->pg->mid) : 0;
}

/*
 * mercury_progressor_hgclass: extract hg_class_t* from progressor
 */
hg_class_t *mercury_progressor_hgclass(progressor_handle_t *hand) {
    return hand ? margo_get_class(hand->pg->mid) : NULL;
}

/*
 * mercury_progressor_hgcontext: extract hg_context_t* from progressor
 */
hg_context_t *mercury_progressor_hgcontext(progressor_handle_t *hand) {
    return hand ? margo_get_context(hand->pg->mid) : NULL;
}

/*
 * mercury_progressor_addrstring: extract local address string.
 * valid as long as handle is active, no need to free return value.
 */
char *mercury_progressor_addrstring(progressor_handle_t *hand) {
    return hand ? hand->pg->self_addr : NULL;
}

/*
 * mercury_progressor_needed: handle needs threads to be running
 */
hg_return_t mercury_progressor_needed(progressor_handle_t *hand) {
    if(!hand) return HG_INVALID_ARG;
    ++hand->needed;
    return HG_SUCCESS;
}

/*
 * mercury_progressor_idle: progressor handle no longer needs progressor
 * service running.
 */
hg_return_t mercury_progressor_idle(progressor_handle_t *hand) {
    if(!hand) return HG_INVALID_ARG;
    --hand->needed;
    return HG_SUCCESS;
}
