/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2000,2002-2003,2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */
#ifndef __XFS_ATTR_H__
#define	__XFS_ATTR_H__

struct xfs_inode;
struct xfs_da_args;
struct xfs_attr_list_context;

/*
 * Large attribute lists are structured around Btrees where all the data
 * elements are in the leaf nodes.  Attribute names are hashed into an int,
 * then that int is used as the index into the Btree.  Since the hashval
 * of an attribute name may not be unique, we may have duplicate keys.
 * The internal links in the Btree are logical block offsets into the file.
 *
 * Small attribute lists use a different format and are packed as tightly
 * as possible so as to fit into the literal area of the inode.
 */

/*
 * The maximum size (into the kernel or returned from the kernel) of an
 * attribute value or the buffer used for an attr_list() call.  Larger
 * sizes will result in an ERANGE return code.
 */
#define	ATTR_MAX_VALUELEN	(64*1024)	/* max length of a value */

/*
 * Kernel-internal version of the attrlist cursor.
 */
struct xfs_attrlist_cursor_kern {
	__u32	hashval;	/* hash value of next entry to add */
	__u32	blkno;		/* block containing entry (suggestion) */
	__u32	offset;		/* offset in list of equal-hashvals */
	__u16	pad1;		/* padding to match user-level */
	__u8	pad2;		/* padding to match user-level */
	__u8	initted;	/* T/F: cursor has been initialized */
};


/*========================================================================
 * Structure used to pass context around among the routines.
 *========================================================================*/


/* void; state communicated via *context */
typedef void (*put_listent_func_t)(struct xfs_attr_list_context *, int,
			      unsigned char *, int, int);

struct xfs_attr_list_context {
	struct xfs_trans	*tp;
	struct xfs_inode	*dp;		/* inode */
	struct xfs_attrlist_cursor_kern cursor;	/* position in list */
	void			*buffer;	/* output buffer */

	/*
	 * Abort attribute list iteration if non-zero.  Can be used to pass
	 * error values to the xfs_attr_list caller.
	 */
	int			seen_enough;
	bool			allow_incomplete;

	ssize_t			count;		/* num used entries */
	int			dupcnt;		/* count dup hashvals seen */
	int			bufsize;	/* total buffer size */
	int			firstu;		/* first used byte in buffer */
	unsigned int		attr_filter;	/* XFS_ATTR_{ROOT,SECURE} */
	int			resynch;	/* T/F: resynch with cursor */
	put_listent_func_t	put_listent;	/* list output fmt function */
	int			index;		/* index into output buffer */
};


/*
 * ========================================================================
 * Structure used to pass context around among the delayed routines.
 * ========================================================================
 */

