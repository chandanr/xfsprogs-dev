#include "libxfs.h"
#include "libxlog.h"
#include "xfs_log_priv.h"

/*
 * Allocate a new ticket. Failing to get a new ticket makes it really hard to
 * recover, so we don't allow failure here. Also, we allocate in a context that
 * we don't want to be issuing transactions from, so we need to tell the
 * allocation code this as well.
 *
 * We don't reserve any space for the ticket - we are going to steal whatever
 * space we require from transactions as they commit. To ensure we reserve all
 * the space required, we need to set the current reservation of the ticket to
 * zero so that we know to steal the initial transaction overhead from the
 * first transaction commit.
 */
static struct xlog_ticket *
xlog_cil_ticket_alloc(
	struct xlog	*log)
{
	struct xlog_ticket *tic;

	tic = xlog_ticket_alloc(log, 0, 1, 0);

	/*
	 * set the current reservation to zero so we know to steal the basic
	 * transaction overhead reservation from the first transaction commit.
	 */
	tic->t_curr_res = 0;
	tic->t_iclog_hdrs = 0;
	return tic;
}

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
 * Aggregate the CIL per cpu structures into global counts, lists, etc and
 * clear the percpu state ready for the next context to use. This is called
 * from the push code with the context lock held exclusively, hence nothing else
 * will be accessing or modifying the per-cpu counters.
 */
static void
xlog_cil_push_pcp_aggregate(
	struct xfs_cil		*cil,
	struct xfs_cil_ctx	*ctx)
{
	struct xlog_cil_pcp	*cilpcp;
	int			cpu;

	for_each_cpu(cpu, &ctx->cil_pcpmask) {
		cilpcp = per_cpu_ptr(cil->xc_pcp, cpu);

		ctx->ticket->t_curr_res += cilpcp->space_reserved;
		cilpcp->space_reserved = 0;

		if (!list_empty(&cilpcp->busy_extents)) {
			list_splice_init(&cilpcp->busy_extents,
					&ctx->busy_extents.extent_list);
		}
		if (!list_empty(&cilpcp->log_items))
			list_splice_init(&cilpcp->log_items, &ctx->log_items);

		/*
		 * We're in the middle of switching cil contexts.  Reset the
		 * counter we use to detect when the current context is nearing
		 * full.
		 */
		cilpcp->space_used = 0;
	}
}

struct xlog_cil_trans_hdr {
	struct xlog_op_header	oph[2];
	struct xfs_trans_header	thdr;
	struct xfs_log_iovec	lhdr[2];
};

/*
 * Pull all the log vectors off the items in the CIL, and remove the items from
 * the CIL. We don't need the CIL lock here because it's only needed on the
 * transaction commit side which is currently locked out by the flush lock.
 *
 * If a log item is marked with a whiteout, we do not need to write it to the
 * journal and so we just move them to the whiteout list for the caller to
 * dispose of appropriately.
 */
static void
xlog_cil_build_lv_chain(
	struct xfs_cil_ctx	*ctx,
	struct list_head	*whiteouts,
	uint32_t		*num_iovecs,
	uint32_t		*num_bytes)
{
	while (!list_empty(&ctx->log_items)) {
		struct xfs_log_item	*item;
		struct xfs_log_vec	*lv;

		item = list_first_entry(&ctx->log_items,
					struct xfs_log_item, li_cil);

		if (test_bit(XFS_LI_WHITEOUT, &item->li_flags)) {
			list_move(&item->li_cil, whiteouts);
			trace_xfs_cil_whiteout_skip(item);
			continue;
		}

		lv = item->li_lv;
		lv->lv_order_id = item->li_order_id;

		/* we don't write ordered log vectors */
		if (lv->lv_buf_len != XFS_LOG_VEC_ORDERED)
			*num_bytes += lv->lv_bytes;
		*num_iovecs += lv->lv_niovecs;
		list_add_tail(&lv->lv_list, &ctx->lv_chain);

		list_del_init(&item->li_cil);
		item->li_order_id = 0;
		item->li_lv = NULL;
	}
}

/*
 * CIL item reordering compare function. We want to order in ascending ID order,
 * but we want to leave items with the same ID in the order they were added to
 * the list. This is important for operations like reflink where we log 4 order
 * dependent intents in a single transaction when we overwrite an existing
 * shared extent with a new shared extent. i.e. BUI(unmap), CUI(drop),
 * CUI (inc), BUI(remap)...
 */
