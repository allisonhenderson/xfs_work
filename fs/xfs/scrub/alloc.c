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
#include "scrub/xfs_scrub.h"
#include "scrub/scrub.h"
#include "scrub/common.h"
#include "scrub/btree.h"
#include "scrub/trace.h"

/*
 * Set us up to scrub free space btrees.
 * Push everything out of the log so that the busy extent list is empty.
 */
int
xfs_scrub_setup_ag_allocbt(
	struct xfs_scrub_context	*sc,
	struct xfs_inode		*ip)
{
	return xfs_scrub_setup_ag_btree(sc, ip, sc->try_harder);
}

/* Free space btree scrubber. */

/* Scrub a bnobt/cntbt record. */
STATIC int
xfs_scrub_allocbt_helper(
	struct xfs_scrub_btree		*bs,
	union xfs_btree_rec		*rec)
{
	struct xfs_mount		*mp = bs->cur->bc_mp;
	struct xfs_agf			*agf;
	struct xfs_btree_cur		**xcur;
	struct xfs_scrub_ag		*psa;
	unsigned long long		rec_end;
	xfs_agblock_t			fbno;
	xfs_agblock_t			bno;
	xfs_extlen_t			flen;
	xfs_extlen_t			len;
	int				has_otherrec;
	int				error = 0;

	bno = be32_to_cpu(rec->alloc.ar_startblock);
	len = be32_to_cpu(rec->alloc.ar_blockcount);
	agf = XFS_BUF_TO_AGF(bs->sc->sa.agf_bp);
	rec_end = (unsigned long long)bno + len;

	xfs_scrub_btree_check_ok(bs->sc, bs->cur, 0,
			bno < mp->m_sb.sb_agblocks &&
			bno < be32_to_cpu(agf->agf_length) &&
			rec_end <= mp->m_sb.sb_agblocks &&
			rec_end <= be32_to_cpu(agf->agf_length));

	psa = &bs->sc->sa;
	/*
	 * Ensure there's a corresponding cntbt/bnobt record matching
	 * this bnobt/cntbt record, respectively.
	 */
	xcur = bs->cur == psa->bno_cur ? &psa->cnt_cur : &psa->bno_cur;
	while (*xcur) {
		error = xfs_alloc_lookup_le(*xcur, bno, len, &has_otherrec);
		if (!xfs_scrub_should_xref(bs->sc, &error, xcur) ||
		    !xfs_scrub_btree_xref_check_ok(bs->sc, *xcur, 0,
				has_otherrec))
			break;

		error = xfs_alloc_get_rec(*xcur, &fbno, &flen,
				&has_otherrec);
		if (!xfs_scrub_should_xref(bs->sc, &error, xcur) ||
		    !xfs_scrub_btree_xref_check_ok(bs->sc, *xcur, 0,
				has_otherrec))
			break;

		xfs_scrub_btree_xref_check_ok(bs->sc, *xcur, 0,
				fbno == bno && flen == len);
		break;
	}

	return error;
}

/* Scrub the freespace btrees for some AG. */
STATIC int
xfs_scrub_allocbt(
	struct xfs_scrub_context	*sc,
	xfs_btnum_t			which)
{
	struct xfs_owner_info		oinfo;
	struct xfs_btree_cur		*cur;

	xfs_rmap_ag_owner(&oinfo, XFS_RMAP_OWN_AG);
	cur = which == XFS_BTNUM_BNO ? sc->sa.bno_cur : sc->sa.cnt_cur;
	return xfs_scrub_btree(sc, cur, xfs_scrub_allocbt_helper,
			&oinfo, NULL);
}

int
xfs_scrub_bnobt(
	struct xfs_scrub_context	*sc)
{
	return xfs_scrub_allocbt(sc, XFS_BTNUM_BNO);
}

int
xfs_scrub_cntbt(
	struct xfs_scrub_context	*sc)
{
	return xfs_scrub_allocbt(sc, XFS_BTNUM_CNT);
}
