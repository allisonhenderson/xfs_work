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
#ifndef __XFS_SCRUB_COMMON_H__
#define __XFS_SCRUB_COMMON_H__

/*
 * Grab a transaction.  If we're going to repair something, we need to
 * ensure there's enough reservation to make all the changes.  If not,
 * we can use an empty transaction.
 */
static inline int
xfs_scrub_trans_alloc(
	struct xfs_scrub_metadata	*sm,
	struct xfs_mount		*mp,
	struct xfs_trans_res		*resp,
	uint				blocks,
	uint				rtextents,
	uint				flags,
	struct xfs_trans		**tpp)
{
	return xfs_trans_alloc_empty(mp, tpp);
}

/* Setup functions */
int xfs_scrub_setup_fs(struct xfs_scrub_context *sc, struct xfs_inode *ip);

#endif	/* __XFS_SCRUB_COMMON_H__ */
