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
#include "xfs_icache.h"
#include "xfs_itable.h"
#include "xfs_da_format.h"
#include "xfs_da_btree.h"
#include "xfs_dir2.h"
#include "xfs_dir2_priv.h"
#include "scrub/xfs_scrub.h"
#include "scrub/scrub.h"
#include "scrub/common.h"
#include "scrub/trace.h"
#include "scrub/dabtree.h"

/* Set us up to scrub directories. */
int
xfs_scrub_setup_directory(
	struct xfs_scrub_context	*sc,
	struct xfs_inode		*ip)
{
	return xfs_scrub_setup_inode_contents(sc, ip, 0);
}

/* Directories */

/* Scrub a directory entry. */

struct xfs_scrub_dir_ctx {
	struct dir_context		dc;
	struct xfs_scrub_context	*sc;
};

/* Check that an inode's mode matches a given DT_ type. */
STATIC int
xfs_scrub_dir_check_ftype(
	struct xfs_scrub_dir_ctx	*sdc,
	xfs_fileoff_t			offset,
	xfs_ino_t			inum,
	int				dtype)
{
	struct xfs_mount		*mp = sdc->sc->mp;
	struct xfs_inode		*ip;
	int				ino_dtype;
	int				error = 0;

	if (!xfs_sb_version_hasftype(&mp->m_sb)) {
		xfs_scrub_fblock_check_ok(sdc->sc, XFS_DATA_FORK, offset,
				dtype == DT_UNKNOWN || dtype == DT_DIR);
		goto out;
	}

	error = xfs_iget(mp, sdc->sc->tp, inum, 0, 0, &ip);
	if (!xfs_scrub_fblock_op_ok(sdc->sc, XFS_DATA_FORK, offset, &error))
		goto out;

	/* Convert mode to the DT_* values that dir_emit uses. */
	ino_dtype = (VFS_I(ip)->i_mode & S_IFMT) >> 12;
	xfs_scrub_fblock_check_ok(sdc->sc, XFS_DATA_FORK, offset,
			ino_dtype == dtype);
	iput(VFS_I(ip));
out:
	return error;
}

/* Scrub a single directory entry. */
STATIC int
xfs_scrub_dir_actor(
	struct dir_context		*dc,
	const char			*name,
	int				namelen,
	loff_t				pos,
	u64				ino,
	unsigned			type)
{
	struct xfs_mount		*mp;
	struct xfs_inode		*ip;
	struct xfs_scrub_dir_ctx	*sdc;
	struct xfs_name			xname;
	xfs_ino_t			lookup_ino;
	xfs_dablk_t			offset;
	int				error = 0;

	sdc = container_of(dc, struct xfs_scrub_dir_ctx, dc);
	ip = sdc->sc->ip;
	mp = ip->i_mount;
	offset = xfs_dir2_db_to_da(mp->m_dir_geo,
			xfs_dir2_dataptr_to_db(mp->m_dir_geo, pos));

	/* Does this inode number make sense? */
	if (!xfs_scrub_fblock_check_ok(sdc->sc, XFS_DATA_FORK, offset,
			xfs_dir_ino_validate(mp, ino) == 0 &&
			!xfs_internal_inum(mp, ino)))
		goto out;

	/* Verify that we can look up this name by hash. */
	xname.name = name;
	xname.len = namelen;
	xname.type = XFS_DIR3_FT_UNKNOWN;

	error = xfs_dir_lookup(sdc->sc->tp, ip, &xname, &lookup_ino, NULL);
	if (!xfs_scrub_fblock_op_ok(sdc->sc, XFS_DATA_FORK, offset, &error))
		goto fail_xref;
	if (!xfs_scrub_fblock_check_ok(sdc->sc, XFS_DATA_FORK, offset,
			lookup_ino == ino))
		goto out;

	if (!strncmp(".", name, namelen)) {
		/* If this is "." then check that the inum matches the dir. */
		xfs_scrub_fblock_check_ok(sdc->sc, XFS_DATA_FORK, offset,
				(!xfs_sb_version_hasftype(&mp->m_sb) ||
				 type == DT_DIR) &&
				ino == ip->i_ino);
	} else if (!strncmp("..", name, namelen)) {
		/*
		 * If this is ".." in the root inode, check that the inum
		 * matches this dir.
		 */
		xfs_scrub_fblock_check_ok(sdc->sc, XFS_DATA_FORK, offset,
				(!xfs_sb_version_hasftype(&mp->m_sb) ||
				 type == DT_DIR) &&
				(ip->i_ino != mp->m_sb.sb_rootino ||
				 ino == ip->i_ino));
	}

	/* Verify the file type. */
	error = xfs_scrub_dir_check_ftype(sdc, offset, lookup_ino, type);
	if (error)
		goto out;
out:
	return error;
fail_xref:
	return error ? error : -EFSCORRUPTED;
}

