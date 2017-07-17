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
	struct xfs_refcount_irec	irec;
	unsigned long long		rec_end;
	xfs_agblock_t			eoag;
	bool				has_cowflag;
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
