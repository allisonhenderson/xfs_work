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
#include "xfs_inode_buf.h"
#include "xfs_inode_fork.h"
#include "xfs_ialloc.h"
#include "xfs_log.h"
#include "xfs_trans_priv.h"
#include "xfs_reflink.h"
#include "xfs_rmap.h"
#include "xfs_bmap.h"
#include "xfs_bmap_util.h"
#include "scrub/xfs_scrub.h"
#include "scrub/scrub.h"
#include "scrub/common.h"
#include "scrub/trace.h"

/* Set us up with an inode. */
int
xfs_scrub_setup_inode(
	struct xfs_scrub_context	*sc,
	struct xfs_inode		*ip)
{
	struct xfs_mount		*mp = sc->mp;
	int				error;

	/*
	 * Try to get the inode.  If the verifiers fail, we try again
	 * in raw mode.
	 */
	error = xfs_scrub_get_inode(sc, ip);
	switch (error) {
	case 0:
		break;
	case -EFSCORRUPTED:
	case -EFSBADCRC:
		/* Push everything out of the log onto disk prior to check. */
		error = _xfs_log_force(mp, XFS_LOG_SYNC, NULL);
		if (error)
			return error;
		xfs_ail_push_all_sync(mp->m_ail);
		return 0;
	default:
		return error;
	}

	/* Got the inode, lock it and we're ready to go. */
	sc->ilock_flags = XFS_IOLOCK_EXCL | XFS_MMAPLOCK_EXCL;
	xfs_ilock(sc->ip, sc->ilock_flags);
	error = xfs_scrub_trans_alloc(sc->sm, mp, &M_RES(mp)->tr_itruncate,
			0, 0, 0, &sc->tp);
	if (error)
		goto out_unlock;
	sc->ilock_flags |= XFS_ILOCK_EXCL;
	xfs_ilock(sc->ip, XFS_ILOCK_EXCL);

	return error;
out_unlock:
	xfs_iunlock(sc->ip, sc->ilock_flags);
	if (sc->ip != ip)
		iput(VFS_I(sc->ip));
	sc->ip = NULL;
	return error;
}

/* Inode core */

/* Make sure the rmap thinks there's an inode here. */
STATIC int
xfs_scrub_inode_xref_rmap(
	struct xfs_scrub_context	*sc,
	xfs_ino_t			ino)
{
	struct xfs_owner_info		oinfo;
	struct xfs_scrub_ag		sa = { 0 };
	xfs_agnumber_t			agno;
	xfs_agblock_t			agbno;
	bool				has_rmap;
	int				error;

	agno = XFS_INO_TO_AGNO(sc->mp, ino);
	agbno = XFS_INO_TO_AGBNO(sc->mp, ino);
	xfs_rmap_ag_owner(&oinfo, XFS_RMAP_OWN_INODES);
	error = xfs_scrub_ag_init(sc, agno, &sa);
	if (!xfs_scrub_xref_op_ok(sc, agno, agbno, &error))
		return error;

	error = xfs_rmap_record_exists(sa.rmap_cur, agbno,
			1, &oinfo, &has_rmap);
	if (xfs_scrub_should_xref(sc, &error, &sa.rmap_cur))
		xfs_scrub_ino_xref_check_ok(sc, ino, NULL,
				has_rmap);
	xfs_scrub_ag_free(sc, &sa);
	return error;
}

