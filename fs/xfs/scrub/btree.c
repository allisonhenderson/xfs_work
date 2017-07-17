/*
 * Copyright (C) 2017 Oracle.  All Rights Reserved.
 *
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it would be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write the Free Software Foundation,
 * Inc.,  51 Franklin St, Fifth Floor, Boston, MA  02110-1301, USA.
 */
#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"
#include "xfs_defer.h"
#include "xfs_btree.h"
#include "xfs_bit.h"
#include "xfs_log_format.h"
#include "xfs_trans.h"
#include "xfs_sb.h"
#include "xfs_inode.h"
#include "xfs_alloc.h"
#include "scrub/scrub.h"
#include "scrub/common.h"
#include "scrub/btree.h"
#include "scrub/trace.h"

/* btree scrubbing */

static const union xfs_btree_ptr nullptr = {
	.s = cpu_to_be32(NULLAGBLOCK),
	.l = cpu_to_be64(NULLFSBLOCK),
};

/* Check for btree operation errors . */
static bool
__xfs_scrub_btree_op_ok(
	struct xfs_scrub_context	*sc,
	struct xfs_btree_cur		*cur,
	int				level,
	int				*error,
	bool				xref,
	void				*ret_ip)
{
	if (*error == 0)
		return true;

	switch (*error) {
	case -EDEADLOCK:
		/* Used to restart an op with deadlock avoidance. */
		trace_xfs_scrub_deadlock_retry(sc->ip, sc->sm, *error);
		break;
	case -EFSBADCRC:
	case -EFSCORRUPTED:
		/* Note the badness but don't abort. */
		sc->sm->sm_flags |= xfs_scrub_corrupt_flag(xref);
		*error = 0;
		/* fall through */
	default:
		if (cur->bc_flags & XFS_BTREE_ROOT_IN_INODE)
			trace_xfs_scrub_ifork_btree_op_error(sc, cur, level,
					*error, ret_ip);
		else
			trace_xfs_scrub_btree_op_error(sc, cur, level,
					*error, ret_ip);
		break;
	}
	return false;
}

bool
xfs_scrub_btree_op_ok(
	struct xfs_scrub_context	*sc,
	struct xfs_btree_cur		*cur,
	int				level,
	int				*error)
{
	return __xfs_scrub_btree_op_ok(sc, cur, level, error, false,
			__return_address);
}

bool
xfs_scrub_btree_xref_op_ok(
	struct xfs_scrub_context	*sc,
	struct xfs_btree_cur		*cur,
	int				level,
	int				*error)
{
	return __xfs_scrub_btree_op_ok(sc, cur, level, error, true,
			__return_address);
}

/* Check for btree corruption. */
static bool
__xfs_scrub_btree_check_ok(
	struct xfs_scrub_context	*sc,
	struct xfs_btree_cur		*cur,
	int				level,
	bool				fs_ok,
	bool				xref,
	void				*ret_ip)
{
	if (fs_ok)
		return fs_ok;

	sc->sm->sm_flags |= xfs_scrub_corrupt_flag(xref);

	if (cur->bc_flags & XFS_BTREE_ROOT_IN_INODE)
		trace_xfs_scrub_ifork_btree_error(sc, cur, level,
				ret_ip);
	else
		trace_xfs_scrub_btree_error(sc, cur, level,
				ret_ip);
	return fs_ok;
}

bool
xfs_scrub_btree_check_ok(
	struct xfs_scrub_context	*sc,
	struct xfs_btree_cur		*cur,
	int				level,
	bool				fs_ok)
{
	return __xfs_scrub_btree_check_ok(sc, cur, level, fs_ok, false,
			__return_address);
}

bool
xfs_scrub_btree_xref_check_ok(
	struct xfs_scrub_context	*sc,
	struct xfs_btree_cur		*cur,
	int				level,
	bool				fs_ok)
{
	return __xfs_scrub_btree_check_ok(sc, cur, level, fs_ok, true,
			__return_address);
}

/*
 * Make sure this record is in order and doesn't stray outside of the parent
 * keys.
 */
