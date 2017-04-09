/* scst_compat.c
 * Compatibility for SCST running in usermode
 * Copyright 2016 David A. Butterfield
 *
 * This provides the interface between the daemon and the kernel code (running in usermode).
 * The daemon runs on the main (TID == PID) thread, and the kernel threads are created using
 * pthread_create.
 *
 * When the daemon "opens the kernel" ctl_dev, we direct the call to SCST_init, which invokes
 * the init functions declared in various parts of the kernel logic and compatibility code,
 * starting the various threads implemented by SCST and the compatibility logic.
 *
 * Daemon ioctl calls to the ctl_dev are also intercepted and directed to SCST_ctldev_ioctl
 * below, which in turn calls into ctr_fops.compat_ioctl.
 *
 * The daemon open of the "netlink" is simulated by a socketpair(2) created in SCST_nl_open
 * (below), with one end of the socket being returned to the daemon as its "netlink" endpoint.
 * From the kernel side, our implementation of event_send writes events to the other end of
 * the socketpair, which arrives at the daemon thread as a POLLIN ready on its "netlink".
 *
 * The SIGINT handler (below) begins a clean shutdown by closing the kernel end of the socket
 * pair:  the daemon receives this as an event on its netlink file descriptor and initiates
 * shutdown by calling SCST_exit below.  This calls all the various exit functions counterpart
 * to the init functions previously called by SCST_init.
 */
#define NAME SCST_COMPAT

#ifndef SCST_COMPAT_H
  #error Makefile failed to force-include scst_compat.h as required
#endif

#include "iscsi_trace_flag.h"
#include "iscsi.h"

extern errno_t UMC_INIT_init_scst(void);			/* scst_main.c */
extern errno_t UMC_INIT_init_scst_vdisk_driver(void);		/* scst_vdisk.c */
extern errno_t UMC_INIT_iscsi_init(void);			/* iscsi.c */

extern void SCST_param_create_num_threads(void);
extern void SCST_param_create_scst_vdisk_ID(void);
extern void SCST_param_create_scst_threads(void);
extern void SCST_param_create_scst_max_cmd_mem(void);
extern void SCST_param_create_scst_max_dev_cmd_mem(void);
extern void SCST_param_create_forcibly_close_sessions(void);

extern int scst_threads;

static void sigint_handler(uint32_t signum);
static void sigquit_handler(uint32_t signum);

#include "mtelib.h"				/* system service Implementor */

/* Master init() call for all the SCST "kernel modules" --
 * called from daemon:  main() => kernel_open() => create_and_open_dev();
 * The returned "file descriptor" is just a token we make up, passed back to SCST_ctldev_ioctl
 */
int
SCST_init(const char *dev, int readonly)
{
    errno_t err;

    /* Call the various module init functions -- we must know all their names here, which
     * are automatically generated by the module_init and module_exit macros
     */

    sys_service_set(MTE_sys_service_get());	/* install MTE as sys_service provider */

    sys_service_init(NULL/*cfg*/);  /* initialize sys_service provider, sys_thread_current */
    /*** Now we have a memory allocator ***/

    err = UMC_init("/fuse/scst/proc");		/* usermode_lib.c must be first after sys */
    verify_noerr(err, "UMC_init");

    mte_signal_handler_set(SIGINT, sigint_handler);
    mte_signal_handler_set(SIGQUIT, sigquit_handler);

    err = UMC_INIT_init_scst();			/* scst_main.c */
    verify_noerr(err, "init_scst");

    err = UMC_INIT_init_scst_vdisk_driver();    /* scst_vdisk.c */
    verify_noerr(err, "init_scst_vdisk_driver");

    err = UMC_INIT_iscsi_init();		/* iscsi.c */
    verify_noerr(err, "iscsi_init");

    SCST_param_create_num_threads(); 
    SCST_param_create_scst_vdisk_ID(); 
    SCST_param_create_scst_threads(); 
    SCST_param_create_scst_max_cmd_mem(); 
    SCST_param_create_scst_max_dev_cmd_mem(); 
    SCST_param_create_forcibly_close_sessions(); 

    err = ctr_fops.open(NULL, NULL);
    verify_eq(err, E_OK);

    assert(!strcmp(dev, "iscsi-scst-ctl"));
    return 12345;   //XXX   "fd"
}

