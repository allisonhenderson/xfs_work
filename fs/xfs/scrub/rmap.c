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
#include "xfs_rmap.h"
#include "xfs_alloc.h"
#include "xfs_ialloc.h"
#include "xfs_bmap_btree.h"
#include "xfs_refcount.h"
#include "scrub/xfs_scrub.h"
#include "scrub/scrub.h"
#include "scrub/common.h"
#include "scrub/btree.h"
#include "scrub/trace.h"

/*
 * Set us up to scrub reverse mapping btrees.
 */
int
xfs_scrub_setup_ag_rmapbt(
	struct xfs_scrub_context	*sc,
	struct xfs_inode		*ip)
{
	return xfs_scrub_setup_ag_btree(sc, ip, false);
}

/* Reverse-mapping scrubber. */

/* Cross-reference a rmap against the refcount btree. */
STATIC void
xfs_scrub_rmapbt_xref_refc(
	struct xfs_scrub_btree		*bs,
	struct xfs_rmap_irec		*irec,
	bool				non_inode,
	bool				is_attr,
	bool				is_bmbt,
	bool				is_unwritten)
{
	struct xfs_scrub_ag		*psa = &bs->sc->sa;
	struct xfs_refcount_irec	crec;
	unsigned long long		rec_end;
	xfs_agblock_t			fbno;
	xfs_extlen_t			flen;
	bool				has_cowflag;
	int				has_refcount;
	int				error;

	if (irec->rm_owner != XFS_RMAP_OWN_COW) {
		/* If this is shared, must be a data fork extent. */
		error = xfs_refcount_find_shared(psa->refc_cur,
				irec->rm_startblock, irec->rm_blockcount,
				&fbno, &flen, false);
		if (xfs_scrub_should_xref(bs->sc, &error, &psa->refc_cur))
			xfs_scrub_btree_xref_check_ok(bs->sc, psa->refc_cur, 0,
					flen == 0 ||
					(!non_inode && !is_attr &&
					 !is_bmbt && !is_unwritten));
		return;
	}

	/* Check this CoW staging extent. */
	error = xfs_refcount_lookup_le(psa->refc_cur,
			irec->rm_startblock + XFS_REFC_COW_START,
			&has_refcount);
	if (!xfs_scrub_should_xref(bs->sc, &error, &psa->refc_cur) ||
	    !xfs_scrub_btree_xref_check_ok(bs->sc, psa->refc_cur, 0,
			has_refcount))
		return;

	error = xfs_refcount_get_rec(psa->refc_cur, &crec, &has_refcount);
	if (!xfs_scrub_should_xref(bs->sc, &error, &psa->refc_cur) ||
	    !xfs_scrub_btree_xref_check_ok(bs->sc, psa->refc_cur, 0,
			has_refcount))
		return;

	has_cowflag = !!(crec.rc_startblock & XFS_REFC_COW_START);
	xfs_scrub_btree_xref_check_ok(bs->sc, psa->refc_cur, 0,
			(crec.rc_refcount == 1 && has_cowflag) ||
			(crec.rc_refcount != 1 && !has_cowflag));

	crec.rc_startblock &= ~XFS_REFC_COW_START;
	rec_end = (unsigned long long)crec.rc_startblock + crec.rc_blockcount;
	xfs_scrub_btree_xref_check_ok(bs->sc, psa->refc_cur, 0,
			crec.rc_startblock <= irec->rm_startblock &&
			rec_end >= irec->rm_startblock + irec->rm_blockcount &&
			crec.rc_refcount == 1);
}

struct xfs_scrub_rmapbt_xref_bmbt {
	xfs_fsblock_t		fsb;
	xfs_extlen_t		len;
};