static int
xlog_cil_order_cmp(
	void			*priv,
	const struct list_head	*a,
	const struct list_head	*b)
{
	struct xfs_log_vec	*l1 = container_of(a, struct xfs_log_vec, lv_list);
	struct xfs_log_vec	*l2 = container_of(b, struct xfs_log_vec, lv_list);

	return l1->lv_order_id > l2->lv_order_id;
}

/*
 * Build a checkpoint transaction header to begin the journal transaction.  We
 * need to account for the space used by the transaction header here as it is
 * not accounted for in xlog_write().
 *
 * This is the only place we write a transaction header, so we also build the
 * log opheaders that indicate the start of a log transaction and wrap the
 * transaction header. We keep the start record in it's own log vector rather
 * than compacting them into a single region as this ends up making the logic
 * in xlog_write() for handling empty opheaders for start, commit and unmount
 * records much simpler.
 */
static void
xlog_cil_build_trans_hdr(
	struct xfs_cil_ctx	*ctx,
	struct xlog_cil_trans_hdr *hdr,
	struct xfs_log_vec	*lvhdr,
	int			num_iovecs)
{
	struct xlog_ticket	*tic = ctx->ticket;
	__be32			tid = cpu_to_be32(tic->t_tid);

	memset(hdr, 0, sizeof(*hdr));

	/* Log start record */
	hdr->oph[0].oh_tid = tid;
	hdr->oph[0].oh_clientid = XFS_TRANSACTION;
	hdr->oph[0].oh_flags = XLOG_START_TRANS;

	/* log iovec region pointer */
	hdr->lhdr[0].i_addr = &hdr->oph[0];
	hdr->lhdr[0].i_len = sizeof(struct xlog_op_header);
	hdr->lhdr[0].i_type = XLOG_REG_TYPE_LRHEADER;

	/* log opheader */
	hdr->oph[1].oh_tid = tid;
	hdr->oph[1].oh_clientid = XFS_TRANSACTION;
	hdr->oph[1].oh_len = cpu_to_be32(sizeof(struct xfs_trans_header));

	/* transaction header in host byte order format */
	hdr->thdr.th_magic = XFS_TRANS_HEADER_MAGIC;
	hdr->thdr.th_type = XFS_TRANS_CHECKPOINT;
	hdr->thdr.th_tid = tic->t_tid;
	hdr->thdr.th_num_items = num_iovecs;

	/* log iovec region pointer */
	hdr->lhdr[1].i_addr = &hdr->oph[1];
	hdr->lhdr[1].i_len = sizeof(struct xlog_op_header) +
				sizeof(struct xfs_trans_header);
	hdr->lhdr[1].i_type = XLOG_REG_TYPE_TRANSHDR;

	lvhdr->lv_niovecs = 2;
	lvhdr->lv_iovecp = &hdr->lhdr[0];
	lvhdr->lv_bytes = hdr->lhdr[0].i_len + hdr->lhdr[1].i_len;

	tic->t_curr_res -= lvhdr->lv_bytes;
}


/*
 * Ensure that the order of log writes follows checkpoint sequence order. This
 * relies on the context LSN being zero until the log write has guaranteed the
 * LSN that the log write will start at via xlog_state_get_iclog_space().
 */
enum _record_type {
	_START_RECORD,
	_COMMIT_RECORD,
};

static int
xlog_cil_order_write(
	struct xfs_cil		*cil,
	xfs_csn_t		sequence,
	enum _record_type	record)
{
	struct xfs_cil_ctx	*ctx;

restart:
	spin_lock(&cil->xc_push_lock);
	list_for_each_entry(ctx, &cil->xc_committing, committing) {
		/*
		 * Avoid getting stuck in this loop because we were woken by the
		 * shutdown, but then went back to sleep once already in the
		 * shutdown state.
		 */
		if (xlog_is_shutdown(cil->xc_log)) {
			spin_unlock(&cil->xc_push_lock);
			return -EIO;
		}

		/*
		 * Higher sequences will wait for this one so skip them.
		 * Don't wait for our own sequence, either.
		 */
		if (ctx->sequence >= sequence)
			continue;

		/* Wait until the LSN for the record has been recorded. */
		switch (record) {
		case _START_RECORD:
			if (!ctx->start_lsn) {
				xlog_wait(&cil->xc_start_wait, &cil->xc_push_lock);
				goto restart;
			}
			break;
		case _COMMIT_RECORD:
			if (!ctx->commit_lsn) {
				xlog_wait(&cil->xc_commit_wait, &cil->xc_push_lock);
				goto restart;
			}
			break;
		}
	}
	spin_unlock(&cil->xc_push_lock);
	return 0;
}

