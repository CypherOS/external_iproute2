/*
 * bearer.c	TIPC bearer functionality.
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 *
 * Authors:	Richard Alpe <richard.alpe@ericsson.com>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>
#include <errno.h>

#include <linux/tipc_netlink.h>
#include <linux/tipc.h>
#include <linux/genetlink.h>

#include <libmnl/libmnl.h>
#include <sys/socket.h>

#include "cmdl.h"
#include "msg.h"
#include "bearer.h"

static void _print_bearer_opts(void)
{
	fprintf(stderr,
		"\nOPTIONS\n"
		" priority              - Bearer link priority\n"
		" tolerance             - Bearer link tolerance\n"
		" window                - Bearer link window\n");
}

static void _print_bearer_media(void)
{
	fprintf(stderr,
		"\nMEDIA\n"
		" udp                   - User Datagram Protocol\n"
		" ib                    - Infiniband\n"
		" eth                   - Ethernet\n");
}

static void cmd_bearer_enable_l2_help(struct cmdl *cmdl)
{
	fprintf(stderr,
		"Usage: %s bearer enable media MEDIA device DEVICE [OPTIONS]\n"
		"\nOPTIONS\n"
		" domain DOMAIN         - Discovery domain\n"
		" priority PRIORITY     - Bearer priority\n",
		cmdl->argv[0]);
}

static void cmd_bearer_enable_udp_help(struct cmdl *cmdl)
{
	fprintf(stderr,
		"Usage: %s bearer enable media udp name NAME localip IP [OPTIONS]\n"
		"\nOPTIONS\n"
		" domain DOMAIN         - Discovery domain\n"
		" priority PRIORITY     - Bearer priority\n"
		" localport PORT        - Local UDP port (default 6118)\n"
		" remoteip IP           - Remote IP address\n"
		" remoteport IP         - Remote UDP port (default 6118)\n",
		cmdl->argv[0]);
}

static int enable_l2_bearer(struct nlmsghdr *nlh, struct opt *opts,
			    struct cmdl *cmdl)
{
	struct opt *opt;
	char id[TIPC_MAX_BEARER_NAME];

	if (!(opt = get_opt(opts, "device"))) {
		fprintf(stderr, "error: missing bearer device\n");
		return -EINVAL;
	}
	snprintf(id, sizeof(id), "eth:%s", opt->val);
	mnl_attr_put_strz(nlh, TIPC_NLA_BEARER_NAME, id);

	return 0;
}

static int get_netid_cb(const struct nlmsghdr *nlh, void *data)
{
	struct genlmsghdr *genl = mnl_nlmsg_get_payload(nlh);
	struct nlattr *info[TIPC_NLA_MAX + 1] = {};
	struct nlattr *attrs[TIPC_NLA_NET_MAX + 1] = {};
	int *netid = (int*)data;

	mnl_attr_parse(nlh, sizeof(*genl), parse_attrs, info);
	if (!info[TIPC_NLA_NET])
		return MNL_CB_ERROR;
	mnl_attr_parse_nested(info[TIPC_NLA_NET], parse_attrs, attrs);
	if (!attrs[TIPC_NLA_NET_ID])
		return MNL_CB_ERROR;
	*netid = mnl_attr_get_u32(attrs[TIPC_NLA_NET_ID]);

	return MNL_CB_OK;
}

static int generate_multicast(short af, char *buf, int bufsize)
{
	int netid;
	char mnl_msg[MNL_SOCKET_BUFFER_SIZE];
	struct nlmsghdr *nlh;

	if (!(nlh = msg_init(mnl_msg, TIPC_NL_NET_GET))) {
		fprintf(stderr, "error, message initialization failed\n");
		return -1;
	}
	if (msg_dumpit(nlh, get_netid_cb, &netid)) {
		fprintf(stderr, "error, failed to fetch TIPC network id from kernel\n");
		return -EINVAL;
	}
	if (af == AF_INET)
		snprintf(buf, bufsize, "228.0.%u.%u", (netid>>8) & 0xFF, netid & 0xFF);
	else
		snprintf(buf, bufsize, "ff02::%u", netid);

	return 0;
}

static int enable_udp_bearer(struct nlmsghdr *nlh, struct opt *opts,
			     struct cmdl *cmdl)
{
	int err;
	struct opt *opt;
	struct nlattr *nest;
	char buf[INET6_ADDRSTRLEN];
	char *locport = "6118";
	char *remport = "6118";
	char *locip = NULL;
	char *remip = NULL;
	char name[TIPC_MAX_BEARER_NAME];
	struct addrinfo *loc = NULL;
	struct addrinfo *rem = NULL;
	struct addrinfo hints = {
		.ai_family = AF_UNSPEC,
		.ai_socktype = SOCK_DGRAM
	};

	if (help_flag) {
		cmd_bearer_enable_udp_help(cmdl);
		/* TODO find a better error code? */
		return -EINVAL;
	}

	if (!(opt = get_opt(opts, "name"))) {
		fprintf(stderr, "error, udp bearer name missing\n");
		cmd_bearer_enable_udp_help(cmdl);
		return -EINVAL;
	}
	snprintf(name, sizeof(name), "udp:%s", opt->val);

	if (!(opt = get_opt(opts, "localip"))) {
		fprintf(stderr, "error, udp bearer localip missing\n");
		cmd_bearer_enable_udp_help(cmdl);
		return -EINVAL;
	}
	locip = opt->val;

	if ((opt = get_opt(opts, "remoteip")))
		remip = opt->val;

	if ((opt = get_opt(opts, "localport")))
		locport = opt->val;

	if ((opt = get_opt(opts, "remoteport")))
		remport = opt->val;

	if ((err = getaddrinfo(locip, locport, &hints, &loc))) {
		fprintf(stderr, "UDP local address error: %s\n",
			gai_strerror(err));
		return err;
	}

	if (!remip) {
		if (generate_multicast(loc->ai_family, buf, sizeof(buf))) {
			fprintf(stderr, "Failed to generate multicast address\n");
			return -EINVAL;
		}
		remip = buf;
	}

	if ((err = getaddrinfo(remip, remport, &hints, &rem))) {
		fprintf(stderr, "UDP remote address error: %s\n",
			gai_strerror(err));
		freeaddrinfo(loc);
		return err;
	}

	if (rem->ai_family != loc->ai_family) {
		fprintf(stderr, "UDP local and remote AF mismatch\n");
		return -EINVAL;
	}

	mnl_attr_put_strz(nlh, TIPC_NLA_BEARER_NAME, name);

	nest = mnl_attr_nest_start(nlh, TIPC_NLA_BEARER_UDP_OPTS);
	mnl_attr_put(nlh, TIPC_NLA_UDP_LOCAL, loc->ai_addrlen, loc->ai_addr);
	mnl_attr_put(nlh, TIPC_NLA_UDP_REMOTE, rem->ai_addrlen, rem->ai_addr);
	mnl_attr_nest_end(nlh, nest);

	freeaddrinfo(rem);
	freeaddrinfo(loc);

	return 0;
}

