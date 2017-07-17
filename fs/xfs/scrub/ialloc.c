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
#include "xfs_ialloc.h"
#include "xfs_ialloc_btree.h"
#include "xfs_icache.h"
#include "xfs_rmap.h"
#include "xfs_log.h"
#include "xfs_trans_priv.h"
#include "xfs_alloc.h"
#include "scrub/xfs_scrub.h"
#include "scrub/scrub.h"
#include "scrub/common.h"
#include "scrub/btree.h"
#include "scrub/trace.h"

/*
 * Set us up to scrub inode btrees.
 * If we detect a discrepancy between the inobt and the inode,
 * try again after forcing logged inode cores out to disk.
 */
int
xfs_scrub_setup_ag_iallocbt(
	struct xfs_scrub_context	*sc,
	struct xfs_inode		*ip)
{
	return xfs_scrub_setup_ag_btree(sc, ip, sc->try_harder);
}

/* Inode btree scrubber. */

/* Scrub a chunk of an inobt record. */
STATIC bool
xfs_scrub_iallocbt_chunk(
	struct xfs_scrub_btree		*bs,
	struct xfs_inobt_rec_incore	*irec,
	xfs_agino_t			agino,
	xfs_extlen_t			len)
{
	struct xfs_mount		*mp = bs->cur->bc_mp;
	struct xfs_agf			*agf;
	struct xfs_scrub_ag		*psa;
	unsigned long long		rec_end;
	xfs_agblock_t			eoag;
	xfs_agblock_t			bno;
	bool				is_freesp;
	int				error = 0;

	agf = XFS_BUF_TO_AGF(bs->sc->sa.agf_bp);
	eoag = be32_to_cpu(agf->agf_length);
	bno = XFS_AGINO_TO_AGBNO(mp, agino);
	rec_end = (unsigned long long)bno + len;

	if (!xfs_scrub_btree_check_ok(bs->sc, bs->cur, 0,
			bno < mp->m_sb.sb_agblocks && bno < eoag &&
			rec_end <= mp->m_sb.sb_agblocks && rec_end <= eoag))
		return false;

	/* Cross-reference with the AG headers. */
	xfs_scrub_btree_xref_check_ok(bs->sc, bs->cur, 0,
			!xfs_scrub_extent_covers_ag_head(mp, bno, len));

	psa = &bs->sc->sa;
	/* Cross-reference with the bnobt. */
	if (psa->bno_cur) {
		error = xfs_alloc_has_record(psa->bno_cur, bno, len,
				&is_freesp);
		if (xfs_scrub_should_xref(bs->sc, &error, &psa->bno_cur))
			xfs_scrub_btree_xref_check_ok(bs->sc, psa->bno_cur, 0,
					!is_freesp);
	}

	return true;
}

/* Count the number of free inodes. */
static unsigned int
xfs_scrub_iallocbt_freecount(
	xfs_inofree_t			freemask)
{
	int				bits = XFS_INODES_PER_CHUNK;
	unsigned int			ret = 0;

	while (bits--) {
		if (freemask & 1)
			ret++;
		freemask >>= 1;
	}

	return ret;
}

/* Check a particular inode with ir_free. */
STATIC int
xfs_scrub_iallocbt_check_cluster_freemask(
	struct xfs_scrub_btree		*bs,
	xfs_ino_t			fsino,
	xfs_agino_t			chunkino,
	xfs_agino_t			clusterino,
	struct xfs_inobt_rec_incore	*irec,
	struct xfs_buf			*bp)
{
	struct xfs_dinode		*dip;
	struct xfs_mount		*mp = bs->cur->bc_mp;
	bool				freemask_ok;
	bool				inuse;
	int				error;

	dip = xfs_buf_offset(bp, clusterino * mp->m_sb.sb_inodesize);
	if (!xfs_scrub_btree_check_ok(bs->sc, bs->cur, 0,
			be16_to_cpu(dip->di_magic) == XFS_DINODE_MAGIC &&
			(dip->di_version < 3 || be64_to_cpu(dip->di_ino) ==
				fsino + clusterino)))
		goto out;

