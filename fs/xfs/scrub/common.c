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
#include "xfs_itable.h"
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
#include "scrub/btree.h"

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

/*
 * AG scrubbing
 *
 * These helpers facilitate locking an allocation group's header
 * buffers, setting up cursors for all btrees that are present, and
 * cleaning everything up once we're through.
 */

/* Grab all the headers for an AG. */
int
xfs_scrub_ag_read_headers(
	struct xfs_scrub_context	*sc,
	xfs_agnumber_t			agno,
	struct xfs_buf			**agi,
	struct xfs_buf			**agf,
	struct xfs_buf			**agfl)
{
	struct xfs_mount		*mp = sc->mp;
	int				error;

	error = xfs_ialloc_read_agi(mp, sc->tp, agno, agi);
	if (error)
		goto out;

	error = xfs_alloc_read_agf(mp, sc->tp, agno, 0, agf);
	if (error)
		goto out;
	if (!*agf) {
		error = -ENOMEM;
		goto out;
	}

	error = xfs_alloc_read_agfl(mp, sc->tp, agno, agfl);
	if (error)
		goto out;

out:
	return error;
}

/* Release all the AG btree cursors. */
void
xfs_scrub_ag_btcur_free(
	struct xfs_scrub_ag		*sa)
{
	if (sa->refc_cur)
		xfs_btree_del_cursor(sa->refc_cur, XFS_BTREE_ERROR);
	if (sa->rmap_cur)
		xfs_btree_del_cursor(sa->rmap_cur, XFS_BTREE_ERROR);
	if (sa->fino_cur)
		xfs_btree_del_cursor(sa->fino_cur, XFS_BTREE_ERROR);
	if (sa->ino_cur)
		xfs_btree_del_cursor(sa->ino_cur, XFS_BTREE_ERROR);
	if (sa->cnt_cur)
		xfs_btree_del_cursor(sa->cnt_cur, XFS_BTREE_ERROR);
	if (sa->bno_cur)
		xfs_btree_del_cursor(sa->bno_cur, XFS_BTREE_ERROR);

	sa->refc_cur = NULL;
	sa->rmap_cur = NULL;
	sa->fino_cur = NULL;
	sa->ino_cur = NULL;
	sa->bno_cur = NULL;
	sa->cnt_cur = NULL;
}

/* Initialize all the btree cursors for an AG. */
int
xfs_scrub_ag_btcur_init(
	struct xfs_scrub_context	*sc,
	struct xfs_scrub_ag		*sa)
{
	struct xfs_mount		*mp = sc->mp;
	xfs_agnumber_t			agno = sa->agno;

	if (sa->agf_bp) {
		/* Set up a bnobt cursor for cross-referencing. */
		sa->bno_cur = xfs_allocbt_init_cursor(mp, sc->tp, sa->agf_bp,
				agno, XFS_BTNUM_BNO);
		if (!sa->bno_cur)
			goto err;

		/* Set up a cntbt cursor for cross-referencing. */
		sa->cnt_cur = xfs_allocbt_init_cursor(mp, sc->tp, sa->agf_bp,
				agno, XFS_BTNUM_CNT);
		if (!sa->cnt_cur)
			goto err;
	}

	/* Set up a inobt cursor for cross-referencing. */
	if (sa->agi_bp) {
		sa->ino_cur = xfs_inobt_init_cursor(mp, sc->tp, sa->agi_bp,
					agno, XFS_BTNUM_INO);
		if (!sa->ino_cur)
			goto err;
	}

	/* Set up a finobt cursor for cross-referencing. */
	if (sa->agi_bp && xfs_sb_version_hasfinobt(&mp->m_sb)) {
		sa->fino_cur = xfs_inobt_init_cursor(mp, sc->tp, sa->agi_bp,
				agno, XFS_BTNUM_FINO);
		if (!sa->fino_cur)
			goto err;
	}

