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
#include "xfs_icache.h"
#include "xfs_itable.h"
#include "xfs_alloc.h"
#include "xfs_alloc_btree.h"
#include "xfs_bmap.h"
#include "xfs_bmap_btree.h"
#include "xfs_ialloc.h"
#include "xfs_ialloc_btree.h"
#include "xfs_refcount.h"
#include "xfs_refcount_btree.h"
#include "xfs_rmap.h"
#include "xfs_rmap_btree.h"
#include "scrub/xfs_scrub.h"
#include "scrub/scrub.h"
#include "scrub/common.h"
#include "scrub/trace.h"
#include "scrub/scrub.h"
#include "scrub/btree.h"

/*
 * Online Scrub and Repair
 *
 * Traditionally, XFS (the kernel driver) did not know how to check or
 * repair on-disk data structures.  That task was left to the xfs_check
 * and xfs_repair tools, both of which require taking the filesystem
 * offline for a thorough but time consuming examination.  Online
 * scrub & repair, on the other hand, enables us to check the metadata
 * for obvious errors while carefully stepping around the filesystem's
 * ongoing operations, locking rules, etc.
 *
 * Given that most XFS metadata consist of records stored in a btree,
 * most of the checking functions iterate the btree blocks themselves
 * looking for irregularities.  When a record block is encountered, each
 * record can be checked for obviously bad values.  Record values can
 * also be cross-referenced against other btrees to look for potential
 * misunderstandings between pieces of metadata.
 *
 * It is expected that the checkers responsible for per-AG metadata
 * structures will lock the AG headers (AGI, AGF, AGFL), iterate the
 * metadata structure, and perform any relevant cross-referencing before
 * unlocking the AG and returning the results to userspace.  These
 * scrubbers must not keep an AG locked for too long to avoid tying up
 * the block and inode allocators.
 *
 * Block maps and b-trees rooted in an inode present a special challenge
 * because they can involve extents from any AG.  The general scrubber
 * structure of lock -> check -> xref -> unlock still holds, but AG
 * locking order rules /must/ be obeyed to avoid deadlocks.  The
 * ordering rule, of course, is that we must lock in increasing AG
 * order.  Helper functions are provided to track which AG headers we've
 * already locked.  If we detect an imminent locking order violation, we
 * can signal a potential deadlock, in which case the scrubber can jump
 * out to the top level, lock all the AGs in order, and retry the scrub.
 *
 * For file data (directories, extended attributes, symlinks) scrub, we
 * can simply lock the inode and walk the data.  For btree data
 * (directories and attributes) we follow the same btree-scrubbing
 * strategy outlined previously to check the records.
 *
 * We use a bit of trickery with transactions to avoid buffer deadlocks
 * if there is a cycle in the metadata.  The basic problem is that
 * travelling down a btree involves locking the current buffer at each
 * tree level.  If a pointer should somehow point back to a buffer that
 * we've already examined, we will deadlock due to the second buffer
 * locking attempt.  Note however that grabbing a buffer in transaction
 * context links the locked buffer to the transaction.  If we try to
 * re-grab the buffer in the context of the same transaction, we avoid
 * the second lock attempt and continue.  Between the verifier and the
 * scrubber, something will notice that something is amiss and report
 * the corruption.  Therefore, each scrubber will allocate an empty
 * transaction, attach buffers to it, and cancel the transaction at the
 * end of the scrub run.  Cancelling a non-dirty transaction simply
 * unlocks the buffers.
 *
 * There are four pieces of data that scrub can communicate to
 * userspace.  The first is the error code (errno), which can be used to
 * communicate operational errors in performing the scrub.  There are
 * also three flags that can be set in the scrub context.  If the data
 * structure itself is corrupt, the CORRUPT flag will be set.  If
 * the metadata is correct but otherwise suboptimal, the PREEN flag
 * will be set.
 *
 * We perform secondary validation of filesystem metadata by
 * cross-referencing every record with all other available metadata.
 * For example, for block mapping extents, we verify that there are no
 * records in the free space and inode btrees corresponding to that
 * space extent and that there is a corresponding entry in the reverse
 * mapping btree.  Inconsistent metadata is noted by setting the
 * XCORRUPT flag; btree query function errors are noted by setting the
 * XFAIL flag and deleting the cursor to prevent further attempts to
 * cross-reference with a defective btree.
 */

/*
 * Test scrubber -- userspace uses this to probe if we're willing to
 * scrub or repair a given mountpoint.
 */
