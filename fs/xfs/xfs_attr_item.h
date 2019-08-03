// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2019 Oracle.  All Rights Reserved.
 * Author: Allison Henderson <allison.henderson@oracle.com>
 */
#ifndef	__XFS_ATTR_ITEM_H__
#define	__XFS_ATTR_ITEM_H__

/* kernel only ATTRI/ATTRD definitions */

struct xfs_mount;
struct kmem_zone;

/*
 * Max number of attrs in fast allocation path.
 */
#define XFS_ATTRI_MAX_FAST_ATTRS        1


/*
 * Define ATTR flag bits. Manipulated by set/clear/test_bit operators.
 */
#define	XFS_ATTRI_RECOVERED	1


/* iovec length must be 32-bit aligned */
#define ATTR_NVEC_SIZE(size) (size == sizeof(int32_t) ? sizeof(int32_t) : \
				size + sizeof(int32_t) - \
				(size % sizeof(int32_t)))

/*
 * This is the "attr intention" log item.  It is used to log the fact that some
 * need to be processed.  It is used in conjunction with the "attr done" log
 * item described below.
 *
 * The ATTRI is reference counted so that it is not freed prior to both the
 * ATTRI and ATTRD being committed and unpinned. This ensures the ATTRI is
 * inserted into the AIL even in the event of out of order ATTRI/ATTRD
 * processing. In other words, an ATTRI is born with two references:
 *
 *      1.) an ATTRI held reference to track ATTRI AIL insertion
 *      2.) an ATTRD held reference to track ATTRD commit
 *
 * On allocation, both references are the responsibility of the caller. Once the
 * ATTRI is added to and dirtied in a transaction, ownership of reference one
 * transfers to the transaction. The reference is dropped once the ATTRI is
 * inserted to the AIL or in the event of failure along the way (e.g., commit
 * failure, log I/O error, etc.). Note that the caller remains responsible for
 * the ATTRD reference under all circumstances to this point. The caller has no
 * means to detect failure once the transaction is committed, however.
 * Therefore, an ATTRD is required after this point, even in the event of
 * unrelated failure.
 *
 * Once an ATTRD is allocated and dirtied in a transaction, reference two
 * transfers to the transaction. The ATTRD reference is dropped once it reaches
 * the unpin handler. Similar to the ATTRI, the reference also drops in the
 * event of commit failure or log I/O errors. Note that the ATTRD is not
 * inserted in the AIL, so at this point both the ATTI and ATTRD are freed.
 */
struct xfs_attri_log_item {
	xfs_log_item_t			attri_item;
	atomic_t			attri_refcount;
	unsigned long			attri_flags;	/* misc flags */
	int				attri_name_len;
	void				*attri_name;
	int				attri_value_len;
	void				*attri_value;
	struct xfs_attri_log_format	attri_format;
};

/*
 * This is the "attr done" log item.  It is used to log the fact that some attrs
 * earlier mentioned in an attri item have been freed.
 */
struct xfs_attrd_log_item {
	struct xfs_log_item		attrd_item;
	struct xfs_attri_log_item	*attrd_attrip;
	struct xfs_attrd_log_format	attrd_format;
};

/*
 * Max number of attrs in fast allocation path.
 */
#define	XFS_ATTRD_MAX_FAST_ATTRS	1

extern struct kmem_zone	*xfs_attri_zone;
extern struct kmem_zone	*xfs_attrd_zone;

struct xfs_attri_log_item	*xfs_attri_init(struct xfs_mount *mp);
struct xfs_attrd_log_item	*xfs_attrd_init(struct xfs_mount *mp,
					struct xfs_attri_log_item *attrip);
int xfs_attri_copy_format(struct xfs_log_iovec *buf,
			   struct xfs_attri_log_format *dst_attri_fmt);
int xfs_attrd_copy_format(struct xfs_log_iovec *buf,
			   struct xfs_attrd_log_format *dst_attrd_fmt);
void			xfs_attri_item_free(struct xfs_attri_log_item *attrip);
void			xfs_attri_release(struct xfs_attri_log_item *attrip);

int			xfs_attri_recover(struct xfs_mount *mp,
					struct xfs_attri_log_item *attrip);

#endif	/* __XFS_ATTR_ITEM_H__ */
