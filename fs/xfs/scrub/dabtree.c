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
#include "xfs_inode_fork.h"
#include "xfs_da_format.h"
#include "xfs_da_btree.h"
#include "xfs_dir2.h"
#include "xfs_dir2_priv.h"
#include "xfs_attr_leaf.h"
#include "scrub/xfs_scrub.h"
#include "scrub/scrub.h"
#include "scrub/common.h"
#include "scrub/trace.h"
#include "scrub/dabtree.h"

/* Directory/Attribute Btree */

/* Check for da btree operation errors. */
bool
xfs_scrub_da_op_ok(
	struct xfs_scrub_da_btree	*ds,
	int				level,
	int				*error)
{
	struct xfs_scrub_context	*sc = ds->sc;

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
		sc->sm->sm_flags |= XFS_SCRUB_OFLAG_CORRUPT;
		*error = 0;
		/* fall through */
	default:
		trace_xfs_scrub_file_op_error(sc, ds->dargs.whichfork,
				xfs_dir2_da_to_db(ds->dargs.geo,
					ds->state->path.blk[level].blkno),
				*error, __return_address);
		break;
	}
	return false;
}

/* Check for da btree corruption. */
bool
xfs_scrub_da_check_ok(
	struct xfs_scrub_da_btree	*ds,
	int				level,
	bool				fs_ok)
{
	struct xfs_scrub_context	*sc = ds->sc;

	if (fs_ok)
		return fs_ok;

	sc->sm->sm_flags |= XFS_SCRUB_OFLAG_CORRUPT;

	trace_xfs_scrub_fblock_error(sc, ds->dargs.whichfork,
			xfs_dir2_da_to_db(ds->dargs.geo,
				ds->state->path.blk[level].blkno),
			__return_address);
	return fs_ok;
}

/* Find an entry at a certain level in a da btree. */
STATIC void *
xfs_scrub_da_btree_entry(
	struct xfs_scrub_da_btree	*ds,
	int				level,
	int				rec)
{
	char				*ents;
	void				*(*fn)(void *);
	size_t				sz;
	struct xfs_da_state_blk		*blk;

	/* Dispatch the entry finding function. */
	blk = &ds->state->path.blk[level];
	switch (blk->magic) {
	case XFS_ATTR_LEAF_MAGIC:
	case XFS_ATTR3_LEAF_MAGIC:
		fn = (xfs_da_leaf_ents_fn)xfs_attr3_leaf_entryp;
		sz = sizeof(struct xfs_attr_leaf_entry);
		break;
	case XFS_DIR2_LEAFN_MAGIC:
	case XFS_DIR3_LEAFN_MAGIC:
		fn = (xfs_da_leaf_ents_fn)ds->dargs.dp->d_ops->leaf_ents_p;
		sz = sizeof(struct xfs_dir2_leaf_entry);
		break;
	case XFS_DIR2_LEAF1_MAGIC:
	case XFS_DIR3_LEAF1_MAGIC:
		fn = (xfs_da_leaf_ents_fn)ds->dargs.dp->d_ops->leaf_ents_p;
		sz = sizeof(struct xfs_dir2_leaf_entry);
		break;
	case XFS_DA_NODE_MAGIC:
	case XFS_DA3_NODE_MAGIC:
		fn = (xfs_da_leaf_ents_fn)ds->dargs.dp->d_ops->node_tree_p;
		sz = sizeof(struct xfs_da_node_entry);
		break;
	default:
		return NULL;
	}

	ents = fn(blk->bp->b_addr);
	return ents + (sz * rec);
}

/* Scrub a da btree hash (key). */
int
xfs_scrub_da_btree_hash(
	struct xfs_scrub_da_btree	*ds,
	int				level,
	__be32				*hashp)
{
	struct xfs_da_state_blk		*blks;
	struct xfs_da_node_entry	*btree;
	xfs_dahash_t			hash;
	xfs_dahash_t			parent_hash;

	/* Is this hash in order? */
	hash = be32_to_cpu(*hashp);
	xfs_scrub_da_check_ok(ds, level, hash >= ds->hashes[level]);
	ds->hashes[level] = hash;

	if (level == 0)
		return 0;

	/* Is this hash no larger than the parent hash? */
	blks = ds->state->path.blk;
	btree = xfs_scrub_da_btree_entry(ds, level - 1, blks[level - 1].index);
	parent_hash = be32_to_cpu(btree->hashval);
	xfs_scrub_da_check_ok(ds, level, hash <= parent_hash);

	return 0;
}

