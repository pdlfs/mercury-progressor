/*
 * Copyright (c) 2019-2020, Carnegie Mellon University.
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
 * mercury-progressor.c  mercury progressor component code
 * 29-Oct-2019  chuck@ece.cmu.edu
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/queue.h>                 /* list */

#include <mercury_thread.h>            /* hg_thread_t */
#include <mercury_thread_condition.h>  /* cond vars and mutex */

#include "mercury-progressor.h"

#define DEF_PROGTIMEOUT 100 /* network progress timeout (HG_Progress) */

/*
 * begin: internal useprobe code
 */

#ifdef RUSAGE_THREAD
#define USEPROBE_THREAD RUSAGE_THREAD  /* linux-specific */
#else
#define USEPROBE_THREAD RUSAGE_SELF    /* fallback if THREAD not available */
#endif

struct useprobe {
    int who;                /* flag to getrusage */
    struct timeval t0, t1;
    struct rusage r0, r1;
};

/* load starting values into useprobe */
static void useprobe_start(struct useprobe *up, int who) {
    up->who = who;
    if (gettimeofday(&up->t0, NULL) < 0 || getrusage(up->who, &up->r0) < 0) {
        fprintf(stderr, "useprobe_start syscall failed?\n");
        abort();
    }
}


/* load end values into useprobe */
static void useprobe_end(struct useprobe *up) {
    if (gettimeofday(&up->t1, NULL) < 0 || getrusage(up->who, &up->r1) < 0) {
        fprintf(stderr, "useprobe_ned syscall failed\n");
        abort();
    }
}

/* compute t1-t0 */
static void useprobe_subtime(struct timeval *rv, struct timeval *t1,
                             struct timeval *t0) {
    rv->tv_sec  = t1->tv_sec  - t0->tv_sec;
    rv->tv_usec = t1->tv_usec - t0->tv_usec;
    if (rv->tv_usec < 0) {
        rv->tv_sec--;
        rv->tv_usec += 1000000;
    }
}

/* compute r1-r0 */
static void useprobe_subusage(struct rusage *rv, struct rusage *r1,
                              struct rusage *r0) {
    useprobe_subtime(&rv->ru_utime, &r1->ru_utime, &r0->ru_utime);
    useprobe_subtime(&rv->ru_stime, &r1->ru_stime, &r0->ru_stime);
    rv->ru_maxrss   = r1->ru_maxrss   - r0->ru_maxrss;
    rv->ru_ixrss    = r1->ru_ixrss    - r0->ru_ixrss;
    rv->ru_idrss    = r1->ru_idrss    - r0->ru_idrss;
    rv->ru_isrss    = r1->ru_isrss    - r0->ru_isrss;
    rv->ru_minflt   = r1->ru_minflt   - r0->ru_minflt;
    rv->ru_majflt   = r1->ru_majflt   - r0->ru_majflt;
    rv->ru_nswap    = r1->ru_nswap    - r0->ru_nswap;
    rv->ru_inblock  = r1->ru_inblock  - r0->ru_inblock;
    rv->ru_oublock  = r1->ru_oublock  - r0->ru_oublock;
    rv->ru_msgsnd   = r1->ru_msgsnd   - r0->ru_msgsnd;
    rv->ru_msgrcv   = r1->ru_msgrcv   - r0->ru_msgrcv;
    rv->ru_nsignals = r1->ru_nsignals - r0->ru_nsignals;
    rv->ru_nvcsw    = r1->ru_nvcsw    - r0->ru_nvcsw;
    rv->ru_nivcsw   = r1->ru_nivcsw   - r0->ru_nivcsw;
}

/*
 * end: internal useprobe code
 */

/*
 * handlist: list of progressor handles
 */
LIST_HEAD(handlist, progressor_handle);

/*
 * progressor: the main structure.
 */
struct progressor {
    /* fields that are set at init and never changed */
    hg_class_t *hg_cls;      /* our class */
    hg_context_t *hg_ctx;    /* our context */
    char *myaddrstring;      /* our local address, malloc'd */

    /* fields that can be read/changed without locking */
    unsigned int progtimeout;/* HG_Progress timeout value */