/*
 * Below is a state machine diagram for attr remove operations. The  XFS_DAS_*
 * states indicate places where the function would return -EAGAIN, and then
 * immediately resume from after being recalled by the calling function. States
 * marked as a "subroutine state" indicate that they belong to a subroutine, and
 * so the calling function needs to pass them back to that subroutine to allow
 * it to finish where it left off. But they otherwise do not have a role in the
 * calling function other than just passing through.
 *
 * xfs_attr_remove_iter()
 *	  XFS_DAS_RM_SHRINK ─┐
 *	  (subroutine state) │
 *	                     └─>xfs_attr_node_removename()
 *	                                      │
 *	                                      v
 *	                                   need to
 *	                                shrink tree? ─n─┐
 *	                                      │         │
 *	                                      y         │
 *	                                      │         │
 *	                                      v         │
 *	                              XFS_DAS_RM_SHRINK │
 *	                                      │         │
 *	                                      v         │
 *	                                     done <─────┘
 *
 *
 * Below is a state machine diagram for attr set operations.
 *
 *  xfs_attr_set_iter()
 *             │
 *             v
 *   ┌───n── fork has
 *   │	    only 1 blk?
 *   │		│
 *   │		y
 *   │		│
 *   │		v
 *   │	xfs_attr_leaf_try_add()
 *   │		│
 *   │		v
 *   │	     had enough
 *   ├───n────space?
 *   │		│
 *   │		y
 *   │		│
 *   │		v
 *   │	XFS_DAS_FOUND_LBLK ──┐
 *   │	                     │
 *   │	XFS_DAS_FLIP_LFLAG ──┤
 *   │	(subroutine state)   │
 *   │		             │
 *   │		             └─>xfs_attr_leaf_addname()
 *   │		                      │
 *   │		                      v
 *   │		                   was this
 *   │		                   a rename? ──n─┐
 *   │		                      │          │
 *   │		                      y          │
 *   │		                      │          │
 *   │		                      v          │
 *   │		                flip incomplete  │
 *   │		                    flag         │
 *   │		                      │          │
 *   │		                      v          │
 *   │		              XFS_DAS_FLIP_LFLAG │
 *   │		                      │          │
 *   │		                      v          │
 *   │		                    remove       │
 *   │		XFS_DAS_RM_LBLK ─> old name      │
 *   │		         ^            │          │
 *   │		         │            v          │
 *   │		         └──────y── more to      │
 *   │		                    remove       │
 *   │		                      │          │
 *   │		                      n          │
 *   │		                      │          │
 *   │		                      v          │
 *   │		                     done <──────┘
 *   └──> XFS_DAS_FOUND_NBLK ──┐
 *	  (subroutine state)   │
 *	                       │
 *	  XFS_DAS_ALLOC_NODE ──┤
 *	  (subroutine state)   │
 *	                       │
 *	  XFS_DAS_FLIP_NFLAG ──┤
 *	  (subroutine state)   │
 *	                       │
 *	                       └─>xfs_attr_node_addname()
 *	                               │
 *	                               v
 *	                       find space to store
 *	                      attr. Split if needed
 *	                               │
 *	                               v
 *	                       XFS_DAS_FOUND_NBLK
 *	                               │
 *	                               v
 *	                 ┌─────n──  need to
 *	                 │        alloc blks?
 *	                 │             │
 *	                 │             y
 *	                 │             │
 *	                 │             v
 *	                 │  ┌─>XFS_DAS_ALLOC_NODE
 *	                 │  │          │
 *	                 │  │          v
 *	                 │  └──y── need to alloc
 *	                 │         more blocks?
 *	                 │             │
 *	                 │             n
 *	                 │             │
 *	                 │             v
 *	                 │          was this
 *	                 └────────> a rename? ──n─┐
 *	                               │          │
 *	                               y          │
 *	                               │          │
 *	                               v          │
 *	                         flip incomplete  │
 *	                             flag         │
 *	                               │          │
 *	                               v          │
 *	                       XFS_DAS_FLIP_NFLAG │
 *	                               │          │
 *	                               v          │
 *	                             remove       │
 *	         XFS_DAS_RM_NBLK ─> old name      │
 *	                  ^            │          │
 *	                  │            v          │
 *	                  └──────y── more to      │
 *	                             remove       │
 *	                               │          │
 *	                               n          │
 *	                               │          │
 *	                               v          │
 *	                              done <──────┘
 *
 */

/*
 * Enum values for xfs_delattr_context.da_state
 *
 * These values are used by delayed attribute operations to keep track  of where
 * they were before they returned -EAGAIN.  A return code of -EAGAIN signals the
 * calling function to roll the transaction, and then recall the subroutine to
 * finish the operation.  The enum is then used by the subroutine to jump back
 * to where it was and resume executing where it left off.
 */
enum xfs_delattr_state {
				      /* Zero is uninitalized */
	XFS_DAS_INIT		= 1,  /* Context has been initialized */
	XFS_DAS_RM_SHRINK,	      /* We are shrinking the tree */
	XFS_DAS_FOUND_LBLK,	      /* We found leaf blk for attr */
	XFS_DAS_FOUND_NBLK,	      /* We found node blk for attr */
	XFS_DAS_FLIP_LFLAG,	      /* Flipped leaf INCOMPLETE attr flag */
	XFS_DAS_RM_LBLK,	      /* A rename is removing leaf blocks */
	XFS_DAS_ALLOC_NODE,	      /* We are allocating node blocks */
	XFS_DAS_FLIP_NFLAG,	      /* Flipped node INCOMPLETE attr flag */
	XFS_DAS_RM_NBLK,	      /* A rename is removing node blocks */
};