	/* Set up a rmapbt cursor for cross-referencing. */
	if (sa->agf_bp && xfs_sb_version_hasrmapbt(&mp->m_sb)) {
		sa->rmap_cur = xfs_rmapbt_init_cursor(mp, sc->tp, sa->agf_bp,
				agno);
		if (!sa->rmap_cur)
			goto err;
	}

	/* Set up a refcountbt cursor for cross-referencing. */
	if (sa->agf_bp && xfs_sb_version_hasreflink(&mp->m_sb)) {
		sa->refc_cur = xfs_refcountbt_init_cursor(mp, sc->tp,
				sa->agf_bp, agno, NULL);
		if (!sa->refc_cur)
			goto err;
	}

	return 0;
err:
	return -ENOMEM;
}

/* Release the AG header context and btree cursors. */
void
xfs_scrub_ag_free(
	struct xfs_scrub_context	*sc,
	struct xfs_scrub_ag		*sa)
{
	xfs_scrub_ag_btcur_free(sa);
	if (sa->agfl_bp) {
		xfs_trans_brelse(sc->tp, sa->agfl_bp);
		sa->agfl_bp = NULL;
	}
	if (sa->agf_bp) {
		xfs_trans_brelse(sc->tp, sa->agf_bp);
		sa->agf_bp = NULL;
	}
	if (sa->agi_bp) {
		xfs_trans_brelse(sc->tp, sa->agi_bp);
		sa->agi_bp = NULL;
	}
	sa->agno = NULLAGNUMBER;
}

/*
 * For scrub, grab the AGI and the AGF headers, in that order.  Locking
 * order requires us to get the AGI before the AGF.  We use the
 * transaction to avoid deadlocking on crosslinked metadata buffers;
 * either the caller passes one in (bmap scrub) or we have to create a
 * transaction ourselves.
 */
int
xfs_scrub_ag_init(
	struct xfs_scrub_context	*sc,
	xfs_agnumber_t			agno,
	struct xfs_scrub_ag		*sa)
{
	int				error;

	sa->agno = agno;
	error = xfs_scrub_ag_read_headers(sc, agno, &sa->agi_bp,
			&sa->agf_bp, &sa->agfl_bp);
	if (error)
		return error;

	return xfs_scrub_ag_btcur_init(sc, sa);
}

/*
 * Load and verify an AG header for further AG header examination.
 * If this header is not the target of the examination, don't return
 * the buffer if a runtime or verifier error occurs.
 */
STATIC int
xfs_scrub_load_ag_header(
	struct xfs_scrub_context	*sc,
	xfs_daddr_t			daddr,
	struct xfs_buf			**bpp,
	const struct xfs_buf_ops	*ops,
	bool				is_target)
{
	struct xfs_mount		*mp = sc->mp;
	int				error;

	*bpp = NULL;
	error = xfs_trans_read_buf(mp, sc->tp, mp->m_ddev_targp,
			XFS_AG_DADDR(mp, sc->sa.agno, daddr),
			XFS_FSS_TO_BB(mp, 1), 0, bpp, ops);
	return is_target ? error : 0;
}

/*
 * Load as many of the AG headers and btree cursors as we can for an
 * examination and cross-reference of an AG header.
 */