STATIC int
xfs_scrub_btree_rec(
	struct xfs_scrub_btree	*bs)
{
	struct xfs_btree_cur	*cur = bs->cur;
	union xfs_btree_rec	*rec;
	union xfs_btree_key	key;
	union xfs_btree_key	hkey;
	union xfs_btree_key	*keyp;
	struct xfs_btree_block	*block;
	struct xfs_btree_block	*keyblock;
	struct xfs_buf		*bp;

	block = xfs_btree_get_block(cur, 0, &bp);
	rec = xfs_btree_rec_addr(cur, cur->bc_ptrs[0], block);

	trace_xfs_scrub_btree_rec(bs->sc, cur, 0);

	/* If this isn't the first record, are they in order? */
	xfs_scrub_btree_check_ok(bs->sc, cur, 0, bs->firstrec ||
			cur->bc_ops->recs_inorder(cur, &bs->lastrec, rec));
	bs->firstrec = false;
	memcpy(&bs->lastrec, rec, cur->bc_ops->rec_len);

	if (cur->bc_nlevels == 1)
		return 0;

	/* Is this at least as large as the parent low key? */
	cur->bc_ops->init_key_from_rec(&key, rec);
	keyblock = xfs_btree_get_block(cur, 1, &bp);
	keyp = xfs_btree_key_addr(cur, cur->bc_ptrs[1], keyblock);
	xfs_scrub_btree_check_ok(bs->sc, cur, 1,
			cur->bc_ops->diff_two_keys(cur, &key, keyp) >= 0);

	if (!(cur->bc_flags & XFS_BTREE_OVERLAPPING))
		return 0;

	/* Is this no larger than the parent high key? */
	cur->bc_ops->init_high_key_from_rec(&hkey, rec);
	keyp = xfs_btree_high_key_addr(cur, cur->bc_ptrs[1], keyblock);
	xfs_scrub_btree_check_ok(bs->sc, cur, 1,
			cur->bc_ops->diff_two_keys(cur, keyp, &hkey) >= 0);

	return 0;
}

/*
 * Make sure this key is in order and doesn't stray outside of the parent
 * keys.
 */
STATIC int
xfs_scrub_btree_key(
	struct xfs_scrub_btree	*bs,
	int			level)
{
	struct xfs_btree_cur	*cur = bs->cur;
	union xfs_btree_key	*key;
	union xfs_btree_key	*keyp;
	struct xfs_btree_block	*block;
	struct xfs_btree_block	*keyblock;
	struct xfs_buf		*bp;

	block = xfs_btree_get_block(cur, level, &bp);
	key = xfs_btree_key_addr(cur, cur->bc_ptrs[level], block);

	trace_xfs_scrub_btree_key(bs->sc, cur, level);

	/* If this isn't the first key, are they in order? */
	xfs_scrub_btree_check_ok(bs->sc, cur, level, bs->firstkey[level] ||
			cur->bc_ops->keys_inorder(cur, &bs->lastkey[level], key));
	bs->firstkey[level] = false;
	memcpy(&bs->lastkey[level], key, cur->bc_ops->key_len);

	if (level + 1 >= cur->bc_nlevels)
		return 0;

	/* Is this at least as large as the parent low key? */
	keyblock = xfs_btree_get_block(cur, level + 1, &bp);
	keyp = xfs_btree_key_addr(cur, cur->bc_ptrs[level + 1], keyblock);
	xfs_scrub_btree_check_ok(bs->sc, cur, level,
			cur->bc_ops->diff_two_keys(cur, key, keyp) >= 0);

	if (!(cur->bc_flags & XFS_BTREE_OVERLAPPING))
		return 0;

	/* Is this no larger than the parent high key? */
	key = xfs_btree_high_key_addr(cur, cur->bc_ptrs[level], block);
	keyp = xfs_btree_high_key_addr(cur, cur->bc_ptrs[level + 1], keyblock);
	xfs_scrub_btree_check_ok(bs->sc, cur, level,
			cur->bc_ops->diff_two_keys(cur, keyp, key) >= 0);

	return 0;
}

/* Check a btree pointer. */
static int
xfs_scrub_btree_ptr(
	struct xfs_scrub_btree		*bs,
	int				level,
	union xfs_btree_ptr		*ptr)
{
	struct xfs_btree_cur		*cur = bs->cur;
	xfs_daddr_t			daddr;
	xfs_daddr_t			eofs;