    /* stats, only prog thread writes.  zeroed each time we start threads */
    struct useprobe usage;   /* time and rusage info */
    uint64_t nprogress;      /* number of calls to progress function */
    uint64_t ntrigger;       /* number of calls to trigger function */

    /* plock and fields locked by it */
    hg_thread_mutex_t plock; /* lock for progressor */
    hg_thread_cond_t pcv;    /* block here to wait for thread ops to finish */
    int prg_refcnt;          /* # of mercury_progress_handle's that ref us */
    int nrunreqs;            /* # of handles that need the threads running */
    int threadop_inprogress; /* 1 if a threadop is in progress */
    int threadop_wanted;     /* # of clients that want to do an op */
    int do_shutdown;         /* set to start shutdown thread op */
    int is_running;          /* set if progress thread is running */
    hg_thread_t mythread;    /* currently running thread (if is_running) */
    struct handlist hlist;   /* list of handles waiting for stat info */
};

/*
 * progressor handle: contains a reference to a progressor structure.
 */
struct progressor_handle {
    /* fields that are set at init and never changed */
    struct progressor *myprogressor;  /* progressor we are a handle for */

    /* locked by myprogressor plock */
    LIST_ENTRY(progressor_handle) hq; /* linkage for hlist */

    /* hlock and fields locked by it */
    hg_thread_mutex_t hlock; /* lock for handle */
    hg_thread_cond_t hcv;    /* block here to wait for ops to complete */
    int hand_busy;           /* for serialization: we are busy in an op */
    int hwant_wakeup;        /* thread is blocked on hcv and wants a wakeup */
    int need_count;          /* #of progressor needed reqs active for handle */
    int need_load;           /* waiting for progress to load stats in "ps" */
    struct progressor_stats *ps;  /* to be filled in */
};

/*
 * additional notes:
 *
 * data structures: we have one or more progressor_handles that
 * reference a single progressor.  the progressor and first
 * progressor_handle are created by mercury_progressor_init().
 * additional progressor_handles are created by calling
 * mercury_progressor_duphandle().  the idea here is that each
 * service that uses mercury has its own handle to reference it with.
 * mercury_progressor_freehandle() frees a handle.  when the last handle
 * is freed, the progressor is also freed.  the external API only
 * operations on progressor_handles.
 *
 * lock ordering: to avoid deadlock => lock handle first, then progressor.
 *
 * serialization: we serialize multi-thread operations using condition
 * variables.   as these operations are infrequent (mostly at startup
 * and shutdown), this serialization is not on the critical path and
 * should not be a performance issue.
 *
 * the progressor uses "pcv" to serialize the two types of threadops
 * (start threads, stop threads) it supports.   threads block on "pcv" if:
 *
 *   1. they are waiting for some other thread to finish performing
 *      a serialized threadop (namely, start or stop the progress thread).
 *       => wakeup: in this case the thread currently finishing performing
 *          the threadop uses hg_thread_cond_signal() to cause one
 *          of the waiting threads to wakeup and retry.
 *
 *   2. the thread is the one currently performing a threadop and is
 *      waiting for the progress thread to indicate it is running (for
 *      start threadop) or has stopped (for stop threadop).
 *       => wakeup: in this case the starting or stopping progress
 *          thread must use hg_thread_cond_broadcast() to wake up
 *          all the waiting threads, including the thread currently
 *          performing the threadop (i.e. the one we want).  we expect
 *          that most of the time there will only be one waiting thread.
 *
 * to perform a threadop, you need to be able to change threadop_inprogress
 * from 0 to 1.   if it is already 1, then you have to use "pcv" to wait
 * for it to clear so you can set it.
 *
 * example:
 *     startup thread op:   (call if "is_running" is zero)
 *      - client thread waits until it can set threadop_inprogress to one
 *      - if "is_running" is one, then someone else started for you.
 *        ELSE: pthread_create() a new progress thread
 *              block on pcv until "is_running" is set by the new thread
 *              [the new progress thread will broadcast a wakup on pcv]
 *      - clear threadop_inprogress, if threadop_wanted: signal on pcv
 *      - done.
 *
 * the progressor_handle uses "hcv" to serialize both start/stop ops
 * (at the handle level) and access to the progressor_stats "ps" pointer.
 * threads block on "hcv" if:
 *
 *   1. they are waiting for "ps" to be null so they can do a stat request
 *       => wakeup: by the thread owning the serialization when it is done
 *   2. they own "ps" and the handle is on the progressor's hlist waiting
 *      for the progress thread to copy the stat info out to "ps"
 *       => wakeup: by the progress thread after it copies the stats out
 *   3. they want to perform a start or stop progress thread op
 *       => wakeup: by the thread owning the serialization when it is done
 *
 * for "hcv" we use hand_busy and hwant_wakup to manage access.  we always
 * use hg_thread_cond_broadcast() for hcv since threads block on "hcv" for
 * multiple reasons.
 */

