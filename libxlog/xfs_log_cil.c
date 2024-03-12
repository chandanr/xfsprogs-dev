#include "libxfs.h"
#include "libxlog.h"
#include "xfs_log_priv.h"

static inline void
xlog_cil_set_iclog_hdr_count(struct xfs_cil *cil)
{
	struct xlog	*log = cil->xc_log;

	atomic_set(&cil->xc_iclog_hdrs,
		   (XLOG_CIL_BLOCKING_SPACE_LIMIT(log) /
			(log->l_iclog_size - log->l_iclog_hsize)));
}

/*
 * Unavoidable forward declaration - xlog_cil_push_work() calls
 * xlog_cil_ctx_alloc() itself.
 */
static void xlog_cil_push_work(struct work_struct *work);

static struct xfs_cil_ctx *
xlog_cil_ctx_alloc(void)
{
	struct xfs_cil_ctx	*ctx;

	ctx = kmem_zalloc(sizeof(*ctx), KM_NOFS);
	INIT_LIST_HEAD(&ctx->committing);
	INIT_LIST_HEAD(&ctx->busy_extents.extent_list);
	INIT_LIST_HEAD(&ctx->log_items);
	INIT_LIST_HEAD(&ctx->lv_chain);
	INIT_WORK(&ctx->push_work, xlog_cil_push_work);
	return ctx;
}

static void
xlog_cil_ctx_switch(
	struct xfs_cil		*cil,
	struct xfs_cil_ctx	*ctx)
{
	xlog_cil_set_iclog_hdr_count(cil);
	set_bit(XLOG_CIL_EMPTY, &cil->xc_flags);
	set_bit(XLOG_CIL_PCP_SPACE, &cil->xc_flags);
	ctx->sequence = ++cil->xc_current_sequence;
	ctx->cil = cil;
	cil->xc_ctx = ctx;
}

/*
 * Push the Committed Item List to the log.
 *
 * If the current sequence is the same as xc_push_seq we need to do a flush. If
 * xc_push_seq is less than the current sequence, then it has already been
 * flushed and we don't need to do anything - the caller will wait for it to
 * complete if necessary.
 *
 * xc_push_seq is checked unlocked against the sequence number for a match.
 * Hence we can allow log forces to run racily and not issue pushes for the
 * same sequence twice.  If we get a race between multiple pushes for the same
 * sequence they will block on the first one and then abort, hence avoiding
 * needless pushes.
 */
static void
xlog_cil_push_work(		/* chandan: do this later */
	struct work_struct	*work)
{
	struct xfs_cil_ctx	*ctx =
		container_of(work, struct xfs_cil_ctx, push_work);
	struct xfs_cil		*cil = ctx->cil;
	struct xlog		*log = cil->xc_log;
	struct xfs_cil_ctx	*new_ctx;
	int			num_iovecs = 0;
	int			num_bytes = 0;
	int			error = 0;
	struct xlog_cil_trans_hdr thdr;
	struct xfs_log_vec	lvhdr = {};
	xfs_csn_t		push_seq;
	bool			push_commit_stable;
	LIST_HEAD		(whiteouts);
	struct xlog_ticket	*ticket;

	new_ctx = xlog_cil_ctx_alloc();
	new_ctx->ticket = xlog_cil_ticket_alloc(log);

	down_write(&cil->xc_ctx_lock);

	spin_lock(&cil->xc_push_lock);
	push_seq = cil->xc_push_seq;
	ASSERT(push_seq <= ctx->sequence);
	push_commit_stable = cil->xc_push_commit_stable;
	cil->xc_push_commit_stable = false;

	/*
	 * As we are about to switch to a new, empty CIL context, we no longer
	 * need to throttle tasks on CIL space overruns. Wake any waiters that
	 * the hard push throttle may have caught so they can start committing
	 * to the new context. The ctx->xc_push_lock provides the serialisation
	 * necessary for safely using the lockless waitqueue_active() check in
	 * this context.
	 */
	if (waitqueue_active(&cil->xc_push_wait))
		wake_up_all(&cil->xc_push_wait);

	xlog_cil_push_pcp_aggregate(cil, ctx);

	/*
	 * Check if we've anything to push. If there is nothing, then we don't
	 * move on to a new sequence number and so we have to be able to push
	 * this sequence again later.
	 */
	if (test_bit(XLOG_CIL_EMPTY, &cil->xc_flags)) {
		cil->xc_push_seq = 0;
		spin_unlock(&cil->xc_push_lock);
		goto out_skip;
	}


	/* check for a previously pushed sequence */
	if (push_seq < ctx->sequence) {
		spin_unlock(&cil->xc_push_lock);
		goto out_skip;
	}