	if (!xfs_scrub_btree_check_ok(bs->sc, cur, level,
			xfs_btree_diff_two_ptrs(cur, ptr, &nullptr) != 0))
		goto corrupt;
	if (cur->bc_flags & XFS_BTREE_LONG_PTRS) {
		daddr = XFS_FSB_TO_DADDR(cur->bc_mp, be64_to_cpu(ptr->l));
	} else {
		if (!xfs_scrub_btree_check_ok(bs->sc, cur, level,
				cur->bc_private.a.agno != NULLAGNUMBER))
			goto corrupt;

		daddr = XFS_AGB_TO_DADDR(cur->bc_mp, cur->bc_private.a.agno,
				be32_to_cpu(ptr->s));
	}
	eofs = XFS_FSB_TO_BB(cur->bc_mp, cur->bc_mp->m_sb.sb_dblocks);
	if (!xfs_scrub_btree_check_ok(bs->sc, cur, level,
			daddr != 0 && daddr < eofs))
		goto corrupt;

	return 0;

corrupt:
	return -EFSCORRUPTED;
}

/* Check that a btree block's sibling matches what we expect it. */
STATIC int
xfs_scrub_btree_block_check_sibling(
	struct xfs_scrub_btree		*bs,
	int				level,
	int				direction,
	union xfs_btree_ptr		*sibling)
{
	struct xfs_btree_cur		*cur = bs->cur;
	struct xfs_btree_block		*pblock;
	struct xfs_buf			*pbp;
	struct xfs_btree_cur		*ncur;
	union xfs_btree_ptr		*pp;
	int				success;
	int				error;

	if (!xfs_btree_diff_two_ptrs(cur, &nullptr, sibling))
		return 0;

	error = xfs_btree_dup_cursor(cur, &ncur);
	if (error)
		return error;

	if (direction > 0)
		error = xfs_btree_increment(ncur, level + 1, &success);
	else
		error = xfs_btree_decrement(ncur, level + 1, &success);
	if (!xfs_scrub_btree_op_ok(bs->sc, cur, level + 1, &error) ||
	    !xfs_scrub_btree_check_ok(bs->sc, cur, level + 1, success))
		goto out;

	pblock = xfs_btree_get_block(ncur, level + 1, &pbp);
	pp = xfs_btree_ptr_addr(ncur, ncur->bc_ptrs[level + 1], pblock);
	error = xfs_scrub_btree_ptr(bs, level + 1, pp);
	if (error) {
		/*
		 * _scrub_btree_ptr already recorded a garbage sibling.
		 * Don't let the EFSCORRUPTED bubble up and prevent more
		 * scanning of the data structure.
		 */
		error = 0;
		goto out;
	}

	xfs_scrub_btree_check_ok(bs->sc, cur, level,
			!xfs_btree_diff_two_ptrs(cur, pp, sibling));
out:
	xfs_btree_del_cursor(ncur, XFS_BTREE_ERROR);
	return error;
}

/* Check the siblings of a btree block. */
STATIC int
xfs_scrub_btree_block_check_siblings(
	struct xfs_scrub_btree		*bs,
	struct xfs_btree_block		*block)
{
	struct xfs_btree_cur		*cur = bs->cur;
	union xfs_btree_ptr		leftsib;
	union xfs_btree_ptr		rightsib;
	int				level;
	int				error = 0;

	xfs_btree_get_sibling(cur, block, &leftsib, XFS_BB_LEFTSIB);
	xfs_btree_get_sibling(cur, block, &rightsib, XFS_BB_RIGHTSIB);
	level = xfs_btree_get_level(block);

	/* Root block should never have siblings. */
	if (level == cur->bc_nlevels - 1) {
		xfs_scrub_btree_check_ok(bs->sc, cur, level,
				!xfs_btree_diff_two_ptrs(cur, &nullptr,
					&leftsib) &&
				!xfs_btree_diff_two_ptrs(cur, &nullptr,
					&rightsib));
		goto out;
	}

	/* Does the left sibling match the parent level left block? */
	error = xfs_scrub_btree_block_check_sibling(bs, level, -1, &leftsib);
	if (error)
		return error;