static void cmd_bearer_enable_help(struct cmdl *cmdl)
{
	fprintf(stderr,
		"Usage: %s bearer enable [OPTIONS] media MEDIA ARGS...\n\n"
		"OPTIONS\n"
		" domain DOMAIN         - Discovery domain\n"
		" priority PRIORITY     - Bearer priority\n",
		cmdl->argv[0]);
	_print_bearer_media();
}

static int cmd_bearer_enable(struct nlmsghdr *nlh, const struct cmd *cmd,
			     struct cmdl *cmdl, void *data)
{
	int err;
	struct opt *opt;
	struct nlattr *nest;
	char buf[MNL_SOCKET_BUFFER_SIZE];
	char *media;
	struct opt opts[] = {
		{ "device",		NULL },
		{ "domain",		NULL },
		{ "localip",		NULL },
		{ "localport",		NULL },
		{ "media",		NULL },
		{ "name",		NULL },
		{ "priority",		NULL },
		{ "remoteip",		NULL },
		{ "remoteport",		NULL },
		{ NULL }
	};

	if (parse_opts(opts, cmdl) < 0) {
		if (help_flag)
			(cmd->help)(cmdl);
		return -EINVAL;
	}

	if (!(opt = get_opt(opts, "media"))) {
		if (help_flag)
			(cmd->help)(cmdl);
		else
			fprintf(stderr, "error, missing bearer media\n");
		return -EINVAL;
	}
	media = opt->val;