	/*
	 * We are now going to push this context, so add it to the committing
	 * list before we do anything else. This ensures that anyone waiting on
	 * this push can easily detect the difference between a "push in
	 * progress" and "CIL is empty, nothing to do".
	 *
	 * IOWs, a wait loop can now check for:
	 *	the current sequence not being found on the committing list;
	 *	an empty CIL; and
	 *	an unchanged sequence number
	 * to detect a push that had nothing to do and therefore does not need
	 * waiting on. If the CIL is not empty, we get put on the committing
	 * list before emptying the CIL and bumping the sequence number. Hence
	 * an empty CIL and an unchanged sequence number means we jumped out
	 * above after doing nothing.
	 *
	 * Hence the waiter will either find the commit sequence on the
	 * committing list or the sequence number will be unchanged and the CIL
	 * still dirty. In that latter case, the push has not yet started, and
	 * so the waiter will have to continue trying to check the CIL
	 * committing list until it is found. In extreme cases of delay, the
	 * sequence may fully commit between the attempts the wait makes to wait
	 * on the commit sequence.
	 */
	list_add(&ctx->committing, &cil->xc_committing);
	spin_unlock(&cil->xc_push_lock);

	xlog_cil_build_lv_chain(ctx, &whiteouts, &num_iovecs, &num_bytes);

	/*
	 * Switch the contexts so we can drop the context lock and move out
	 * of a shared context. We can't just go straight to the commit record,
	 * though - we need to synchronise with previous and future commits so
	 * that the commit records are correctly ordered in the log to ensure
	 * that we process items during log IO completion in the correct order.
	 *
	 * For example, if we get an EFI in one checkpoint and the EFD in the
	 * next (e.g. due to log forces), we do not want the checkpoint with
	 * the EFD to be committed before the checkpoint with the EFI.  Hence
	 * we must strictly order the commit records of the checkpoints so
	 * that: a) the checkpoint callbacks are attached to the iclogs in the
	 * correct order; and b) the checkpoints are replayed in correct order
	 * in log recovery.
	 *
	 * Hence we need to add this context to the committing context list so
	 * that higher sequences will wait for us to write out a commit record
	 * before they do.
	 *
	 * xfs_log_force_seq requires us to mirror the new sequence into the cil
	 * structure atomically with the addition of this sequence to the
	 * committing list. This also ensures that we can do unlocked checks
	 * against the current sequence in log forces without risking
	 * deferencing a freed context pointer.
	 */
	spin_lock(&cil->xc_push_lock);
	xlog_cil_ctx_switch(cil, new_ctx);
	spin_unlock(&cil->xc_push_lock);
	up_write(&cil->xc_ctx_lock);

	/*
	 * Sort the log vector chain before we add the transaction headers.
	 * This ensures we always have the transaction headers at the start
	 * of the chain.
	 */
	list_sort(NULL, &ctx->lv_chain, xlog_cil_order_cmp);

	/*
	 * Build a checkpoint transaction header and write it to the log to
	 * begin the transaction. We need to account for the space used by the
	 * transaction header here as it is not accounted for in xlog_write().
	 * Add the lvhdr to the head of the lv chain we pass to xlog_write() so
	 * it gets written into the iclog first.
	 */
	xlog_cil_build_trans_hdr(ctx, &thdr, &lvhdr, num_iovecs);
	num_bytes += lvhdr.lv_bytes;
	list_add(&lvhdr.lv_list, &ctx->lv_chain);

	/*
	 * Take the lvhdr back off the lv_chain immediately after calling
	 * xlog_cil_write_chain() as it should not be passed to log IO
	 * completion.
	 */
	error = xlog_cil_write_chain(ctx, num_bytes);
	list_del(&lvhdr.lv_list);
	if (error)
		goto out_abort_free_ticket;

	error = xlog_cil_write_commit_record(ctx);
	if (error)
		goto out_abort_free_ticket;

	/*
	 * Grab the ticket from the ctx so we can ungrant it after releasing the
	 * commit_iclog. The ctx may be freed by the time we return from
	 * releasing the commit_iclog (i.e. checkpoint has been completed and
	 * callback run) so we can't reference the ctx after the call to
	 * xlog_state_release_iclog().
	 */
	ticket = ctx->ticket;

