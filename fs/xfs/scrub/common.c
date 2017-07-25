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
#include "xfs_alloc.h"
#include "xfs_alloc_btree.h"
#include "xfs_bmap.h"
#include "xfs_bmap_btree.h"
#include "xfs_ialloc.h"
#include "xfs_ialloc_btree.h"
#include "xfs_refcount.h"
#include "xfs_refcount_btree.h"
#include "xfs_rmap.h"
#include "xfs_rmap_btree.h"
#include "scrub/xfs_scrub.h"
#include "scrub/scrub.h"
#include "scrub/common.h"
#include "scrub/trace.h"

/* Common code for the metadata scrubbers. */

/* Check for operational errors. */
bool
xfs_scrub_op_ok(
	struct xfs_scrub_context	*sc,
	xfs_agnumber_t			agno,
	xfs_agblock_t			bno,
	int				*error)
{
	switch (*error) {
	case 0:
		return true;
	case -EDEADLOCK:
		/* Used to restart an op with deadlock avoidance. */
		trace_xfs_scrub_deadlock_retry(sc->ip, sc->sm, *error);
		break;
	case -EFSBADCRC:
	case -EFSCORRUPTED:
		/* Note the badness but don't abort. */
		sc->sm->sm_flags |= XFS_SCRUB_OFLAG_CORRUPT;
		*error = 0;
		/* fall through */
	default:
		trace_xfs_scrub_op_error(sc, agno, bno, *error,
				__return_address);
		break;
	}
	return false;
}

/* Check for operational errors for a file offset. */
bool
xfs_scrub_fblock_op_ok(
	struct xfs_scrub_context	*sc,
	int				whichfork,
	xfs_fileoff_t			offset,
	int				*error)
{
	switch (*error) {
	case 0:
		return true;
	case -EDEADLOCK:
		/* Used to restart an op with deadlock avoidance. */
		trace_xfs_scrub_deadlock_retry(sc->ip, sc->sm, *error);
		break;
	case -EFSBADCRC:
	case -EFSCORRUPTED:
		/* Note the badness but don't abort. */
		sc->sm->sm_flags |= XFS_SCRUB_OFLAG_CORRUPT;
		*error = 0;
		/* fall through */
	default:
		trace_xfs_scrub_file_op_error(sc, whichfork, offset, *error,
				__return_address);
		break;
	}
	return false;
}

/* Check for metadata block optimization possibilities. */
bool
xfs_scrub_block_preen_ok(
	struct xfs_scrub_context	*sc,
	struct xfs_buf			*bp,
	bool				fs_ok)
{
	struct xfs_mount		*mp = sc->mp;
	xfs_fsblock_t			fsbno;
	xfs_agnumber_t			agno;
	xfs_agblock_t			bno;

	if (fs_ok)
		return fs_ok;

	fsbno = XFS_DADDR_TO_FSB(mp, bp->b_bn);
	agno = XFS_FSB_TO_AGNO(mp, fsbno);
	bno = XFS_FSB_TO_AGBNO(mp, fsbno);

	sc->sm->sm_flags |= XFS_SCRUB_OFLAG_PREEN;
	trace_xfs_scrub_block_preen(sc, agno, bno, __return_address);
	return fs_ok;
}

/* Check for inode metadata optimization possibilities. */
bool
xfs_scrub_ino_preen_ok(
	struct xfs_scrub_context	*sc,
	struct xfs_buf			*bp,
	bool				fs_ok)
{
	struct xfs_inode		*ip = sc->ip;
	struct xfs_mount		*mp = sc->mp;
	xfs_fsblock_t			fsbno;
	xfs_agnumber_t			agno;
	xfs_agblock_t			bno;

	if (fs_ok)
		return fs_ok;

	if (bp) {
		fsbno = XFS_DADDR_TO_FSB(mp, bp->b_bn);
		agno = XFS_FSB_TO_AGNO(mp, fsbno);
		bno = XFS_FSB_TO_AGBNO(mp, fsbno);
	} else {
		agno = XFS_INO_TO_AGNO(mp, ip->i_ino);
		bno = XFS_INO_TO_AGINO(mp, ip->i_ino);
	}

	sc->sm->sm_flags |= XFS_SCRUB_OFLAG_PREEN;
	trace_xfs_scrub_ino_preen(sc, ip->i_ino, agno, bno, __return_address);
	return fs_ok;
}

