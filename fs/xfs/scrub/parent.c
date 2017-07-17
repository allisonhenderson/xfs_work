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
#include "xfs_dir2.h"
#include "xfs_dir2_priv.h"
#include "scrub/xfs_scrub.h"
#include "scrub/scrub.h"
#include "scrub/common.h"
#include "scrub/trace.h"

/* Set us up to scrub parents. */
int
xfs_scrub_setup_parent(
	struct xfs_scrub_context	*sc,
	struct xfs_inode		*ip)
{
	return xfs_scrub_setup_inode_contents(sc, ip, 0);
}

/* Parent pointers */

/* Look for an entry in a parent pointing to this inode. */

struct xfs_scrub_parent_ctx {
	struct dir_context		dc;
	xfs_ino_t			ino;
	xfs_nlink_t			nr;
};

/* Look for a single entry in a directory pointing to an inode. */
STATIC int
xfs_scrub_parent_actor(
	struct dir_context		*dc,
	const char			*name,
	int				namelen,
	loff_t				pos,
	u64				ino,
	unsigned			type)
{
	struct xfs_scrub_parent_ctx	*spc;

	spc = container_of(dc, struct xfs_scrub_parent_ctx, dc);
	if (spc->ino == ino)
		spc->nr++;
	return 0;
}

/* Count the number of dentries in the parent dir that point to this inode. */
STATIC int
xfs_scrub_parent_count_parent_dentries(
	struct xfs_scrub_context	*sc,
	struct xfs_inode		*parent,
	xfs_nlink_t			*nr)
{
	struct xfs_scrub_parent_ctx	spc = {
		.dc.actor = xfs_scrub_parent_actor,
		.dc.pos = 0,
		.ino = sc->ip->i_ino,
		.nr = 0,
	};
	struct xfs_ifork		*ifp;
	size_t				bufsize;
	loff_t				oldpos;
	uint				lock_mode;
	int				error;

	/*
	 * Load the parent directory's extent map.  A regular directory
	 * open would start readahead (and thus load the extent map)
	 * before we even got to a readdir call, but this isn't
	 * guaranteed here.
	 */
	lock_mode = xfs_ilock_data_map_shared(parent);
	ifp = XFS_IFORK_PTR(parent, XFS_DATA_FORK);
	if (XFS_IFORK_FORMAT(parent, XFS_DATA_FORK) == XFS_DINODE_FMT_BTREE &&
	    !(ifp->if_flags & XFS_IFEXTENTS)) {
		error = xfs_iread_extents(sc->tp, parent, XFS_DATA_FORK);
		if (error) {
			xfs_iunlock(parent, lock_mode);
			return error;
		}
	}
	xfs_iunlock(parent, lock_mode);

	/*
	 * Iterate the parent dir to confirm that there is
	 * exactly one entry pointing back to the inode being
	 * scanned.
	 */
	bufsize = (size_t)min_t(loff_t, 32768, parent->i_d.di_size);
	oldpos = 0;
	while (true) {
		error = xfs_readdir(sc->tp, parent, &spc.dc, bufsize);
		if (error)
			goto out;
		if (oldpos == spc.dc.pos)
			break;
		oldpos = spc.dc.pos;
	}
	*nr = spc.nr;
out:
	return error;
}

/* Scrub a parent pointer. */
int
xfs_scrub_parent(
	struct xfs_scrub_context	*sc)
{
	struct xfs_mount		*mp = sc->mp;
	struct xfs_inode		*dp = NULL;
	xfs_ino_t			dnum;
	xfs_nlink_t			nr;
	int				tries = 0;
	int				error;

	/*
	 * If we're a directory, check that the '..' link points up to
	 * a directory that has one entry pointing to us.
	 */
	if (!S_ISDIR(VFS_I(sc->ip)->i_mode))
		return -ENOENT;

	/*
	 * The VFS grabs a read or write lock via i_rwsem before it reads
	 * or writes to a directory.  If we've gotten this far we've
	 * already obtained IOLOCK_EXCL, which (since 4.10) is the same as
	 * getting a write lock on i_rwsem.  Therefore, it is safe for us
	 * to drop the ILOCK here in order to do directory lookups.
	 */
	sc->ilock_flags &= ~(XFS_ILOCK_EXCL | XFS_MMAPLOCK_EXCL);
	xfs_iunlock(sc->ip, XFS_ILOCK_EXCL | XFS_MMAPLOCK_EXCL);

