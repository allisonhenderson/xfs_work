// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2019 Oracle.  All Rights Reserved.
 * Author: Allison Henderson <allison.henderson@oracle.com>
 */

#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_format.h"
#include "xfs_log_format.h"
#include "xfs_trans_resv.h"
#include "xfs_bit.h"
#include "xfs_shared.h"
#include "xfs_mount.h"
#include "xfs_defer.h"
#include "xfs_trans.h"
#include "xfs_trans_priv.h"
#include "xfs_buf_item.h"
#include "xfs_attr_item.h"
#include "xfs_log.h"
#include "xfs_btree.h"
#include "xfs_rmap.h"
#include "xfs_inode.h"
#include "xfs_icache.h"
#include "xfs_da_format.h"
#include "xfs_da_btree.h"
#include "xfs_attr.h"
#include "xfs_shared.h"
#include "xfs_attr_item.h"
#include "xfs_alloc.h"
#include "xfs_bmap.h"
#include "xfs_trace.h"
#include "libxfs/xfs_da_format.h"
#include "xfs_inode.h"
#include "xfs_quota.h"


/*
 * This routine is called to allocate an "attr free done" log item.
 */
struct xfs_attrd_log_item *
xfs_trans_get_attrd(struct xfs_trans		*tp,
		  struct xfs_attri_log_item	*attrip)
{
	struct xfs_attrd_log_item		*attrdp;

	ASSERT(tp != NULL);

	attrdp = xfs_attrd_init(tp->t_mountp, attrip);
	ASSERT(attrdp != NULL);

	/*
	 * Get a log_item_desc to point at the new item.
	 */
	xfs_trans_add_item(tp, &attrdp->attrd_item);
	return attrdp;
}

/*
 * Delete an attr and log it to the ATTRD. Note that the transaction is marked
 * dirty regardless of whether the attr delete succeeds or fails to support the
 * ATTRI/ATTRD lifecycle rules.
 */
int
xfs_trans_attr(
	struct xfs_da_args		*args,
	struct xfs_attrd_log_item	*attrdp,
	uint32_t			op_flags)
{
	int				error;

	error = xfs_qm_dqattach_locked(args->dp, 0);
	if (error)
		return error;

	switch (op_flags) {
	case XFS_ATTR_OP_FLAGS_SET:
		args->op_flags |= XFS_DA_OP_ADDNAME;
		error = xfs_attr_set_args(args);
		break;
	case XFS_ATTR_OP_FLAGS_REMOVE:
		ASSERT(XFS_IFORK_Q((args->dp)));
		error = xfs_attr_remove_args(args);
		break;
	default:
		error = -EFSCORRUPTED;
	}

	/*
	 * Mark the transaction dirty, even on error. This ensures the
	 * transaction is aborted, which:
	 *
	 * 1.) releases the ATTRI and frees the ATTRD
	 * 2.) shuts down the filesystem
	 */
	args->trans->t_flags |= XFS_TRANS_DIRTY;
	set_bit(XFS_LI_DIRTY, &attrdp->attrd_item.li_flags);

	return error;
}

static int
xfs_attr_diff_items(
	void				*priv,
	struct list_head		*a,
	struct list_head		*b)
{
	return 0;
}

/* Get an ATTRI. */
STATIC void *
xfs_attr_create_intent(
	struct xfs_trans		*tp,
	unsigned int			count)
{
	struct xfs_attri_log_item	*attrip;

	ASSERT(tp != NULL);
	ASSERT(count == 1);

	attrip = xfs_attri_init(tp->t_mountp);
	ASSERT(attrip != NULL);

	/*
	 * Get a log_item_desc to point at the new item.
	 */
	xfs_trans_add_item(tp, &attrip->attri_item);
	return attrip;
}

/* Log an attr to the intent item. */
STATIC void
xfs_attr_log_item(
	struct xfs_trans		*tp,
	void				*intent,
	struct list_head		*item)
{
	struct xfs_attri_log_item	*attrip = intent;
	struct xfs_attr_item		*attr;
	struct xfs_attri_log_format	*attrp;
	char				*name_value;

