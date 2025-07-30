// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2001,2004-2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */
#ifndef MKFS_PROTO_H_
#define MKFS_PROTO_H_

enum proto_source_type {
	PROTO_SRC_NONE = 0,
	PROTO_SRC_PROTOFILE,
	PROTO_SRC_DIR
};
struct proto_source {
	enum	proto_source_type type;
	char	*data;
};

struct proto_source setup_proto(char *fname);
void parse_proto(struct xfs_mount *mp, struct fsxattr *fsx,
		 struct proto_source *protosource,
		 int proto_slashes_are_spaces,
		 int proto_preserve_atime);
void res_failed(int err);

#endif /* MKFS_PROTO_H_ */
