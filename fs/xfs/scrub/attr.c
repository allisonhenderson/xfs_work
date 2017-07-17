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
#include "xfs_da_format.h"
#include "xfs_da_btree.h"
#include "xfs_dir2.h"
#include "xfs_attr.h"
#include "xfs_attr_leaf.h"
#include "scrub/xfs_scrub.h"
#include "scrub/scrub.h"
#include "scrub/common.h"
#include "scrub/dabtree.h"
#include "scrub/trace.h"

#include <linux/posix_acl_xattr.h>
#include <linux/xattr.h>

/* Set us up to scrub an inode's extended attributes. */
int
xfs_scrub_setup_xattr(
	struct xfs_scrub_context	*sc,
	struct xfs_inode		*ip)
{
	/* Allocate the buffer without the inode lock held. */
	sc->buf = kmem_zalloc_large(XATTR_SIZE_MAX, KM_SLEEP);
	if (!sc->buf)
		return -ENOMEM;

	return xfs_scrub_setup_inode_contents(sc, ip, 0);
}

/* Extended Attributes */

struct xfs_scrub_xattr {
	struct xfs_attr_list_context	context;
	struct xfs_scrub_context	*sc;
};

/* Check that an extended attribute key can be looked up by hash. */
static void
xfs_scrub_xattr_listent(
	struct xfs_attr_list_context	*context,
	int				flags,
	unsigned char			*name,
	int				namelen,
	int				valuelen)
{
	struct xfs_scrub_xattr		*sx;
	struct xfs_da_args		args = {0};
	int				error = 0;

	sx = container_of(context, struct xfs_scrub_xattr, context);

	args.flags = ATTR_KERNOTIME;
	if (flags & XFS_ATTR_ROOT)
		args.flags |= ATTR_ROOT;
	else if (flags & XFS_ATTR_SECURE)
		args.flags |= ATTR_SECURE;
	args.geo = context->dp->i_mount->m_attr_geo;
	args.whichfork = XFS_ATTR_FORK;
	args.dp = context->dp;
	args.name = name;
	args.namelen = namelen;
	args.hashval = xfs_da_hashname(args.name, args.namelen);
	args.trans = context->tp;
	args.value = sx->sc->buf;
	args.valuelen = XATTR_SIZE_MAX;

	error = xfs_attr_get_ilocked(context->dp, &args);
	if (error == -EEXIST)
		error = 0;
	if (!xfs_scrub_fblock_op_ok(sx->sc, XFS_ATTR_FORK, args.blkno, &error))
		goto fail_xref;
	xfs_scrub_fblock_check_ok(sx->sc, XFS_ATTR_FORK, args.blkno,
			args.valuelen == valuelen);

fail_xref:
	return;
}

/* Scrub a attribute btree record. */
STATIC int
xfs_scrub_xattr_rec(
	struct xfs_scrub_da_btree	*ds,
	int				level,
	void				*rec)
{
	struct xfs_mount		*mp = ds->state->mp;
	struct xfs_attr_leaf_entry	*ent = rec;
	struct xfs_da_state_blk		*blk;
	struct xfs_attr_leaf_name_local	*lentry;
	struct xfs_attr_leaf_name_remote	*rentry;
	struct xfs_buf			*bp;
	xfs_dahash_t			calc_hash;
	xfs_dahash_t			hash;
	int				nameidx;
	int				hdrsize;
	unsigned int			badflags;
	int				error;

	blk = &ds->state->path.blk[level];

	/* Check the hash of the entry. */
	error = xfs_scrub_da_btree_hash(ds, level, &ent->hashval);
	if (error)
		goto out;

	/* Find the attr entry's location. */
	bp = blk->bp;
	hdrsize = xfs_attr3_leaf_hdr_size(bp->b_addr);
	nameidx = be16_to_cpu(ent->nameidx);
	if (!xfs_scrub_da_check_ok(ds, level, nameidx >= hdrsize &&
			nameidx < mp->m_attr_geo->blksize))
		goto out;

	/* Retrieve the entry and check it. */
	hash = be32_to_cpu(ent->hashval);
	badflags = ~(XFS_ATTR_LOCAL | XFS_ATTR_ROOT | XFS_ATTR_SECURE |
			XFS_ATTR_INCOMPLETE);
	xfs_scrub_da_check_ok(ds, level, (ent->flags & badflags) == 0);
	if (ent->flags & XFS_ATTR_LOCAL) {
		lentry = (struct xfs_attr_leaf_name_local *)
				(((char *)bp->b_addr) + nameidx);
		if (!xfs_scrub_da_check_ok(ds, level,
				lentry->namelen < MAXNAMELEN))
			goto out;
		calc_hash = xfs_da_hashname(lentry->nameval, lentry->namelen);
	} else {
		rentry = (struct xfs_attr_leaf_name_remote *)
				(((char *)bp->b_addr) + nameidx);
		if (!xfs_scrub_da_check_ok(ds, level,
				rentry->namelen < MAXNAMELEN))
			goto out;
		calc_hash = xfs_da_hashname(rentry->name, rentry->namelen);
	}
	xfs_scrub_da_check_ok(ds, level, calc_hash == hash);

out:
	return error;
}

/* Scrub the extended attribute metadata. */
int
xfs_scrub_xattr(
	struct xfs_scrub_context	*sc)
{
	struct xfs_scrub_xattr		sx = { 0 };
	struct attrlist_cursor_kern	cursor = { 0 };
	int				error = 0;

	if (!xfs_inode_hasattr(sc->ip))
		return -ENOENT;

	memset(&sx, 0, sizeof(sx));
	/* Check attribute tree structure */
	error = xfs_scrub_da_btree(sc, XFS_ATTR_FORK, xfs_scrub_xattr_rec);
	if (error)
		goto out;

	/* Check that every attr key can also be looked up by hash. */
	sx.context.dp = sc->ip;
	sx.context.cursor = &cursor;
	sx.context.resynch = 1;
	sx.context.put_listent = xfs_scrub_xattr_listent;
	sx.context.tp = sc->tp;
	sx.sc = sc;

	/*
	 * Look up every xattr in this file by name.
	 *
	 * The VFS only locks i_rwsem when modifying attrs, so keep all
	 * three locks held because that's the only way to ensure we're
	 * the only thread poking into the da btree.  We traverse the da
	 * btree while holding a leaf buffer locked for the xattr name
	 * iteration, which doesn't really follow the usual buffer
	 * locking order.
	 */
	error = xfs_attr_list_int_ilocked(&sx.context);
	if (!xfs_scrub_fblock_op_ok(sc, XFS_ATTR_FORK, 0, &error))
		goto out;
out:
	return error;
}