	if (!(nlh = msg_init(buf, TIPC_NL_BEARER_ENABLE))) {
		fprintf(stderr, "error: message initialisation failed\n");
		return -1;
	}
	nest = mnl_attr_nest_start(nlh, TIPC_NLA_BEARER);

	if ((opt = get_opt(opts, "domain")))
		mnl_attr_put_u32(nlh, TIPC_NLA_BEARER_DOMAIN, atoi(opt->val));

	if ((opt = get_opt(opts, "priority"))) {
		struct nlattr *props;

		props = mnl_attr_nest_start(nlh, TIPC_NLA_BEARER_PROP);
		mnl_attr_put_u32(nlh, TIPC_NLA_PROP_PRIO, atoi(opt->val));
		mnl_attr_nest_end(nlh, props);
	}

	if (strcmp(media, "udp") == 0) {
		if (help_flag) {
			cmd_bearer_enable_udp_help(cmdl);
			return -EINVAL;
		}
		if ((err = enable_udp_bearer(nlh, opts, cmdl)))
			return err;
	} else if ((strcmp(media, "eth") == 0) || (strcmp(media, "udp") == 0)) {
		if (help_flag) {
			cmd_bearer_enable_l2_help(cmdl);
			return -EINVAL;
		}
		if ((err = enable_l2_bearer(nlh, opts, cmdl)))
			return err;
	} else {
		fprintf(stderr, "error, invalid media type \"%s\"\n", media);
		return -EINVAL;
	}

	mnl_attr_nest_end(nlh, nest);

	return msg_doit(nlh, NULL, NULL);
}

static int add_l2_bearer(struct nlmsghdr *nlh, struct opt *opts)
{
	struct opt *opt;
	char id[TIPC_MAX_BEARER_NAME];

	if (!(opt = get_opt(opts, "device"))) {
		fprintf(stderr, "error: missing bearer device\n");
		return -EINVAL;
	}
	snprintf(id, sizeof(id), "eth:%s", opt->val);

	mnl_attr_put_strz(nlh, TIPC_NLA_BEARER_NAME, id);

	return 0;
}

static int add_udp_bearer(struct nlmsghdr *nlh, struct opt *opts)
{
	struct opt *opt;
	char id[TIPC_MAX_BEARER_NAME];

	if (!(opt = get_opt(opts, "name"))) {
		fprintf(stderr, "error: missing bearer name\n");
		return -EINVAL;
	}
	snprintf(id, sizeof(id), "udp:%s", opt->val);

	mnl_attr_put_strz(nlh, TIPC_NLA_BEARER_NAME, id);

	return 0;
}

static void cmd_bearer_disable_l2_help(struct cmdl *cmdl)
{
	fprintf(stderr, "Usage: %s bearer disable media udp device DEVICE\n",
		cmdl->argv[0]);
}

static void cmd_bearer_disable_udp_help(struct cmdl *cmdl)
{
	fprintf(stderr, "Usage: %s bearer disable media udp name NAME\n",
		cmdl->argv[0]);
}

static void cmd_bearer_disable_help(struct cmdl *cmdl)
{
	fprintf(stderr, "Usage: %s bearer disable media MEDIA ARGS...\n",
		cmdl->argv[0]);
	_print_bearer_media();
}

static int cmd_bearer_disable(struct nlmsghdr *nlh, const struct cmd *cmd,
			      struct cmdl *cmdl, void *data)
{
	int err;
	char *media;
	char buf[MNL_SOCKET_BUFFER_SIZE];
	struct nlattr *nest;
	struct opt *opt;
	struct opt opts[] = {
		{ "device",		NULL },
		{ "name",		NULL },
		{ "media",		NULL },
		{ NULL }
	};

	if (parse_opts(opts, cmdl) < 0) {
		if (help_flag)
			(cmd->help)(cmdl);
		return -EINVAL;
	}

	if (!(opt = get_opt(opts, "media"))) {
		if (help_flag)
			(cmd->help)(cmdl);
		else
			fprintf(stderr, "error, missing bearer media\n");
		return -EINVAL;
	}
	media = opt->val;

