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

/*
 * mercury-progressor.h  public API for mercury progressor component
 * 29-Oct-2019  chuck@ece.cmu.edu
 */

#include <sys/time.h>          /* for timeval */
#include <sys/resource.h>      /* for rusage */

#include <mercury.h>           /* for mercury data types */

typedef struct progressor_handle progressor_handle_t;

typedef struct margo_instance* margo_instance_id; /* avoid including margo.h if not present */

/*
 * progressor_stats: run stats for a progressor and its handle.
 * note that the stats are zeroed each time the threads are started.
 * so if threads are run multiple times, only the latest run's
 * stats are available.
 */
struct progressor_stats {
    int is_running;            /* non-zero if threads are currently running */
    int needed;                /* number of needed requests for this handle */
    int progressor_refcnt;     /* progressor reference count */
    struct timeval runtime;    /* time latest thread set are/were running */
    struct rusage runusage;    /* usage latest thread set are/were using */
    uint64_t nprogress;        /* mercury progress fn counter */
    uint64_t ntrigger;         /* mercury trigger fn counter */
};

#ifdef __cplusplus
extern "C" {
#endif

/*
 * mercury_progressor_init: allocate and init a mercury progressor
 * and return a handle to it.   this just creates the base progressor
 * structure and handle... it does not start any progress threads.
 * if successful, the progressor has a reference count of 1 (the
 * handle we return).
 *
 * @param hgclass mercury class the progressor is associated with
 * @param hgcontext mercury context the progressor is associated with
 * @return handle to progressor, or NULL (for allocation error)
 */
progressor_handle_t *mercury_progressor_init(hg_class_t *hgclass,
                                             hg_context_t *hgcontext);

/**
 * mercury_progressor_init_from_margo: allocate and init a mercury
 * progressor and return a handle to it.  The progressor will be
 * created from a margo instance. Contrary to mercury_progressor_init,
 * which does nto start the progress thread, the margo instance will
 * already have a running progress thread.
 *
 * If mercury-progressor has not been built with Margo support,
 * this function will fail and return NULL.
 *
 * @param mid margo instance to associate with the processor
 *
 * @return handle to progressor, or NULL (for allocation error)
 */
progressor_handle_t *mercury_progressor_init_from_margo(margo_instance_id mid);

/*
 * mercury_progressor_get_progtimeout: get the current timeout setting
 * that is used with HG_Progress() in the timeout loop.
 *
 * @param the progressor handle to read the timeout from
 * @return the current timeout in msec
 */
unsigned int mercury_progressor_get_progtimeout(progressor_handle_t *hand);

/*
 * mercury_progressor_set_progtimeout: set the current timeout (in msec).
 * used with HG_Progress() in the timeout loop.  the change will
 * be applied the next time HG_Progress() is called.
 *
 * @param the progressor handle to read the timeout from
 * @param timeout the new timeout value
 */
void mercury_progressor_set_progtimeout(progressor_handle_t *hand,
                                        unsigned int timeout);

/*
 * mercury_progressor_duphandle: allocate a new progressor handle
 * that references the progressor the current handle references.
 * on success, this increases the progressor's reference count by 1.
 * services whose init function needs to retain a reference to a
 * progressor should use this function to get their own private
 * handle.
 *
 * @param hand the progressor handle to dupliate
 * @return the new progressor handle, or NULL (for allocation error)
 */
progressor_handle_t *mercury_progressor_duphandle(progressor_handle_t *hand);

/*
 * mercury_progressor_freehandle: drop a handle reference to a progressor.
 * if the handle current 'needs' threads, we drop that prior to freeing.
 * if we are dropping the final handle reference to a progressor, then
 * we free the progressor.   it is the responsibility of the service
 * that is using the handle to ensure that it has purged all other
 * references to the handle across any threads it owns prior to freeing
 * the handle, since this call will invalidate the handle.
 *
 * NOTE: you cannot call freehandle from an RPC callback function,
 * as the callback runs in the context of the progress thread and
 * the progress thread cannot free itself.
 *
 * @param hand the progressor handle to free
 * @return HG_SUCCESS or error code
 */
hg_return_t mercury_progressor_freehandle(progressor_handle_t *hand);

/*
 * mercury_progressor_getstats: use progressor handle to query the
 * underlying progressor for its stats.  if the progressor thread
 * is running, then this function blocks waiting for the thread to
 * update the stats.
 *
 * XXX: we assume the user's trigger callback functions are well
 * behaved and don't highjack the progressor thread for extended
 * periods of time.   maybe put a timeout on it?
 *
 * @param hand the progressor handle we are currently holding a reference to
 * @param ps pointer to a stats structure to be filled out
 * @return HG_SUCCESS or an error code
 */
hg_return_t mercury_progressor_getstats(progressor_handle_t *hand,
                                        struct progressor_stats *ps);

/*
 * mercury_progressor_nprogress: returns the number of times
 * we've called HG_Progress().   Unlike getstats(), this function
 * will not block the caller since it does not need to communicate
 * with a progressor thread.
 */
uint64_t mercury_progressor_nprogress(progressor_handle_t *hand);

/*
 * mercury_progressor_ntrigger: returns the number of times
 * we've called HG_Trigger().   Unlike getstats(), this function
 * will not block the caller since it does not need to communicate
 * with a progressor thread.
 */
uint64_t mercury_progressor_ntrigger(progressor_handle_t *hand);

/*
 * mercury_progressor_hgclass: return hg_class_t* assocaited with
 * the progressor our handle points to.  clients can safely cache
 * the return value while the progressor is allocated.
 *
 * @param hand the progressor handle of interest
 * @return the hgclass
 */
hg_class_t *mercury_progressor_hgclass(progressor_handle_t *hand);

/*
 * mercury_progressor_hgcontext: return hg_context_t* assocaited with
 * the progressor our handle points to.  clients can safely cache
 * the return value while the progressor is allocated.
 *
 * @param hand the progressor handle of interest
 * @return the hgcontext
 */
hg_context_t *mercury_progressor_hgcontext(progressor_handle_t *hand);

/*
 * mercury_progressor_hgclass: return margo_instance_id associated with
 * the progressor our handle points to.  clients can safely cache
 * the return value while the progressor is allocated.
 *
 * If mercury-progressor was not build with Margo support, this function
 * will return NULL.
 *
 * @param hand the progressor handle of interest
 * @return the margo_instance_id
 */
margo_instance_id mercury_progressor_mid(progressor_handle_t *hand);

/*
 * mercury_progressor_addrstring: return pointer to a C string containing
 * the address of our local mercury address.   valid as long as the
 * progressor is allocated.
 *
 * note: due to mercury limitations, this only works if mercury
 * was init'd with the "listen" flag set.   if "listen" was not
 * set, then this function always returns NULL.
 *
 * @param hand the progressor handle of interest
 * @return the address string (NULL on error)
 */
char *mercury_progressor_addrstring(progressor_handle_t *hand);


/*
 * mercury_progressor_needed: indicate this progressor handle needs
 * progressor service.   bumps "needed" reference count in this handle
 * and has progressor start the progress thread if it isn't already
 * running.
 *
 * @param hand handle to the progressor we need service from
 * @return HG_SUCCESS or error code
 */
hg_return_t mercury_progressor_needed(progressor_handle_t *hand);


/*
 * mercury_progressor_idle: indicate this progressor handle no longer
 * needs the progressor service running.   drops "needed" reference
 * count in the handle.  this may cause the progressor to stop the
 * progress thread if nothing else needs it.
 *
 * NOTE: you cannot call idle from an RPC callback function,
 * as the callback runs in the context of the progress thread and
 * the progress thread cannot stop itself.
 *
 * @param hand handle to the progressor we need service from
 * @return HG_SUCCESS or error code
 */
hg_return_t mercury_progressor_idle(progressor_handle_t *hand);

#ifdef __cplusplus
}
#endif