	/* Does the right sibling match the parent level right block? */
	error = xfs_scrub_btree_block_check_sibling(bs, level, 1, &rightsib);
	if (error)
		return error;
out:
	return error;
}

struct check_owner {
	struct list_head	list;
	xfs_daddr_t		daddr;
};

/*
 * Make sure this btree block isn't in the free list and that there's
 * an rmap record for it.
 */
STATIC int
xfs_scrub_btree_check_block_owner(
	struct xfs_scrub_btree		*bs,
	xfs_daddr_t			daddr)
{
	struct xfs_scrub_ag		sa = { 0 };
	struct xfs_scrub_ag		*psa;
	xfs_agnumber_t			agno;
	xfs_agblock_t			bno;
	bool				is_freesp;
	int				error = 0;

	agno = xfs_daddr_to_agno(bs->cur->bc_mp, daddr);
	bno = xfs_daddr_to_agbno(bs->cur->bc_mp, daddr);

	if (bs->cur->bc_flags & XFS_BTREE_LONG_PTRS) {
		error = xfs_scrub_ag_init(bs->sc, agno, &sa);
		if (error)
			return error;
		psa = &sa;
	} else {
		psa = &bs->sc->sa;
	}

	/* Cross-reference with the bnobt. */
	if (psa->bno_cur) {
		error = xfs_alloc_has_record(psa->bno_cur, bno, 1, &is_freesp);
		if (xfs_scrub_should_xref(bs->sc, &error, &psa->bno_cur))
			xfs_scrub_btree_xref_check_ok(bs->sc, psa->bno_cur, 0,
					!is_freesp);
	}

	if (psa == &sa)
		xfs_scrub_ag_free(bs->sc, &sa);

	return error;
}

/* Check the owner of a btree block. */
STATIC int
xfs_scrub_btree_check_owner(
	struct xfs_scrub_btree		*bs,
	struct xfs_buf			*bp)
{
	struct xfs_btree_cur		*cur = bs->cur;
	struct check_owner		*co;

	if ((cur->bc_flags & XFS_BTREE_ROOT_IN_INODE) && bp == NULL)
		return 0;

	/*
	 * We want to cross-reference each btree block with the bnobt
	 * and the rmapbt.  We cannot cross-reference the bnobt or
	 * rmapbt while scanning the bnobt or rmapbt, respectively,
	 * because we cannot alter the cursor and we'd prefer not to
	 * duplicate cursors.  Therefore, save the buffer daddr for
	 * later scanning.
	 */
	if (cur->bc_btnum == XFS_BTNUM_BNO || cur->bc_btnum == XFS_BTNUM_RMAP) {
		co = kmem_alloc(sizeof(struct check_owner), KM_SLEEP | KM_NOFS);
		co->daddr = XFS_BUF_ADDR(bp);
		list_add_tail(&co->list, &bs->to_check);
		return 0;
	}

	return xfs_scrub_btree_check_block_owner(bs, XFS_BUF_ADDR(bp));
}

/* Grab and scrub a btree block. */
STATIC int
xfs_scrub_btree_block(
	struct xfs_scrub_btree		*bs,
	int				level,
	union xfs_btree_ptr		*pp,
	struct xfs_btree_block		**pblock,
	struct xfs_buf			**pbp)
{
	int				error;

	error = xfs_btree_lookup_get_block(bs->cur, level, pp, pblock);
	if (error)
		return error;

	xfs_btree_get_block(bs->cur, level, pbp);
	error = xfs_btree_check_block(bs->cur, *pblock, level, *pbp);
	if (error)
		return error;

	error = xfs_scrub_btree_check_owner(bs, *pbp);
	if (error)
		return error;

	return xfs_scrub_btree_block_check_siblings(bs, *pblock);
}

/*
 * Visit all nodes and leaves of a btree.  Check that all pointers and
 * records are in order, that the keys reflect the records, and use a callback
 * so that the caller can verify individual records.  The callback is the same
 * as the one for xfs_btree_query_range, so therefore this function also
 * returns XFS_BTREE_QUERY_RANGE_ABORT, zero, or a negative error code.
 */
