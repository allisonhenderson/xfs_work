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
#include "xfs_bmap.h"
#include "xfs_quota.h"
#include "xfs_qm.h"
#include "xfs_dquot.h"
#include "xfs_dquot_item.h"
#include "scrub/xfs_scrub.h"
#include "scrub/scrub.h"
#include "scrub/common.h"
#include "scrub/trace.h"

/* Convert a scrub type code to a DQ flag, or return 0 if error. */
static inline uint
xfs_scrub_quota_to_dqtype(
	struct xfs_scrub_context	*sc)
{
	switch (sc->sm->sm_type) {
	case XFS_SCRUB_TYPE_UQUOTA:
		return XFS_DQ_USER;
	case XFS_SCRUB_TYPE_GQUOTA:
		return XFS_DQ_GROUP;
	case XFS_SCRUB_TYPE_PQUOTA:
		return XFS_DQ_PROJ;
	default:
		return 0;
	}
}

/* Set us up to scrub a quota. */
int
xfs_scrub_setup_quota(
	struct xfs_scrub_context	*sc,
	struct xfs_inode		*ip)
{
	uint				dqtype;

	if (sc->sm->sm_agno || sc->sm->sm_ino || sc->sm->sm_gen)
		return -EINVAL;

	dqtype = xfs_scrub_quota_to_dqtype(sc);
	if (dqtype == 0)
		return -EINVAL;
	return 0;
}

/* Quotas. */

/* Scrub the fields in an individual quota item. */
STATIC void
xfs_scrub_quota_item(
	struct xfs_scrub_context	*sc,
	uint				dqtype,
	struct xfs_dquot		*dq,
	xfs_dqid_t			id)
{
	struct xfs_mount		*mp = sc->mp;
	struct xfs_disk_dquot		*d = &dq->q_core;
	struct xfs_quotainfo		*qi = mp->m_quotainfo;
	xfs_fileoff_t			offset;
	unsigned long long		bsoft;
	unsigned long long		isoft;
	unsigned long long		rsoft;
	unsigned long long		bhard;
	unsigned long long		ihard;
	unsigned long long		rhard;
	unsigned long long		bcount;
	unsigned long long		icount;
	unsigned long long		rcount;
	xfs_ino_t			inodes;

	/* Did we get the dquot we wanted? */
	offset = id * qi->qi_dqperchunk;
	xfs_scrub_fblock_check_ok(sc, XFS_DATA_FORK, offset,
			id <= be32_to_cpu(d->d_id) &&
			dqtype == (d->d_flags & XFS_DQ_ALLTYPES));

	/* Check the limits. */
	bhard = be64_to_cpu(d->d_blk_hardlimit);
	ihard = be64_to_cpu(d->d_ino_hardlimit);
	rhard = be64_to_cpu(d->d_rtb_hardlimit);

	bsoft = be64_to_cpu(d->d_blk_softlimit);
	isoft = be64_to_cpu(d->d_ino_softlimit);
	rsoft = be64_to_cpu(d->d_rtb_softlimit);

	inodes = XFS_AGINO_TO_INO(mp, mp->m_sb.sb_agcount, 0);

	/*
	 * Warn if the limits are larger than the fs.  Administrators
	 * can do this, though in production this seems suspect.
	 */
	xfs_scrub_fblock_warn_ok(sc, XFS_DATA_FORK, offset,
			bhard <= mp->m_sb.sb_dblocks &&
			ihard <= inodes &&
			rhard <= mp->m_sb.sb_rblocks &&
			bsoft <= mp->m_sb.sb_dblocks &&
			isoft <= inodes &&
			rsoft <= mp->m_sb.sb_rblocks);

	/* Soft limit must be less than the hard limit. */
	xfs_scrub_fblock_check_ok(sc, XFS_DATA_FORK, offset,
			bsoft <= bhard &&
			isoft <= ihard &&
			rsoft <= rhard);

	/* Check the resource counts. */
	bcount = be64_to_cpu(d->d_bcount);
	icount = be64_to_cpu(d->d_icount);
	rcount = be64_to_cpu(d->d_rtbcount);
	inodes = percpu_counter_sum(&mp->m_icount);

	/*
	 * Check that usage doesn't exceed physical limits.  However, on
	 * a reflink filesystem we're allowed to exceed physical space
	 * if there are no quota limits.
	 */
	if (xfs_sb_version_hasreflink(&mp->m_sb))
		xfs_scrub_fblock_warn_ok(sc, XFS_DATA_FORK, offset,
				bcount <= mp->m_sb.sb_dblocks);
	else
		xfs_scrub_fblock_check_ok(sc, XFS_DATA_FORK, offset,
				bcount <= mp->m_sb.sb_dblocks);
	xfs_scrub_fblock_check_ok(sc, XFS_DATA_FORK, offset,
			icount <= inodes && rcount <= mp->m_sb.sb_rblocks);

	/*
	 * We can violate the hard limits if the admin suddenly sets a
	 * lower limit than the actual usage.  However, we flag it for
	 * admin review.
	 */
	xfs_scrub_fblock_warn_ok(sc, XFS_DATA_FORK, offset,
			(id == 0 || bhard == 0 || bcount <= bhard) &&
			(id == 0 || ihard == 0 || icount <= ihard) &&
			(id == 0 || rhard == 0 || rcount <= rhard));
}