	attr = container_of(item, struct xfs_attr_item, xattri_list);
	name_value = ((char *)attr) + sizeof(struct xfs_attr_item);

	tp->t_flags |= XFS_TRANS_DIRTY;
	set_bit(XFS_LI_DIRTY, &attrip->attri_item.li_flags);

	/*
	 * At this point the xfs_attr_item has been constructed, and we've
	 * created the log intent. Fill in the attri log item and log format
	 * structure with fields from this xfs_attr_item
	 */
	attrp = &attrip->attri_format;
	attrp->alfi_ino = attr->xattri_ip->i_ino;
	attrp->alfi_op_flags = attr->xattri_op_flags;
	attrp->alfi_value_len = attr->xattri_value_len;
	attrp->alfi_name_len = attr->xattri_name_len;
	attrp->alfi_attr_flags = attr->xattri_flags;

	attrip->attri_name = name_value;
	attrip->attri_value = &name_value[attr->xattri_name_len];
	attrip->attri_name_len = attr->xattri_name_len;
	attrip->attri_value_len = attr->xattri_value_len;
}

/* Get an ATTRD so we can process all the attrs. */
STATIC void *
xfs_attr_create_done(
	struct xfs_trans		*tp,
	void				*intent,
	unsigned int			count)
{
	return xfs_trans_get_attrd(tp, intent);
}

/* Process an attr. */
STATIC int
xfs_attr_finish_item(
	struct xfs_trans		*tp,
	struct list_head		*item,
	void				*done_item,
	void				**state)
{
	struct xfs_attr_item		*attr;
	char				*name_value;
	int				error;
	int				local;
	struct xfs_da_args		args;
	struct xfs_name			name;
	struct xfs_attrd_log_item	*attrdp;
	struct xfs_attri_log_item	*attrip;

	attr = container_of(item, struct xfs_attr_item, xattri_list);
	name_value = ((char *)attr) + sizeof(struct xfs_attr_item);

	name.name = name_value;
	name.len = attr->xattri_name_len;
	name.type = attr->xattri_flags;
	error = xfs_attr_args_init(&args, attr->xattri_ip, &name);
	if (error)
		goto out;

	args.hashval = xfs_da_hashname(args.name, args.namelen);
	args.value = &name_value[attr->xattri_name_len];
	args.valuelen = attr->xattri_value_len;
	args.op_flags = XFS_DA_OP_OKNOENT;
	args.total = xfs_attr_calc_size(&args, &local);
	args.trans = tp;

	error = xfs_trans_attr(&args, done_item,
			attr->xattri_op_flags);
out:
	/*
	 * We are about to free the xfs_attr_item, so we need to remove any
	 * refrences that are currently pointing at its members
	 */
	attrdp = (struct xfs_attrd_log_item *)done_item;
	attrip = attrdp->attrd_attrip;
	attrip->attri_name = NULL;
	attrip->attri_value = NULL;
	attrip->attri_name_len = 0;
	attrip->attri_value_len = 0;

	kmem_free(attr);
	return error;
}

/* Abort all pending ATTRs. */
STATIC void
xfs_attr_abort_intent(
	void				*intent)
{
	xfs_attri_release(intent);
}

/* Cancel an attr */
STATIC void
xfs_attr_cancel_item(
	struct list_head		*item)
{
	struct xfs_attr_item	*attr;

	attr = container_of(item, struct xfs_attr_item, xattri_list);
	kmem_free(attr);
}

const struct xfs_defer_op_type xfs_attr_defer_type = {
	.max_items	= XFS_ATTRI_MAX_FAST_ATTRS,
	.diff_items	= xfs_attr_diff_items,
	.create_intent	= xfs_attr_create_intent,
	.abort_intent	= xfs_attr_abort_intent,
	.log_item	= xfs_attr_log_item,
	.create_done	= xfs_attr_create_done,
	.finish_item	= xfs_attr_finish_item,
	.cancel_item	= xfs_attr_cancel_item,
};

static inline struct xfs_attri_log_item *ATTRI_ITEM(struct xfs_log_item *lip)
{
	return container_of(lip, struct xfs_attri_log_item, attri_item);
}