/* Called from daemon code to issue an "ioctl" request to the kernel code */
int
SCST_ctldev_ioctl(int fd_arg, unsigned int cmd, unsigned long arg)
{
    assert_eq(fd_arg, 12345);	//XXX

    /* The return value isn't necessarily an error; e.g. ADD_TARGET returns tid */
    long ret = ctr_fops.compat_ioctl(NULL, cmd, arg);

    trace("ctr_fops.compat_ioctl cmd=%u/0x%x arg=%"PRIu64"/0x%"PRIx64" returns %"PRId64"/%"PRIx64,
	  cmd, cmd, arg, arg, ret, ret);

    if (ret < 0) {
	/* Translate kernel-style error return to usermode-style */
	errno = -ret;
	ret = -1;
    }
    return ret;
}

extern void SCST_param_remove_num_threads(void);
extern void SCST_param_remove_scst_vdisk_ID(void);
extern void SCST_param_remove_scst_threads(void);
extern void SCST_param_remove_scst_max_cmd_mem(void);
extern void SCST_param_remove_scst_max_dev_cmd_mem(void);
extern void SCST_param_remove_forcibly_close_sessions(void);

extern void UMC_EXIT_exit_scst(void);
extern void UMC_EXIT_exit_scst_vdisk_driver(void);
extern void UMC_EXIT_iscsi_exit(void);

static int SCST_nl_fdwrite = -1;    /* kernel end of nl_fd socket */

void SCST_exit(void);

void
SCST_exit(void)
{
    errno_t err = ctr_fops.release(NULL, NULL);
    verify_eq(err, E_OK);

    SCST_param_remove_num_threads(); 
    SCST_param_remove_scst_vdisk_ID(); 
    SCST_param_remove_scst_threads(); 
    SCST_param_remove_scst_max_cmd_mem(); 
    SCST_param_remove_scst_max_dev_cmd_mem(); 
    SCST_param_remove_forcibly_close_sessions(); 

    UMC_EXIT_iscsi_exit();
    UMC_EXIT_exit_scst_vdisk_driver();
    UMC_EXIT_exit_scst();

    UMC_exit();		    /* penultimate before sys_service_fini() */
    sys_service_fini();	    /* frees memory allocator */

    MTE_sys_service_put(SYS_SERVICE);
    sys_service_set(NULL);
}

/* These signal handlers are invoked off the itqthread event loop,
 * not an asynchronous signal delivery
 */

/* Start a clean shutdown, freeing all allocations for a short valgrind output */
static void
sigint_handler(uint32_t signum)
{
    assert_eq(signum, SIGINT);
    static bool shutdown_started = false;

    if (shutdown_started) {
	sys_warning("Recursive SIGINT->SCST_shutdown ignored");
    } else {
	shutdown_started = true;
	/* Close the kernel end of nl_fd, to get daemon thread to drive the exit
	 * back through SCST_exit
	 */
	assert(SCST_nl_fdwrite != -1);
	sys_notice("SCST usermode shutdown initiated by SIGINT");
	int rc = close(SCST_nl_fdwrite);
	expect_rc(rc, close, "close(SCST_nl_fdwrite)");
	SCST_nl_fdwrite = -1;
    }
}

extern void sigint_hack(void);
void
sigint_hack(void)
{
    sigint_handler(SIGINT);
}

/* Exit the program with some diagnostics but no attempt to clean up state or memory */
static void
sigquit_handler(uint32_t signum)
{
    assert_eq(signum, SIGQUIT);
    //XXX dump something useful here...
    exit(SIGQUIT);
}

/******************************************************************************/

/* Called from daemon code to open an event-notification "nl" socket */
int
SCST_nl_open(void)
{
    int fd[2];
    errno_t err = UMC_kernelize(socketpair(AF_LOCAL,
					   SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC, 0, fd));
    verify_rc(err, socketpair);
    SCST_nl_fdwrite = fd[1];
    return fd[0];
}

#include <linux/netlink.h>