/* Cross reference the inode fields with the forks. */
STATIC void
xfs_scrub_inode_xref_bmap(
	struct xfs_scrub_context	*sc,
	struct xfs_dinode		*dip,
	uint16_t			mode,
	uint16_t			flags)
{
	struct xfs_bmbt_irec		got;
	struct xfs_ifork		*ifp;
	struct xfs_mount		*mp = sc->mp;
	xfs_fileoff_t			lblk;
	xfs_extnum_t			idx;
	xfs_extnum_t			nextents;
	xfs_filblks_t			count;
	xfs_filblks_t			acount;
	bool				found;
	int				error;

	/* Walk all the extents to check nextents/naextents/nblocks. */
	error = xfs_bmap_count_blocks(sc->tp, sc->ip, XFS_DATA_FORK,
			&nextents, &count);
	if (!xfs_scrub_should_xref(sc, &error, NULL))
		return;
	xfs_scrub_ino_xref_check_ok(sc, sc->ip->i_ino, NULL,
			nextents >= be32_to_cpu(dip->di_nextents));

	error = xfs_bmap_count_blocks(sc->tp, sc->ip, XFS_ATTR_FORK,
			&nextents, &acount);
	if (!xfs_scrub_should_xref(sc, &error, NULL))
		return;
	xfs_scrub_ino_xref_check_ok(sc, sc->ip->i_ino, NULL,
			nextents == be16_to_cpu(dip->di_anextents));

	/* Check nblocks against the inode. */
	xfs_scrub_ino_xref_check_ok(sc, sc->ip->i_ino, NULL,
			count + acount == be64_to_cpu(dip->di_nblocks));

	/* Make sure we don't have any written extents after EOF. */
	if (S_ISREG(mode) && !(flags & XFS_DIFLAG_PREALLOC) &&
	    (dip->di_format == XFS_DINODE_FMT_EXTENTS ||
	     dip->di_format == XFS_DINODE_FMT_BTREE)) {
		lblk = XFS_B_TO_FSB(mp, i_size_read(VFS_I(sc->ip)));
		ifp = XFS_IFORK_PTR(sc->ip, XFS_DATA_FORK);
		found = xfs_iext_lookup_extent(sc->ip, ifp, lblk, &idx, &got);
		while (found) {
			xfs_scrub_fblock_xref_check_ok(sc, XFS_DATA_FORK,
					got.br_startoff,
					got.br_startoff < lblk ||
					got.br_state != XFS_EXT_NORM);
			lblk = got.br_startoff + got.br_blockcount;
			found = xfs_iext_get_extent(ifp, ++idx, &got);
		}
	}
}

/* Scrub an inode. */
int
xfs_scrub_inode(
	struct xfs_scrub_context	*sc)
{
	struct xfs_imap			imap;
	struct xfs_dinode		di;
	struct xfs_mount		*mp = sc->mp;
	struct xfs_buf			*bp = NULL;
	struct xfs_dinode		*dip;
	xfs_ino_t			ino;
	size_t				fork_recs;
	unsigned long long		isize;
	uint64_t			flags2;
	xfs_extnum_t			nextents;
	uint32_t			extsize;
	uint32_t			cowextsize;
	uint16_t			flags;
	uint16_t			mode;
	bool				has_shared;
	int				error = 0;

	/* Did we get the in-core inode, or are we doing this manually? */
	if (sc->ip) {
		ino = sc->ip->i_ino;
		xfs_inode_to_disk(sc->ip, &di, 0);
		dip = &di;
	} else {
		/* Map & read inode. */
		ino = sc->sm->sm_ino;
		error = xfs_imap(mp, sc->tp, ino, &imap, XFS_IGET_UNTRUSTED);
		if (error == -EINVAL) {
			/*
			 * Inode could have gotten deleted out from under us;
			 * just forget about it.
			 */
			error = -ENOENT;
			goto out;
		}
		if (!xfs_scrub_op_ok(sc, XFS_INO_TO_AGNO(mp, ino),
				XFS_INO_TO_AGBNO(mp, ino), &error))
			goto out;

		error = xfs_trans_read_buf(mp, sc->tp, mp->m_ddev_targp,
				imap.im_blkno, imap.im_len, XBF_UNMAPPED, &bp,
				NULL);
		if (!xfs_scrub_op_ok(sc, XFS_INO_TO_AGNO(mp, ino),
				XFS_INO_TO_AGBNO(mp, ino), &error))
			goto out;

		/* Is this really the inode we want? */
		bp->b_ops = &xfs_inode_buf_ops;
		dip = xfs_buf_offset(bp, imap.im_boffset);
		if (!xfs_scrub_ino_check_ok(sc, ino, bp,
				xfs_dinode_verify(mp, ino, dip) &&
				xfs_dinode_good_version(mp, dip->di_version)))
			goto out;
		if (be32_to_cpu(dip->di_gen) != sc->sm->sm_gen) {
			error = -ENOENT;
			goto out;
		}
	}