void
xfs_attri_item_free(
	struct xfs_attri_log_item	*attrip)
{
	kmem_free(attrip->attri_item.li_lv_shadow);
	kmem_free(attrip);
}

/*
 * This returns the number of iovecs needed to log the given attri item. We
 * only need 1 iovec for an attri item.  It just logs the attr_log_format
 * structure.
 */
static inline int
xfs_attri_item_sizeof(
	struct xfs_attri_log_item *attrip)
{
	return sizeof(struct xfs_attri_log_format);
}

STATIC void
xfs_attri_item_size(
	struct xfs_log_item	*lip,
	int			*nvecs,
	int			*nbytes)
{
	struct xfs_attri_log_item       *attrip = ATTRI_ITEM(lip);

	*nvecs += 1;
	*nbytes += xfs_attri_item_sizeof(attrip);

	if (attrip->attri_name_len > 0) {
		*nvecs += 1;
		*nbytes += ATTR_NVEC_SIZE(attrip->attri_name_len);
	}

	if (attrip->attri_value_len > 0) {
		*nvecs += 1;
		*nbytes += ATTR_NVEC_SIZE(attrip->attri_value_len);
	}
}

/*
 * This is called to fill in the vector of log iovecs for the given attri log
 * item. We use only 1 iovec, and we point that at the attri_log_format
 * structure embedded in the attri item. It is at this point that we assert
 * that all of the attr slots in the attri item have been filled.
 */
STATIC void
xfs_attri_item_format(
	struct xfs_log_item	*lip,
	struct xfs_log_vec	*lv)
{
	struct xfs_attri_log_item	*attrip = ATTRI_ITEM(lip);
	struct xfs_log_iovec	*vecp = NULL;

	attrip->attri_format.alfi_type = XFS_LI_ATTRI;
	attrip->attri_format.alfi_size = 1;

	/*
	 * This size accounting must be done before copying the attrip into the
	 * iovec.  If we do it after, the wrong size will be recorded to the log
	 * and we trip across assertion checks for bad region sizes later during
	 * the log recovery.
	 */
	if (attrip->attri_name_len > 0)
		attrip->attri_format.alfi_size++;
	if (attrip->attri_value_len > 0)
		attrip->attri_format.alfi_size++;

	xlog_copy_iovec(lv, &vecp, XLOG_REG_TYPE_ATTRI_FORMAT,
			&attrip->attri_format,
			xfs_attri_item_sizeof(attrip));
	if (attrip->attri_name_len > 0)
		xlog_copy_iovec(lv, &vecp, XLOG_REG_TYPE_ATTR_NAME,
				attrip->attri_name,
				ATTR_NVEC_SIZE(attrip->attri_name_len));

	if (attrip->attri_value_len > 0)
		xlog_copy_iovec(lv, &vecp, XLOG_REG_TYPE_ATTR_VALUE,
				attrip->attri_value,
				ATTR_NVEC_SIZE(attrip->attri_value_len));
}


/*
 * Pinning has no meaning for an attri item, so just return.
 */
STATIC void
xfs_attri_item_pin(
	struct xfs_log_item	*lip)
{
}

/*
 * The unpin operation is the last place an ATTRI is manipulated in the log. It
 * is either inserted in the AIL or aborted in the event of a log I/O error. In
 * either case, the ATTRI transaction has been successfully committed to make
 * it this far. Therefore, we expect whoever committed the ATTRI to either
 * construct and commit the ATTRD or drop the ATTRD's reference in the event of
 * error. Simply drop the log's ATTRI reference now that the log is done with
 * it.
 */
STATIC void
xfs_attri_item_unpin(
	struct xfs_log_item	*lip,
	int			remove)
{
	struct xfs_attri_log_item	*attrip = ATTRI_ITEM(lip);

	xfs_attri_release(attrip);
}

/*
 * attri items have no locking or pushing.  However, since ATTRIs are pulled
 * from the AIL when their corresponding ATTRDs are committed to disk, their
 * situation is very similar to being pinned.  Return XFS_ITEM_PINNED so that
 * the caller will eventually flush the log.  This should help in getting the
 * ATTRI out of the AIL.
 */
