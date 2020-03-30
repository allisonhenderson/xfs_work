/*
 * Copyright (c) 2017 Oracle, Inc.
 * All Rights Reserved.
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
 * along with this program; if not, write the Free Software Foundation Inc.
 */
#ifndef	__XFS_PARENT_UTILS_H__
#define	__XFS_PARENT_UTILS_H__

/*
 * Parent pointer attribute prototypes
 */
int xfs_parent_add_deferred(struct xfs_inode *parent, struct xfs_trans *tp,
	       struct xfs_inode *child, const unsigned char *child_name,
	       unsigned int child_namelen, uint32_t diroffset);
#endif	/* __XFS_PARENT_UTILS_H__ */