/*
 * prototypes
 */
static hg_thread_ret_t progress_main(void *arg);  /* at end of file */

/*
 * helper functions
 */

/*
 * cond_init: init cond and its mutex in one op.  return negative on failure.
 */
static int cond_init(hg_thread_cond_t *cvp, hg_thread_mutex_t *lp) {
    if (hg_thread_mutex_init(lp) < 0)
        return(-1);
    if (hg_thread_cond_init(cvp) >= 0)
        return(1);
    (void)hg_thread_mutex_destroy(lp);    /* ignore errors from this */
    return(-1);
}

/*
 * cond_destroy: destroy cond and mutex (assume they are not in use).
 */
static void cond_destroy(hg_thread_cond_t *cvp, hg_thread_mutex_t *lp) {
    (void)hg_thread_cond_destroy(cvp);
    (void)hg_thread_mutex_destroy(lp);
}

/*
 * new_progressor: allocate a new progressor
 */
static struct progressor *new_progressor() {
    struct progressor *pg;
    pg = malloc(sizeof(*pg));
    if (!pg) return(NULL);
    memset(pg, 0, sizeof(*pg));
    pg->progtimeout = DEF_PROGTIMEOUT;
    LIST_INIT(&pg->hlist);
    if (cond_init(&pg->pcv, &pg->plock) < 0) {
        free(pg);
        return(NULL);
    }
    return(pg);
}

/*
 * free_progressor: free a progressor
 */
static void free_progressor(struct progressor *pg) {
    if (pg->prg_refcnt)
        fprintf(stderr,
        "mercury_progressor: warning: free with non-zero refcnt (%d)\n",
                pg->prg_refcnt);
    if (pg->myaddrstring)
        free(pg->myaddrstring);
    cond_destroy(&pg->pcv, &pg->plock);
    free(pg);
}

/*
 * new_handle: allocate a new handle
 */
static struct progressor_handle *new_handle() {
    struct progressor_handle *phand;
    phand = malloc(sizeof(*phand));
    if (!phand) return(NULL);
    memset(phand, 0, sizeof(*phand));
    if (cond_init(&phand->hcv, &phand->hlock) < 0) {
        free(phand);
        return(NULL);
    }
    return(phand);
}

/*
 * free_handle: free a handle
 */
static void free_handle(struct progressor_handle *phand) {
    cond_destroy(&phand->hcv, &phand->hlock);
    free(phand);
}

/*
 * is_progthread: is the current thread the progress thread?
 * mainly used for error checking.
 */
static int is_progthread(struct progressor_handle *phand) {
    struct progressor *pg = phand->myprogressor;
    hg_thread_t me;
    if (!pg->is_running)
        return(0);
    me = hg_thread_self();
    return(hg_thread_equal(me, pg->mythread));
}

/*
 * start_needing_threads: a handle now needs a running progress thread.
 * we bump nrunreqs.  if it was zero before, then we need to perform
 * a start threadop.
 *
 * NOTE: caller must be holding plock to call us
 */