STATIC uint
xfs_attri_item_push(
	struct xfs_log_item	*lip,
	struct list_head	*buffer_list)
{
	return XFS_ITEM_PINNED;
}

/*
 * The ATTRI is logged only once and cannot be moved in the log, so simply
 * return the lsn at which it's been logged.
 */
STATIC xfs_lsn_t
xfs_attri_item_committed(
	struct xfs_log_item	*lip,
	xfs_lsn_t		lsn)
{
	return lsn;
}

STATIC void
xfs_attri_item_committing(
	struct xfs_log_item	*lip,
	xfs_lsn_t		lsn)
{
}

STATIC void
xfs_attri_item_release(
	struct xfs_log_item	*lip)
{
	xfs_attri_release(ATTRI_ITEM(lip));
}

/*
 * This is the ops vector shared by all attri log items.
 */
static const struct xfs_item_ops xfs_attri_item_ops = {
	.iop_size	= xfs_attri_item_size,
	.iop_format	= xfs_attri_item_format,
	.iop_pin	= xfs_attri_item_pin,
	.iop_unpin	= xfs_attri_item_unpin,
	.iop_committed	= xfs_attri_item_committed,
	.iop_push	= xfs_attri_item_push,
	.iop_committing = xfs_attri_item_committing,
	.iop_release    = xfs_attri_item_release,
};


/*
 * Allocate and initialize an attri item
 */
struct xfs_attri_log_item *
xfs_attri_init(
	struct xfs_mount	*mp)

{
	struct xfs_attri_log_item	*attrip;
	uint				size;

	size = (uint)(sizeof(struct xfs_attri_log_item));
	attrip = kmem_zalloc(size, KM_SLEEP);

	xfs_log_item_init(mp, &attrip->attri_item, XFS_LI_ATTRI,
			  &xfs_attri_item_ops);
	attrip->attri_format.alfi_id = (uintptr_t)(void *)attrip;
	atomic_set(&attrip->attri_refcount, 2);

	return attrip;
}

/*
 * Copy an attr format buffer from the given buf, and into the destination attr
 * format structure.
 */
int
xfs_attri_copy_format(struct xfs_log_iovec *buf,
		      struct xfs_attri_log_format *dst_attr_fmt)
{
	struct xfs_attri_log_format *src_attr_fmt = buf->i_addr;
	uint len = sizeof(struct xfs_attri_log_format);

	if (buf->i_len != len)
		return -EFSCORRUPTED;

	memcpy((char *)dst_attr_fmt, (char *)src_attr_fmt, len);
	return 0;
}

/*
 * Freeing the attrip requires that we remove it from the AIL if it has already
 * been placed there. However, the ATTRI may not yet have been placed in the
 * AIL when called by xfs_attri_release() from ATTRD processing due to the
 * ordering of committed vs unpin operations in bulk insert operations. Hence
 * the reference count to ensure only the last caller frees the ATTRI.
 */
void
xfs_attri_release(
	struct xfs_attri_log_item	*attrip)
{
	ASSERT(atomic_read(&attrip->attri_refcount) > 0);
	if (atomic_dec_and_test(&attrip->attri_refcount)) {
		xfs_trans_ail_remove(&attrip->attri_item,
				     SHUTDOWN_LOG_IO_ERROR);
		xfs_attri_item_free(attrip);
	}
}

static inline struct xfs_attrd_log_item *ATTRD_ITEM(struct xfs_log_item *lip)
{
	return container_of(lip, struct xfs_attrd_log_item, attrd_item);
}

STATIC void
xfs_attrd_item_free(struct xfs_attrd_log_item *attrdp)
{
	kmem_free(attrdp->attrd_item.li_lv_shadow);
	kmem_free(attrdp);
}

/*
 * This returns the number of iovecs needed to log the given attrd item.
 * We only need 1 iovec for an attrd item.  It just logs the attr_log_format
 * structure.
 */
static inline int
xfs_attrd_item_sizeof(
	struct xfs_attrd_log_item *attrdp)
{
	return sizeof(struct xfs_attrd_log_format);
}

