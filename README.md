# mercury-progressor

The mercury-progressor library provides a service for sharing and
managing a progress/trigger thread among multiple independent mercury-based
services that want to use the same instance of mercury for RPC service.

# API

The mercury-progressor API is similar to the classic POSIX file
descriptor model.  It provides a reference descriptor/handle to a
progress/trigger thread that is thread-safe and can be dup'd and
shared by mercury-based services.   It also provides a mechanism
for accessing the progress/trigger thread's usage stats.

## application initialization

An application that wants to use the mercury-progressor service
must first initialize mercury using the standard HG_Init()/HG_Init_opt()
and HG_Context_create() calls.   Once that has been done, then
the progressor can be initialized using the init function:

```
/*
 * returns a handle to a freshly allocated mercury progressor.
 */
progressor_handle_t *mercury_progressor_init(hg_class_t *hgclass,
                                             hg_context_t *hgcontext);
```

The progressor handle returned by the init function may then
be passed to services that wish to share this instance of mercury.

When the application is complete, it should release its reference
to the progressor handle using:

```
/*
 * drop a handle reference to a progressor.  if this is the final
 * reference to the progressor, then the progressor is freed.
 */
hg_return_t mercury_progressor_freehandle(progressor_handle_t *hand);
```

## service library usage

A service that uses mercury-progressor should provide an init function
that includes a mercury-progressor handle as part of its argument.
The progressor handle provide a mechanism for the service to access
the shared instance of mercury.  Services that use mercury-progressor
do not call HG_Init()/HG_Init_opt() themselves.

The mercury-progressor service provides the following API calls to
provide access to the underlying instance of mercury:

```
/* access mercury instance's class and context */
hg_class_t *mercury_progressor_hgclass(progressor_handle_t *hand);
hg_context_t *mercury_progressor_hgcontext(progressor_handle_t *hand);

/* return local address string */
char *mercury_progressor_addrstring(progressor_handle_t *hand);
```

The return values of these calls can be cached.   They will remain
valid as long as there is an active reference to the mercury progressor.
Note that due to mercury limitations, addrstring() only works if mercury
was init'd with the "listen" flag set.   If the "listen" was not
set, then addrstring() always returns NULL.

Most service libraries that use mercury-progressor will want to
retain their own private reference to the progressor.  This is
normally done in the service library init function:

```
/*
 * create a new handle that is a dup of the input.  must be freed
 * when the handle is no longer needed.
 */
progressor_handle_t *mercury_progressor_duphandle(progressor_handle_t *hand);
```

When a service that is using mercury-progressor needs to have the
progress/trigger thread running (e.g. for RPC callback processing)
it calls:

```
/*
 * start progress/trigger thread (if needed) and add a reference to it
 */
hg_return_t mercury_progressor_needed(progressor_handle_t *hand);
```

When the service no longer needs a running progress/trigger thread,
it can call:

```
/*
 * drop reference to progress/trigger thread.  stop thread if no longer needed.
 */
hg_return_t mercury_progressor_idle(progressor_handle_t *hand);
```

## runtime stats

The progressor keeps track of its resource usage using counters and
the POSIX rusage data structure.   Service libraries using progressor
can query the progress/trigger thread's resource usage using the
progressor_stats structure and the mercury_progressor_getstats()
API call:

```
/*
 * progressor_stats: run stats for a progressor and its handle.
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

/*
 * get latest stats (blocks until thread can fill "ps").
 */
hg_return_t mercury_progressor_getstats(progressor_handle_t *hand,
                                        struct progressor_stats *ps);
```

Note that the getstats function may block waiting for the progress/trigger
thread to call getrusage.   The values of nprogress and ntrigger can
be accessed directly without blocking using the following two functions:

```
uint64_t mercury_progressor_nprogress(progressor_handle_t *hand);
uint64_t mercury_progressor_ntrigger(progressor_handle_t *hand);
```

## usage notes

All services that use mercury-progressor must provide an init
function that takes a progressor_stats as an argument.  The
progressor_stats argument can be used to access the mercury
class and context.  Services must document the cases where they
require a mercury class that has been init'd in "listen" mode
(i.e. "na_listen == TRUE").  If a service requires mercury to
be in "listen" mode, it should use HG_Class_is_listening() to
verify mercury is in the correct mode and fail if not.  Applications
must init mercury in "listen" mode if any one of the services
they are using requires "listen" mode.

Since the mercury progress/trigger thread is shared among service
libraries, there is a good chance that the progress/trigger thread
is already running when a service library init function is called.
Thus, service libraries should be prepared to have their RPC handle
function called as soon as it is registered (e.g. with HG_Register_name())
even if they haven't called mercury_progressor_needed() yet.

An easy way to handle this is to use the per-RPC pointer managed by
HG_Register_data()/HG_Registered_data() to indicate if the service
library is ready to handle inbound RPC requests.  For example, if
the RPC handle function finds that HG_Registered_data() returns NULL,
then it can assume the service is not ready and the inbound RPC
should be dropped.

Also, note that you cannot call freehandle or idle from an RPC callback
function.  This is because RPC callbacks run in the context of the
progress thread, and we don't want the progress thread to deadlock
waiting for itself to exit.  The freehandle and idle functions check
for this illegal usage and abort the program if it is detected.