/* Scrub a directory btree record. */
STATIC int
xfs_scrub_dir_rec(
	struct xfs_scrub_da_btree	*ds,
	int				level,
	void				*rec)
{
	struct xfs_mount		*mp = ds->state->mp;
	struct xfs_dir2_leaf_entry	*ent = rec;
	struct xfs_inode		*dp = ds->dargs.dp;
	struct xfs_dir2_data_entry	*dent;
	struct xfs_buf			*bp;
	xfs_ino_t			ino;
	xfs_dablk_t			rec_bno;
	xfs_dir2_db_t			db;
	xfs_dir2_data_aoff_t		off;
	xfs_dir2_dataptr_t		ptr;
	xfs_dahash_t			calc_hash;
	xfs_dahash_t			hash;
	unsigned int			tag;
	int				error;

	/* Check the hash of the entry. */
	error = xfs_scrub_da_btree_hash(ds, level, &ent->hashval);
	if (error)
		goto out;

	/* Valid hash pointer? */
	ptr = be32_to_cpu(ent->address);
	if (ptr == 0)
		return 0;

	/* Find the directory entry's location. */
	db = xfs_dir2_dataptr_to_db(mp->m_dir_geo, ptr);
	off = xfs_dir2_dataptr_to_off(mp->m_dir_geo, ptr);
	rec_bno = xfs_dir2_db_to_da(mp->m_dir_geo, db);

	if (!xfs_scrub_da_check_ok(ds, level, rec_bno < mp->m_dir_geo->leafblk))
		goto out;
	error = xfs_dir3_data_read(ds->dargs.trans, dp, rec_bno, -2, &bp);
	if (!xfs_scrub_fblock_op_ok(ds->sc, XFS_DATA_FORK, rec_bno, &error) ||
	    !xfs_scrub_fblock_check_ok(ds->sc, XFS_DATA_FORK, rec_bno,
			bp != NULL))
		goto out;

	/* Retrieve the entry and check it. */
	dent = (struct xfs_dir2_data_entry *)(((char *)bp->b_addr) + off);
	ino = be64_to_cpu(dent->inumber);
	hash = be32_to_cpu(ent->hashval);
	tag = be16_to_cpup(dp->d_ops->data_entry_tag_p(dent));
	xfs_scrub_fblock_check_ok(ds->sc, XFS_DATA_FORK, rec_bno,
			xfs_dir_ino_validate(mp, ino) == 0 &&
			!xfs_internal_inum(mp, ino) &&
			tag == off);
	if (!xfs_scrub_fblock_check_ok(ds->sc, XFS_DATA_FORK, rec_bno,
			dent->namelen < MAXNAMELEN))
		goto out_relse;
	calc_hash = xfs_da_hashname(dent->name, dent->namelen);
	xfs_scrub_fblock_check_ok(ds->sc, XFS_DATA_FORK, rec_bno,
			calc_hash == hash);

out_relse:
	xfs_trans_brelse(ds->dargs.trans, bp);
out:
	return error;
}