/* Scrub all of a quota type's items. */
int
xfs_scrub_quota(
	struct xfs_scrub_context	*sc)
{
	struct xfs_bmbt_irec		irec = { 0 };
	struct xfs_mount		*mp = sc->mp;
	struct xfs_inode		*ip;
	struct xfs_quotainfo		*qi = mp->m_quotainfo;
	struct xfs_dquot		*dq;
	xfs_fileoff_t			max_dqid_off;
	xfs_fileoff_t			off = 0;
	xfs_dqid_t			id = 0;
	uint				dqtype;
	int				nimaps;
	int				error;

	if (!XFS_IS_QUOTA_RUNNING(mp) || !XFS_IS_QUOTA_ON(mp))
		return -ENOENT;

	mutex_lock(&qi->qi_quotaofflock);
	dqtype = xfs_scrub_quota_to_dqtype(sc);
	if (!xfs_this_quota_on(sc->mp, dqtype)) {
		error = -ENOENT;
		goto out;
	}

	/* Attach to the quota inode and set sc->ip so that reporting works. */
	ip = xfs_quota_inode(sc->mp, dqtype);
	sc->ip = ip;

	/* Look for problem extents. */
	xfs_ilock(ip, XFS_ILOCK_EXCL);
	max_dqid_off = ((xfs_dqid_t)-1) / qi->qi_dqperchunk;
	while (1) {
		if (xfs_scrub_should_terminate(&error))
			break;

		off = irec.br_startoff + irec.br_blockcount;
		nimaps = 1;
		error = xfs_bmapi_read(ip, off, -1, &irec, &nimaps,
				XFS_BMAPI_ENTIRE);
		if (!xfs_scrub_fblock_op_ok(sc, XFS_DATA_FORK, off, &error))
			goto out_unlock;
		if (!nimaps)
			break;
		if (irec.br_startblock == HOLESTARTBLOCK)
			continue;

		/*
		 * Unwritten extents or blocks mapped above the highest
		 * quota id shouldn't happen.
		 */
		xfs_scrub_fblock_check_ok(sc, XFS_DATA_FORK, off,
				!isnullstartblock(irec.br_startblock) &&
				irec.br_startoff <= max_dqid_off &&
				irec.br_startoff + irec.br_blockcount <=
					max_dqid_off + 1);
	}
	xfs_iunlock(ip, XFS_ILOCK_EXCL);

	/* Check all the quota items. */
	while (id < ((xfs_dqid_t)-1ULL)) {
		if (xfs_scrub_should_terminate(&error))
			break;

		error = xfs_qm_dqget(mp, NULL, id, dqtype, XFS_QMOPT_DQNEXT,
				&dq);
		if (error == -ENOENT)
			break;
		if (!xfs_scrub_fblock_op_ok(sc, XFS_DATA_FORK,
				id * qi->qi_dqperchunk, &error))
			goto out;

		xfs_scrub_quota_item(sc, dqtype, dq, id);

		id = be32_to_cpu(dq->q_core.d_id) + 1;
		xfs_qm_dqput(dq);
		if (!id)
			break;
	}
	goto out;

out_unlock:
	xfs_iunlock(ip, XFS_ILOCK_EXCL);
out:
	sc->ip = NULL;
	mutex_unlock(&qi->qi_quotaofflock);
	return error;
}
