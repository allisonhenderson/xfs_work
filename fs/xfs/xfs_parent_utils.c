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
#include "xfs_ioctl.h"
#include "xfs_parent.h"
#include "xfs_da_btree.h"

/*
 * Get the parent pointers for a given inode
 *
 * Returns 0 on success and non zero on error
 */
int
xfs_attr_get_parent_pointer(struct xfs_inode		*ip,
			    struct xfs_pptr_info	*ppi)

{

	struct xfs_attrlist		*alist;
	struct xfs_attrlist_ent		*aent;
	struct xfs_parent_ptr		*xpp;
	struct xfs_parent_name_rec	*xpnr;
	char				*namebuf;
	unsigned int			namebuf_size;
	int				name_len;
	int				error = 0;
	unsigned int			ioc_flags = XFS_IOC_ATTR_PARENT;
	unsigned int			flags = XFS_ATTR_PARENT;
	int				i;
	struct xfs_attr_list_context	context;
	struct xfs_da_args		args;

	/* Allocate a buffer to store the attribute names */
	namebuf_size = sizeof(struct xfs_attrlist) +
		       (ppi->pi_ptrs_size) * sizeof(struct xfs_attrlist_ent);
	namebuf = kvzalloc(namebuf_size, GFP_KERNEL);
	if (!namebuf)
		return -ENOMEM;

	memset(&context, 0, sizeof(struct xfs_attr_list_context));
	error = xfs_ioc_attr_list_context_init(ip, namebuf, namebuf_size,
			ioc_flags, &context);

	/* Copy the cursor provided by caller */
	memcpy(&context.cursor, &ppi->pi_cursor,
	       sizeof(struct xfs_attrlist_cursor));

	if (error)
		goto out_kfree;

	xfs_ilock(ip, XFS_ILOCK_EXCL);

	error = xfs_attr_list_ilocked(&context);
	if (error)
		goto out_kfree;

	alist = (struct xfs_attrlist *)namebuf;
	for (i = 0; i < alist->al_count; i++) {
		xpp = XFS_PPINFO_TO_PP(ppi, i);
		memset(xpp, 0, sizeof(struct xfs_parent_ptr));
		aent = (struct xfs_attrlist_ent *)
			&namebuf[alist->al_offset[i]];
		xpnr = (struct xfs_parent_name_rec *)(aent->a_name);

		if (aent->a_valuelen > XFS_PPTR_MAXNAMELEN) {
			error = -ERANGE;
			goto out_kfree;
		}
		name_len = aent->a_valuelen;

		memset(&args, 0, sizeof(args));
		args.geo = ip->i_mount->m_attr_geo;
		args.whichfork = XFS_ATTR_FORK;
		args.dp = ip;
		args.name = (char *)xpnr;
		args.namelen = sizeof(struct xfs_parent_name_rec);
		args.attr_filter = flags;
		args.hashval = xfs_da_hashname(args.name, args.namelen);
		args.value = (unsigned char *)(xpp->xpp_name);
		args.valuelen = name_len;
		args.op_flags = XFS_DA_OP_OKNOENT;

		error = xfs_attr_get_ilocked(&args);
		error = (error == -EEXIST ? 0 : error);
		if (error)
			goto out_kfree;

		xpp->xpp_namelen = name_len;
		xfs_init_parent_ptr(xpp, xpnr);
	}
	ppi->pi_ptrs_used = alist->al_count;
	if (!alist->al_more)
		ppi->pi_flags |= XFS_PPTR_OFLAG_DONE;

	/* Update the caller with the current cursor position */
	memcpy(&ppi->pi_cursor, &context.cursor,
		sizeof(struct xfs_attrlist_cursor));

out_kfree:
	xfs_iunlock(ip, XFS_ILOCK_EXCL);
	kmem_free(namebuf);

	return error;
}