/* Scrub a da btree pointer. */
STATIC int
xfs_scrub_da_btree_ptr(
	struct xfs_scrub_da_btree	*ds,
	int				level,
	xfs_dablk_t			blkno)
{
	xfs_scrub_da_check_ok(ds, level, blkno >= ds->lowest &&
			(ds->highest == 0 || blkno < ds->highest));

	return 0;
}

/*
 * The da btree scrubber can handle leaf1 blocks as a degenerate
 * form of da btree.  Since the regular da code doesn't handle
 * leaf1, we must multiplex the verifiers.
 */
static void
xfs_scrub_da_btree_read_verify(
	struct xfs_buf		*bp)
{
	struct xfs_da_blkinfo	*info = bp->b_addr;

	switch (be16_to_cpu(info->magic)) {
	case XFS_DIR2_LEAF1_MAGIC:
	case XFS_DIR3_LEAF1_MAGIC:
		bp->b_ops = &xfs_dir3_leaf1_buf_ops;
		bp->b_ops->verify_read(bp);
		return;
	default:
		bp->b_ops = &xfs_da3_node_buf_ops;
		bp->b_ops->verify_read(bp);
		return;
	}
}
static void
xfs_scrub_da_btree_write_verify(
	struct xfs_buf	*bp)
{
	struct xfs_da_blkinfo	*info = bp->b_addr;

	switch (be16_to_cpu(info->magic)) {
	case XFS_DIR2_LEAF1_MAGIC:
	case XFS_DIR3_LEAF1_MAGIC:
		bp->b_ops = &xfs_dir3_leaf1_buf_ops;
		bp->b_ops->verify_write(bp);
		return;
	default:
		bp->b_ops = &xfs_da3_node_buf_ops;
		bp->b_ops->verify_write(bp);
		return;
	}
}

static const struct xfs_buf_ops xfs_scrub_da_btree_buf_ops = {
	.name = "xfs_scrub_da_btree",
	.verify_read = xfs_scrub_da_btree_read_verify,
	.verify_write = xfs_scrub_da_btree_write_verify,
};

/* Check a block's sibling. */
STATIC int
xfs_scrub_da_btree_block_check_sibling(
	struct xfs_scrub_da_btree	*ds,
	int				level,
	int				direction,
	xfs_dablk_t			sibling)
{
	int				retval;
	int				error;

	if (!sibling)
		return 0;

	/* Move the alternate cursor back one block. */
	memcpy(&ds->state->altpath, &ds->state->path,
			sizeof(ds->state->altpath));
	error = xfs_da3_path_shift(ds->state, &ds->state->altpath,
			direction, false, &retval);
	if (!xfs_scrub_da_op_ok(ds, level, &error) ||
	    !xfs_scrub_da_check_ok(ds, level, retval == 0))
		return error;

	xfs_scrub_da_check_ok(ds, level,
			ds->state->altpath.blk[level].blkno == sibling);
	xfs_trans_brelse(ds->dargs.trans, ds->state->altpath.blk[level].bp);
	return error;
}

/* Check a block's sibling pointers. */
STATIC int
xfs_scrub_da_btree_block_check_siblings(
	struct xfs_scrub_da_btree	*ds,
	int				level,
	struct xfs_da_blkinfo		*hdr)
{
	xfs_dablk_t			forw;
	xfs_dablk_t			back;
	int				error = 0;

	forw = be32_to_cpu(hdr->forw);
	back = be32_to_cpu(hdr->back);

	/* Top level blocks should not have sibling pointers. */
	if (level == 0) {
		xfs_scrub_da_check_ok(ds, level, forw == 0 && back == 0);
		return error;
	}

	/* Check back (left) pointer. */
	error = xfs_scrub_da_btree_block_check_sibling(ds, level, 0, back);
	if (error)
		goto out;

	/* Check forw (right) pointer. */
	error = xfs_scrub_da_btree_block_check_sibling(ds, level, 1, forw);

out:
	memset(&ds->state->altpath, 0, sizeof(ds->state->altpath));
	return error;
}