int
xfs_scrub_load_ag_headers(
	struct xfs_scrub_context	*sc,
	xfs_agnumber_t			agno,
	unsigned int			type)
{
	struct xfs_mount		*mp = sc->mp;
	int				error;

	ASSERT(type == XFS_SCRUB_TYPE_AGF || type == XFS_SCRUB_TYPE_AGFL ||
	       type == XFS_SCRUB_TYPE_AGI);
	memset(&sc->sa, 0, sizeof(sc->sa));
	sc->sa.agno = agno;

	error = xfs_scrub_load_ag_header(sc, XFS_AGI_DADDR(mp),
			&sc->sa.agi_bp, &xfs_agi_buf_ops,
			type == XFS_SCRUB_TYPE_AGI);
	if (error)
		return error;

	error = xfs_scrub_load_ag_header(sc, XFS_AGF_DADDR(mp),
			&sc->sa.agf_bp, &xfs_agf_buf_ops,
			type == XFS_SCRUB_TYPE_AGF);
	if (error)
		return error;

	error = xfs_scrub_load_ag_header(sc, XFS_AGFL_DADDR(mp),
			&sc->sa.agfl_bp, &xfs_agfl_buf_ops,
			type == XFS_SCRUB_TYPE_AGFL);
	if (error)
		return error;

	return 0;
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

/* Set us up with AG headers and btree cursors. */
int
xfs_scrub_setup_ag_btree(
	struct xfs_scrub_context	*sc,
	struct xfs_inode		*ip,
	bool				force_log)
{
	int				error;

	error = xfs_scrub_setup_ag_header(sc, ip);
	if (error)
		return error;

	return xfs_scrub_ag_init(sc, sc->sm->sm_agno, &sc->sa);
}

/*
 * Given an inode and the scrub control structure, grab either the
 * inode referenced in the control structure or the inode passed in.
 * The inode is not locked.
 */
int
xfs_scrub_get_inode(
	struct xfs_scrub_context	*sc,
	struct xfs_inode		*ip_in)
{
	struct xfs_mount		*mp = sc->mp;
	struct xfs_inode		*ips = NULL;
	int				error;

	if (sc->sm->sm_agno || (sc->sm->sm_gen && !sc->sm->sm_ino))
		return -EINVAL;

	/* We want to scan the inode we already had opened. */
	if (sc->sm->sm_ino == 0 || sc->sm->sm_ino == ip_in->i_ino) {
		sc->ip = ip_in;
		return 0;
	}

	/* Look up the inode, see if the generation number matches. */
	if (xfs_internal_inum(mp, sc->sm->sm_ino))
		return -ENOENT;
	error = xfs_iget(mp, NULL, sc->sm->sm_ino, XFS_IGET_UNTRUSTED,
			0, &ips);
	if (error == -ENOENT || error == -EINVAL) {
		/* inode doesn't exist... */
		return -ENOENT;
	} else if (error) {
		trace_xfs_scrub_op_error(sc,
				XFS_INO_TO_AGNO(mp, sc->sm->sm_ino),
				XFS_INO_TO_AGBNO(mp, sc->sm->sm_ino),
				error, __return_address);
		return error;
	}
	if (VFS_I(ips)->i_generation != sc->sm->sm_gen) {
		iput(VFS_I(ips));
		return -ENOENT;
	}

	sc->ip = ips;
	return 0;
}

/* Set us up to scrub a file's contents. */
int
xfs_scrub_setup_inode_contents(
	struct xfs_scrub_context	*sc,
	struct xfs_inode		*ip,
	unsigned int			resblks)
{
	struct xfs_mount		*mp = sc->mp;
	int				error;

	error = xfs_scrub_get_inode(sc, ip);
	if (error)
		return error;

	/* Got the inode, lock it and we're ready to go. */
	sc->ilock_flags = XFS_IOLOCK_EXCL | XFS_MMAPLOCK_EXCL;
	xfs_ilock(sc->ip, sc->ilock_flags);
	error = xfs_scrub_trans_alloc(sc->sm, mp, &M_RES(mp)->tr_itruncate,
			resblks, 0, 0, &sc->tp);
	if (error)
		goto out_unlock;
	sc->ilock_flags |= XFS_ILOCK_EXCL;
	xfs_ilock(sc->ip, XFS_ILOCK_EXCL);

	return 0;
out_unlock:
	xfs_iunlock(sc->ip, sc->ilock_flags);
	if (sc->ip != ip)
		iput(VFS_I(sc->ip));
	sc->ip = NULL;
	return error;
}