/* Is this free entry either in the bestfree or smaller than all of them? */
static inline bool
xfs_scrub_directory_check_free_entry(
	struct xfs_dir2_data_free	*bf,
	struct xfs_dir2_data_unused	*dup)
{
	struct xfs_dir2_data_free	*dfp;
	unsigned int			smallest;

	smallest = -1U;
	for (dfp = &bf[0]; dfp < &bf[XFS_DIR2_DATA_FD_COUNT]; dfp++) {
		if (dfp->offset &&
		    be16_to_cpu(dfp->length) == be16_to_cpu(dup->length))
			return true;
		if (smallest < be16_to_cpu(dfp->length))
			smallest = be16_to_cpu(dfp->length);
	}

	return be16_to_cpu(dup->length) <= smallest;
}

/* Check free space info in a directory data block. */
STATIC int
xfs_scrub_directory_data_bestfree(
	struct xfs_scrub_context	*sc,
	xfs_dablk_t			lblk,
	bool				is_block)
{
	struct xfs_dir2_data_unused	*dup;
	struct xfs_dir2_data_free	*dfp;
	struct xfs_buf			*bp;
	struct xfs_dir2_data_free	*bf;
	struct xfs_mount		*mp = sc->mp;
	char				*ptr;
	char				*endptr;
	u16				tag;
	int				newlen;
	int				offset;
	int				error;

	if (is_block) {
		/* dir block format */
		xfs_scrub_fblock_check_ok(sc, XFS_DATA_FORK, lblk, lblk ==
				XFS_B_TO_FSBT(mp, XFS_DIR2_DATA_OFFSET));
		error = xfs_dir3_block_read(sc->tp, sc->ip, &bp);
	} else {
		/* dir data format */
		error = xfs_dir3_data_read(sc->tp, sc->ip, lblk, -1, &bp);
	}
	if (!xfs_scrub_fblock_op_ok(sc, XFS_DATA_FORK, lblk, &error))
		goto out;

	/* Do the bestfrees correspond to actual free space? */
	bf = sc->ip->d_ops->data_bestfree_p(bp->b_addr);
	for (dfp = &bf[0]; dfp < &bf[XFS_DIR2_DATA_FD_COUNT]; dfp++) {
		offset = be16_to_cpu(dfp->offset);
		if (!xfs_scrub_fblock_check_ok(sc, XFS_DATA_FORK, lblk,
				offset < BBTOB(bp->b_length)) || !offset)
			continue;
		dup = (struct xfs_dir2_data_unused *)(bp->b_addr + offset);
		tag = be16_to_cpu(*xfs_dir2_data_unused_tag_p(dup));

		xfs_scrub_fblock_check_ok(sc, XFS_DATA_FORK, lblk,
				dup->freetag ==
					cpu_to_be16(XFS_DIR2_DATA_FREE_TAG) &&
				be16_to_cpu(dup->length) ==
					be16_to_cpu(dfp->length) &&
				tag == ((char *)dup - (char *)bp->b_addr));
	}

	/* Make sure the bestfrees are actually the best free spaces. */
	ptr = (char *)sc->ip->d_ops->data_entry_p(bp->b_addr);
	if (is_block) {
		struct xfs_dir2_block_tail	*btp;

		btp = xfs_dir2_block_tail_p(sc->mp->m_dir_geo, bp->b_addr);
		endptr = (char *)xfs_dir2_block_leaf_p(btp);
	} else
		endptr = (char *)bp->b_addr + BBTOB(bp->b_length);
	while (ptr < endptr) {
		dup = (struct xfs_dir2_data_unused *)ptr;
		/* Skip real entries */
		if (dup->freetag != cpu_to_be16(XFS_DIR2_DATA_FREE_TAG)) {
			struct xfs_dir2_data_entry	*dep;

			dep = (struct xfs_dir2_data_entry *)ptr;
			newlen = sc->ip->d_ops->data_entsize(dep->namelen);
			if (!xfs_scrub_fblock_check_ok(sc, XFS_DATA_FORK, lblk,
					newlen > 0))
				goto out_buf;
			ptr += newlen;
			xfs_scrub_fblock_check_ok(sc, XFS_DATA_FORK, lblk,
					ptr <= endptr);
			continue;
		}

		/* Spot check this free entry */
		tag = be16_to_cpu(*xfs_dir2_data_unused_tag_p(dup));
		xfs_scrub_fblock_check_ok(sc, XFS_DATA_FORK, lblk,
				tag == ((char *)dup - (char *)bp->b_addr));

		/*
		 * Either this entry is a bestfree or it's smaller than
		 * any of the bestfrees.
		 */
		xfs_scrub_fblock_check_ok(sc, XFS_DATA_FORK, lblk,
				xfs_scrub_directory_check_free_entry(bf, dup));

		/* Move on. */
		newlen = be16_to_cpu(dup->length);
		if (!xfs_scrub_fblock_check_ok(sc, XFS_DATA_FORK, lblk,
				newlen > 0))
			goto out_buf;
		ptr += newlen;
		xfs_scrub_fblock_check_ok(sc, XFS_DATA_FORK, lblk,
				ptr <= endptr);
	}
out_buf:
	xfs_trans_brelse(sc->tp, bp);
out:
	return error;
}

