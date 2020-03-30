/*
 * Copyright (c) 2015 Red Hat, Inc.
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it would be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write the Free Software Foundation
 */
#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_format.h"
#include "xfs_da_format.h"
#include "xfs_log_format.h"
#include "xfs_shared.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"
#include "xfs_bmap_btree.h"
#include "xfs_inode.h"
#include "xfs_error.h"
#include "xfs_trace.h"
#include "xfs_trans.h"
#include "xfs_da_btree.h"
#include "xfs_attr.h"
#include "xfs_da_btree.h"
#include "xfs_attr_sf.h"
#include "xfs_bmap.h"

/*
 * Parent pointer attribute handling.
 *
 * Because the attribute value is a filename component, it will never be longer
 * than 255 bytes. This means the attribute will always be a local format
 * attribute as it is xfs_attr_leaf_entsize_local_max() for v5 filesystems will
 * always be larger than this (max is 75% of block size).
 *
 * Creating a new parent attribute will always create a new attribute - there
 * should never, ever be an existing attribute in the tree for a new inode.
 * ENOSPC behaviour is problematic - creating the inode without the parent
 * pointer is effectively a corruption, so we allow parent attribute creation
 * to dip into the reserve block pool to avoid unexpected ENOSPC errors from
 * occurring.
 */


/* Initializes a xfs_parent_name_rec to be stored as an attribute name */
void
xfs_init_parent_name_rec(
	struct xfs_parent_name_rec	*rec,
	struct xfs_inode		*ip,
	uint32_t			p_diroffset)
{
	xfs_ino_t			p_ino = ip->i_ino;
	uint32_t			p_gen = VFS_I(ip)->i_generation;

	rec->p_ino = cpu_to_be64(p_ino);
	rec->p_gen = cpu_to_be32(p_gen);
	rec->p_diroffset = cpu_to_be32(p_diroffset);
}

/* Initializes a xfs_parent_name_irec from an xfs_parent_name_rec */
void
xfs_init_parent_name_irec(
	struct xfs_parent_name_irec	*irec,
	struct xfs_parent_name_rec	*rec)
{
	irec->p_ino = be64_to_cpu(rec->p_ino);
	irec->p_gen = be32_to_cpu(rec->p_gen);
	irec->p_diroffset = be32_to_cpu(rec->p_diroffset);
}

/*
 * Directly add a parent pointer instead of as a deferred operation
 * Currently only used during protofile creation
 */
int
xfs_parent_add(
	struct xfs_inode		*parent,
	struct xfs_inode		*child,
	const unsigned char		*child_name,
	uint32_t			child_namelen,
	uint32_t			diroffset)
{
	struct xfs_parent_name_rec	rec;
	int				error, err2;
	int				local = 0;
	int				rsvd = 0;
	struct xfs_trans_res		tres;
	struct xfs_mount		*mp = child->i_mount;
	int				flags = XFS_ATTR_PARENT;

	struct xfs_da_args		args = {
		.geo = child->i_mount->m_attr_geo,
		.whichfork = XFS_ATTR_FORK,
		.dp = child,
		.name = (const unsigned char *)&rec,
		.namelen = sizeof(rec),
		.attr_filter = flags,
		.hashval = xfs_da_hashname((const unsigned char *)&rec,
					   sizeof(rec)),
		.value = (void *)child_name,
		.valuelen = child_namelen,
		.op_flags = XFS_DA_OP_OKNOENT | XFS_DA_OP_ADDNAME,
	};
	args.total = xfs_attr_calc_size(&args, &local);

	xfs_init_parent_name_rec(&rec, parent, diroffset);


	tres.tr_logres = M_RES(mp)->tr_attrsetm.tr_logres +
		M_RES(mp)->tr_attrsetrt.tr_logres * args.total;
	tres.tr_logcount = XFS_ATTRSET_LOG_COUNT;
	tres.tr_logflags = XFS_TRANS_PERM_LOG_RES;

	/*
	 * Root fork attributes can use reserved data blocks for this
	 * operation if necessary
	 */
	error = xfs_trans_alloc(mp, &tres, args.total, 0,
			rsvd ? XFS_TRANS_RESERVE : 0, &args.trans);
	if (error)
		goto out;

	/*
	 * If the inode doesn't have an attribute fork, add one.
	 * (inode must not be locked when we call this routine)
	 */
	if (XFS_IFORK_Q(child) == 0) {
		int sf_size = sizeof(xfs_attr_sf_hdr_t) +
			XFS_ATTR_SF_ENTSIZE_BYNAME(args.namelen,
			args.valuelen);

		error = xfs_bmap_add_attrfork(child, sf_size, rsvd);
		if (error)
			return error;
	}

	xfs_trans_ijoin(args.trans, child, 0);

	error = xfs_attr_set_args(&args);
	if (error && error != -EAGAIN)
		goto out;

	err2 = xfs_trans_commit(args.trans);
	if (err2) {
		error = err2;
		goto out;
	}

	return error;

out:
	if (args.trans)
		xfs_trans_cancel(args.trans);

	return error;
}