/* Load a dir/attribute block from a btree. */
STATIC int
xfs_scrub_da_btree_block(
	struct xfs_scrub_da_btree	*ds,
	int				level,
	xfs_dablk_t			blkno)
{
	struct xfs_da_state_blk		*blk;
	struct xfs_da_intnode		*node;
	struct xfs_da_node_entry	*btree;
	struct xfs_da3_blkinfo		*hdr3;
	struct xfs_da_args		*dargs = &ds->dargs;
	struct xfs_inode		*ip = ds->dargs.dp;
	xfs_ino_t			owner;
	int				*pmaxrecs;
	struct xfs_da3_icnode_hdr	nodehdr;
	int				error;

	blk = &ds->state->path.blk[level];
	ds->state->path.active = level + 1;

	/* Release old block. */
	if (blk->bp) {
		xfs_trans_brelse(dargs->trans, blk->bp);
		blk->bp = NULL;
	}

	/* Check the pointer. */
	blk->blkno = blkno;
	error = xfs_scrub_da_btree_ptr(ds, level, blkno);
	if (error) {
		blk->blkno = 0;
		goto out;
	}

	/* Read the buffer. */
	error = xfs_da_read_buf(dargs->trans, dargs->dp, blk->blkno, -2,
			&blk->bp, dargs->whichfork,
			&xfs_scrub_da_btree_buf_ops);
	if (!xfs_scrub_da_op_ok(ds, level, &error))
		goto out_nobuf;

	/* It's ok for a directory not to have a da btree in it. */
	if (ds->dargs.whichfork == XFS_DATA_FORK && level == 0 &&
			blk->bp == NULL)
		goto out_nobuf;
	if (!xfs_scrub_da_check_ok(ds, level, blk->bp != NULL))
		goto out_nobuf;

	hdr3 = blk->bp->b_addr;
	blk->magic = be16_to_cpu(hdr3->hdr.magic);
	pmaxrecs = &ds->maxrecs[level];

	/* Check the owner. */
	if (xfs_sb_version_hascrc(&ip->i_mount->m_sb)) {
		owner = be64_to_cpu(hdr3->owner);
		error = -EFSCORRUPTED;
		if (!xfs_scrub_da_check_ok(ds, level, owner == ip->i_ino))
			goto out;
	}

	/* Check the siblings. */
	error = xfs_scrub_da_btree_block_check_siblings(ds, level, &hdr3->hdr);
	if (error)
		goto out;

	/* Interpret the buffer. */
	error = -EFSCORRUPTED;
	switch (blk->magic) {
	case XFS_ATTR_LEAF_MAGIC:
	case XFS_ATTR3_LEAF_MAGIC:
		xfs_trans_buf_set_type(dargs->trans, blk->bp,
				XFS_BLFT_ATTR_LEAF_BUF);
		blk->magic = XFS_ATTR_LEAF_MAGIC;
		blk->hashval = xfs_attr_leaf_lasthash(blk->bp, pmaxrecs);
		xfs_scrub_da_check_ok(ds, level, ds->tree_level == 0);
		break;
	case XFS_DIR2_LEAFN_MAGIC:
	case XFS_DIR3_LEAFN_MAGIC:
		xfs_trans_buf_set_type(dargs->trans, blk->bp,
				XFS_BLFT_DIR_LEAFN_BUF);
		blk->magic = XFS_DIR2_LEAFN_MAGIC;
		blk->hashval = xfs_dir2_leaf_lasthash(ip, blk->bp, pmaxrecs);
		xfs_scrub_da_check_ok(ds, level, ds->tree_level == 0);
		break;
	case XFS_DIR2_LEAF1_MAGIC:
	case XFS_DIR3_LEAF1_MAGIC:
		xfs_trans_buf_set_type(dargs->trans, blk->bp,
				XFS_BLFT_DIR_LEAF1_BUF);
		blk->magic = XFS_DIR2_LEAF1_MAGIC;
		blk->hashval = xfs_dir2_leaf_lasthash(ip, blk->bp, pmaxrecs);
		xfs_scrub_da_check_ok(ds, level, ds->tree_level == 0);
		break;
	case XFS_DA_NODE_MAGIC:
	case XFS_DA3_NODE_MAGIC:
		xfs_trans_buf_set_type(dargs->trans, blk->bp,
				XFS_BLFT_DA_NODE_BUF);
		blk->magic = XFS_DA_NODE_MAGIC;
		node = blk->bp->b_addr;
		ip->d_ops->node_hdr_from_disk(&nodehdr, node);
		btree = ip->d_ops->node_tree_p(node);
		*pmaxrecs = nodehdr.count;
		blk->hashval = be32_to_cpu(btree[*pmaxrecs - 1].hashval);
		if (level == 0) {
			if (!xfs_scrub_da_check_ok(ds, level,
					nodehdr.level < XFS_DA_NODE_MAXDEPTH))
				goto out;
			ds->tree_level = nodehdr.level;
		} else
			if (!xfs_scrub_da_check_ok(ds, level,
					ds->tree_level == nodehdr.level))
				goto out;
		break;
	default:
		xfs_scrub_da_check_ok(ds, level, false);
		xfs_trans_brelse(dargs->trans, blk->bp);
		blk->bp = NULL;
		blk->blkno = 0;
		break;
	}
	error = 0;

out:
	return error;
out_nobuf:
	blk->blkno = 0;
	return error;
}