	if (!(nlh = msg_init(buf, TIPC_NL_BEARER_DISABLE))) {
		fprintf(stderr, "error, message initialisation failed\n");
		return -1;
	}

	nest = mnl_attr_nest_start(nlh, TIPC_NLA_BEARER);

	if (strcmp(media, "udp") == 0) {
		if (help_flag) {
			cmd_bearer_disable_udp_help(cmdl);
			return -EINVAL;
		}
		if ((err = add_udp_bearer(nlh, opts)))
			return err;
	} else if ((strcmp(media, "eth") == 0) || (strcmp(media, "udp") == 0)) {
		if (help_flag) {
			cmd_bearer_disable_l2_help(cmdl);
			return -EINVAL;
		}
		if ((err = add_l2_bearer(nlh, opts)))
			return err;
	} else {
		fprintf(stderr, "error, invalid media type \"%s\"\n", media);
		return -EINVAL;
	}
	mnl_attr_nest_end(nlh, nest);

	return msg_doit(nlh, NULL, NULL);

}

static void cmd_bearer_set_help(struct cmdl *cmdl)
{
	fprintf(stderr, "Usage: %s bearer set OPTION media MEDIA ARGS...\n",
		cmdl->argv[0]);
	_print_bearer_opts();
	_print_bearer_media();
}

static void cmd_bearer_set_udp_help(struct cmdl *cmdl)
{
	fprintf(stderr, "Usage: %s bearer set OPTION media udp name NAME\n\n",
		cmdl->argv[0]);
	_print_bearer_opts();
}

static void cmd_bearer_set_l2_help(struct cmdl *cmdl, char *media)
{
	fprintf(stderr,
		"Usage: %s bearer set [OPTION]... media %s device DEVICE\n",
		cmdl->argv[0], media);
	_print_bearer_opts();
}

static int cmd_bearer_set_prop(struct nlmsghdr *nlh, const struct cmd *cmd,
			 struct cmdl *cmdl, void *data)
{
	int err;
	int val;
	int prop;
	char *media;
	char buf[MNL_SOCKET_BUFFER_SIZE];
	struct nlattr *props;
	struct nlattr *attrs;
	struct opt *opt;
	struct opt opts[] = {
		{ "device",		NULL },
		{ "media",		NULL },
		{ "name",		NULL },
		{ NULL }
	};

	if (strcmp(cmd->cmd, "priority") == 0)
		prop = TIPC_NLA_PROP_PRIO;
	else if ((strcmp(cmd->cmd, "tolerance") == 0))
		prop = TIPC_NLA_PROP_TOL;
	else if ((strcmp(cmd->cmd, "window") == 0))
		prop = TIPC_NLA_PROP_WIN;
	else
		return -EINVAL;

	if (help_flag) {
		(cmd->help)(cmdl);
		return -EINVAL;
	}

	if (cmdl->optind >= cmdl->argc) {
		fprintf(stderr, "error, missing value\n");
		return -EINVAL;
	}
	val = atoi(shift_cmdl(cmdl));

	if (parse_opts(opts, cmdl) < 0)
		return -EINVAL;

	if (!(nlh = msg_init(buf, TIPC_NL_BEARER_SET))) {
		fprintf(stderr, "error, message initialisation failed\n");
		return -1;
	}
	attrs = mnl_attr_nest_start(nlh, TIPC_NLA_BEARER);

	props = mnl_attr_nest_start(nlh, TIPC_NLA_BEARER_PROP);
	mnl_attr_put_u32(nlh, prop, val);
	mnl_attr_nest_end(nlh, props);

	if (!(opt = get_opt(opts, "media"))) {
		fprintf(stderr, "error, missing media\n");
		return -EINVAL;
	}
	media = opt->val;

	if (strcmp(media, "udp") == 0) {
		if (help_flag) {
			cmd_bearer_set_udp_help(cmdl);
			return -EINVAL;
		}
		if ((err = add_udp_bearer(nlh, opts)))
			return err;
	} else if ((strcmp(media, "eth") == 0) || (strcmp(media, "udp") == 0)) {
		if (help_flag) {
			cmd_bearer_set_l2_help(cmdl, media);
			return -EINVAL;
		}
		if ((err = add_l2_bearer(nlh, opts)))
			return err;
	} else {
		fprintf(stderr, "error, invalid media type \"%s\"\n", media);
		return -EINVAL;
	}
	mnl_attr_nest_end(nlh, attrs);