/* Called from kernel code to issue a notification to the daemon through the "nl" socket */
// XXX Does the data need to be copied?  I don't think so, but should check
errno_t
event_send(uint32_t tid, uint64_t sid, uint32_t cid, uint32_t cookie,
	   enum iscsi_kern_event_code code, const char * param1, const char * param2)
{ 
    struct nlmsghdr hdr = {
	.nlmsg_len   = IGNORED,	    /* Length of message including header */
	.nlmsg_type  = IGNORED,     /* Message content */
	.nlmsg_flags = IGNORED,     /* Additional flags */
	.nlmsg_seq   = IGNORED,     /* Sequence number */
	.nlmsg_pid   = IGNORED,     /* Sending process port ID */
    };

    struct iscsi_kern_event event = {
	.tid = tid,
	.sid = sid,
	.cid = cid,
	.code = code,
	.cookie = cookie,
	.param1_size = param1 ? strlen(param1) : 0,
	.param2_size = param2 ? strlen(param2) : 0,
    };

    /* Assumes writev(2) is OK with zero-length iov_len elements (XXXX check) */
    struct iovec iov[] = {
	[0] = { .iov_base = &hdr,		 .iov_len = sizeof(hdr)       },
	[1] = { .iov_base = &event,		 .iov_len = sizeof(event)     },
	[2] = { .iov_base = _unconstify(param1), .iov_len = event.param1_size },   //XXX
	[3] = { .iov_base = _unconstify(param2), .iov_len = event.param2_size },   //XXX
    };

    return UMC_kernelize(writev(SCST_nl_fdwrite, iov, ARRAY_SIZE(iov)));
}

/******************************************************************************/

extern __thread char sys_pthread_name[16];

/* Callback when an AIO thread is created to set up a "current" pointer for it --
 * the AIO thread calls back into "kernel" code which expects this
 */
extern void aios_thread_init(void * unused);	//XXX
void
aios_thread_init(void * unused)
{
    expect_eq(unused, NULL);
    UMC_current_set(
	    UMC_current_init(
		    UMC_current_alloc(),
		    sys_thread_current(),
		    (void *)aios_thread_init,
		    unused,
		    kstrdup(sys_pthread_name, IGNORED)));

    errno_t err = pthread_setname_np(pthread_self(), sys_pthread_name);
    expect_noerr(err, "pthread_setname_np");
}

extern void aios_thread_exit(void * unused);	//XXX
void
aios_thread_exit(void * unused)
{
    expect_eq(unused, NULL);
    assert(current);
    UMC_current_free(current);
    UMC_current_set(NULL);
}

/******************************************************************************/

#include "scst.h"

int event_init(void)	{ return E_OK; }
void event_exit(void)	{ }

extern int scst_event_init(void);   int scst_event_init(void)	{ return E_OK; }
extern void scst_event_exit(void);  void scst_event_exit(void)	{ }

struct scst_cmd;
struct scst_tgt;
struct scst_mgmt_cmd;
struct scst_event_entry;
struct scst_tgt_template;
struct scst_dev_type;

extern void scst_event_queue(uint32_t event_code, const char *issuer_name, struct scst_event_entry *e);

extern int scst_event_queue_lun_not_found(const struct scst_cmd *cmd);
extern int scst_event_queue_negative_luns_inquiry(const struct scst_tgt *tgt, const char *initiator_name);
extern int scst_event_queue_tm_fn_received(struct scst_mgmt_cmd *mcmd);

void scst_event_queue(uint32_t event_code, const char *issuer_name, struct scst_event_entry *e) \
	    { FATAL(scst_event_queue); }
int scst_event_queue_lun_not_found(const struct scst_cmd *cmd) \
	    { sys_warning("SKIP queuing event scst_event_queue_lun_not_found"); return E_OK; }
int scst_event_queue_negative_luns_inquiry(const struct scst_tgt *tgt, const char *initiator_name) \
	    { sys_warning("SKIP queuing event scst_event_queue_negative_luns_inquiry"); return E_OK; }
int scst_event_queue_tm_fn_received(struct scst_mgmt_cmd *mcmd) \
	    { sys_warning("SKIP queuing event scst_event_queue_tm_fn_received"); return E_OK; }

int scsi_reset_provider(struct scsi_device * sdev, int flags) { FATAL(scsi_reset_provider); }