static void start_needing_threads(struct progressor *pg) {

    /* wait until (threadop_inprogress == 0) to serialize access */
    while (pg->threadop_inprogress) {
        pg->threadop_wanted++;
        hg_thread_cond_wait(&pg->pcv, &pg->plock);
        pg->threadop_wanted--;
    }
    /* now we hold plock and threadop_inprogress is zero */

    pg->nrunreqs++;

    /* we can just return if already running */
    if (pg->is_running) {
        return;
    }

    /* we must start the progress thread ourself, as described above */
    pg->threadop_inprogress = 1;    /* we own the threadop */
    pg->do_shutdown = 0;
    if (hg_thread_create(&pg->mythread, progress_main, pg) < 0) {
        fprintf(stderr, "mercury_progressor: hg_thread_create failed!?\n");
        /* XXX: better error handling? */
        abort();
    }
    while (pg->is_running == 0) {
        hg_thread_cond_wait(&pg->pcv, &pg->plock);  /* wait for startup */
    }
    pg->threadop_inprogress = 0;    /* done! */

    if (pg->threadop_wanted)
        hg_thread_cond_signal(&pg->pcv);   /* if others are waiting */
    return;
}

/*
 * stop_needing_threads: a handle no longer needs a running progress
 * thread.  we drop nrunreqs.  if it drops to zero, then we need to
 * perform a stop threadop.
 *
 * NOTE: caller must be holding plock to call us
 */
static void stop_needing_threads(struct progressor *pg) {

    /* wait until (threadop_inprogress == 0) to serialize access */
    while (pg->threadop_inprogress) {
        pg->threadop_wanted++;
        hg_thread_cond_wait(&pg->pcv, &pg->plock);
        pg->threadop_wanted--;
    }
    /* now we hold plock and threadop_inprogress is zero */

    if (pg->nrunreqs > 0)
        pg->nrunreqs--;

    /* we can just return if we no longer need to stop anything */
    if (pg->nrunreqs > 0 || pg->is_running == 0) {
        return;
    }

    /* we must stop the progress thread using the procedure noted above */
    pg->threadop_inprogress = 1;    /* we own the threadop */
    pg->do_shutdown = 1;            /* signals progress thread to exit */
    while (pg->is_running != 0) {
        hg_thread_cond_wait(&pg->pcv, &pg->plock);  /* wait for prog. exit */
    }
    hg_thread_join(pg->mythread);   /* collect exit info */
    memset(&pg->mythread, 0, sizeof(pg->mythread));
    pg->threadop_inprogress = 0;    /* done! */

    if (pg->threadop_wanted)
        hg_thread_cond_signal(&pg->pcv);   /* if others are waiting */
    return;
}

/*
 * load_stats: load stats from the progressor into a progressor_stats
 */
static void load_stats(struct progressor_stats *ps, struct progressor *pg) {
    ps->is_running = pg->is_running;
    /* ps->needed will be filled in by mercury_progressor_getstats() */
    ps->progressor_refcnt = pg->prg_refcnt;
    useprobe_subtime(&ps->runtime, &pg->usage.t1, &pg->usage.t0);
    useprobe_subusage(&ps->runusage, &pg->usage.r1, &pg->usage.r0);
    ps->nprogress = pg->nprogress;
    ps->ntrigger = pg->ntrigger;
}

/*
 * API functions!
 */

/*
 * mercury_progressor_init: allocate and init progressor
 */
progressor_handle_t *mercury_progressor_init(hg_class_t *hgclass,
                                             hg_context_t *hgcontext) {
    hg_addr_t self = NULL;
    char *me = NULL;
    hg_size_t bsize;
    struct progressor *pg = NULL;
    struct progressor_handle *phand = NULL;

    /*
     * first generate our local address string
     * XXX: some na's only can do this if listening, so restrict it
     */
    if (HG_Class_is_listening(hgclass)) {
        if (HG_Addr_self(hgclass, &self) != HG_SUCCESS)
            goto error;
        if (HG_Addr_to_string(hgclass, NULL, &bsize, self) != HG_SUCCESS)
            goto error;
        me = malloc(bsize);
        if (!me)
            goto error;
        if (HG_Addr_to_string(hgclass, me, &bsize, self) != HG_SUCCESS)
            goto error;
        HG_Addr_free(hgclass, self);
        self = NULL;
    }

    /* now setup the progressor */
    if ((pg = new_progressor()) == NULL)
        goto error;
    pg->hg_cls = hgclass;
    pg->hg_ctx = hgcontext;
    pg->myaddrstring = me;     /* progressor now owns this string */
    me = NULL;

    /* and setup the handle itself */
    if ((phand = new_handle()) == NULL)
        goto error;
    phand->myprogressor = pg;
    pg->prg_refcnt++;

    /* done! */
    return(phand);

  error:
    if (self) HG_Addr_free(hgclass, self);
    if (me) free(me);
    if (pg) free_progressor(pg);
    if (phand) free_handle(phand);
    return(NULL);
}