int
xfs_scrub_btree(
	struct xfs_scrub_context	*sc,
	struct xfs_btree_cur		*cur,
	xfs_scrub_btree_rec_fn		scrub_fn,
	struct xfs_owner_info		*oinfo,
	void				*private)
{
	struct xfs_scrub_btree		bs = {0};
	union xfs_btree_ptr		ptr;
	union xfs_btree_ptr		*pp;
	union xfs_btree_rec		*recp;
	struct xfs_btree_block		*block;
	int				level;
	struct xfs_buf			*bp;
	struct check_owner		*co;
	struct check_owner		*n;
	int				i;
	int				error = 0;

	/* Initialize scrub state */
	bs.cur = cur;
	bs.scrub_rec = scrub_fn;
	bs.oinfo = oinfo;
	bs.firstrec = true;
	bs.private = private;
	bs.sc = sc;
	for (i = 0; i < XFS_BTREE_MAXLEVELS; i++)
		bs.firstkey[i] = true;
	INIT_LIST_HEAD(&bs.to_check);

	/* Don't try to check a tree with a height we can't handle. */
	if (!xfs_scrub_btree_check_ok(sc, cur, 0, cur->bc_nlevels > 0 &&
			cur->bc_nlevels <= XFS_BTREE_MAXLEVELS))
		goto out;

	/* Make sure the root isn't in the superblock. */
	if (!(cur->bc_flags & XFS_BTREE_ROOT_IN_INODE)) {
		cur->bc_ops->init_ptr_from_cur(cur, &ptr);
		error = xfs_scrub_btree_ptr(&bs, cur->bc_nlevels, &ptr);
		if (!xfs_scrub_btree_op_ok(sc, cur, cur->bc_nlevels - 1, &error))
			goto out;
	}

	/* Load the root of the btree. */
	level = cur->bc_nlevels - 1;
	cur->bc_ops->init_ptr_from_cur(cur, &ptr);
	error = xfs_scrub_btree_block(&bs, level, &ptr, &block, &bp);
	if (!xfs_scrub_btree_op_ok(sc, cur, cur->bc_nlevels - 1, &error))
		goto out;

	cur->bc_ptrs[level] = 1;

	while (level < cur->bc_nlevels) {
		block = xfs_btree_get_block(cur, level, &bp);

		if (level == 0) {
			/* End of leaf, pop back towards the root. */
			if (cur->bc_ptrs[level] >
			    be16_to_cpu(block->bb_numrecs)) {
				if (level < cur->bc_nlevels - 1)
					cur->bc_ptrs[level + 1]++;
				level++;
				continue;
			}

			/* Records in order for scrub? */
			error = xfs_scrub_btree_rec(&bs);
			if (error)
				goto out;

			/* Call out to the record checker. */
			recp = xfs_btree_rec_addr(cur, cur->bc_ptrs[0], block);
			error = bs.scrub_rec(&bs, recp);
			if (error < 0 ||
			    error == XFS_BTREE_QUERY_RANGE_ABORT)
				break;
			if (xfs_scrub_should_terminate(&error))
				break;

			cur->bc_ptrs[level]++;
			continue;
		}

		/* End of node, pop back towards the root. */
		if (cur->bc_ptrs[level] > be16_to_cpu(block->bb_numrecs)) {
			if (level < cur->bc_nlevels - 1)
				cur->bc_ptrs[level + 1]++;
			level++;
			continue;
		}

		/* Keys in order for scrub? */
		error = xfs_scrub_btree_key(&bs, level);
		if (error)
			goto out;

		/* Drill another level deeper. */
		pp = xfs_btree_ptr_addr(cur, cur->bc_ptrs[level], block);
		error = xfs_scrub_btree_ptr(&bs, level, pp);
		if (error) {
			error = 0;
			cur->bc_ptrs[level]++;
			continue;
		}
		level--;
		error = xfs_scrub_btree_block(&bs, level, pp, &block, &bp);
		if (!xfs_scrub_btree_op_ok(sc, cur, level, &error))
			goto out;

		cur->bc_ptrs[level] = 1;
	}

out:
	/* Process deferred owner checks on btree blocks. */
	list_for_each_entry_safe(co, n, &bs.to_check, list) {
		if (!error)
			error = xfs_scrub_btree_check_block_owner(&bs,
					co->daddr);
		list_del(&co->list);
		kmem_free(co);
	}

	return error;
}