int
xfs_scrub_tester(
	struct xfs_scrub_context	*sc)
{
	if (sc->sm->sm_ino || sc->sm->sm_agno)
		return -EINVAL;
	if (sc->sm->sm_gen & XFS_SCRUB_OFLAG_CORRUPT)
		sc->sm->sm_flags |= XFS_SCRUB_OFLAG_CORRUPT;
	if (sc->sm->sm_gen & XFS_SCRUB_OFLAG_PREEN)
		sc->sm->sm_flags |= XFS_SCRUB_OFLAG_PREEN;
	if (sc->sm->sm_gen & XFS_SCRUB_OFLAG_XFAIL)
		sc->sm->sm_flags |= XFS_SCRUB_OFLAG_XFAIL;
	if (sc->sm->sm_gen & XFS_SCRUB_OFLAG_XCORRUPT)
		sc->sm->sm_flags |= XFS_SCRUB_OFLAG_XCORRUPT;
	if (sc->sm->sm_gen & XFS_SCRUB_OFLAG_INCOMPLETE)
		sc->sm->sm_flags |= XFS_SCRUB_OFLAG_INCOMPLETE;
	if (sc->sm->sm_gen & XFS_SCRUB_OFLAG_WARNING)
		sc->sm->sm_flags |= XFS_SCRUB_OFLAG_WARNING;
	if (sc->sm->sm_gen & ~XFS_SCRUB_FLAGS_OUT)
		return -ENOENT;

	return 0;
}

/* Scrub setup and teardown */

/* Free all the resources and finish the transactions. */
STATIC int
xfs_scrub_teardown(
	struct xfs_scrub_context	*sc,
	struct xfs_inode		*ip_in,
	int				error)
{
	xfs_scrub_ag_free(sc, &sc->sa);
	if (sc->tp) {
		xfs_trans_cancel(sc->tp);
		sc->tp = NULL;
	}
	if (sc->ip) {
		xfs_iunlock(sc->ip, sc->ilock_flags);
		if (sc->ip != ip_in)
			iput(VFS_I(sc->ip));
		sc->ip = NULL;
	}
	if (sc->buf) {
		kmem_free(sc->buf);
		sc->buf = NULL;
	}
	return error;
}

/* Scrubbing dispatch. */

static const struct xfs_scrub_meta_ops meta_scrub_ops[] = {
	{ /* ioctl presence test */
		.setup	= xfs_scrub_setup_fs,
		.scrub	= xfs_scrub_tester,
	},
	{ /* superblock */
		.setup	= xfs_scrub_setup_ag_header,
		.scrub	= xfs_scrub_superblock,
	},
	{ /* agf */
		.setup	= xfs_scrub_setup_ag_header,
		.scrub	= xfs_scrub_agf,
	},
	{ /* agfl */
		.setup	= xfs_scrub_setup_ag_header,
		.scrub	= xfs_scrub_agfl,
	},
	{ /* agi */
		.setup	= xfs_scrub_setup_ag_header,
		.scrub	= xfs_scrub_agi,
	},
	{ /* bnobt */
		.setup	= xfs_scrub_setup_ag_allocbt,
		.scrub	= xfs_scrub_bnobt,
	},
	{ /* cntbt */
		.setup	= xfs_scrub_setup_ag_allocbt,
		.scrub	= xfs_scrub_cntbt,
	},
	{ /* inobt */
		.setup	= xfs_scrub_setup_ag_iallocbt,
		.scrub	= xfs_scrub_inobt,
	},
	{ /* finobt */
		.setup	= xfs_scrub_setup_ag_iallocbt,
		.scrub	= xfs_scrub_finobt,
		.has	= xfs_sb_version_hasfinobt,
	},
	{ /* rmapbt */
		.setup	= xfs_scrub_setup_ag_rmapbt,
		.scrub	= xfs_scrub_rmapbt,
		.has	= xfs_sb_version_hasrmapbt,
	},
	{ /* refcountbt */
		.setup	= xfs_scrub_setup_ag_refcountbt,
		.scrub	= xfs_scrub_refcountbt,
		.has	= xfs_sb_version_hasreflink,
	},
	{ /* inode record */
		.setup	= xfs_scrub_setup_inode,
		.scrub	= xfs_scrub_inode,
	},
	{ /* inode data fork */
		.setup	= xfs_scrub_setup_inode_bmap_data,
		.scrub	= xfs_scrub_bmap_data,
	},
	{ /* inode attr fork */
		.setup	= xfs_scrub_setup_inode_bmap,
		.scrub	= xfs_scrub_bmap_attr,
	},
	{ /* inode CoW fork */
		.setup	= xfs_scrub_setup_inode_bmap,
		.scrub	= xfs_scrub_bmap_cow,
	},
	{ /* directory */
		.setup	= xfs_scrub_setup_directory,
		.scrub	= xfs_scrub_directory,
	},
	{ /* extended attributes */
		.setup	= xfs_scrub_setup_xattr,
		.scrub	= xfs_scrub_xattr,
	},
	{ /* symbolic link */
		.setup	= xfs_scrub_setup_symlink,
		.scrub	= xfs_scrub_symlink,
	},
	{ /* parent pointers */
		.setup	= xfs_scrub_setup_parent,
		.scrub	= xfs_scrub_parent,
	},
#ifdef CONFIG_XFS_RT
	{ /* realtime bitmap */
		.setup	= xfs_scrub_setup_rt,
		.scrub	= xfs_scrub_rtbitmap,
		.has	= xfs_sb_version_hasrealtime,
	},
	{ /* realtime summary */
		.setup	= xfs_scrub_setup_rt,
		.scrub	= xfs_scrub_rtsummary,
		.has	= xfs_sb_version_hasrealtime,
	},
#else
	{ NULL },
	{ NULL },
#endif
#ifdef CONFIG_XFS_QUOTA
	{ /* user quota */
		.setup = xfs_scrub_setup_quota,
		.scrub = xfs_scrub_quota,
	},
	{ /* group quota */
		.setup = xfs_scrub_setup_quota,
		.scrub = xfs_scrub_quota,
	},
	{ /* project quota */
		.setup = xfs_scrub_setup_quota,
		.scrub = xfs_scrub_quota,
	},
#else
	{ NULL },
	{ NULL },
	{ NULL },
#endif
};

