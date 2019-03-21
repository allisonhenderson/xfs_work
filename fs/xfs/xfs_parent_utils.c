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
#include "xfs_log_format.h"
#include "xfs_shared.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"
#include "xfs_bmap_btree.h"
#include "xfs_inode.h"
#include "xfs_error.h"
#include "xfs_trace.h"
#include "xfs_trans.h"
#include "xfs_da_format.h"
#include "xfs_da_btree.h"
#include "xfs_attr.h"
#include "xfs_parent.h"
#include "xfs_da_btree.h"

/*
 * Add a parent record to an inode with existing parent records.
 */
int
xfs_parent_add_deferred(
	struct xfs_inode        *parent,
	struct xfs_trans	*tp,
	struct xfs_inode        *child,
	struct xfs_name         *child_name,
	uint32_t                diroffset)
{
	struct xfs_parent_name_rec rec;

	xfs_init_parent_name_rec(&rec, parent, diroffset);

	return xfs_attr_set_deferred(child, tp, (const char *)&rec, sizeof(rec),
		child_name->name, child_name->len, ATTR_PARENT);
}

/*
 * Remove a parent record from a child inode.
 */
int
xfs_parent_remove_deferred(
	struct xfs_inode	*parent,
	struct xfs_trans	*tp,
	struct xfs_inode	*child,
	xfs_dir2_dataptr_t	diroffset)
{
	struct xfs_parent_name_rec rec;

	xfs_init_parent_name_rec(&rec, parent, diroffset);

	return xfs_attr_remove_deferred(child, tp, (const char *)&rec,
		sizeof(rec), ATTR_PARENT);
}

/*
 * Get the parent pointers for a given inode
 *
 * Returns 0 on success and non zero on error
 */
int
xfs_attr_get_parent_pointer(struct xfs_inode		*ip,
			    struct xfs_pptr_info	*ppi)

{

	struct attrlist			*alist;
	struct attrlist_ent		*aent;
	struct xfs_parent_ptr		*xpp;
	struct xfs_parent_name_rec	*xpnr;
	char				*namebuf;
	unsigned int			namebuf_size;
	int				name_len;
	int				error = 0;
	unsigned int			flags = ATTR_PARENT;
	int				i;
	struct xfs_attr_list_context	context;
	struct xfs_da_args		args;

	/* Allocate a buffer to store the attribute names */
	namebuf_size = sizeof(struct attrlist) +
		       (ppi->pi_ptrs_size) * sizeof(struct attrlist_ent);
	namebuf = kmem_zalloc_large(namebuf_size, KM_SLEEP);
	if (!namebuf)
		return -ENOMEM;

	error = xfs_attr_list_context_init(ip, namebuf, namebuf_size, flags,
			(attrlist_cursor_kern_t *)&ppi->pi_cursor, &context);
	if (error)
		goto out_kfree;

	xfs_ilock(ip, XFS_ILOCK_EXCL);

	error = xfs_attr_list_int_ilocked(&context);
	if (error)
		goto out_kfree;

	alist = (struct attrlist *)namebuf;
	for (i = 0; i < alist->al_count; i++) {
		xpp = XFS_PPINFO_TO_PP(ppi, i);
		memset(xpp, 0, sizeof(struct xfs_parent_ptr));
		aent = (struct attrlist_ent *) &namebuf[alist->al_offset[i]];
		xpnr = (struct xfs_parent_name_rec *)(aent->a_name);

		if (aent->a_valuelen > XFS_PPTR_MAXNAMELEN) {
			error = -ERANGE;
			goto out_kfree;
		}
		name_len = aent->a_valuelen;

		error = xfs_attr_args_init(&args, ip, (char *)xpnr,
				sizeof(struct xfs_parent_name_rec), flags);
		if (error)
			goto out_kfree;

		args.value = (unsigned char *)(xpp->xpp_name);
		args.valuelen = name_len;
		args.op_flags = XFS_DA_OP_OKNOENT;

		error = xfs_attr_get_ilocked(ip, &args);
		error = (error == -EEXIST ? 0 : error);
		if (error)
			goto out_kfree;

		xpp->xpp_namelen = name_len;
		xfs_init_parent_ptr(xpp, xpnr);
	}
	ppi->pi_ptrs_used = alist->al_count;
	if (!alist->al_more)
		ppi->pi_flags |= XFS_PPTR_OFLAG_DONE;

out_kfree:
	xfs_iunlock(ip, XFS_ILOCK_EXCL);
	kmem_free(namebuf);

	return error;
}

