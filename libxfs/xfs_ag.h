/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2018 Red Hat, Inc.
 * All rights reserved.
 */

#ifndef __LIBXFS_AG_H
#define __LIBXFS_AG_H 1

struct xfs_mount;
struct xfs_trans;

struct aghdr_init_data {
	/* per ag data */
	xfs_agblock_t		agno;		/* ag to init */
	xfs_extlen_t		agsize;		/* new AG size */
	struct list_head	buffer_list;	/* buffer writeback list */
	xfs_rfsblock_t		nfree;		/* cumulative new free space */

	/* per header data */
	xfs_daddr_t		daddr;		/* header location */
	size_t			numblks;	/* size of header */
	xfs_btnum_t		type;		/* type of btree root block */
};

int xfs_ag_init_headers(struct xfs_mount *mp, struct aghdr_init_data *id);
int xfs_ag_shrink_space(struct xfs_mount *mp, struct xfs_trans **tpp,
			xfs_agnumber_t agno, xfs_extlen_t delta);
int xfs_ag_extend_space(struct xfs_mount *mp, struct xfs_trans *tp,
			struct aghdr_init_data *id, xfs_extlen_t len);
int xfs_ag_get_geometry(struct xfs_mount *mp, xfs_agnumber_t agno,
			struct xfs_ag_geometry *ageo);

/*
 * Perag iteration APIs
 *
 * XXX: for_each_perag_range() usage really needs an iterator to clean up when
 * we terminate at end_agno because we may have taken a reference to the perag
 * beyond end_agno. Right now callers have to be careful to catch and clean that
 * up themselves. This is not necessary for the callers of for_each_perag() and
 * for_each_perag_from() because they terminate at sb_agcount where there are
 * no perag structures in tree beyond end_agno.
 */
#define for_each_perag_range(mp, next_agno, end_agno, pag) \
	for ((pag) = xfs_perag_get((mp), (next_agno)); \
		(pag) != NULL && (next_agno) <= (end_agno); \
		(next_agno) = (pag)->pag_agno + 1, \
		xfs_perag_put(pag), \
		(pag) = xfs_perag_get((mp), (next_agno)))

#define for_each_perag_from(mp, next_agno, pag) \
	for_each_perag_range((mp), (next_agno), (mp)->m_sb.sb_agcount, (pag))


#define for_each_perag(mp, agno, pag) \
	(agno) = 0; \
	for_each_perag_from((mp), (agno), (pag))

#define for_each_perag_tag(mp, agno, pag, tag) \
	for ((agno) = 0, (pag) = xfs_perag_get_tag((mp), 0, (tag)); \
		(pag) != NULL; \
		(agno) = (pag)->pag_agno + 1, \
		xfs_perag_put(pag), \
		(pag) = xfs_perag_get_tag((mp), (agno), (tag)))

#endif /* __LIBXFS_AG_H */