	/* Look up '..' */
	error = xfs_dir_lookup(sc->tp, sc->ip, &xfs_name_dotdot, &dnum, NULL);
	if (!xfs_scrub_fblock_op_ok(sc, XFS_DATA_FORK, 0, &error))
		goto out;

	/* Is this the root dir?  Then '..' must point to itself. */
	if (sc->ip == mp->m_rootip) {
		xfs_scrub_fblock_check_ok(sc, XFS_DATA_FORK, 0,
				sc->ip->i_ino == mp->m_sb.sb_rootino &&
				dnum == sc->ip->i_ino);
		return 0;
	}

try_again:
	/* Otherwise, '..' must not point to ourselves. */
	if (!xfs_scrub_fblock_check_ok(sc, XFS_DATA_FORK, 0,
			sc->ip->i_ino != dnum))
		goto out;

	error = xfs_iget(mp, sc->tp, dnum, 0, 0, &dp);
	if (!xfs_scrub_fblock_op_ok(sc, XFS_DATA_FORK, 0, &error))
		goto out;
	if (!xfs_scrub_fblock_check_ok(sc, XFS_DATA_FORK, 0,
			dp != sc->ip))
		goto out_rele;

	/*
	 * We prefer to keep the inode locked while we lock and search
	 * its alleged parent for a forward reference.  However, this
	 * child -> parent scheme can deadlock with the parent -> child
	 * scheme that is normally used.  Therefore, if we can lock the
	 * parent, just validate the references and get out.
	 */
	if (xfs_ilock_nowait(dp, XFS_IOLOCK_SHARED)) {
		error = xfs_scrub_parent_count_parent_dentries(sc, dp, &nr);
		if (!xfs_scrub_fblock_op_ok(sc, XFS_DATA_FORK, 0, &error))
			goto out_unlock;
		xfs_scrub_fblock_check_ok(sc, XFS_DATA_FORK, 0, nr == 1);
		goto out_unlock;
	}

	/*
	 * The game changes if we get here.  We failed to lock the parent,
	 * so we're going to try to verify both pointers while only holding
	 * one lock so as to avoid deadlocking with something that's actually
	 * trying to traverse down the directory tree.
	 */
	xfs_iunlock(sc->ip, sc->ilock_flags);
	sc->ilock_flags = 0;
	xfs_ilock(dp, XFS_IOLOCK_SHARED);

	/* Go looking for our dentry. */
	error = xfs_scrub_parent_count_parent_dentries(sc, dp, &nr);
	if (!xfs_scrub_fblock_op_ok(sc, XFS_DATA_FORK, 0, &error))
		goto out_unlock;

	/* Drop the parent lock, relock this inode. */
	xfs_iunlock(dp, XFS_IOLOCK_SHARED);
	sc->ilock_flags = XFS_IOLOCK_EXCL;
	xfs_ilock(sc->ip, sc->ilock_flags);

	/* Look up '..' to see if the inode changed. */
	error = xfs_dir_lookup(sc->tp, sc->ip, &xfs_name_dotdot, &dnum, NULL);
	if (!xfs_scrub_fblock_op_ok(sc, XFS_DATA_FORK, 0, &error))
		goto out_rele;

	/* Drat, parent changed.  Try again! */
	if (dnum != dp->i_ino) {
		iput(VFS_I(dp));
		tries++;
		if (tries < 20)
			goto try_again;
		xfs_scrub_check_thoroughness(sc, false);
		goto out;
	}
	iput(VFS_I(dp));

	/*
	 * '..' didn't change, so check that there was only one entry
	 * for us in the parent.
	 */
	xfs_scrub_fblock_check_ok(sc, XFS_DATA_FORK, 0, nr == 1);
	goto out;

out_unlock:
	xfs_iunlock(dp, XFS_IOLOCK_SHARED);
out_rele:
	iput(VFS_I(dp));
out:
	return error;
}