	/*
	 * If the checkpoint spans multiple iclogs, wait for all previous iclogs
	 * to complete before we submit the commit_iclog. We can't use state
	 * checks for this - ACTIVE can be either a past completed iclog or a
	 * future iclog being filled, while WANT_SYNC through SYNC_DONE can be a
	 * past or future iclog awaiting IO or ordered IO completion to be run.
	 * In the latter case, if it's a future iclog and we wait on it, the we
	 * will hang because it won't get processed through to ic_force_wait
	 * wakeup until this commit_iclog is written to disk.  Hence we use the
	 * iclog header lsn and compare it to the commit lsn to determine if we
	 * need to wait on iclogs or not.
	 */
	spin_lock(&log->l_icloglock);
	if (ctx->start_lsn != ctx->commit_lsn) {
		xfs_lsn_t	plsn;

		plsn = be64_to_cpu(ctx->commit_iclog->ic_prev->ic_header.h_lsn);
		if (plsn && XFS_LSN_CMP(plsn, ctx->commit_lsn) < 0) {
			/*
			 * Waiting on ic_force_wait orders the completion of
			 * iclogs older than ic_prev. Hence we only need to wait
			 * on the most recent older iclog here.
			 */
			xlog_wait_on_iclog(ctx->commit_iclog->ic_prev);
			spin_lock(&log->l_icloglock);
		}

		/*
		 * We need to issue a pre-flush so that the ordering for this
		 * checkpoint is correctly preserved down to stable storage.
		 */
		ctx->commit_iclog->ic_flags |= XLOG_ICL_NEED_FLUSH;
	}

	/*
	 * The commit iclog must be written to stable storage to guarantee
	 * journal IO vs metadata writeback IO is correctly ordered on stable
	 * storage.
	 *
	 * If the push caller needs the commit to be immediately stable and the
	 * commit_iclog is not yet marked as XLOG_STATE_WANT_SYNC to indicate it
	 * will be written when released, switch it's state to WANT_SYNC right
	 * now.
	 */
	ctx->commit_iclog->ic_flags |= XLOG_ICL_NEED_FUA;
	if (push_commit_stable &&
	    ctx->commit_iclog->ic_state == XLOG_STATE_ACTIVE)
		xlog_state_switch_iclogs(log, ctx->commit_iclog, 0);
	ticket = ctx->ticket;
	xlog_state_release_iclog(log, ctx->commit_iclog, ticket);

	/* Not safe to reference ctx now! */

	spin_unlock(&log->l_icloglock);
	xlog_cil_cleanup_whiteouts(&whiteouts);
	xfs_log_ticket_ungrant(log, ticket);
	return;

out_skip:
	up_write(&cil->xc_ctx_lock);
	xfs_log_ticket_put(new_ctx->ticket);
	kmem_free(new_ctx);
	return;

out_abort_free_ticket:
	ASSERT(xlog_is_shutdown(log));
	xlog_cil_cleanup_whiteouts(&whiteouts);
	if (!ctx->commit_iclog) {
		xfs_log_ticket_ungrant(log, ctx->ticket);
		xlog_cil_committed(ctx);
		return;
	}
	spin_lock(&log->l_icloglock);
	ticket = ctx->ticket;
	xlog_state_release_iclog(log, ctx->commit_iclog, ticket);
	/* Not safe to reference ctx now! */
	spin_unlock(&log->l_icloglock);
	xfs_log_ticket_ungrant(log, ticket);
}

/*
 * Perform initial CIL structure initialisation.
 */
int
xlog_cil_init(
	struct xlog		*log)
{
	struct xfs_cil		*cil;
	struct xfs_cil_ctx	*ctx;
	struct xlog_cil_pcp	*cilpcp;
	int			cpu;

	cil = kmem_zalloc(sizeof(*cil), KM_MAYFAIL);
	if (!cil)
		return -ENOMEM;
	/*
	 * Limit the CIL pipeline depth to 4 concurrent works to bound the
	 * concurrency the log spinlocks will be exposed to.
	 */
	cil->xc_push_wq = alloc_workqueue("xfs-cil/%s",
			XFS_WQFLAGS(WQ_FREEZABLE | WQ_MEM_RECLAIM | WQ_UNBOUND),
			4, log->l_mp->m_super->s_id);
	if (!cil->xc_push_wq)
		goto out_destroy_cil;

	cil->xc_log = log;
	cil->xc_pcp = alloc_percpu(struct xlog_cil_pcp);
	if (!cil->xc_pcp)
		goto out_destroy_wq;

	for_each_possible_cpu(cpu) {
		cilpcp = per_cpu_ptr(cil->xc_pcp, cpu);
		INIT_LIST_HEAD(&cilpcp->busy_extents);
		INIT_LIST_HEAD(&cilpcp->log_items);
	}

	INIT_LIST_HEAD(&cil->xc_committing);
	spin_lock_init(&cil->xc_push_lock);
	init_waitqueue_head(&cil->xc_push_wait);
	init_rwsem(&cil->xc_ctx_lock);
	init_waitqueue_head(&cil->xc_start_wait);
	init_waitqueue_head(&cil->xc_commit_wait);
	log->l_cilp = cil;

	ctx = xlog_cil_ctx_alloc();
	xlog_cil_ctx_switch(cil, ctx);
	return 0;

out_destroy_wq:
	destroy_workqueue(cil->xc_push_wq);
out_destroy_cil:
	kmem_free(cil);
	return -ENOMEM;
}