/*
 * mercury_progressor_get_progtimeout: get progtimeout (msec)
 */
unsigned int mercury_progressor_get_progtimeout(progressor_handle_t *hand) {
    return(hand->myprogressor->progtimeout);
}

/*
 * mercury_progressor_set_progtimeout: set progtimeout (msec)
 */
void mercury_progressor_set_progtimeout(progressor_handle_t *hand,
                                        unsigned int timeout) {
    hand->myprogressor->progtimeout = timeout;
}

/*
 * mercury_progressor_duphandle: create a duplicate handle.
 */
progressor_handle_t *mercury_progressor_duphandle(progressor_handle_t *hand) {
    struct progressor_handle *newhand;
    newhand = new_handle();
    if (!newhand)
        return(NULL);

    newhand->myprogressor = hand->myprogressor;
    hg_thread_mutex_lock(&newhand->myprogressor->plock);
    newhand->myprogressor->prg_refcnt++;
    hg_thread_mutex_unlock(&newhand->myprogressor->plock);
    return(newhand);
}

/*
 * mercury_progressor_freehandle: free a progressor handle.   the
 * caller should be holding the last reference to the handle.
 */
hg_return_t mercury_progressor_freehandle(progressor_handle_t *hand) {
    struct progressor *pg = hand->myprogressor;
    int stop_needing, last_ref;

    if (is_progthread(hand)) {   /* sanity check: cannot stop ourself! */
        fprintf(stderr, "mercury_progressor_freehandle: USAGE ERROR\n");
        fprintf(stderr, "cannot call freehandle from progress thread\n");
        abort();
    }

    /* first dispose of the handle */
    hg_thread_mutex_lock(&hand->hlock);
    if (hand->hand_busy || hand->hwant_wakeup) {
        /* XXX: can we recover from this rather than panic? */
        fprintf(stderr, "mercury_progressor_freehandle: handle in use?\n");
        abort();
    }
    stop_needing = (hand->need_count > 0);
    hand->myprogressor = NULL;   /* to be safe, old value is in "pg" */
    hg_thread_mutex_unlock(&hand->hlock);
    free_handle(hand);

    /* now drop reference to the progressor itself */
    hg_thread_mutex_lock(&pg->plock);
    if (stop_needing && pg->nrunreqs > 0)
        stop_needing_threads(pg);            /* holding plock */
    if (pg->prg_refcnt > 0) pg->prg_refcnt--;
    last_ref = (pg->prg_refcnt == 0);
    if (last_ref && pg->nrunreqs != 0) {
        /* XXX: can we recover from this rather than panic? */
        fprintf(stderr,
            "mercury_progressor_freehandle: last ref, but threads running?\n");
        abort();
    }
    hg_thread_mutex_unlock(&pg->plock);
    if (last_ref) {
        free_progressor(pg);
    }

    return(HG_SUCCESS);
}

/*
 * mercury_progressor_getstats: get stats from progress thread
 */
