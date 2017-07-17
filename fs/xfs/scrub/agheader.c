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
#include "scrub/xfs_scrub.h"
#include "scrub/scrub.h"
#include "scrub/common.h"
#include "scrub/trace.h"

/* Set us up to check an AG header. */
int
xfs_scrub_setup_ag_header(
	struct xfs_scrub_context	*sc,
	struct xfs_inode		*ip)
{
	struct xfs_mount		*mp = sc->mp;

	if (sc->sm->sm_agno >= mp->m_sb.sb_agcount ||
	    sc->sm->sm_ino || sc->sm->sm_gen)
		return -EINVAL;
	return xfs_scrub_setup_fs(sc, ip);
}

/* Find the size of the AG, in blocks. */
static inline xfs_agblock_t
xfs_scrub_ag_blocks(
	struct xfs_mount	*mp,
	xfs_agnumber_t		agno)
{
	ASSERT(agno < mp->m_sb.sb_agcount);

	if (agno < mp->m_sb.sb_agcount - 1)
		return mp->m_sb.sb_agblocks;
	return mp->m_sb.sb_dblocks - (agno * mp->m_sb.sb_agblocks);
}

/* Walk all the blocks in the AGFL. */
int
xfs_scrub_walk_agfl(
	struct xfs_scrub_context	*sc,
	int				(*fn)(struct xfs_scrub_context *,
					      xfs_agblock_t bno, void *),
	void				*priv)
{
	struct xfs_agf			*agf;
	__be32				*agfl_bno;
	struct xfs_mount		*mp = sc->mp;
	unsigned int			flfirst;
	unsigned int			fllast;
	int				i;
	int				error;

	agf = XFS_BUF_TO_AGF(sc->sa.agf_bp);
	agfl_bno = XFS_BUF_TO_AGFL_BNO(mp, sc->sa.agfl_bp);
	flfirst = be32_to_cpu(agf->agf_flfirst);
	fllast = be32_to_cpu(agf->agf_fllast);

	/* Skip an empty AGFL. */
	if (agf->agf_flcount == cpu_to_be32(0))
		return 0;

	/* first to last is a consecutive list. */
	if (fllast >= flfirst) {
		for (i = flfirst; i <= fllast; i++) {
			error = fn(sc, be32_to_cpu(agfl_bno[i]), priv);
			if (error)
				return error;
		}

		return 0;
	}

	/* first to the end */
	for (i = flfirst; i < XFS_AGFL_SIZE(mp); i++) {
		error = fn(sc, be32_to_cpu(agfl_bno[i]), priv);
		if (error)
			return error;
	}

	/* the start to last. */
	for (i = 0; i <= fllast; i++) {
		error = fn(sc, be32_to_cpu(agfl_bno[i]), priv);
		if (error)
			return error;
	}

	return 0;
}

/* Superblock */

/* Scrub the filesystem superblock. */
int
xfs_scrub_superblock(
	struct xfs_scrub_context	*sc)
{
	struct xfs_mount		*mp = sc->mp;
	struct xfs_buf			*bp;
	struct xfs_dsb			*sb;
	xfs_agnumber_t			agno;
	uint32_t			v2_ok;
	__be32				features_mask;
	int				error;
	__be16				vernum_mask;

	agno = sc->sm->sm_agno;
	if (agno == 0)
		return 0;

	error = xfs_trans_read_buf(mp, sc->tp, mp->m_ddev_targp,
		  XFS_AGB_TO_DADDR(mp, agno, XFS_SB_BLOCK(mp)),
		  XFS_FSS_TO_BB(mp, 1), 0, &bp, &xfs_sb_buf_ops);
	if (!xfs_scrub_op_ok(sc, agno, XFS_SB_BLOCK(mp), &error))
		return error;

	sb = XFS_BUF_TO_SBP(bp);

	/*
	 * Verify the geometries match.  Fields that are permanently
	 * set by mkfs are checked; fields that can be updated later
	 * (and are not propagated to backup superblocks) are preen
	 * checked.
	 */
	xfs_scrub_block_check_ok(sc, bp,
			sb->sb_blocksize == cpu_to_be32(mp->m_sb.sb_blocksize));

	xfs_scrub_block_check_ok(sc, bp,
			sb->sb_dblocks == cpu_to_be64(mp->m_sb.sb_dblocks));

	xfs_scrub_block_check_ok(sc, bp,
			sb->sb_rblocks == cpu_to_be64(mp->m_sb.sb_rblocks));