	return msg_doit(nlh, NULL, NULL);
}

static int cmd_bearer_set(struct nlmsghdr *nlh, const struct cmd *cmd,
			  struct cmdl *cmdl, void *data)
{
	const struct cmd cmds[] = {
		{ "priority",	cmd_bearer_set_prop,	cmd_bearer_set_help },
		{ "tolerance",	cmd_bearer_set_prop,	cmd_bearer_set_help },
		{ "window",	cmd_bearer_set_prop,	cmd_bearer_set_help },
		{ NULL }
	};

	return run_cmd(nlh, cmd, cmds, cmdl, NULL);
}

static void cmd_bearer_get_help(struct cmdl *cmdl)
{
	fprintf(stderr, "Usage: %s bearer get OPTION media MEDIA ARGS...\n",
		cmdl->argv[0]);
	_print_bearer_opts();
	_print_bearer_media();
}

static void cmd_bearer_get_udp_help(struct cmdl *cmdl)
{
	fprintf(stderr, "Usage: %s bearer get OPTION media udp name NAME\n\n",
		cmdl->argv[0]);
	_print_bearer_opts();
}

static void cmd_bearer_get_l2_help(struct cmdl *cmdl, char *media)
{
	fprintf(stderr,
		"Usage: %s bearer get [OPTION]... media %s device DEVICE\n",
		cmdl->argv[0], media);
	_print_bearer_opts();
}

static int bearer_get_cb(const struct nlmsghdr *nlh, void *data)
{
	int *prop = data;
	struct genlmsghdr *genl = mnl_nlmsg_get_payload(nlh);
	struct nlattr *info[TIPC_NLA_MAX + 1] = {};
	struct nlattr *attrs[TIPC_NLA_BEARER_MAX + 1] = {};
	struct nlattr *props[TIPC_NLA_PROP_MAX + 1] = {};

	mnl_attr_parse(nlh, sizeof(*genl), parse_attrs, info);
	if (!info[TIPC_NLA_BEARER])
		return MNL_CB_ERROR;

	mnl_attr_parse_nested(info[TIPC_NLA_BEARER], parse_attrs, attrs);
	if (!attrs[TIPC_NLA_BEARER_PROP])
		return MNL_CB_ERROR;

	mnl_attr_parse_nested(attrs[TIPC_NLA_BEARER_PROP], parse_attrs, props);
	if (!props[*prop])
		return MNL_CB_ERROR;

	printf("%u\n", mnl_attr_get_u32(props[*prop]));

	return MNL_CB_OK;
}

static int cmd_bearer_get_prop(struct nlmsghdr *nlh, const struct cmd *cmd,
			       struct cmdl *cmdl, void *data)
{
	int err;
	int prop;
	char *media;
	char buf[MNL_SOCKET_BUFFER_SIZE];
	struct nlattr *attrs;
	struct opt *opt;
	struct opt opts[] = {
		{ "device",		NULL },
		{ "media",		NULL },
		{ "name",		NULL },
		{ NULL }
	};

	if (strcmp(cmd->cmd, "priority") == 0)
		prop = TIPC_NLA_PROP_PRIO;
	else if ((strcmp(cmd->cmd, "tolerance") == 0))
		prop = TIPC_NLA_PROP_TOL;
	else if ((strcmp(cmd->cmd, "window") == 0))
		prop = TIPC_NLA_PROP_WIN;
	else
		return -EINVAL;

	if (help_flag) {
		(cmd->help)(cmdl);
		return -EINVAL;
	}

	if (parse_opts(opts, cmdl) < 0)
		return -EINVAL;

	if (!(nlh = msg_init(buf, TIPC_NL_BEARER_GET))) {
		fprintf(stderr, "error, message initialisation failed\n");
		return -1;
	}

	if (!(opt = get_opt(opts, "media"))) {
		fprintf(stderr, "error, missing media\n");
		return -EINVAL;
	}
	media = opt->val;