	freemask_ok = !!(irec->ir_free & XFS_INOBT_MASK(chunkino + clusterino));
	error = xfs_icache_inode_is_allocated(mp, bs->cur->bc_tp,
			fsino + clusterino, &inuse);
	if (error == -ENOENT) {
		/* Not cached, just read the disk buffer */
		freemask_ok ^= !!(dip->di_mode);
		if (!bs->sc->try_harder && !freemask_ok)
			return -EDEADLOCK;
	} else if (error < 0) {
		/* Inode is only half assembled, don't bother. */
		freemask_ok = true;
	} else {
		/* Inode is all there. */
		freemask_ok ^= inuse;
	}
	xfs_scrub_btree_check_ok(bs->sc, bs->cur, 0, freemask_ok);
out:
	return 0;
}

/* Make sure the free mask is consistent with what the inodes think. */
STATIC int
xfs_scrub_iallocbt_check_freemask(
	struct xfs_scrub_btree		*bs,
	struct xfs_inobt_rec_incore	*irec)
{
	struct xfs_owner_info		oinfo;
	struct xfs_imap			imap;
	struct xfs_mount		*mp = bs->cur->bc_mp;
	struct xfs_dinode		*dip;
	struct xfs_buf			*bp;
	xfs_ino_t			fsino;
	xfs_agino_t			nr_inodes;
	xfs_agino_t			agino;
	xfs_agino_t			chunkino;
	xfs_agino_t			clusterino;
	xfs_agblock_t			agbno;
	int				blks_per_cluster;
	uint16_t			holemask;
	uint16_t			ir_holemask;
	int				error = 0;

	/* Make sure the freemask matches the inode records. */
	blks_per_cluster = xfs_icluster_size_fsb(mp);
	nr_inodes = XFS_OFFBNO_TO_AGINO(mp, blks_per_cluster, 0);
	xfs_rmap_ag_owner(&oinfo, XFS_RMAP_OWN_INODES);

	for (agino = irec->ir_startino;
	     agino < irec->ir_startino + XFS_INODES_PER_CHUNK;
	     agino += blks_per_cluster * mp->m_sb.sb_inopblock) {
		fsino = XFS_AGINO_TO_INO(mp, bs->cur->bc_private.a.agno, agino);
		chunkino = agino - irec->ir_startino;
		agbno = XFS_AGINO_TO_AGBNO(mp, agino);

		/* Compute the holemask mask for this cluster. */
		for (clusterino = 0, holemask = 0; clusterino < nr_inodes;
		     clusterino += XFS_INODES_PER_HOLEMASK_BIT)
			holemask |= XFS_INOBT_MASK((chunkino + clusterino) /
					XFS_INODES_PER_HOLEMASK_BIT);

		/* The whole cluster must be a hole or not a hole. */
		ir_holemask = (irec->ir_holemask & holemask);
		xfs_scrub_btree_check_ok(bs->sc, bs->cur, 0,
				ir_holemask == holemask || ir_holemask == 0);

		/* If any part of this is a hole, skip it. */
		if (ir_holemask)
			continue;

		/* Grab the inode cluster buffer. */
		imap.im_blkno = XFS_AGB_TO_DADDR(mp, bs->cur->bc_private.a.agno,
				agbno);
		imap.im_len = XFS_FSB_TO_BB(mp, blks_per_cluster);
		imap.im_boffset = 0;

		error = xfs_imap_to_bp(mp, bs->cur->bc_tp, &imap,
				&dip, &bp, 0, 0);
		if (!xfs_scrub_btree_op_ok(bs->sc, bs->cur, 0, &error))
			continue;

		/* Which inodes are free? */
		for (clusterino = 0; clusterino < nr_inodes; clusterino++) {
			error = xfs_scrub_iallocbt_check_cluster_freemask(bs,
					fsino, chunkino, clusterino, irec, bp);
			if (error) {
				xfs_trans_brelse(bs->cur->bc_tp, bp);
				return error;
			}
		}

		xfs_trans_brelse(bs->cur->bc_tp, bp);
	}

	return error;
}

/* Scrub an inobt/finobt record. */
STATIC int
xfs_scrub_iallocbt_helper(
	struct xfs_scrub_btree		*bs,
	union xfs_btree_rec		*rec)
{
	struct xfs_mount		*mp = bs->cur->bc_mp;
	struct xfs_agi			*agi;
	struct xfs_inobt_rec_incore	irec;
	uint64_t			holes;
	xfs_agino_t			agino;
	xfs_agblock_t			agbno;
	xfs_extlen_t			len;
	int				holecount;
	int				i;
	int				error = 0;
	unsigned int			real_freecount;
	uint16_t			holemask;