hg_return_t mercury_progressor_getstats(progressor_handle_t *hand,
                                        struct progressor_stats *ps) {
    struct progressor *pg = hand->myprogressor;

    /*
     * first we need to install ps into the handle.  the handle serializes
     * getstats() operations, so we may have to wait our turn if there
     * is some other thread already getting stats.
     */
    hg_thread_mutex_lock(&hand->hlock);

    while (hand->hand_busy) {  /* busy? */
        hand->hwant_wakeup++;
        hg_thread_cond_wait(&hand->hcv, &hand->hlock);
        hand->hwant_wakeup--;
    }

    hand->hand_busy = 1;
    hand->ps = ps;    /* we now own it */
    hg_thread_mutex_lock(&pg->plock);    /* holding hlock and plock */

    if (pg->is_running == 0) {
        /* no prog thread, directly copy old values */
        load_stats(ps, pg);
        hg_thread_mutex_unlock(&pg->plock);
    } else if (is_progthread(hand)) {
        /* we are prog thread, we can update status and return them now */
        useprobe_end(&pg->usage);
        load_stats(ps, pg);
        hg_thread_mutex_unlock(&pg->plock);
    } else {
        hand->need_load = 1;
        LIST_INSERT_HEAD(&pg->hlist, hand, hq);
        hg_thread_mutex_unlock(&pg->plock);

        while (hand->need_load) {
            /*
             * when it gets a chance, the progress thread will walk hlist,
             * calling load_stats() and cleaning need load.
             */
            hand->hwant_wakeup++;
            hg_thread_cond_wait(&hand->hcv, &hand->hlock);
            hand->hwant_wakeup--;
        }
    }
    /* in either case: we hold hlock, have dropped plock */
    hand->ps = NULL;
    hand->hand_busy = 0;
    if (hand->hwant_wakeup)
        hg_thread_cond_broadcast(&hand->hcv);  /* needs to be broadcast */

    /* 'needed' is the only data from handle, we fill it here */
    ps->needed = hand->need_count;

    hg_thread_mutex_unlock(&hand->hlock);
    return(HG_SUCCESS);
}

/*
 * mercury_progressor_nprogress: extract nprogress from progressor
 */
uint64_t mercury_progressor_nprogress(progressor_handle_t *hand) {
    return(hand->myprogressor->nprogress);
}

/*
 * mercury_progressor_ntrigger: extract ntrigger from progressor
 */
uint64_t mercury_progressor_ntrigger(progressor_handle_t *hand) {
    return(hand->myprogressor->ntrigger);
}

/*
 * mercury_progressor_hgclass: extract hg_class_t* from progressor
 */
hg_class_t *mercury_progressor_hgclass(progressor_handle_t *hand) {
    return(hand->myprogressor->hg_cls);
}

/*
 * mercury_progressor_hgcontext: extract hg_context_t* from progressor
 */
hg_context_t *mercury_progressor_hgcontext(progressor_handle_t *hand) {
    return(hand->myprogressor->hg_ctx);
}

/*
 * mercury_progressor_addrstring: extract local address string.
 * valid as long as handle is active, no need to free return value.
 */
char *mercury_progressor_addrstring(progressor_handle_t *hand) {
    return(hand->myprogressor->myaddrstring);
}

/*
 * mercury_progressor_needed: handle needs threads to be running
 */
hg_return_t mercury_progressor_needed(progressor_handle_t *hand) {
    struct progressor *pg = hand->myprogressor;

    hg_thread_mutex_lock(&hand->hlock);

    /* wait until (hand_busy == 0) to serialize access */
    while (hand->hand_busy) {
        hand->hwant_wakeup++;
        hg_thread_cond_wait(&hand->hcv, &hand->hlock);
        hand->hwant_wakeup--;
    }

    hand->need_count++;     /* gain reference */

    /* if thread was already running, we are done */
    if (hand->need_count > 1) {
        goto done;
    }

    /* set busy to block off other threads and unlock */
    hand->hand_busy = 1;
    hg_thread_mutex_unlock(&hand->hlock);

    hg_thread_mutex_lock(&pg->plock);
    start_needing_threads(pg);          /* put progressor to work */
    hg_thread_mutex_unlock(&pg->plock);

    /* now we need to relock and clear hand_busy */
    hg_thread_mutex_lock(&hand->hlock);
    hand->hand_busy = 0;
    if (hand->hwant_wakeup) {
        hg_thread_cond_broadcast(&hand->hcv);
    }

done:
    hg_thread_mutex_unlock(&hand->hlock);
    return(HG_SUCCESS);
}

/*
 * mercury_progressor_idle: progressor handle no longer needs progressor
 * service running.
 */