	attrs = mnl_attr_nest_start(nlh, TIPC_NLA_BEARER);
	if (strcmp(media, "udp") == 0) {
		if (help_flag) {
			cmd_bearer_get_udp_help(cmdl);
			return -EINVAL;
		}
		if ((err = add_udp_bearer(nlh, opts)))
			return err;
	} else if ((strcmp(media, "eth") == 0) || (strcmp(media, "udp") == 0)) {
		if (help_flag) {
			cmd_bearer_get_l2_help(cmdl, media);
			return -EINVAL;
		}
		if ((err = add_l2_bearer(nlh, opts)))
			return err;
	} else {
		fprintf(stderr, "error, invalid media type \"%s\"\n", media);
		return -EINVAL;
	}
	mnl_attr_nest_end(nlh, attrs);

	return msg_doit(nlh, bearer_get_cb, &prop);
}

static int cmd_bearer_get(struct nlmsghdr *nlh, const struct cmd *cmd,
			  struct cmdl *cmdl, void *data)
{
	const struct cmd cmds[] = {
		{ "priority",	cmd_bearer_get_prop,	cmd_bearer_get_help },
		{ "tolerance",	cmd_bearer_get_prop,	cmd_bearer_get_help },
		{ "window",	cmd_bearer_get_prop,	cmd_bearer_get_help },
		{ NULL }
	};

	return run_cmd(nlh, cmd, cmds, cmdl, NULL);
}

static int bearer_list_cb(const struct nlmsghdr *nlh, void *data)
{
	struct genlmsghdr *genl = mnl_nlmsg_get_payload(nlh);
	struct nlattr *info[TIPC_NLA_MAX + 1] = {};
	struct nlattr *attrs[TIPC_NLA_BEARER_MAX + 1] = {};

	mnl_attr_parse(nlh, sizeof(*genl), parse_attrs, info);
	if (!info[TIPC_NLA_BEARER]) {
		fprintf(stderr, "No bearer in netlink response\n");
		return MNL_CB_ERROR;
	}

	mnl_attr_parse_nested(info[TIPC_NLA_BEARER], parse_attrs, attrs);
	if (!attrs[TIPC_NLA_BEARER_NAME]) {
		fprintf(stderr, "Bearer name missing in netlink response\n");
		return MNL_CB_ERROR;
	}

	printf("%s\n", mnl_attr_get_str(attrs[TIPC_NLA_BEARER_NAME]));

	return MNL_CB_OK;
}

static int cmd_bearer_list(struct nlmsghdr *nlh, const struct cmd *cmd,
			   struct cmdl *cmdl, void *data)
{
	char buf[MNL_SOCKET_BUFFER_SIZE];

	if (help_flag) {
		fprintf(stderr, "Usage: %s bearer list\n", cmdl->argv[0]);
		return -EINVAL;
	}

	if (!(nlh = msg_init(buf, TIPC_NL_BEARER_GET))) {
		fprintf(stderr, "error, message initialisation failed\n");
		return -1;
	}

	return msg_dumpit(nlh, bearer_list_cb, NULL);
}

void cmd_bearer_help(struct cmdl *cmdl)
{
	fprintf(stderr,
		"Usage: %s bearer COMMAND [ARGS] ...\n"
		"\n"
		"COMMANDS\n"
		" enable                - Enable a bearer\n"
		" disable               - Disable a bearer\n"
		" set                   - Set various bearer properties\n"
		" get                   - Get various bearer properties\n"
		" list                  - List bearers\n", cmdl->argv[0]);
}

int cmd_bearer(struct nlmsghdr *nlh, const struct cmd *cmd, struct cmdl *cmdl,
	       void *data)
{
	const struct cmd cmds[] = {
		{ "disable",	cmd_bearer_disable,	cmd_bearer_disable_help },
		{ "enable",	cmd_bearer_enable,	cmd_bearer_enable_help },
		{ "get",	cmd_bearer_get,		cmd_bearer_get_help },
		{ "list",	cmd_bearer_list,	NULL },
		{ "set",	cmd_bearer_set,		cmd_bearer_set_help },
		{ NULL }
	};

	return run_cmd(nlh, cmd, cmds, cmdl, NULL);
}
