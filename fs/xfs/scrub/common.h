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

/* Should we end the scrub early? */
static inline bool
xfs_scrub_should_terminate(
	int		*error)
{
	if (fatal_signal_pending(current)) {
		if (*error == 0)
			*error = -EAGAIN;
		return true;
	}
	return false;
}

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

/* Check for operational errors for a block check. */
bool xfs_scrub_op_ok(struct xfs_scrub_context *sc, xfs_agnumber_t agno,
		     xfs_agblock_t bno, int *error);

/* Check for operational errors for a file offset check. */
bool xfs_scrub_fblock_op_ok(struct xfs_scrub_context *sc, int whichfork,
			  xfs_fileoff_t offset, int *error);

/* Check for metadata block optimization possibilities. */
bool xfs_scrub_block_preen_ok(struct xfs_scrub_context *sc, struct xfs_buf *bp,
			      bool fs_ok);

/* Check for inode metadata optimization possibilities. */
bool xfs_scrub_ino_preen_ok(struct xfs_scrub_context *sc, struct xfs_buf *bp,
			    bool fs_ok);

/* Check for metadata block corruption. */
bool xfs_scrub_block_check_ok(struct xfs_scrub_context *sc, struct xfs_buf *bp,
			      bool fs_ok);

/* Check for inode metadata corruption. */
bool xfs_scrub_ino_check_ok(struct xfs_scrub_context *sc, xfs_ino_t ino,
			    struct xfs_buf *bp, bool fs_ok);

/* Check for file fork block corruption. */
bool xfs_scrub_fblock_check_ok(struct xfs_scrub_context *sc, int whichfork,
			       xfs_fileoff_t offset, bool fs_ok);

/* Check for inode metadata non-corruption weirdness problems. */
bool xfs_scrub_ino_warn_ok(struct xfs_scrub_context *sc, struct xfs_buf *bp,
			   bool fs_ok);

/* Check for file data block non-corruption weirdness problems. */
bool xfs_scrub_fblock_warn_ok(struct xfs_scrub_context *sc, int whichfork,
			      xfs_fileoff_t offset, bool fs_ok);

/* Signal an incomplete scrub. */
bool xfs_scrub_check_thoroughness(struct xfs_scrub_context *sc, bool fs_ok);

/* Setup functions */
int xfs_scrub_setup_fs(struct xfs_scrub_context *sc, struct xfs_inode *ip);
int xfs_scrub_setup_ag_header(struct xfs_scrub_context *sc,
			      struct xfs_inode *ip);
int xfs_scrub_setup_ag_allocbt(struct xfs_scrub_context *sc,
			       struct xfs_inode *ip);
int xfs_scrub_setup_ag_iallocbt(struct xfs_scrub_context *sc,
				struct xfs_inode *ip);
int xfs_scrub_setup_ag_rmapbt(struct xfs_scrub_context *sc,
			      struct xfs_inode *ip);
int xfs_scrub_setup_ag_refcountbt(struct xfs_scrub_context *sc,
				  struct xfs_inode *ip);
int xfs_scrub_setup_inode(struct xfs_scrub_context *sc,
			  struct xfs_inode *ip);
int xfs_scrub_setup_inode_bmap(struct xfs_scrub_context *sc,
			       struct xfs_inode *ip);
int xfs_scrub_setup_inode_bmap_data(struct xfs_scrub_context *sc,
				    struct xfs_inode *ip);
int xfs_scrub_setup_directory(struct xfs_scrub_context *sc,
			      struct xfs_inode *ip);
int xfs_scrub_setup_xattr(struct xfs_scrub_context *sc,
			  struct xfs_inode *ip);

void xfs_scrub_ag_free(struct xfs_scrub_context *sc, struct xfs_scrub_ag *sa);
int xfs_scrub_ag_init(struct xfs_scrub_context *sc, xfs_agnumber_t agno,
		      struct xfs_scrub_ag *sa);
int xfs_scrub_ag_read_headers(struct xfs_scrub_context *sc, xfs_agnumber_t agno,
			      struct xfs_buf **agi, struct xfs_buf **agf,
			      struct xfs_buf **agfl);
void xfs_scrub_ag_btcur_free(struct xfs_scrub_ag *sa);
int xfs_scrub_ag_btcur_init(struct xfs_scrub_context *sc,
			    struct xfs_scrub_ag *sa);
int xfs_scrub_load_ag_headers(struct xfs_scrub_context *sc, xfs_agnumber_t agno,
			      unsigned int type);
int xfs_scrub_walk_agfl(struct xfs_scrub_context *sc,
			int (*fn)(struct xfs_scrub_context *, xfs_agblock_t bno,
				  void *),
			void *priv);

int xfs_scrub_setup_ag_btree(struct xfs_scrub_context *sc,
			     struct xfs_inode *ip, bool force_log);
int xfs_scrub_get_inode(struct xfs_scrub_context *sc, struct xfs_inode *ip_in);
int xfs_scrub_setup_inode_contents(struct xfs_scrub_context *sc,
				   struct xfs_inode *ip, unsigned int resblks);

#endif	/* __XFS_SCRUB_COMMON_H__ */