	xfs_scrub_block_check_ok(sc, bp,
			sb->sb_rextents == cpu_to_be64(mp->m_sb.sb_rextents));

	xfs_scrub_block_preen_ok(sc, bp,
			uuid_equal(&sb->sb_uuid, &mp->m_sb.sb_uuid));

	xfs_scrub_block_check_ok(sc, bp,
			sb->sb_logstart == cpu_to_be64(mp->m_sb.sb_logstart));

	xfs_scrub_block_preen_ok(sc, bp,
			sb->sb_rootino == cpu_to_be64(mp->m_sb.sb_rootino));

	xfs_scrub_block_preen_ok(sc, bp,
			sb->sb_rbmino == cpu_to_be64(mp->m_sb.sb_rbmino));

	xfs_scrub_block_preen_ok(sc, bp,
			sb->sb_rsumino == cpu_to_be64(mp->m_sb.sb_rsumino));

	xfs_scrub_block_check_ok(sc, bp,
			sb->sb_rextsize == cpu_to_be32(mp->m_sb.sb_rextsize));

	xfs_scrub_block_check_ok(sc, bp,
			sb->sb_agblocks == cpu_to_be32(mp->m_sb.sb_agblocks));

	xfs_scrub_block_check_ok(sc, bp,
			sb->sb_agcount == cpu_to_be32(mp->m_sb.sb_agcount));

	xfs_scrub_block_check_ok(sc, bp,
			sb->sb_rbmblocks == cpu_to_be32(mp->m_sb.sb_rbmblocks));

	xfs_scrub_block_check_ok(sc, bp,
			sb->sb_logblocks == cpu_to_be32(mp->m_sb.sb_logblocks));

	/* Check sb_versionnum bits that are set at mkfs time. */
	vernum_mask = cpu_to_be16(~XFS_SB_VERSION_OKBITS |
				  XFS_SB_VERSION_NUMBITS |
				  XFS_SB_VERSION_ALIGNBIT |
				  XFS_SB_VERSION_DALIGNBIT |
				  XFS_SB_VERSION_SHAREDBIT |
				  XFS_SB_VERSION_LOGV2BIT |
				  XFS_SB_VERSION_SECTORBIT |
				  XFS_SB_VERSION_EXTFLGBIT |
				  XFS_SB_VERSION_DIRV2BIT);
	xfs_scrub_block_check_ok(sc, bp,
			(sb->sb_versionnum & vernum_mask) ==
			(cpu_to_be16(mp->m_sb.sb_versionnum) & vernum_mask));

	/* Check sb_versionnum bits that can be set after mkfs time. */
	vernum_mask = cpu_to_be16(XFS_SB_VERSION_ATTRBIT |
				  XFS_SB_VERSION_NLINKBIT |
				  XFS_SB_VERSION_QUOTABIT);
	xfs_scrub_block_preen_ok(sc, bp,
			(sb->sb_versionnum & vernum_mask) ==
			(cpu_to_be16(mp->m_sb.sb_versionnum) & vernum_mask));

	xfs_scrub_block_check_ok(sc, bp,
			sb->sb_sectsize == cpu_to_be16(mp->m_sb.sb_sectsize));

	xfs_scrub_block_check_ok(sc, bp,
			sb->sb_inodesize == cpu_to_be16(mp->m_sb.sb_inodesize));

	xfs_scrub_block_check_ok(sc, bp,
			sb->sb_inopblock == cpu_to_be16(mp->m_sb.sb_inopblock));

	xfs_scrub_block_preen_ok(sc, bp,
			!memcmp(sb->sb_fname, mp->m_sb.sb_fname,
				sizeof(sb->sb_fname)));

	xfs_scrub_block_check_ok(sc, bp,
			sb->sb_blocklog == mp->m_sb.sb_blocklog);

	xfs_scrub_block_check_ok(sc, bp,
			sb->sb_sectlog == mp->m_sb.sb_sectlog);

	xfs_scrub_block_check_ok(sc, bp,
			sb->sb_inodelog == mp->m_sb.sb_inodelog);

	xfs_scrub_block_check_ok(sc, bp,
			sb->sb_inopblog == mp->m_sb.sb_inopblog);

	xfs_scrub_block_check_ok(sc, bp,
			sb->sb_agblklog == mp->m_sb.sb_agblklog);

	xfs_scrub_block_check_ok(sc, bp,
			sb->sb_rextslog == mp->m_sb.sb_rextslog);

	xfs_scrub_block_preen_ok(sc, bp,
			sb->sb_imax_pct == mp->m_sb.sb_imax_pct);

