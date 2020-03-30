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

/*
 * Add a parent record to an inode with existing parent records.
 */
int
xfs_parent_add_deferred(
	struct xfs_inode		*parent,
	struct xfs_trans		*tp,
	struct xfs_inode		*child,
	const unsigned char		*child_name,
	unsigned int			child_namelen,
	uint32_t			diroffset)
{
	struct xfs_parent_name_rec	rec;

	xfs_init_parent_name_rec(&rec, parent, diroffset);

	return xfs_attr_set_deferred(child, tp, (const char *)&rec, sizeof(rec),
				     XFS_ATTR_PARENT, child_name,
				     child_namelen);
}