/* Is this the bmbt block we're looking for? */
STATIC int
xfs_scrub_rmapbt_xref_bmap_find_bmbt_block(
	struct xfs_btree_cur	*cur,
	int			level,
	void			*data)
{
	struct xfs_buf		*bp;
	struct xfs_scrub_rmapbt_xref_bmbt	*x = data;
	xfs_fsblock_t		fsb;

	xfs_btree_get_block(cur, level, &bp);
	if (!bp)
		return 0;

	fsb = XFS_DADDR_TO_FSB(cur->bc_mp, bp->b_bn);
	if (fsb >= x->fsb && fsb < x->fsb + x->len)
		return XFS_BTREE_QUERY_RANGE_ABORT;
	return 0;
}

/* Try to find a matching bmap extent for this inode data/attr fork rmap. */
STATIC void
xfs_scrub_rmapbt_xref_bmap(
	struct xfs_scrub_btree	*bs,
	struct xfs_rmap_irec	*irec,
	bool			is_attr,
	bool			is_bmbt,
	bool			is_unwritten)
{
	struct xfs_scrub_rmapbt_xref_bmbt	x;
	struct xfs_bmbt_irec	got;
	struct xfs_inode	*ip;
	struct xfs_ifork	*ifp;
	struct xfs_btree_cur	*cur;
	xfs_fileoff_t		off;
	xfs_fileoff_t		endoff;
	xfs_fsblock_t		fsb;
	xfs_extnum_t		idx;
	xfs_agnumber_t		agno;
	uint			lockflags;
	bool			found;
	int			whichfork;
	int			error;
	uint8_t			fmt;

	fsb = XFS_AGB_TO_FSB(bs->sc->mp, bs->sc->sa.agno, irec->rm_startblock);

	/*
	 * We can't access the AGI of a lower AG due to locking rules,
	 * so skip this check if inodes aren't aligned and the inode is
	 * in a lower AG.
	 */
	agno = XFS_INO_TO_AGNO(bs->sc->mp, irec->rm_owner);
	if (!xfs_scrub_check_thoroughness(bs->sc,
			bs->sc->mp->m_inoalign_mask != 0 ||
			agno >= bs->sc->sa.agno))
		return;

	/* Grab the inode. */
	error = xfs_iget(bs->sc->mp, bs->sc->tp, irec->rm_owner, 0, 0, &ip);
	if (!xfs_scrub_should_xref(bs->sc, &error, NULL))
		return;

	whichfork = is_attr ? XFS_ATTR_FORK : XFS_DATA_FORK;
	ifp = XFS_IFORK_PTR(ip, whichfork);
	lockflags = XFS_IOLOCK_SHARED | XFS_MMAPLOCK_SHARED | XFS_ILOCK_SHARED;

lock_again:
	/*
	 * Try to grab the inode lock.  We cannot block here because the
	 * usual XFS locking order is inode -> AGF, whereas here we have
	 * the AGF but want an inode.  Blocking here could result in
	 * deadlock, so we'll take an incomplete check over that.
	 */
	if (!xfs_ilock_nowait(ip, lockflags))
		goto out_rele;

	/* Inode had better have extent maps. */
	fmt = XFS_IFORK_FORMAT(ip, whichfork);
	if (!xfs_scrub_btree_xref_check_ok(bs->sc, bs->cur, 0,
			ifp != NULL &&
			(fmt == XFS_DINODE_FMT_BTREE ||
			 fmt == XFS_DINODE_FMT_EXTENTS)))
		goto out_unlock;

	/* If we haven't loaded the extent list, try to relock with excl. */
	if (!(ifp->if_flags & XFS_IFEXTENTS)) {
		if (!(lockflags & XFS_ILOCK_EXCL)) {
			xfs_iunlock(ip, lockflags);
			lockflags |= XFS_ILOCK_EXCL;
			lockflags &= ~XFS_ILOCK_SHARED;
			goto lock_again;
		}
		error = xfs_iread_extents(bs->sc->tp, ip, whichfork);
		if (error)
			goto out_unlock;
	}

	/* If this is a bmbt record, see if we can find it. */
	if (is_bmbt) {
		x.fsb = fsb;
		x.len = irec->rm_blockcount;
		cur = xfs_bmbt_init_cursor(bs->sc->mp, bs->sc->tp, ip,
				whichfork);
		error = xfs_btree_visit_blocks(cur,
				xfs_scrub_rmapbt_xref_bmap_find_bmbt_block,
				&x);
		xfs_scrub_btree_xref_check_ok(bs->sc, cur, 0,
				error == XFS_BTREE_QUERY_RANGE_ABORT);
		xfs_btree_del_cursor(cur, error ? XFS_BTREE_ERROR :
				XFS_BTREE_NOERROR);
		goto out_unlock;
	}

	/* Now go make sure we find a bmap extent to cover this rmap. */
	off = irec->rm_offset;
	endoff = irec->rm_offset + irec->rm_blockcount - 1;
	found = xfs_iext_lookup_extent(ip, ifp, off, &idx, &got);
	xfs_scrub_btree_xref_check_ok(bs->sc, bs->cur, 0, found);
	while (found) {
		if (!xfs_scrub_btree_xref_check_ok(bs->sc, bs->cur, 0,
				got.br_startoff <= off &&
				got.br_startoff <= endoff))
			goto out_unlock;
		xfs_scrub_btree_xref_check_ok(bs->sc, bs->cur, 0,
				(got.br_state == XFS_EXT_NORM ||
				 is_unwritten) &&
				(got.br_state == XFS_EXT_UNWRITTEN ||
				 !is_unwritten) &&
				got.br_startblock + (off - got.br_startoff) ==
				fsb);

		off = got.br_startoff + got.br_blockcount;
		fsb = got.br_startblock + got.br_blockcount;
		if (off >= endoff)
			break;
		found = xfs_iext_get_extent(ifp, ++idx, &got);
		xfs_scrub_btree_xref_check_ok(bs->sc, bs->cur, 0,
				found &&
				got.br_startoff == off &&
				got.br_startblock == fsb);
	}

out_unlock:
	xfs_iunlock(ip, lockflags);
out_rele:
	iput(VFS_I(ip));
}