	/*
	 * Skip the summary counters since we track them in memory anyway.
	 * sb_icount, sb_ifree, sb_fdblocks, sb_frexents
	 */

	xfs_scrub_block_preen_ok(sc, bp,
			sb->sb_uquotino == cpu_to_be64(mp->m_sb.sb_uquotino));

	xfs_scrub_block_preen_ok(sc, bp,
			sb->sb_gquotino == cpu_to_be64(mp->m_sb.sb_gquotino));

	/*
	 * Skip the quota flags since repair will force quotacheck.
	 * sb_qflags
	 */

	xfs_scrub_block_check_ok(sc, bp,
			sb->sb_flags == mp->m_sb.sb_flags);

	xfs_scrub_block_check_ok(sc, bp,
			sb->sb_shared_vn == mp->m_sb.sb_shared_vn);

	xfs_scrub_block_check_ok(sc, bp,
			sb->sb_inoalignmt == cpu_to_be32(mp->m_sb.sb_inoalignmt));

	xfs_scrub_block_preen_ok(sc, bp,
			sb->sb_unit == cpu_to_be32(mp->m_sb.sb_unit));

	xfs_scrub_block_preen_ok(sc, bp,
			sb->sb_width == cpu_to_be32(mp->m_sb.sb_width));

	xfs_scrub_block_check_ok(sc, bp,
			sb->sb_dirblklog == mp->m_sb.sb_dirblklog);

	xfs_scrub_block_check_ok(sc, bp,
			sb->sb_logsectlog == mp->m_sb.sb_logsectlog);

	xfs_scrub_block_check_ok(sc, bp,
			sb->sb_logsectsize ==
			cpu_to_be16(mp->m_sb.sb_logsectsize));

	xfs_scrub_block_check_ok(sc, bp,
			sb->sb_logsunit == cpu_to_be32(mp->m_sb.sb_logsunit));

	/* Do we see any invalid bits in sb_features2? */
	if (!xfs_sb_version_hasmorebits(&mp->m_sb)) {
		xfs_scrub_block_check_ok(sc, bp, sb->sb_features2 == 0);
	} else {
		v2_ok = XFS_SB_VERSION2_OKBITS;
		if (XFS_SB_VERSION_NUM(&mp->m_sb) >= XFS_SB_VERSION_5)
			v2_ok |= XFS_SB_VERSION2_CRCBIT;

		xfs_scrub_block_check_ok(sc, bp,
				!(sb->sb_features2 & cpu_to_be32(~v2_ok)));

		xfs_scrub_block_preen_ok(sc, bp,
				sb->sb_features2 == sb->sb_bad_features2);
	}

	/* Check sb_features2 flags that are set at mkfs time. */
	features_mask = cpu_to_be32(XFS_SB_VERSION2_LAZYSBCOUNTBIT |
				    XFS_SB_VERSION2_PROJID32BIT |
				    XFS_SB_VERSION2_CRCBIT |
				    XFS_SB_VERSION2_FTYPE);
	xfs_scrub_block_check_ok(sc, bp,
			(sb->sb_features2 & features_mask) ==
			(cpu_to_be32(mp->m_sb.sb_features2) & features_mask));

	/* Check sb_features2 flags that can be set after mkfs time. */
	features_mask = cpu_to_be32(XFS_SB_VERSION2_ATTR2BIT);
	xfs_scrub_block_check_ok(sc, bp,
			(sb->sb_features2 & features_mask) ==
			(cpu_to_be32(mp->m_sb.sb_features2) & features_mask));