/* Visit all nodes and leaves of a da btree. */
int
xfs_scrub_da_btree(
	struct xfs_scrub_context	*sc,
	int				whichfork,
	xfs_scrub_da_btree_rec_fn	scrub_fn)
{
	struct xfs_scrub_da_btree	ds;
	struct xfs_mount		*mp = sc->mp;
	struct xfs_da_state_blk		*blks;
	struct xfs_da_node_entry	*key;
	void				*rec;
	xfs_dablk_t			blkno;
	bool				is_attr;
	int				level;
	int				error;

	memset(&ds, 0, sizeof(ds));
	/* Skip short format data structures; no btree to scan. */
	if (XFS_IFORK_FORMAT(sc->ip, whichfork) != XFS_DINODE_FMT_EXTENTS &&
	    XFS_IFORK_FORMAT(sc->ip, whichfork) != XFS_DINODE_FMT_BTREE)
		return 0;

	/* Set up initial da state. */
	is_attr = whichfork == XFS_ATTR_FORK;
	ds.dargs.geo = is_attr ? mp->m_attr_geo : mp->m_dir_geo;
	ds.dargs.dp = sc->ip;
	ds.dargs.whichfork = whichfork;
	ds.dargs.trans = sc->tp;
	ds.dargs.op_flags = XFS_DA_OP_OKNOENT;
	ds.state = xfs_da_state_alloc();
	ds.state->args = &ds.dargs;
	ds.state->mp = mp;
	ds.sc = sc;
	blkno = ds.lowest = is_attr ? 0 : ds.dargs.geo->leafblk;
	ds.highest = is_attr ? 0 : ds.dargs.geo->freeblk;
	level = 0;

	/* Find the root of the da tree, if present. */
	blks = ds.state->path.blk;
	error = xfs_scrub_da_btree_block(&ds, level, blkno);
	if (error)
		goto out_state;
	if (blks[level].bp == NULL)
		goto out_state;

	blks[level].index = 0;
	while (level >= 0 && level < XFS_DA_NODE_MAXDEPTH) {
		/* Handle leaf block. */
		if (blks[level].magic != XFS_DA_NODE_MAGIC) {
			/* End of leaf, pop back towards the root. */
			if (blks[level].index >= ds.maxrecs[level]) {
				if (level > 0)
					blks[level - 1].index++;
				ds.tree_level++;
				level--;
				continue;
			}

			/* Dispatch record scrubbing. */
			rec = xfs_scrub_da_btree_entry(&ds, level,
					blks[level].index);
			error = scrub_fn(&ds, level, rec);
			if (error < 0 ||
			    error == XFS_BTREE_QUERY_RANGE_ABORT)
				break;
			if (xfs_scrub_should_terminate(&error))
				break;

			blks[level].index++;
			continue;
		}


		/* End of node, pop back towards the root. */
		if (blks[level].index >= ds.maxrecs[level]) {
			if (level > 0)
				blks[level - 1].index++;
			ds.tree_level++;
			level--;
			continue;
		}

		/* Hashes in order for scrub? */
		key = xfs_scrub_da_btree_entry(&ds, level, blks[level].index);
		error = xfs_scrub_da_btree_hash(&ds, level, &key->hashval);
		if (error)
			goto out;

		/* Drill another level deeper. */
		blkno = be32_to_cpu(key->before);
		level++;
		ds.tree_level--;
		error = xfs_scrub_da_btree_block(&ds, level, blkno);
		if (error)
			goto out;
		if (blks[level].bp == NULL)
			goto out;

		blks[level].index = 0;
	}

out:
	/* Release all the buffers we're tracking. */
	for (level = 0; level < XFS_DA_NODE_MAXDEPTH; level++) {
		if (blks[level].bp == NULL)
			continue;
		xfs_trans_brelse(sc->tp, blks[level].bp);
		blks[level].bp = NULL;
	}

out_state:
	xfs_da_state_free(ds.state);
	return error;
}