/* Is this the longest free entry in the block? */
static inline bool
xfs_scrub_directory_check_freesp(
	struct xfs_inode		*dp,
	struct xfs_buf			*dbp,
	unsigned int			len)
{
	struct xfs_dir2_data_free	*bf;
	struct xfs_dir2_data_free	*dfp;
	unsigned int			longest = 0;
	int				offset;

	bf = dp->d_ops->data_bestfree_p(dbp->b_addr);
	for (dfp = &bf[0]; dfp < &bf[XFS_DIR2_DATA_FD_COUNT]; dfp++) {
		offset = be16_to_cpu(dfp->offset);
		if (!offset)
			continue;
		if (longest < be16_to_cpu(dfp->length))
			longest = be16_to_cpu(dfp->length);
	}

	return longest == len;
}

/* Check free space info in a directory leaf1 block. */
STATIC int
xfs_scrub_directory_leaf1_bestfree(
	struct xfs_scrub_context	*sc,
	struct xfs_da_args		*args,
	xfs_dablk_t			lblk)
{
	struct xfs_dir2_leaf_tail	*ltp;
	struct xfs_buf			*dbp;
	struct xfs_buf			*bp;
	struct xfs_mount		*mp = sc->mp;
	__be16				*bestp;
	__u16				best;
	int				i;
	int				error;

	/* Read the free space block */
	error = xfs_dir3_leaf_read(sc->tp, sc->ip, lblk, -1, &bp);
	if (!xfs_scrub_fblock_op_ok(sc, XFS_DATA_FORK, lblk, &error))
		goto out;

	/* Check all the entries. */
	ltp = xfs_dir2_leaf_tail_p(mp->m_dir_geo, bp->b_addr);
	bestp = xfs_dir2_leaf_bests_p(ltp);
	for (i = 0; i < be32_to_cpu(ltp->bestcount); i++, bestp++) {
		best = be16_to_cpu(*bestp);
		if (best == NULLDATAOFF)
			continue;
		error = xfs_dir3_data_read(sc->tp, sc->ip,
				i * args->geo->fsbcount, -1, &dbp);
		if (!xfs_scrub_fblock_op_ok(sc, XFS_DATA_FORK, lblk, &error))
			continue;
		xfs_scrub_fblock_check_ok(sc, XFS_DATA_FORK, lblk,
				xfs_scrub_directory_check_freesp(sc->ip, dbp,
					best));
		xfs_trans_brelse(sc->tp, dbp);
	}
out:
	return error;
}