	if (!xfs_sb_version_hascrc(&mp->m_sb)) {
		/* all v5 fields must be zero */
		xfs_scrub_block_check_ok(sc, bp,
				!memchr_inv(&sb->sb_features_compat, 0,
					sizeof(struct xfs_dsb) -
					offsetof(struct xfs_dsb,
						sb_features_compat)));
	} else {
		/* Check compat flags; all are set at mkfs time. */
		features_mask = cpu_to_be32(XFS_SB_FEAT_COMPAT_UNKNOWN);
		xfs_scrub_block_check_ok(sc, bp,
				(sb->sb_features_compat & features_mask) ==
				(cpu_to_be32(mp->m_sb.sb_features_compat) &
					features_mask));

		/* Check ro compat flags; all are set at mkfs time. */
		features_mask = cpu_to_be32(XFS_SB_FEAT_RO_COMPAT_UNKNOWN |
					    XFS_SB_FEAT_RO_COMPAT_FINOBT |
					    XFS_SB_FEAT_RO_COMPAT_RMAPBT |
					    XFS_SB_FEAT_RO_COMPAT_REFLINK);
		xfs_scrub_block_check_ok(sc, bp,
				(sb->sb_features_ro_compat & features_mask) ==
				(cpu_to_be32(mp->m_sb.sb_features_ro_compat) &
					features_mask));

		/* Check incompat flags; all are set at mkfs time. */
		features_mask = cpu_to_be32(XFS_SB_FEAT_INCOMPAT_UNKNOWN |
					    XFS_SB_FEAT_INCOMPAT_FTYPE |
					    XFS_SB_FEAT_INCOMPAT_SPINODES |
					    XFS_SB_FEAT_INCOMPAT_META_UUID);
		xfs_scrub_block_check_ok(sc, bp,
				(sb->sb_features_incompat & features_mask) ==
				(cpu_to_be32(mp->m_sb.sb_features_incompat) &
					features_mask));

		/* Check log incompat flags; all are set at mkfs time. */
		features_mask = cpu_to_be32(XFS_SB_FEAT_INCOMPAT_LOG_UNKNOWN);
		xfs_scrub_block_check_ok(sc, bp,
				(sb->sb_features_log_incompat & features_mask) ==
				(cpu_to_be32(mp->m_sb.sb_features_log_incompat) &
					features_mask));

		/* Don't care about sb_crc */

		xfs_scrub_block_check_ok(sc, bp,
				sb->sb_spino_align ==
				cpu_to_be32(mp->m_sb.sb_spino_align));

		xfs_scrub_block_preen_ok(sc, bp,
				sb->sb_pquotino ==
				cpu_to_be64(mp->m_sb.sb_pquotino));

		/* Don't care about sb_lsn */
	}

	if (xfs_sb_version_hasmetauuid(&mp->m_sb)) {
		/* The metadata UUID must be the same for all supers */
		xfs_scrub_block_check_ok(sc, bp,
				uuid_equal(&sb->sb_meta_uuid,
					&mp->m_sb.sb_meta_uuid));
	}

	/* Everything else must be zero. */
	xfs_scrub_block_check_ok(sc, bp,
			!memchr_inv(sb + 1, 0,
				BBTOB(bp->b_length) - sizeof(struct xfs_dsb)));

	return error;
}

/* AGF */

/* Scrub the AGF. */
int
xfs_scrub_agf(
	struct xfs_scrub_context	*sc)
{
	struct xfs_mount		*mp = sc->mp;
	struct xfs_agf			*agf;
	xfs_daddr_t			daddr;
	xfs_daddr_t			eofs;
	xfs_agnumber_t			agno;
	xfs_agblock_t			agbno;
	xfs_agblock_t			eoag;
	xfs_agblock_t			agfl_first;
	xfs_agblock_t			agfl_last;
	xfs_agblock_t			agfl_count;
	xfs_agblock_t			fl_count;
	int				level;
	int				error = 0;

	agno = sc->sm->sm_agno;
	error = xfs_scrub_load_ag_headers(sc, agno, XFS_SCRUB_TYPE_AGF);
	if (!xfs_scrub_op_ok(sc, agno, XFS_AGF_BLOCK(sc->mp), &error))
		goto out;

	agf = XFS_BUF_TO_AGF(sc->sa.agf_bp);
	eofs = XFS_FSB_TO_BB(mp, mp->m_sb.sb_dblocks);

	/* Check the AG length */
	eoag = be32_to_cpu(agf->agf_length);
	xfs_scrub_block_check_ok(sc, sc->sa.agf_bp,
			eoag == xfs_scrub_ag_blocks(mp, agno));

	/* Check the AGF btree roots and levels */
	agbno = be32_to_cpu(agf->agf_roots[XFS_BTNUM_BNO]);
	daddr = XFS_AGB_TO_DADDR(mp, agno, agbno);
	xfs_scrub_block_check_ok(sc, sc->sa.agf_bp,
			agbno > XFS_AGI_BLOCK(mp) &&
			agbno < mp->m_sb.sb_agblocks &&
			agbno < eoag && daddr < eofs);

	agbno = be32_to_cpu(agf->agf_roots[XFS_BTNUM_CNT]);
	daddr = XFS_AGB_TO_DADDR(mp, agno, agbno);
	xfs_scrub_block_check_ok(sc, sc->sa.agf_bp,
			agbno > XFS_AGI_BLOCK(mp) &&
			agbno < mp->m_sb.sb_agblocks &&
			agbno < eoag && daddr < eofs);