/* Scrub an rmapbt record. */
STATIC int
xfs_scrub_rmapbt_helper(
	struct xfs_scrub_btree		*bs,
	union xfs_btree_rec		*rec)
{
	struct xfs_mount		*mp = bs->cur->bc_mp;
	struct xfs_agf			*agf;
	struct xfs_scrub_ag		*psa;
	struct xfs_rmap_irec		irec;
	unsigned long long		rec_end;
	xfs_agblock_t			eoag;
	bool				is_freesp;
	bool				non_inode;
	bool				is_unwritten;
	bool				is_bmbt;
	bool				is_attr;
	bool				has_inodes;
	int				error;

	error = xfs_rmap_btrec_to_irec(rec, &irec);
	if (!xfs_scrub_btree_op_ok(bs->sc, bs->cur, 0, &error))
		goto out;

	/* Check extent. */
	agf = XFS_BUF_TO_AGF(bs->sc->sa.agf_bp);
	eoag = be32_to_cpu(agf->agf_length);
	rec_end = (unsigned long long)irec.rm_startblock + irec.rm_blockcount;

	xfs_scrub_btree_check_ok(bs->sc, bs->cur, 0,
			irec.rm_startblock < mp->m_sb.sb_agblocks &&
			irec.rm_startblock < eoag &&
			rec_end <= mp->m_sb.sb_agblocks &&
			rec_end <= eoag);

	/* Check flags. */
	non_inode = XFS_RMAP_NON_INODE_OWNER(irec.rm_owner);
	is_bmbt = irec.rm_flags & XFS_RMAP_BMBT_BLOCK;
	is_attr = irec.rm_flags & XFS_RMAP_ATTR_FORK;
	is_unwritten = irec.rm_flags & XFS_RMAP_UNWRITTEN;

	xfs_scrub_btree_check_ok(bs->sc, bs->cur, 0,
			(!is_bmbt || irec.rm_offset == 0) &&
			(!non_inode || irec.rm_offset == 0) &&
			(!is_unwritten || !(is_bmbt || non_inode || is_attr)) &&
			(!non_inode || !(is_bmbt || is_unwritten || is_attr)));