/* Check free space info in a directory freespace block. */
STATIC int
xfs_scrub_directory_free_bestfree(
	struct xfs_scrub_context	*sc,
	struct xfs_da_args		*args,
	xfs_dablk_t			lblk)
{
	struct xfs_dir3_icfree_hdr	freehdr;
	struct xfs_buf			*dbp;
	struct xfs_buf			*bp;
	__be16				*bestp;
	__be16				best;
	int				i;
	int				error;

	/* Read the free space block */
	error = xfs_dir2_free_read(sc->tp, sc->ip, lblk, &bp);
	if (!xfs_scrub_fblock_op_ok(sc, XFS_DATA_FORK, lblk, &error))
		goto out;

	/* Check all the entries. */
	sc->ip->d_ops->free_hdr_from_disk(&freehdr, bp->b_addr);
	bestp = sc->ip->d_ops->free_bests_p(bp->b_addr);
	for (i = 0; i < freehdr.nvalid; i++, bestp++) {
		best = be16_to_cpu(*bestp);
		if (best == NULLDATAOFF)
			continue;
		error = xfs_dir3_data_read(sc->tp, sc->ip,
				(freehdr.firstdb + i) * args->geo->fsbcount,
				-1, &dbp);
		if (!xfs_scrub_fblock_op_ok(sc, XFS_DATA_FORK, lblk, &error))
			continue;
		xfs_scrub_fblock_check_ok(sc, XFS_DATA_FORK, lblk,
				xfs_scrub_directory_check_freesp(sc->ip, dbp,
					best));
		xfs_trans_brelse(sc->tp, dbp);
	}
out:
	return error;
}

/* Check free space information in directories. */
STATIC int
xfs_scrub_directory_blocks(
	struct xfs_scrub_context	*sc)
{
	struct xfs_bmbt_irec		got;
	struct xfs_da_args		args;
	struct xfs_ifork		*ifp;
	struct xfs_mount		*mp = sc->mp;
	xfs_fileoff_t			leaf_lblk;
	xfs_fileoff_t			free_lblk;
	xfs_fileoff_t			lblk;
	xfs_extnum_t			idx;
	bool				found;
	int				is_block = 0;
	int				error;

	/* Ignore local format directories. */
	if (sc->ip->i_d.di_format != XFS_DINODE_FMT_EXTENTS &&
	    sc->ip->i_d.di_format != XFS_DINODE_FMT_BTREE)
		return 0;

	ifp = XFS_IFORK_PTR(sc->ip, XFS_DATA_FORK);
	lblk = XFS_B_TO_FSB(mp, XFS_DIR2_DATA_OFFSET);
	leaf_lblk = XFS_B_TO_FSB(mp, XFS_DIR2_LEAF_OFFSET);
	free_lblk = XFS_B_TO_FSB(mp, XFS_DIR2_FREE_OFFSET);

	/* Is this a block dir? */
	args.dp = sc->ip;
	args.geo = mp->m_dir_geo;
	args.trans = sc->tp;
	error = xfs_dir2_isblock(&args, &is_block);
	if (!xfs_scrub_fblock_op_ok(sc, XFS_DATA_FORK, lblk, &error))
		goto out;

	/* Iterate all the data extents in the directory... */
	found = xfs_iext_lookup_extent(sc->ip, ifp, lblk, &idx, &got);
	while (found) {
		/* No more data blocks... */
		if (got.br_startoff >= leaf_lblk)
			break;

		/* Check each data block's bestfree data */
		for (lblk = roundup((xfs_dablk_t)got.br_startoff,
				args.geo->fsbcount);
		     lblk < got.br_startoff + got.br_blockcount;
		     lblk += args.geo->fsbcount) {
			error = xfs_scrub_directory_data_bestfree(sc, lblk,
					is_block);
			if (error)
				goto out;
		}

		found = xfs_iext_get_extent(ifp, ++idx, &got);
	}