/* Check for metadata block corruption. */
bool
xfs_scrub_block_check_ok(
	struct xfs_scrub_context	*sc,
	struct xfs_buf			*bp,
	bool				fs_ok)
{
	struct xfs_mount		*mp = sc->mp;
	xfs_fsblock_t			fsbno;
	xfs_agnumber_t			agno;
	xfs_agblock_t			bno;

	if (fs_ok)
		return fs_ok;

	fsbno = XFS_DADDR_TO_FSB(mp, bp->b_bn);
	agno = XFS_FSB_TO_AGNO(mp, fsbno);
	bno = XFS_FSB_TO_AGBNO(mp, fsbno);

	sc->sm->sm_flags |= XFS_SCRUB_OFLAG_CORRUPT;
	trace_xfs_scrub_block_error(sc, agno, bno, __return_address);
	return fs_ok;
}

/* Check for inode metadata corruption. */
bool
xfs_scrub_ino_check_ok(
	struct xfs_scrub_context	*sc,
	xfs_ino_t			ino,
	struct xfs_buf			*bp,
	bool				fs_ok)
{
	struct xfs_inode		*ip = sc->ip;
	struct xfs_mount		*mp = sc->mp;
	xfs_fsblock_t			fsbno;
	xfs_agnumber_t			agno;
	xfs_agblock_t			bno;

	if (fs_ok)
		return fs_ok;

	if (bp) {
		fsbno = XFS_DADDR_TO_FSB(mp, bp->b_bn);
		agno = XFS_FSB_TO_AGNO(mp, fsbno);
		bno = XFS_FSB_TO_AGBNO(mp, fsbno);
	} else {
		agno = XFS_INO_TO_AGNO(mp, ip->i_ino);
		bno = XFS_INO_TO_AGINO(mp, ip->i_ino);
	}

	sc->sm->sm_flags |= XFS_SCRUB_OFLAG_CORRUPT;
	trace_xfs_scrub_ino_error(sc, ino, agno, bno, __return_address);
	return fs_ok;
}

/* Check for file fork block corruption. */
bool
xfs_scrub_fblock_check_ok(
	struct xfs_scrub_context	*sc,
	int				whichfork,
	xfs_fileoff_t			offset,
	bool				fs_ok)
{
	if (fs_ok)
		return fs_ok;

	sc->sm->sm_flags |= XFS_SCRUB_OFLAG_CORRUPT;
	trace_xfs_scrub_fblock_error(sc, whichfork, offset, __return_address);
	return fs_ok;
}

/* Check for inode metadata non-corruption problems. */
bool
xfs_scrub_ino_warn_ok(
	struct xfs_scrub_context	*sc,
	struct xfs_buf			*bp,
	bool				fs_ok)
{
	struct xfs_inode		*ip = sc->ip;
	struct xfs_mount		*mp = sc->mp;
	xfs_fsblock_t			fsbno;
	xfs_agnumber_t			agno;
	xfs_agblock_t			bno;

	if (fs_ok)
		return fs_ok;

	if (bp) {
		fsbno = XFS_DADDR_TO_FSB(mp, bp->b_bn);
		agno = XFS_FSB_TO_AGNO(mp, fsbno);
		bno = XFS_FSB_TO_AGBNO(mp, fsbno);
	} else {
		agno = XFS_INO_TO_AGNO(mp, ip->i_ino);
		bno = XFS_INO_TO_AGINO(mp, ip->i_ino);
	}

	sc->sm->sm_flags |= XFS_SCRUB_OFLAG_WARNING;
	trace_xfs_scrub_ino_warning(sc, ip->i_ino, agno, bno, __return_address);
	return fs_ok;
}

/* Check for file fork block non-corruption problems. */
bool
xfs_scrub_fblock_warn_ok(
	struct xfs_scrub_context	*sc,
	int				whichfork,
	xfs_fileoff_t			offset,
	bool				fs_ok)
{
	if (fs_ok)
		return fs_ok;

	sc->sm->sm_flags |= XFS_SCRUB_OFLAG_WARNING;
	trace_xfs_scrub_fblock_warning(sc, whichfork, offset, __return_address);
	return fs_ok;
}

/* Signal an incomplete scrub. */
bool
xfs_scrub_check_thoroughness(
	struct xfs_scrub_context	*sc,
	bool				fs_ok)
{
	if (fs_ok)
		return fs_ok;

	sc->sm->sm_flags |= XFS_SCRUB_OFLAG_INCOMPLETE;
	trace_xfs_scrub_incomplete(sc, __return_address);
	return fs_ok;
}

/* Per-scrubber setup functions */

/* Set us up with a transaction and an empty context. */
int
xfs_scrub_setup_fs(
	struct xfs_scrub_context	*sc,
	struct xfs_inode		*ip)
{
	return xfs_scrub_trans_alloc(sc->sm, sc->mp,
			&M_RES(sc->mp)->tr_itruncate, 0, 0, 0, &sc->tp);
}