STATIC void
xfs_attrd_item_size(
	struct xfs_log_item	*lip,
	int			*nvecs,
	int			*nbytes)
{
	struct xfs_attrd_log_item	*attrdp = ATTRD_ITEM(lip);
	*nvecs += 1;
	*nbytes += xfs_attrd_item_sizeof(attrdp);
}

/*
 * This is called to fill in the vector of log iovecs for the
 * given attrd log item. We use only 1 iovec, and we point that
 * at the attr_log_format structure embedded in the attrd item.
 * It is at this point that we assert that all of the attr
 * slots in the attrd item have been filled.
 */
STATIC void
xfs_attrd_item_format(
	struct xfs_log_item	*lip,
	struct xfs_log_vec	*lv)
{
	struct xfs_attrd_log_item	*attrdp = ATTRD_ITEM(lip);
	struct xfs_log_iovec	*vecp = NULL;

	attrdp->attrd_format.alfd_type = XFS_LI_ATTRD;
	attrdp->attrd_format.alfd_size = 1;

	xlog_copy_iovec(lv, &vecp, XLOG_REG_TYPE_ATTRD_FORMAT,
			&attrdp->attrd_format, xfs_attrd_item_sizeof(attrdp));
}

/*
 * Pinning has no meaning for an attrd item, so just return.
 */
STATIC void
xfs_attrd_item_pin(
	struct xfs_log_item	*lip)
{
}

/*
 * Since pinning has no meaning for an attrd item, unpinning does
 * not either.
 */
STATIC void
xfs_attrd_item_unpin(
	struct xfs_log_item	*lip,
	int			remove)
{
}

/*
 * There isn't much you can do to push on an attrd item.  It is simply stuck
 * waiting for the log to be flushed to disk.
 */
STATIC uint
xfs_attrd_item_push(
	struct xfs_log_item	*lip,
	struct list_head	*buffer_list)
{
	return XFS_ITEM_PINNED;
}

/*
 * When the attrd item is committed to disk, all we need to do is delete our
 * reference to our partner attri item and then free ourselves. Since we're
 * freeing ourselves we must return -1 to keep the transaction code from
 * further referencing this item.
 */
STATIC xfs_lsn_t
xfs_attrd_item_committed(
	struct xfs_log_item	*lip,
	xfs_lsn_t		lsn)
{
	struct xfs_attrd_log_item	*attrdp = ATTRD_ITEM(lip);

	/*
	 * Drop the ATTRI reference regardless of whether the ATTRD has been
	 * aborted. Once the ATTRD transaction is constructed, it is the sole
	 * responsibility of the ATTRD to release the ATTRI (even if the ATTRI
	 * is aborted due to log I/O error).
	 */
	xfs_attri_release(attrdp->attrd_attrip);
	xfs_attrd_item_free(attrdp);

	return (xfs_lsn_t)-1;
}

STATIC void
xfs_attrd_item_committing(
	struct xfs_log_item	*lip,
	xfs_lsn_t		lsn)
{
}

/*
 * The ATTRD is either committed or aborted if the transaction is cancelled. If
 * the transaction is cancelled, drop our reference to the ATTRI and free the
 * ATTRD.
 */
STATIC void
xfs_attrd_item_release(
	struct xfs_log_item     *lip)
{
	struct xfs_attrd_log_item *attrdp = ATTRD_ITEM(lip);

	xfs_attri_release(attrdp->attrd_attrip);
	xfs_attrd_item_free(attrdp);
}

/*
 * This is the ops vector shared by all attrd log items.
 */
static const struct xfs_item_ops xfs_attrd_item_ops = {
	.iop_size	= xfs_attrd_item_size,
	.iop_format	= xfs_attrd_item_format,
	.iop_pin	= xfs_attrd_item_pin,
	.iop_unpin	= xfs_attrd_item_unpin,
	.iop_committed	= xfs_attrd_item_committed,
	.iop_push	= xfs_attrd_item_push,
	.iop_committing = xfs_attrd_item_committing,
	.iop_release    = xfs_attrd_item_release,
};

/*
 * Allocate and initialize an attrd item
 */
struct xfs_attrd_log_item *
xfs_attrd_init(
	struct xfs_mount		*mp,
	struct xfs_attri_log_item	*attrip)

