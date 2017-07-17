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
#include "xfs_rmap.h"
#include "xfs_alloc.h"
#include "xfs_ialloc.h"
#include "scrub/xfs_scrub.h"
#include "scrub/scrub.h"
#include "scrub/common.h"
#include "scrub/btree.h"
#include "scrub/trace.h"

/*
 * Set us up to scrub reference count btrees.
 */
int
xfs_scrub_setup_ag_refcountbt(
	struct xfs_scrub_context	*sc,
	struct xfs_inode		*ip)
{
	return xfs_scrub_setup_ag_btree(sc, ip, false);
}

/* Reference count btree scrubber. */

/* Scrub a refcountbt record. */
STATIC int
xfs_scrub_refcountbt_helper(
	struct xfs_scrub_btree		*bs,
	union xfs_btree_rec		*rec)
{
	struct xfs_mount		*mp = bs->cur->bc_mp;
	struct xfs_agf			*agf;
	struct xfs_scrub_ag		*psa;
	struct xfs_refcount_irec	irec;
	unsigned long long		rec_end;
	xfs_agblock_t			eoag;
	bool				has_cowflag;
	bool				is_freesp;
	bool				has_inodes;
	int				error = 0;

	irec.rc_startblock = be32_to_cpu(rec->refc.rc_startblock);
	irec.rc_blockcount = be32_to_cpu(rec->refc.rc_blockcount);
	irec.rc_refcount = be32_to_cpu(rec->refc.rc_refcount);
	agf = XFS_BUF_TO_AGF(bs->sc->sa.agf_bp);
	eoag = be32_to_cpu(agf->agf_length);

	has_cowflag = !!(irec.rc_startblock & XFS_REFC_COW_START);
	xfs_scrub_btree_check_ok(bs->sc, bs->cur, 0,
			(irec.rc_refcount == 1 && has_cowflag) ||
			(irec.rc_refcount != 1 && !has_cowflag));
	irec.rc_startblock &= ~XFS_REFC_COW_START;
	rec_end = (unsigned long long)irec.rc_startblock + irec.rc_blockcount;
	xfs_scrub_btree_check_ok(bs->sc, bs->cur, 0,
			irec.rc_startblock < mp->m_sb.sb_agblocks &&
			irec.rc_startblock < eoag &&
			rec_end <= mp->m_sb.sb_agblocks &&
			rec_end <= eoag &&
			irec.rc_refcount >= 1);

	/* Cross-reference with the AG headers. */
	xfs_scrub_btree_xref_check_ok(bs->sc, bs->cur, 0,
			!xfs_scrub_extent_covers_ag_head(mp,
				irec.rc_startblock, irec.rc_blockcount));

	psa = &bs->sc->sa;
	/* Cross-reference with the bnobt. */
	if (psa->bno_cur) {
		error = xfs_alloc_has_record(psa->bno_cur, irec.rc_startblock,
				irec.rc_blockcount, &is_freesp);
		if (xfs_scrub_should_xref(bs->sc, &error, &psa->bno_cur))
			xfs_scrub_btree_xref_check_ok(bs->sc, psa->bno_cur, 0,
					!is_freesp);
	}

	/* Cross-reference with inobt. */
	if (psa->ino_cur) {
		error = xfs_ialloc_has_inodes_at_extent(psa->ino_cur,
				irec.rc_startblock, irec.rc_blockcount,
				&has_inodes);
		if (xfs_scrub_should_xref(bs->sc, &error, &psa->ino_cur))
			xfs_scrub_btree_xref_check_ok(bs->sc, psa->ino_cur, 0,
					!has_inodes);
	}

	/* Cross-reference with finobt. */
	if (psa->fino_cur) {
		error = xfs_ialloc_has_inodes_at_extent(psa->fino_cur,
				irec.rc_startblock, irec.rc_blockcount,
				&has_inodes);
		if (xfs_scrub_should_xref(bs->sc, &error, &psa->fino_cur))
			xfs_scrub_btree_xref_check_ok(bs->sc, psa->fino_cur, 0,
					!has_inodes);
	}

	return error;
}

/* Scrub the refcount btree for some AG. */
int
xfs_scrub_refcountbt(
	struct xfs_scrub_context	*sc)
{
	struct xfs_owner_info		oinfo;

	xfs_rmap_ag_owner(&oinfo, XFS_RMAP_OWN_REFC);
	return xfs_scrub_btree(sc, sc->sa.refc_cur, xfs_scrub_refcountbt_helper,
			&oinfo, NULL);
}