	/* Look for a leaf1 block, which has free info. */
	if (xfs_iext_lookup_extent(sc->ip, ifp, leaf_lblk, &idx, &got) &&
	    got.br_startoff == leaf_lblk &&
	    got.br_blockcount == args.geo->fsbcount &&
	    !xfs_iext_get_extent(ifp, ++idx, &got)) {
		if (!xfs_scrub_fblock_check_ok(sc, XFS_DATA_FORK, lblk,
				!is_block))
			goto not_leaf1;
		error = xfs_scrub_directory_leaf1_bestfree(sc, &args,
				leaf_lblk);
		if (error)
			goto out;
	}
not_leaf1:

	/* Scan for free blocks */
	lblk = free_lblk;
	found = xfs_iext_lookup_extent(sc->ip, ifp, lblk, &idx, &got);
	while (found) {
		/*
		 * Dirs can't have blocks mapped above 2^32.
		 * Single-block dirs shouldn't even be here.
		 */
		lblk = got.br_startoff;
		if (!xfs_scrub_fblock_check_ok(sc, XFS_DATA_FORK, lblk,
				!(lblk & ~((1ULL << 32) - 1ULL))))
			goto out;
		if (!xfs_scrub_fblock_check_ok(sc, XFS_DATA_FORK, lblk,
				!is_block))
			goto nextfree;

		/* Check each dir free block's bestfree data */
		for (lblk = roundup((xfs_dablk_t)got.br_startoff,
				args.geo->fsbcount);
		     lblk < got.br_startoff + got.br_blockcount;
		     lblk += args.geo->fsbcount) {
			error = xfs_scrub_directory_free_bestfree(sc, &args,
					lblk);
			if (error)
				goto out;
		}

nextfree:
		found = xfs_iext_get_extent(ifp, ++idx, &got);
	}
out:
	return error;
}

/* Scrub a whole directory. */
int
xfs_scrub_directory(
	struct xfs_scrub_context	*sc)
{
	struct xfs_scrub_dir_ctx	sdc = {
		.dc.actor = xfs_scrub_dir_actor,
		.dc.pos = 0,
	};
	size_t				bufsize;
	loff_t				oldpos;
	int				error;

	if (!S_ISDIR(VFS_I(sc->ip)->i_mode))
		return -ENOENT;

	/* Plausible size? */
	if (!xfs_scrub_ino_check_ok(sc, sc->ip->i_ino, NULL,
			sc->ip->i_d.di_size >= xfs_dir2_sf_hdr_size(0)))
		goto out;

	/* Check directory tree structure */
	error = xfs_scrub_da_btree(sc, XFS_DATA_FORK, xfs_scrub_dir_rec);
	if (error)
		return error;

	/* Check the freespace. */
	error = xfs_scrub_directory_blocks(sc);
	if (error)
		return error;

	/*
	 * Check that every dirent we see can also be looked up by hash.
	 * Userspace usually asks for a 32k buffer, so we will too.
	 */
	bufsize = (size_t)min_t(loff_t, 32768, sc->ip->i_d.di_size);
	sdc.sc = sc;

	/*
	 * Look up every name in this directory by hash.
	 *
	 * The VFS grabs a read or write lock via i_rwsem before it reads
	 * or writes to a directory.  If we've gotten this far we've
	 * already obtained IOLOCK_EXCL, which (since 4.10) is the same as
	 * getting a write lock on i_rwsem.  Therefore, it is safe for us
	 * to drop the ILOCK here in order to reuse the _readdir and
	 * _dir_lookup routines, which do their own ILOCK locking.
	 */
	oldpos = 0;
	sc->ilock_flags &= ~XFS_ILOCK_EXCL;
	xfs_iunlock(sc->ip, XFS_ILOCK_EXCL);
	while (true) {
		error = xfs_readdir(sc->tp, sc->ip, &sdc.dc, bufsize);
		if (!xfs_scrub_fblock_op_ok(sc, XFS_DATA_FORK, 0, &error))
			goto out;
		if (oldpos == sdc.dc.pos)
			break;
		oldpos = sdc.dc.pos;
	}

out:
	return error;
}
