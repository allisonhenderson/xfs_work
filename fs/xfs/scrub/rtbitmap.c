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
#include "xfs_alloc.h"
#include "xfs_rtalloc.h"
#include "xfs_inode.h"
#include "scrub/xfs_scrub.h"
#include "scrub/scrub.h"
#include "scrub/common.h"
#include "scrub/trace.h"

/* Set us up with the realtime metadata locked. */
int
xfs_scrub_setup_rt(
	struct xfs_scrub_context	*sc,
	struct xfs_inode		*ip)
{
	struct xfs_mount		*mp = sc->mp;
	int				lockmode;
	int				error = 0;

	if (sc->sm->sm_agno || sc->sm->sm_ino || sc->sm->sm_gen)
		return -EINVAL;

	error = xfs_scrub_setup_fs(sc, ip);
	if (error)
		return error;

	lockmode = XFS_ILOCK_EXCL | XFS_ILOCK_RTBITMAP;
	xfs_ilock(mp->m_rbmip, lockmode);
	xfs_trans_ijoin(sc->tp, mp->m_rbmip, lockmode);

	return 0;
}

/* Realtime bitmap. */

/* Scrub a free extent record from the realtime bitmap. */
STATIC int
xfs_scrub_rtbitmap_helper(
	struct xfs_trans		*tp,
	struct xfs_rtalloc_rec		*rec,
	void				*priv)
{
	return 0;
}

/* Scrub the realtime bitmap. */
int
xfs_scrub_rtbitmap(
	struct xfs_scrub_context	*sc)
{
	int				error;

	error = xfs_rtalloc_query_all(sc->tp, xfs_scrub_rtbitmap_helper, NULL);
	if (!xfs_scrub_fblock_op_ok(sc, XFS_DATA_FORK, 0, &error))
		goto out;

out:
	return error;
}

/* Scrub the realtime summary. */
int
xfs_scrub_rtsummary(
	struct xfs_scrub_context	*sc)
{
	/* XXX: implement this some day */
	return -ENOENT;
}