	flags = be16_to_cpu(dip->di_flags);
	if (dip->di_version >= 3)
		flags2 = be64_to_cpu(dip->di_flags2);
	else
		flags2 = 0;

	/* di_mode */
	mode = be16_to_cpu(dip->di_mode);
	xfs_scrub_ino_check_ok(sc, ino, bp, !(mode & ~(S_IALLUGO | S_IFMT)));

	/* v1/v2 fields */
	switch (dip->di_version) {
	case 1:
		xfs_scrub_ino_check_ok(sc, ino, bp,
				dip->di_nlink == 0 &&
				(dip->di_mode || !sc->ip) &&
				dip->di_projid_lo == 0 &&
				dip->di_projid_hi == 0);
		break;
	case 2:
	case 3:
		xfs_scrub_ino_check_ok(sc, ino, bp,
				dip->di_onlink == 0 &&
				(dip->di_mode || !sc->ip) &&
				(dip->di_projid_hi == 0 ||
				 xfs_sb_version_hasprojid32bit(&mp->m_sb)));
		break;
	default:
		ASSERT(0);
		break;
	}

	/*
	 * di_uid/di_gid -- -1 isn't invalid, but there's no way that
	 * userspace could have created that.
	 */
	xfs_scrub_ino_warn_ok(sc, bp,
			dip->di_uid != cpu_to_be32(-1U) &&
			dip->di_gid != cpu_to_be32(-1U));

	/* di_format */
	switch (dip->di_format) {
	case XFS_DINODE_FMT_DEV:
		xfs_scrub_ino_check_ok(sc, ino, bp,
				S_ISCHR(mode) || S_ISBLK(mode) ||
				S_ISFIFO(mode) || S_ISSOCK(mode));
		break;
	case XFS_DINODE_FMT_LOCAL:
		xfs_scrub_ino_check_ok(sc, ino, bp,
				S_ISDIR(mode) || S_ISLNK(mode));
		break;
	case XFS_DINODE_FMT_EXTENTS:
		xfs_scrub_ino_check_ok(sc, ino, bp, S_ISREG(mode) ||
				S_ISDIR(mode) || S_ISLNK(mode));
		break;
	case XFS_DINODE_FMT_BTREE:
		xfs_scrub_ino_check_ok(sc, ino, bp, S_ISREG(mode) ||
				S_ISDIR(mode));
		break;
	case XFS_DINODE_FMT_UUID:
	default:
		xfs_scrub_ino_check_ok(sc, ino, bp, false);
		break;
	}

	/* di_size */
	isize = be64_to_cpu(dip->di_size);
	xfs_scrub_ino_check_ok(sc, ino, bp, !(isize & (1ULL << 63)));
	if (!S_ISDIR(mode) && !S_ISREG(mode) && !S_ISLNK(mode))
		xfs_scrub_ino_check_ok(sc, ino, bp, isize == 0);

	/* di_nblocks */
	if (flags2 & XFS_DIFLAG2_REFLINK) {
		; /* nblocks can exceed dblocks */
	} else if (flags & XFS_DIFLAG_REALTIME) {
		xfs_scrub_ino_check_ok(sc, ino, bp,
				be64_to_cpu(dip->di_nblocks) <
				mp->m_sb.sb_dblocks + mp->m_sb.sb_rblocks);
	} else {
		xfs_scrub_ino_check_ok(sc, ino, bp,
				be64_to_cpu(dip->di_nblocks) <
				mp->m_sb.sb_dblocks);
	}

	/* di_extsize */
	if (flags & XFS_DIFLAG_EXTSIZE) {
		extsize = be32_to_cpu(dip->di_extsize);
		xfs_scrub_ino_check_ok(sc, ino, bp,
				extsize > 0 &&
				extsize <= MAXEXTLEN &&
				(extsize <= mp->m_sb.sb_agblocks / 2 ||
				 (flags & XFS_DIFLAG_REALTIME)));
	}

