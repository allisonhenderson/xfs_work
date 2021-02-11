// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2021 Oracle.  All Rights Reserved.
 * Author: Allison Collins <allison.henderson@oracle.com>
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
#include "xfs_da_format.h"
#include "xfs_log_format.h"
#include "xfs_trans.h"
#include "xfs_bmap.h"
#include "xfs_bmap_btree.h"
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
#include "xfs_trans_space.h"
#include "xfs_log_priv.h"
#include "xfs_log_recover.h"

static const struct xfs_item_ops xfs_attri_item_ops;
static const struct xfs_item_ops xfs_attrd_item_ops;

static inline size_t ATTR_NVEC_SIZE(size_t size)
{
	return 0;
}

static inline struct xfs_attri_log_item *ATTRI_ITEM(struct xfs_log_item *lip)
{
	return NULL;
}

STATIC void
xfs_attri_item_size(
	struct xfs_log_item	*lip,
	int			*nvecs,
	int			*nbytes)
{
}

STATIC void
xfs_attri_item_format(
	struct xfs_log_item	*lip,
	struct xfs_log_vec	*lv)
{
}

STATIC void
xfs_attri_item_unpin(
	struct xfs_log_item	*lip,
	int			remove)
{
}


STATIC void
xfs_attri_item_release(
	struct xfs_log_item	*lip)
{
}

static inline struct xfs_attrd_log_item *ATTRD_ITEM(struct xfs_log_item *lip)
{
	return NULL;
}

STATIC void
xfs_attrd_item_size(
	struct xfs_log_item		*lip,
	int				*nvecs,
	int				*nbytes)
{
}

STATIC void
xfs_attrd_item_format(
	struct xfs_log_item	*lip,
	struct xfs_log_vec	*lv)
{
}

STATIC void
xfs_attrd_item_release(
	struct xfs_log_item		*lip)
{
}

int
xfs_trans_attr_finish_update(
	struct xfs_delattr_context	*dac,
	struct xfs_attrd_log_item	*attrdp,
	struct xfs_buf			**leaf_bp,
	uint32_t			op_flags)
{
	return 0;
}

static struct xfs_log_item *
xfs_attr_create_intent(
	struct xfs_trans		*tp,
	struct list_head		*items,
	unsigned int			count,
	bool				sort)
{
	return NULL;
}

STATIC int
xfs_attr_finish_item(
	struct xfs_trans		*tp,
	struct xfs_log_item		*done,
	struct list_head		*item,
	struct xfs_btree_cur		**state)
{
	return 0;
}

STATIC void
xfs_attr_abort_intent(
	struct xfs_log_item		*intent)
{
}

STATIC void
xfs_attr_cancel_item(
	struct list_head		*item)
{
}

STATIC xfs_lsn_t
xfs_attri_item_committed(
	struct xfs_log_item		*lip,
	xfs_lsn_t			lsn)
{
	return 0;
}

STATIC bool
xfs_attri_item_match(
	struct xfs_log_item	*lip,
	uint64_t		intent_id)
{
	return false;
}

struct xfs_attrd_log_item *
xfs_trans_get_attrd(struct xfs_trans		*tp,
		  struct xfs_attri_log_item	*attrip)
{
	return NULL;
}

static const struct xfs_item_ops xfs_attrd_item_ops = {
	.flags		= XFS_ITEM_RELEASE_WHEN_COMMITTED,
	.iop_size	= xfs_attrd_item_size,
	.iop_format	= xfs_attrd_item_format,
	.iop_release    = xfs_attrd_item_release,
};


static struct xfs_log_item *
xfs_attr_create_done(
	struct xfs_trans		*tp,
	struct xfs_log_item		*intent,
	unsigned int			count)
{
	return NULL;
}

const struct xfs_defer_op_type xfs_attr_defer_type = {
	.max_items	= 1,
	.create_intent	= xfs_attr_create_intent,
	.abort_intent	= xfs_attr_abort_intent,
	.create_done	= xfs_attr_create_done,
	.finish_item	= xfs_attr_finish_item,
	.cancel_item	= xfs_attr_cancel_item,
};

STATIC int
xfs_attri_item_recover(
	struct xfs_log_item		*lip,
	struct list_head		*capture_list)
{
	return 0;
}

static struct xfs_log_item *
xfs_attri_item_relog(
	struct xfs_log_item		*intent,
	struct xfs_trans		*tp)
{
	return NULL;
}

static const struct xfs_item_ops xfs_attri_item_ops = {
	.iop_size	= xfs_attri_item_size,
	.iop_format	= xfs_attri_item_format,
	.iop_unpin	= xfs_attri_item_unpin,
	.iop_committed	= xfs_attri_item_committed,
	.iop_release    = xfs_attri_item_release,
	.iop_recover	= xfs_attri_item_recover,
	.iop_match	= xfs_attri_item_match,
	.iop_relog	= xfs_attri_item_relog,
};



STATIC int
xlog_recover_attri_commit_pass2(
	struct xlog                     *log,
	struct list_head		*buffer_list,
	struct xlog_recover_item        *item,
	xfs_lsn_t                       lsn)
{
	return 0;
}

const struct xlog_recover_item_ops xlog_attri_item_ops = {
	.item_type	= XFS_LI_ATTRI,
	.commit_pass2	= xlog_recover_attri_commit_pass2,
};

STATIC int
xlog_recover_attrd_commit_pass2(
	struct xlog			*log,
	struct list_head		*buffer_list,
	struct xlog_recover_item	*item,
	xfs_lsn_t			lsn)
{
	return 0;
}

const struct xlog_recover_item_ops xlog_attrd_item_ops = {
	.item_type	= XFS_LI_ATTRD,
	.commit_pass2	= xlog_recover_attrd_commit_pass2,
};