	xfs_inobt_btrec_to_irec(mp, rec, &irec);

	xfs_scrub_btree_check_ok(bs->sc, bs->cur, 0,
			irec.ir_count <= XFS_INODES_PER_CHUNK &&
			irec.ir_freecount <= XFS_INODES_PER_CHUNK);
	real_freecount = irec.ir_freecount +
			(XFS_INODES_PER_CHUNK - irec.ir_count);
	xfs_scrub_btree_check_ok(bs->sc, bs->cur, 0,
		real_freecount == xfs_scrub_iallocbt_freecount(irec.ir_free));

	agi = XFS_BUF_TO_AGI(bs->sc->sa.agi_bp);
	agino = irec.ir_startino;
	agbno = XFS_AGINO_TO_AGBNO(mp, irec.ir_startino);
	if (!xfs_scrub_btree_check_ok(bs->sc, bs->cur, 0,
			agbno < be32_to_cpu(agi->agi_length)))
		goto out;
	xfs_scrub_btree_check_ok(bs->sc, bs->cur, 0,
			!(agbno & (xfs_ialloc_cluster_alignment(mp) - 1)) &&
			!(agbno & (xfs_icluster_size_fsb(mp) - 1)));

	/* Handle non-sparse inodes */
	if (!xfs_inobt_issparse(irec.ir_holemask)) {
		len = XFS_B_TO_FSB(mp,
				XFS_INODES_PER_CHUNK * mp->m_sb.sb_inodesize);
		xfs_scrub_btree_check_ok(bs->sc, bs->cur, 0,
				irec.ir_count == XFS_INODES_PER_CHUNK);

		if (!xfs_scrub_iallocbt_chunk(bs, &irec, agino, len))
			goto out;
		goto check_freemask;
	}

	/* Check each chunk of a sparse inode cluster. */
	holemask = irec.ir_holemask;
	holecount = 0;
	len = XFS_B_TO_FSB(mp,
			XFS_INODES_PER_HOLEMASK_BIT * mp->m_sb.sb_inodesize);
	holes = ~xfs_inobt_irec_to_allocmask(&irec);
	xfs_scrub_btree_check_ok(bs->sc, bs->cur, 0,
			(holes & irec.ir_free) == holes &&
			irec.ir_freecount <= irec.ir_count);

	for (i = 0; i < XFS_INOBT_HOLEMASK_BITS; holemask >>= 1,
			i++, agino += XFS_INODES_PER_HOLEMASK_BIT) {
		if (holemask & 1) {
			holecount += XFS_INODES_PER_HOLEMASK_BIT;
			continue;
		}

		if (!xfs_scrub_iallocbt_chunk(bs, &irec, agino, len))
			break;
	}

	xfs_scrub_btree_check_ok(bs->sc, bs->cur, 0,
			holecount <= XFS_INODES_PER_CHUNK &&
			holecount + irec.ir_count == XFS_INODES_PER_CHUNK);

check_freemask:
	error = xfs_scrub_iallocbt_check_freemask(bs, &irec);
	if (error)
		goto out;

out:
	return error;
}

/* Scrub the inode btrees for some AG. */
STATIC int
xfs_scrub_iallocbt(
	struct xfs_scrub_context	*sc,
	xfs_btnum_t			which)
{
	struct xfs_btree_cur		*cur;
	struct xfs_owner_info		oinfo;

	xfs_rmap_ag_owner(&oinfo, XFS_RMAP_OWN_INOBT);
	cur = which == XFS_BTNUM_INO ? sc->sa.ino_cur : sc->sa.fino_cur;
	return xfs_scrub_btree(sc, cur, xfs_scrub_iallocbt_helper,
			&oinfo, NULL);
}

int
xfs_scrub_inobt(
	struct xfs_scrub_context	*sc)
{
	return xfs_scrub_iallocbt(sc, XFS_BTNUM_INO);
}

int
xfs_scrub_finobt(
	struct xfs_scrub_context	*sc)
{
	return xfs_scrub_iallocbt(sc, XFS_BTNUM_FINO);
}
