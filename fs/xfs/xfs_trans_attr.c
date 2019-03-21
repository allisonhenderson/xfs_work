// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2019 Oracle.  All Rights Reserved.
 * Author: Allison Henderson <allison.henderson@oracle.com>
 */
#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_log_format.h"
#include "xfs_trans_resv.h"
#include "xfs_bit.h"
#include "xfs_mount.h"
#include "xfs_defer.h"
#include "xfs_trans.h"
#include "xfs_trans_priv.h"
#include "xfs_attr_item.h"
#include "xfs_alloc.h"
#include "xfs_bmap.h"
#include "xfs_trace.h"
#include "libxfs/xfs_da_format.h"
#include "xfs_da_btree.h"
#include "xfs_attr.h"
#include "xfs_inode.h"
#include "xfs_icache.h"
#include "xfs_quota.h"
#include "xfs_errortag.h"
#include "xfs_error.h"

/*
 * This routine is called to allocate an "attr free done"
 * log item.
 */
struct xfs_attrd_log_item *
xfs_trans_get_attrd(struct xfs_trans		*tp,
		  struct xfs_attri_log_item	*attrip)
{
	struct xfs_attrd_log_item			*attrdp;

	ASSERT(tp != NULL);

	attrdp = xfs_attrd_init(tp->t_mountp, attrip);
	ASSERT(attrdp != NULL);

	/*
	 * Get a log_item_desc to point at the new item.
	 */
	xfs_trans_add_item(tp, &attrdp->item);
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
	struct xfs_buf			**leaf_bp,
	void				*state,
	uint32_t			op_flags)
{
	int				error;

	error = xfs_qm_dqattach_locked(args->dp, 0);
	if (error)
		return error;

	if (XFS_TEST_ERROR(false, args->dp->i_mount, XFS_ERRTAG_DELAYED_ATTR)) {
		error = -EIO;
		goto out;
	}

	switch (op_flags) {
	case XFS_ATTR_OP_FLAGS_SET:
		args->op_flags |= XFS_DA_OP_ADDNAME;
		error = xfs_attr_set_args(args, leaf_bp,
				(enum xfs_attr_state *)state);
		break;
	case XFS_ATTR_OP_FLAGS_REMOVE:
		ASSERT(XFS_IFORK_Q((args->dp)));
		error = xfs_attr_remove_args(args);
		break;
	default:
		error = -EFSCORRUPTED;
	}

out:
	/*
	 * Mark the transaction dirty, even on error. This ensures the
	 * transaction is aborted, which:
	 *
	 * 1.) releases the ATTRI and frees the ATTRD
	 * 2.) shuts down the filesystem
	 */
	args->trans->t_flags |= XFS_TRANS_DIRTY;
	set_bit(XFS_LI_DIRTY, &attrdp->item.li_flags);

	attrdp->attrip->name = (void *)args->name;
	attrdp->attrip->value = (void *)args->value;
	attrdp->attrip->name_len = args->namelen;
	attrdp->attrip->value_len = args->valuelen;

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
	struct xfs_attri_log_item		*attrip;

	ASSERT(tp != NULL);
	ASSERT(count == 1);

	attrip = xfs_attri_init(tp->t_mountp);
	ASSERT(attrip != NULL);

	/*
	 * Get a log_item_desc to point at the new item.
	 */
	xfs_trans_add_item(tp, &attrip->item);
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
	set_bit(XFS_LI_DIRTY, &attrip->item.li_flags);

	attrp = &attrip->format;
	attrp->alfi_ino = attr->xattri_ip->i_ino;
	attrp->alfi_op_flags = attr->xattri_op_flags;
	attrp->alfi_value_len = attr->xattri_value_len;
	attrp->alfi_name_len = attr->xattri_name_len;
	attrp->alfi_attr_flags = attr->xattri_flags;

	attrip->name = name_value;
	attrip->value = &name_value[attr->xattri_name_len];
	attrip->name_len = attr->xattri_name_len;
	attrip->value_len = attr->xattri_value_len;
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
	struct xfs_da_args		*args;

	attr = container_of(item, struct xfs_attr_item, xattri_list);
	args = &attr->xattri_args;

	if (attr->xattri_state == 0) {
		/* Only need to initialize args context once */
		name_value = ((char *)attr) + sizeof(struct xfs_attr_item);
		error = xfs_attr_args_init(args, attr->xattri_ip, name_value,
					   attr->xattri_name_len,
					   attr->xattri_flags);
		if (error)
			goto out;

		args->hashval = xfs_da_hashname(args->name, args->namelen);
		args->value = &name_value[attr->xattri_name_len];
		args->valuelen = attr->xattri_value_len;
		args->op_flags = XFS_DA_OP_OKNOENT;
		args->total = xfs_attr_calc_size(args, &local);
		attr->xattri_leaf_bp = NULL;
	}

	/*
	 * Always reset trans after EAGAIN cycle
	 * since the transaction is new
	 */
	args->trans = tp;

	error = xfs_trans_attr(args, done_item, &attr->xattri_leaf_bp,
			&attr->xattri_state, attr->xattri_op_flags);
out:
	if (error != -EAGAIN)
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