	/* Owner inode within an AG? */
	xfs_scrub_btree_check_ok(bs->sc, bs->cur, 0, non_inode ||
			(XFS_INO_TO_AGNO(mp, irec.rm_owner) <
							mp->m_sb.sb_agcount &&
			 XFS_AGINO_TO_AGBNO(mp,
				XFS_INO_TO_AGINO(mp, irec.rm_owner)) <
							mp->m_sb.sb_agblocks));
	/* Owner inode within the FS? */
	xfs_scrub_btree_check_ok(bs->sc, bs->cur, 0, non_inode ||
			XFS_AGB_TO_DADDR(mp,
				XFS_INO_TO_AGNO(mp, irec.rm_owner),
				XFS_AGINO_TO_AGBNO(mp,
					XFS_INO_TO_AGINO(mp, irec.rm_owner))) <
			XFS_FSB_TO_BB(mp, mp->m_sb.sb_dblocks));

	/* Non-inode owner within the magic values? */
	xfs_scrub_btree_check_ok(bs->sc, bs->cur, 0, !non_inode ||
			(irec.rm_owner > XFS_RMAP_OWN_MIN &&
			 irec.rm_owner <= XFS_RMAP_OWN_FS));

	/* Cross-reference with the AG headers. */
	xfs_scrub_btree_xref_check_ok(bs->sc, bs->cur, 0,
			irec.rm_owner == XFS_RMAP_OWN_FS ||
			!xfs_scrub_extent_covers_ag_head(mp, irec.rm_startblock,
				irec.rm_blockcount));

	psa = &bs->sc->sa;
	/* Cross-reference with the bnobt. */
	if (psa->bno_cur) {
		error = xfs_alloc_has_record(psa->bno_cur, irec.rm_startblock,
				irec.rm_blockcount, &is_freesp);
		if (xfs_scrub_should_xref(bs->sc, &error, &psa->bno_cur))
			xfs_scrub_btree_xref_check_ok(bs->sc, psa->bno_cur, 0,
					!is_freesp);
	}

	/* Cross-reference with inobt. */
	if (psa->ino_cur) {
		error = xfs_ialloc_has_inodes_at_extent(psa->ino_cur,
				irec.rm_startblock, irec.rm_blockcount,
				&has_inodes);
		if (xfs_scrub_should_xref(bs->sc, &error, &psa->ino_cur))
			xfs_scrub_btree_xref_check_ok(bs->sc, psa->ino_cur, 0,
					irec.rm_owner == XFS_RMAP_OWN_INODES ||
					!has_inodes);
	}

	/* Cross-reference with finobt. */
	if (psa->fino_cur) {
		error = xfs_ialloc_has_inodes_at_extent(psa->fino_cur,
				irec.rm_startblock, irec.rm_blockcount,
				&has_inodes);
		if (xfs_scrub_should_xref(bs->sc, &error, &psa->fino_cur))
			xfs_scrub_btree_xref_check_ok(bs->sc, psa->fino_cur, 0,
					irec.rm_owner == XFS_RMAP_OWN_INODES ||
					!has_inodes);
	}

	/* Cross-reference with the refcount btree. */
	if (psa->refc_cur)
		xfs_scrub_rmapbt_xref_refc(bs, &irec, non_inode, is_attr,
				is_bmbt, is_unwritten);

	/* Cross-reference with an inode's bmbt if possible. */
	if (!non_inode)
		xfs_scrub_rmapbt_xref_bmap(bs, &irec, is_attr, is_bmbt,
				is_unwritten);
out:
	return error;
}

/* Scrub the rmap btree for some AG. */
int
xfs_scrub_rmapbt(
	struct xfs_scrub_context	*sc)
{
	struct xfs_owner_info		oinfo;

	xfs_rmap_ag_owner(&oinfo, XFS_RMAP_OWN_AG);
	return xfs_scrub_btree(sc, sc->sa.rmap_cur, xfs_scrub_rmapbt_helper,
			&oinfo, NULL);
}
