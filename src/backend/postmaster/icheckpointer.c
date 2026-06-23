/** 

*/
#include "postgres.h"

#include "miscadmin.h"
#include "libpq/pqsignal.h"
#include "postmaster/bgwriter.h"
#include "postmaster/interrupt.h"
#include "replication/walsender.h"
#include "storage/bufmgr.h"
#include "storage/buf_internals.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/procsignal.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/wait_event.h"
/*
 * GUC parameters
*/
int PGDLLIMPORT ICheckpointTimeout = 10;
bool PGDLLIMPORT enableIckpt = true;

/*
 * Process any new interrupts
*/
static void
HandleICheckpointerInterrupts(void)
{
	if (ProcSignalBarrierPending)
		ProcessProcSignalBarrier();

	if (ConfigReloadPending)
	{
		ConfigReloadPending = false;
		ProcessConfigFile(PGC_SIGHUP);
	}

	if (ShutdownRequestPending)
	{
		/*
		 * From here on, elog(ERROR) should end with exit(1), not send control
		 * back to the sigsetjmp block above.
		 */
		ExitOnAnyError = true;

		proc_exit(0);
	}
}


void ICheckpointerMain(void)
{
    sigjmp_buf local_sigjmp_buf;
    MemoryContext icheckpointer_context;

  	/*
	 * Properly accept or ignore signals the postmaster might send us
	 *
	 * Note: we deliberately ignore SIGTERM, because during a standard Unix
	 * system shutdown cycle, init will SIGTERM all processes at once.  We
	 * want to wait for the backends to exit, whereupon the postmaster will
	 * tell us it's okay to shut down (via SIGUSR2).
	 */
	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGINT, SIG_IGN); /* request checkpoint */
	pqsignal(SIGTERM, SignalHandlerForShutdownRequest); /* ignore SIGTERM */
	/* SIGQUIT handler was already set up by InitPostmasterChild */
	pqsignal(SIGALRM, SIG_IGN);
	pqsignal(SIGPIPE, SIG_IGN);
	pqsignal(SIGUSR1, procsignal_sigusr1_handler);
	pqsignal(SIGUSR2, SignalHandlerForShutdownRequest);

    /*
	 * Reset some signals that are accepted by postmaster but not here
	 */
	pqsignal(SIGCHLD, SIG_DFL);

    /*
	 * Create a memory context that we will do all our work in.  We do this so
	 * that we can reset the context during error recovery and thereby avoid
	 * possible memory leaks.  Formerly this code just ran in
	 * TopMemoryContext, but resetting that would be a really bad idea.
	 */
	icheckpointer_context = AllocSetContextCreate(TopMemoryContext,
												 "ICheckpointer",
												 ALLOCSET_DEFAULT_SIZES);
	MemoryContextSwitchTo(icheckpointer_context);

    /*
	 * If an exception is encountered, processing resumes here.
	 *
	 * You might wonder why this isn't coded as an infinite loop around a
	 * PG_TRY construct.  The reason is that this is the bottom of the
	 * exception stack, and so with PG_TRY there would be no exception handler
	 * in force at all during the CATCH part.  By leaving the outermost setjmp
	 * always active, we have at least some chance of recovering from an error
	 * during error recovery.  (If we get into an infinite loop thereby, it
	 * will soon be stopped by overflow of elog.c's internal state stack.)
	 *
	 * Note that we use sigsetjmp(..., 1), so that the prevailing signal mask
	 * (to wit, BlockSig) will be restored when longjmp'ing to here.  Thus,
	 * signals other than SIGQUIT will be blocked until we complete error
	 * recovery.  It might seem that this policy makes the HOLD_INTERRUPTS()
	 * call redundant, but it is not since InterruptPending might be set
	 * already.
	 */
	if (sigsetjmp(local_sigjmp_buf, 1) != 0)
	{
		/* Since not using PG_TRY, must reset error stack by hand */
		error_context_stack = NULL;

		/* Prevent interrupts while cleaning up */
		HOLD_INTERRUPTS();

		/* Report the error to the server log */
		EmitErrorReport();

		/*
		 * These operations are really just a minimal subset of
		 * AbortTransaction().  We don't have very many resources to worry
		 * about in checkpointer, but we do have LWLocks, buffers, and temp
		 * files.
		 */
		LWLockReleaseAll();
		ConditionVariableCancelSleep();
		pgstat_report_wait_end();
		AbortBufferIO();
		UnlockBuffers();
		ReleaseAuxProcessResources(false);
		AtEOXact_Buffers(false);
		AtEOXact_SMgr();
		AtEOXact_Files(false);
		AtEOXact_HashTables(false);

		/*
		 * Now return to normal top-level context and clear ErrorContext for
		 * next time.
		 */
		MemoryContextSwitchTo(icheckpointer_context);
		FlushErrorState();

		/* Flush any leaked data in the top-level context */
		MemoryContextResetAndDeleteChildren(icheckpointer_context);

		/* Now we can allow interrupts again */
		RESUME_INTERRUPTS();

		/*
		 * Sleep at least 1 second after any error.  A write error is likely
		 * to be repeated, and we don't want to be filling the error logs as
		 * fast as we can.
		 */
		pg_usleep(1000000L);

		/*
		 * Close all open files after any error.  This is helpful on Windows,
		 * where holding deleted files open causes various strange errors.
		 * It's not clear we need it elsewhere, but shouldn't hurt.
		 */
		smgrcloseall();
	}

    /* We can now handle ereport(ERROR) */
	PG_exception_stack = &local_sigjmp_buf;

    /*
	 * Unblock signals (they were blocked when the postmaster forked us)
	 */
	sigprocmask(SIG_SETMASK, &UnBlockSig, NULL);

    /*
     * Loop forever
     */
	for (;;)
	{
		WritebackContext wb_context;

		/* Clear any already-pending wakeups */
		ResetLatch(MyLatch);

		/*
		 * Process any requests or signals received recently.
		 */
		HandleICheckpointerInterrupts();

		if (enableIckpt)
		{
			/*
			 * Incrementally flush dirty buffers from the checkpoint ring
			 * buffer.
			 */
			WritebackContextInit(&wb_context, &checkpoint_flush_after);

			ckptRingBufBufferSync(&wb_context, false);

			/* Issue any pending writebacks */
			IssuePendingWritebacks(&wb_context);
		}

		/* Process any interrupts that arrived while we were working */
		HandleICheckpointerInterrupts();

		/*
		 * Sleep until we are signaled or one second has elapsed.  We wake up
		 * every second so that we can flush buffers at a steady rate.
		 */
		(void) WaitLatch(MyLatch,
						 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 1000L /* 1 second */ ,
						 WAIT_EVENT_CHECKPOINTER_MAIN);
	}
}