/*
 * Defines for xfs_delattr_context.flags
 */
#define XFS_DAC_DEFER_FINISH		0x01 /* finish the transaction */
#define XFS_DAC_NODE_RMVNAME_INIT	0x02 /* xfs_attr_node_removename init */
#define XFS_DAC_LEAF_ADDNAME_INIT	0x04 /* xfs_attr_leaf_addname init*/

/*
 * Context used for keeping track of delayed attribute operations
 */
struct xfs_delattr_context {
	struct xfs_da_args      *da_args;

	/* Used by delayed attributes to hold leaf across transactions */
	struct xfs_buf		*leaf_bp;

	/* Used in xfs_attr_rmtval_set_blk to roll through allocating blocks */
	struct xfs_bmbt_irec	map;
	xfs_dablk_t		lblkno;
	int			blkcnt;

	/* Used in xfs_attr_node_removename to roll through removing blocks */
	struct xfs_da_state     *da_state;
	struct xfs_da_state_blk *blk;

	/* Used to keep track of current state of delayed operation */
	unsigned int            flags;
	enum xfs_delattr_state  dela_state;
};

/*
 * List of attrs to commit later.
 */
struct xfs_attr_item {
	struct xfs_inode	*xattri_ip;
	void			*xattri_value;		/* attr value */
	void			*xattri_name;		/* attr name */
	uint32_t		xattri_op_flags;	/* attr op set or rm */
	uint32_t		xattri_value_len;	/* length of value */
	uint32_t		xattri_name_len;	/* length of name */
	uint32_t		xattri_flags;		/* attr flags */

	/* used to log this item to an intent */
	struct list_head	xattri_list;

	/*
	 * xfs_delattr_context and xfs_da_args need to remain instantiated
	 * across transaction rolls during the defer finish, so store them here
	 */
	struct xfs_da_args		xattri_args;
	struct xfs_delattr_context	xattri_dac;

	/*
	 * A byte array follows the header containing the file name and
	 * attribute value.
	 */
};

#define XFS_ATTR_ITEM_SIZEOF(namelen, valuelen)	\
	(sizeof(struct xfs_attr_item) + (namelen) + (valuelen))


/*========================================================================
 * Function prototypes for the kernel.
 *========================================================================*/

/*
 * Overall external interface routines.
 */
int xfs_attr_inactive(struct xfs_inode *dp);
int xfs_attr_list_ilocked(struct xfs_attr_list_context *);
int xfs_attr_list(struct xfs_attr_list_context *);
int xfs_inode_hasattr(struct xfs_inode *ip);
int xfs_attr_get_ilocked(struct xfs_da_args *args);
int xfs_attr_get(struct xfs_da_args *args);
int xfs_attr_set(struct xfs_da_args *args);
int xfs_attr_set_args(struct xfs_da_args *args);
int xfs_attr_set_iter(struct xfs_delattr_context *dac,
		      struct xfs_buf **leaf_bp);
int xfs_has_attr(struct xfs_da_args *args);
int xfs_attr_remove_args(struct xfs_da_args *args);
int xfs_attr_remove_iter(struct xfs_delattr_context *dac);
bool xfs_attr_namecheck(const void *name, size_t length);
void xfs_delattr_context_init(struct xfs_delattr_context *dac,
			      struct xfs_da_args *args);
int xfs_attr_calc_size(struct xfs_da_args *args, int *local);
int xfs_attr_set_deferred(struct xfs_inode *dp, struct xfs_trans *tp,
			  const unsigned char *name, unsigned int namelen,
			  unsigned int flags, const unsigned char *value,
			  unsigned int valuelen);
int xfs_attr_remove_deferred(struct xfs_inode *dp, struct xfs_trans *tp,
			    const unsigned char *name, unsigned int namelen,
			    unsigned int flags);

#endif	/* __XFS_ATTR_H__ */