	/* di_flags */
	xfs_scrub_ino_check_ok(sc, ino, bp,
			(!(flags & XFS_DIFLAG_IMMUTABLE) ||
			      !(flags & XFS_DIFLAG_APPEND)) &&
			(!(flags & XFS_DIFLAG_FILESTREAM) ||
			      !(flags & XFS_DIFLAG_REALTIME)));

	/* di_nextents */
	nextents = be32_to_cpu(dip->di_nextents);
	fork_recs =  XFS_DFORK_DSIZE(dip, mp) / sizeof(struct xfs_bmbt_rec);
	switch (dip->di_format) {
	case XFS_DINODE_FMT_EXTENTS:
		xfs_scrub_ino_check_ok(sc, ino, bp, nextents <= fork_recs);
		break;
	case XFS_DINODE_FMT_BTREE:
		xfs_scrub_ino_check_ok(sc, ino, bp, nextents > fork_recs);
		break;
	case XFS_DINODE_FMT_LOCAL:
	case XFS_DINODE_FMT_DEV:
	case XFS_DINODE_FMT_UUID:
	default:
		xfs_scrub_ino_check_ok(sc, ino, bp, nextents == 0);
		break;
	}

	/* di_anextents */
	nextents = be16_to_cpu(dip->di_anextents);
	fork_recs =  XFS_DFORK_ASIZE(dip, mp) / sizeof(struct xfs_bmbt_rec);
	switch (dip->di_aformat) {
	case XFS_DINODE_FMT_EXTENTS:
		xfs_scrub_ino_check_ok(sc, ino, bp, nextents <= fork_recs);
		break;
	case XFS_DINODE_FMT_BTREE:
		xfs_scrub_ino_check_ok(sc, ino, bp, nextents > fork_recs);
		break;
	case XFS_DINODE_FMT_LOCAL:
	case XFS_DINODE_FMT_DEV:
	case XFS_DINODE_FMT_UUID:
	default:
		xfs_scrub_ino_check_ok(sc, ino, bp, nextents == 0);
		break;
	}

	/* di_forkoff */
	xfs_scrub_ino_check_ok(sc, ino, bp,
			XFS_DFORK_APTR(dip) <
				(char *)dip + mp->m_sb.sb_inodesize &&
			(dip->di_anextents == 0 || dip->di_forkoff));

	/* di_aformat */
	xfs_scrub_ino_check_ok(sc, ino, bp,
			dip->di_aformat == XFS_DINODE_FMT_LOCAL ||
			dip->di_aformat == XFS_DINODE_FMT_EXTENTS ||
			dip->di_aformat == XFS_DINODE_FMT_BTREE);

	/* di_cowextsize */
	if (flags2 & XFS_DIFLAG2_COWEXTSIZE) {
		cowextsize = be32_to_cpu(dip->di_cowextsize);
		xfs_scrub_ino_check_ok(sc, ino, bp,
				xfs_sb_version_hasreflink(&mp->m_sb) &&
				cowextsize > 0 &&
				cowextsize <= MAXEXTLEN &&
				cowextsize <= mp->m_sb.sb_agblocks / 2);
	}

	/* Now let's do the things that require a live inode. */
	if (!sc->ip)
		goto out;

	/*
	 * Does this inode have the reflink flag set but no shared extents?
	 * Set the preening flag if this is the case.
	 */
	if (xfs_is_reflink_inode(sc->ip)) {
		error = xfs_reflink_inode_has_shared_extents(sc->tp, sc->ip,
				&has_shared);
		if (!xfs_scrub_op_ok(sc, XFS_INO_TO_AGNO(mp, ino),
				XFS_INO_TO_AGBNO(mp, ino), &error))
			goto out;
		xfs_scrub_ino_preen_ok(sc, bp, has_shared == true);
	}

	if (xfs_sb_version_hasrmapbt(&mp->m_sb)) {
		error = xfs_scrub_inode_xref_rmap(sc, ino);
		if (error)
			goto out;
	}

	/* Cross reference the inode fields with the forks. */
	xfs_scrub_inode_xref_bmap(sc, dip, mode, flags);

out:
	if (bp)
		xfs_trans_brelse(sc->tp, bp);
	return error;
}