/* Dispatch metadata scrubbing. */
int
xfs_scrub_metadata(
	struct xfs_inode		*ip,
	struct xfs_scrub_metadata	*sm)
{
	struct xfs_scrub_context	sc;
	struct xfs_mount		*mp = ip->i_mount;
	const struct xfs_scrub_meta_ops	*ops;
	bool				try_harder = false;
	int				error = 0;

	trace_xfs_scrub(ip, sm, error);

	/* Forbidden if we are shut down or mounted norecovery. */
	error = -ESHUTDOWN;
	if (XFS_FORCED_SHUTDOWN(mp))
		goto out;
	error = -ENOTRECOVERABLE;
	if (mp->m_flags & XFS_MOUNT_NORECOVERY)
		goto out;

	/* Check our inputs. */
	error = -EINVAL;
	sm->sm_flags &= ~XFS_SCRUB_FLAGS_OUT;
	if (sm->sm_flags & ~XFS_SCRUB_FLAGS_IN)
		goto out;
	if (memchr_inv(sm->sm_reserved, 0, sizeof(sm->sm_reserved)))
		goto out;

	/* Do we know about this type of metadata? */
	error = -ENOENT;
	if (sm->sm_type >= XFS_SCRUB_TYPE_NR)
		goto out;
	ops = &meta_scrub_ops[sm->sm_type];
	if (ops->scrub == NULL)
		goto out;

	/* Does this fs even support this type of metadata? */
	if (ops->has && !ops->has(&mp->m_sb))
		goto out;

	/* We don't know how to repair anything yet. */
	error = -EOPNOTSUPP;
	if (sm->sm_flags & XFS_SCRUB_IFLAG_REPAIR)
		goto out;

	/* This isn't a stable feature.  Use with care. */
	{
		static bool warned;

		if (!warned)
			xfs_alert(mp,
	"EXPERIMENTAL online scrub feature in use. Use at your own risk!");
		warned = true;
	}

	atomic_inc(&mp->m_scrubbers);

retry_op:
	/* Set up for the operation. */
	memset(&sc, 0, sizeof(sc));
	sc.mp = ip->i_mount;
	sc.sm = sm;
	sc.ops = ops;
	sc.try_harder = try_harder;
	sc.sa.agno = NULLAGNUMBER;
	error = sc.ops->setup(&sc, ip);
	if (error)
		goto out_teardown;

	/* Scrub for errors. */
	error = sc.ops->scrub(&sc);
	if (!try_harder && error == -EDEADLOCK) {
		/*
		 * Scrubbers return -EDEADLOCK to mean 'try harder'.
		 * Tear down everything we hold, then set up again with
		 * preparation for worst-case scenarios.
		 */
		error = xfs_scrub_teardown(&sc, ip, 0);
		if (error)
			goto out_dec;
		try_harder = true;
		goto retry_op;
	} else if (error)
		goto out_teardown;

	if (sc.sm->sm_flags & (XFS_SCRUB_OFLAG_CORRUPT |
			       XFS_SCRUB_OFLAG_XCORRUPT))
		xfs_alert_ratelimited(mp, "Corruption detected during scrub.");

out_teardown:
	error = xfs_scrub_teardown(&sc, ip, error);
out_dec:
	atomic_dec(&mp->m_scrubbers);
out:
	trace_xfs_scrub_done(ip, sm, error);
	return error;
}