/*
 * Write out the log vector change now attached to the CIL context. This will
 * write a start record that needs to be strictly ordered in ascending CIL
 * sequence order so that log recovery will always use in-order start LSNs when
 * replaying checkpoints.
 */
static int
xlog_cil_write_chain(
	struct xfs_cil_ctx	*ctx,
	uint32_t		chain_len)
{
	struct xlog		*log = ctx->cil->xc_log;
	int			error;

	error = xlog_cil_order_write(ctx->cil, ctx->sequence, _START_RECORD);
	if (error)
		return error;
	return xlog_write(log, ctx, &ctx->lv_chain, ctx->ticket, chain_len);
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
xlog_cil_push_work(
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

/*
 * Allocate a new ticket. Failing to get a new ticket makes it really hard to
 * recover, so we don't allow failure here. Also, we allocate in a context that
 * we don't want to be issuing transactions from, so we need to tell the
 * allocation code this as well.
 *
 * We don't reserve any space for the ticket - we are going to steal whatever
 * space we require from transactions as they commit. To ensure we reserve all
 * the space required, we need to set the current reservation of the ticket to
 * zero so that we know to steal the initial transaction overhead from the
 * first transaction commit.
 */
static struct xlog_ticket *
xlog_cil_ticket_alloc(
	struct xlog	*log)
{
	struct xlog_ticket *tic;

	tic = xlog_ticket_alloc(log, 0, 1, 0);

	/*
	 * set the current reservation to zero so we know to steal the basic
	 * transaction overhead reservation from the first transaction commit.
	 */
	tic->t_curr_res = 0;
	tic->t_iclog_hdrs = 0;
	return tic;
}

/*
 * After the first stage of log recovery is done, we know where the head and
 * tail of the log are. We need this log initialisation done before we can
 * initialise the first CIL checkpoint context.
 *
 * Here we allocate a log ticket to track space usage during a CIL push.  This
 * ticket is passed to xlog_write() directly so that we don't slowly leak log
 * space by failing to account for space used by log headers and additional
 * region headers for split regions.
 */
void
xlog_cil_init_post_recoveryo(
	struct xlog	*log)
{
	log->l_cilp->xc_ctx->ticket = xlog_cil_ticket_alloc(log);
	log->l_cilp->xc_ctx->sequence = 1;
	xlog_cil_set_iclog_hdr_count(log->l_cilp);
}

/*
 * Conditionally push the CIL based on the sequence passed in.
 *
 * We only need to push if we haven't already pushed the sequence number given.
 * Hence the only time we will trigger a push here is if the push sequence is
 * the same as the current context.
 *
 * We return the current commit lsn to allow the callers to determine if a
 * iclog flush is necessary following this call.
 */
xfs_lsn_t
xlog_cil_force_seq(
	struct xlog	*log,
	xfs_csn_t	sequence)
{
	struct xfs_cil		*cil = log->l_cilp;
	struct xfs_cil_ctx	*ctx;
	xfs_lsn_t		commit_lsn = NULLCOMMITLSN;

	ASSERT(sequence <= cil->xc_current_sequence);

	if (!sequence)
		sequence = cil->xc_current_sequence;
	trace_xfs_log_force(log->l_mp, sequence, _RET_IP_);

	/*
	 * check to see if we need to force out the current context.
	 * xlog_cil_push() handles racing pushes for the same sequence,
	 * so no need to deal with it here.
	 */
restart:
	xlog_cil_push_now(log, sequence, false);

	/*
	 * See if we can find a previous sequence still committing.
	 * We need to wait for all previous sequence commits to complete
	 * before allowing the force of push_seq to go ahead. Hence block
	 * on commits for those as well.
	 */
	spin_lock(&cil->xc_push_lock);
	list_for_each_entry(ctx, &cil->xc_committing, committing) {
		/*
		 * Avoid getting stuck in this loop because we were woken by the
		 * shutdown, but then went back to sleep once already in the
		 * shutdown state.
		 */
		if (xlog_is_shutdown(log))
			goto out_shutdown;
		if (ctx->sequence > sequence)
			continue;
		if (!ctx->commit_lsn) {
			/*
			 * It is still being pushed! Wait for the push to
			 * complete, then start again from the beginning.
			 */
			XFS_STATS_INC(log->l_mp, xs_log_force_sleep);
			xlog_wait(&cil->xc_commit_wait, &cil->xc_push_lock);
			goto restart;
		}
		if (ctx->sequence != sequence)
			continue;
		/* found it! */
		commit_lsn = ctx->commit_lsn;
	}

	/*
	 * The call to xlog_cil_push_now() executes the push in the background.
	 * Hence by the time we have got here it our sequence may not have been
	 * pushed yet. This is true if the current sequence still matches the
	 * push sequence after the above wait loop and the CIL still contains
	 * dirty objects. This is guaranteed by the push code first adding the
	 * context to the committing list before emptying the CIL.
	 *
	 * Hence if we don't find the context in the committing list and the
	 * current sequence number is unchanged then the CIL contents are
	 * significant.  If the CIL is empty, if means there was nothing to push
	 * and that means there is nothing to wait for. If the CIL is not empty,
	 * it means we haven't yet started the push, because if it had started
	 * we would have found the context on the committing list.
	 */
	if (sequence == cil->xc_current_sequence &&
	    !test_bit(XLOG_CIL_EMPTY, &cil->xc_flags)) {
		spin_unlock(&cil->xc_push_lock);
		goto restart;
	}

	spin_unlock(&cil->xc_push_lock);
	return commit_lsn;

	/*
	 * We detected a shutdown in progress. We need to trigger the log force
	 * to pass through it's iclog state machine error handling, even though
	 * we are already in a shutdown state. Hence we can't return
	 * NULLCOMMITLSN here as that has special meaning to log forces (i.e.
	 * LSN is already stable), so we return a zero LSN instead.
	 */
out_shutdown:
	spin_unlock(&cil->xc_push_lock);
	return 0;
}

/*
 * xlog_cil_push_now() is used to trigger an immediate CIL push to the sequence
 * number that is passed. When it returns, the work will be queued for
 * @push_seq, but it won't be completed.
 *
 * If the caller is performing a synchronous force, we will flush the workqueue
 * to get previously queued work moving to minimise the wait time they will
 * undergo waiting for all outstanding pushes to complete. The caller is
 * expected to do the required waiting for push_seq to complete.
 *
 * If the caller is performing an async push, we need to ensure that the
 * checkpoint is fully flushed out of the iclogs when we finish the push. If we
 * don't do this, then the commit record may remain sitting in memory in an
 * ACTIVE iclog. This then requires another full log force to push to disk,
 * which defeats the purpose of having an async, non-blocking CIL force
 * mechanism. Hence in this case we need to pass a flag to the push work to
 * indicate it needs to flush the commit record itself.
 */
static void
xlog_cil_push_now(
	struct xlog	*log,
	xfs_lsn_t	push_seq,
	bool		async)
{
	struct xfs_cil	*cil = log->l_cilp;

	if (!cil)
		return;

	ASSERT(push_seq && push_seq <= cil->xc_current_sequence);

	/* start on any pending background push to minimise wait time on it */
	if (!async)
		flush_workqueue(cil->xc_push_wq);

	spin_lock(&cil->xc_push_lock);

	/*
	 * If this is an async flush request, we always need to set the
	 * xc_push_commit_stable flag even if something else has already queued
	 * a push. The flush caller is asking for the CIL to be on stable
	 * storage when the next push completes, so regardless of who has queued
	 * the push, the flush requires stable semantics from it.
	 */
	cil->xc_push_commit_stable = async;

	/*
	 * If the CIL is empty or we've already pushed the sequence then
	 * there's no more work that we need to do.
	 */
	if (test_bit(XLOG_CIL_EMPTY, &cil->xc_flags) ||
	    push_seq <= cil->xc_push_seq) {
		spin_unlock(&cil->xc_push_lock);
		return;
	}

	cil->xc_push_seq = push_seq;
	queue_work(cil->xc_push_wq, &cil->xc_ctx->push_work);
	spin_unlock(&cil->xc_push_lock);
}