hg_return_t mercury_progressor_idle(progressor_handle_t *hand) {
    struct progressor *pg = hand->myprogressor;

    if (is_progthread(hand)) {   /* sanity check: cannot stop ourself! */
        fprintf(stderr, "mercury_progressor_idle: USAGE ERROR\n");
        fprintf(stderr, "cannot call idle from progress thread\n");
        abort();
    }

    hg_thread_mutex_lock(&hand->hlock);

    /* wait until (hand_busy == 0) to serialize access */
    while (hand->hand_busy) {
        hand->hwant_wakeup++;
        hg_thread_cond_wait(&hand->hcv, &hand->hlock);
        hand->hwant_wakeup--;
    }

    hand->need_count--;       /* drop reference */

    /* if there will still be a need, we are done */
    if (hand->need_count > 0) {
        goto done;
    }

    /* set busy to block off other threads and unlock */
    hand->hand_busy = 1;
    hg_thread_mutex_unlock(&hand->hlock);

    hg_thread_mutex_lock(&pg->plock);
    stop_needing_threads(pg);          /* don't need thread right now */
    hg_thread_mutex_unlock(&pg->plock);

    /* now we need to relock and clear hand_busy */
    hg_thread_mutex_lock(&hand->hlock);
    hand->hand_busy = 0;
    if (hand->hwant_wakeup) {
        hg_thread_cond_broadcast(&hand->hcv);
    }

done:
    hg_thread_mutex_unlock(&hand->hlock);
    return(HG_SUCCESS);
}

/*
 * progress_main: the main routine of the progress thread
 */
static hg_thread_ret_t progress_main(void *arg) {
    struct progressor *pg = arg;
    hg_context_t *ctx;
    hg_return_t rv;
    unsigned int actual;
    struct handlist tmplist;
    struct progressor_handle *hand;

    /* setup for loop */
    hg_thread_mutex_lock(&pg->plock);
    ctx = pg->hg_ctx;
    useprobe_start(&pg->usage, USEPROBE_THREAD);
    pg->nprogress = pg->ntrigger = 0;
    LIST_INIT(&pg->hlist);    /* just to be safe, should already be empty */
    pg->is_running = 1;
    hg_thread_cond_broadcast(&pg->pcv);  /* wake is_running waiters */
    hg_thread_mutex_unlock(&pg->plock);

    while (pg->do_shutdown == 0) {

        do {
            rv = HG_Trigger(ctx, 0, 1, &actual);  /* triggers callback */
            pg->ntrigger++;   /* no plock, but we are the only writer */
        } while (rv == HG_SUCCESS && actual);
        if (rv != HG_SUCCESS && rv != HG_TIMEOUT) {
            fprintf(stderr, "progress_main: Trigger ERROR %s\n",
                    HG_Error_to_string(rv));
            abort();   /* XXX: what else can we do? */
        }

        rv = HG_Progress(ctx, pg->progtimeout);
        if (rv != HG_SUCCESS && rv != HG_TIMEOUT) {
            fprintf(stderr, "progress_main: Progress ERROR %s\n",
                    HG_Error_to_string(rv));
            abort();   /* XXX: what else can we do? */
        }
        pg->nprogress++;

        /* check hlist (w/o plock, if we miss we'll get it on next loop) */
        if (!LIST_EMPTY(&pg->hlist)) {

            /* first empty hlist into tmplist and update usage */
            LIST_INIT(&tmplist);
            hg_thread_mutex_lock(&pg->plock);
            while ((hand = LIST_FIRST(&pg->hlist)) != NULL) {
                LIST_REMOVE(hand, hq);
                LIST_INSERT_HEAD(&tmplist, hand, hq);
            }
            useprobe_end(&pg->usage);
            hg_thread_mutex_unlock(&pg->plock);

            /* tmplist is private to us... copy out usage and wake waiters */
            while ((hand = LIST_FIRST(&tmplist)) != NULL) {
                LIST_REMOVE(hand, hq);
                hg_thread_mutex_lock(&hand->hlock);
                if (hand->ps && hand->need_load) {  /* just to be safe */
                    load_stats(hand->ps, pg);
                    hand->need_load = 0;
                }
                if (hand->hwant_wakeup) {
                    hg_thread_cond_broadcast(&hand->hcv);
                }
                hg_thread_mutex_unlock(&hand->hlock);
            }
        }
    }

    /* wrap up for exit */
    hg_thread_mutex_lock(&pg->plock);
    useprobe_end(&pg->usage);
    pg->is_running = 0;
    hg_thread_cond_broadcast(&pg->pcv);  /* wake is_running waiters */
    hg_thread_mutex_unlock(&pg->plock);

    return(NULL);
}