{
	struct xfs_attrd_log_item	*attrdp;
	uint				size;

	size = (uint)(sizeof(struct xfs_attrd_log_item));
	attrdp = kmem_zalloc(size, KM_SLEEP);

	xfs_log_item_init(mp, &attrdp->attrd_item, XFS_LI_ATTRD,
			  &xfs_attrd_item_ops);
	attrdp->attrd_attrip = attrip;
	attrdp->attrd_format.alfd_alf_id = attrip->attri_format.alfi_id;

	return attrdp;
}

/*
 * Process an attr intent item that was recovered from the log.  We need to
 * delete the attr that it describes.
 */
int
xfs_attri_recover(
	struct xfs_mount		*mp,
	struct xfs_attri_log_item	*attrip)
{
	struct xfs_inode		*ip;
	struct xfs_attrd_log_item	*attrdp;
	struct xfs_da_args		args;
	struct xfs_attri_log_format	*attrp;
	struct xfs_trans_res		tres;
	int				local;
	int				error = 0;
	int				rsvd = 0;
	struct xfs_name			name;

	ASSERT(!test_bit(XFS_ATTRI_RECOVERED, &attrip->attri_flags));

	/*
	 * First check the validity of the attr described by the ATTRI.  If any
	 * are bad, then assume that all are bad and just toss the ATTRI.
	 */
	attrp = &attrip->attri_format;
	if (
	    !(attrp->alfi_op_flags == XFS_ATTR_OP_FLAGS_SET ||
		attrp->alfi_op_flags == XFS_ATTR_OP_FLAGS_REMOVE) ||
	      (attrp->alfi_value_len > XATTR_SIZE_MAX) ||
	      (attrp->alfi_name_len > XATTR_NAME_MAX) ||
	      (attrp->alfi_name_len == 0)
	) {
		/*
		 * This will pull the ATTRI from the AIL and free the memory
		 * associated with it.
		 */
		set_bit(XFS_ATTRI_RECOVERED, &attrip->attri_flags);
		xfs_attri_release(attrip);
		return -EIO;
	}

	attrp = &attrip->attri_format;
	error = xfs_iget(mp, 0, attrp->alfi_ino, 0, 0, &ip);
	if (error)
		return error;

	name.name = attrip->attri_name;
	name.len = attrp->alfi_name_len;
	name.type = attrp->alfi_attr_flags;
	error = xfs_attr_args_init(&args, ip, &name);
	if (error)
		return error;

	args.hashval = xfs_da_hashname(args.name, args.namelen);
	args.value = attrip->attri_value;
	args.valuelen = attrp->alfi_value_len;
	args.op_flags = XFS_DA_OP_OKNOENT;
	args.total = xfs_attr_calc_size(&args, &local);

	tres.tr_logres = M_RES(mp)->tr_attrsetm.tr_logres +
			M_RES(mp)->tr_attrsetrt.tr_logres * args.total;
	tres.tr_logcount = XFS_ATTRSET_LOG_COUNT;
	tres.tr_logflags = XFS_TRANS_PERM_LOG_RES;

	error = xfs_trans_alloc(mp, &tres, args.total,  0,
				rsvd ? XFS_TRANS_RESERVE : 0, &args.trans);
	if (error)
		return error;
	attrdp = xfs_trans_get_attrd(args.trans, attrip);

	xfs_ilock(ip, XFS_ILOCK_EXCL);

	xfs_trans_ijoin(args.trans, ip, 0);
	error = xfs_trans_attr(&args, attrdp, attrp->alfi_op_flags);
	if (error)
		goto abort_error;


	set_bit(XFS_ATTRI_RECOVERED, &attrip->attri_flags);
	xfs_trans_log_inode(args.trans, ip, XFS_ILOG_CORE | XFS_ILOG_ADATA);
	error = xfs_trans_commit(args.trans);
	xfs_iunlock(ip, XFS_ILOCK_EXCL);
	return error;

abort_error:
	xfs_trans_cancel(args.trans);
	xfs_iunlock(ip, XFS_ILOCK_EXCL);
	return error;
}