	level = be32_to_cpu(agf->agf_levels[XFS_BTNUM_BNO]);
	xfs_scrub_block_check_ok(sc, sc->sa.agf_bp,
			level > 0 && level <= XFS_BTREE_MAXLEVELS);

	level = be32_to_cpu(agf->agf_levels[XFS_BTNUM_CNT]);
	xfs_scrub_block_check_ok(sc, sc->sa.agf_bp,
			level > 0 && level <= XFS_BTREE_MAXLEVELS);

	if (xfs_sb_version_hasrmapbt(&mp->m_sb)) {
		agbno = be32_to_cpu(agf->agf_roots[XFS_BTNUM_RMAP]);
		daddr = XFS_AGB_TO_DADDR(mp, agno, agbno);
		xfs_scrub_block_check_ok(sc, sc->sa.agf_bp,
				agbno > XFS_AGI_BLOCK(mp) &&
				agbno < mp->m_sb.sb_agblocks &&
				agbno < eoag && daddr < eofs);

		level = be32_to_cpu(agf->agf_levels[XFS_BTNUM_RMAP]);
		xfs_scrub_block_check_ok(sc, sc->sa.agf_bp,
				level > 0 && level <= XFS_BTREE_MAXLEVELS);
	}

	if (xfs_sb_version_hasreflink(&mp->m_sb)) {
		agbno = be32_to_cpu(agf->agf_refcount_root);
		daddr = XFS_AGB_TO_DADDR(mp, agno, agbno);
		xfs_scrub_block_check_ok(sc, sc->sa.agf_bp,
				agbno > XFS_AGI_BLOCK(mp) &&
				agbno < mp->m_sb.sb_agblocks &&
				agbno < eoag && daddr < eofs);

		level = be32_to_cpu(agf->agf_refcount_level);
		xfs_scrub_block_check_ok(sc, sc->sa.agf_bp,
				level > 0 && level <= XFS_BTREE_MAXLEVELS);
	}

	/* Check the AGFL counters */
	agfl_first = be32_to_cpu(agf->agf_flfirst);
	agfl_last = be32_to_cpu(agf->agf_fllast);
	agfl_count = be32_to_cpu(agf->agf_flcount);
	if (agfl_last > agfl_first)
		fl_count = agfl_last - agfl_first + 1;
	else
		fl_count = XFS_AGFL_SIZE(mp) - agfl_first + agfl_last + 1;
	xfs_scrub_block_check_ok(sc, sc->sa.agf_bp,
			agfl_count == 0 || fl_count == agfl_count);

out:
	return error;
}

/* AGFL */

struct xfs_scrub_agfl {
	xfs_agblock_t			eoag;
	xfs_daddr_t			eofs;
};

/* Scrub an AGFL block. */
STATIC int
xfs_scrub_agfl_block(
	struct xfs_scrub_context	*sc,
	xfs_agblock_t			agbno,
	void				*priv)
{
	struct xfs_mount		*mp = sc->mp;
	xfs_agnumber_t			agno = sc->sa.agno;
	struct xfs_scrub_agfl		*sagfl = priv;
	int				error = 0;

	xfs_scrub_block_check_ok(sc, sc->sa.agfl_bp,
			agbno > XFS_AGI_BLOCK(mp) &&
			agbno < mp->m_sb.sb_agblocks &&
			agbno < sagfl->eoag &&
			XFS_AGB_TO_DADDR(mp, agno, agbno) < sagfl->eofs);

	return error;
}

/* Scrub the AGFL. */
int
xfs_scrub_agfl(
	struct xfs_scrub_context	*sc)
{
	struct xfs_scrub_agfl		sagfl;
	struct xfs_mount		*mp = sc->mp;
	struct xfs_agf			*agf;
	xfs_agnumber_t			agno;
	int				error;

	agno = sc->sm->sm_agno;
	error = xfs_scrub_load_ag_headers(sc, agno, XFS_SCRUB_TYPE_AGFL);
	if (!xfs_scrub_op_ok(sc, agno, XFS_AGFL_BLOCK(sc->mp), &error))
		goto out;
	if (!sc->sa.agf_bp)
		return -EFSCORRUPTED;

	agf = XFS_BUF_TO_AGF(sc->sa.agf_bp);
	sagfl.eofs = XFS_FSB_TO_BB(mp, mp->m_sb.sb_dblocks);
	sagfl.eoag = be32_to_cpu(agf->agf_length);

	/* Check the blocks in the AGFL. */
	return xfs_scrub_walk_agfl(sc, xfs_scrub_agfl_block, &sagfl);
out:
	return error;
}
