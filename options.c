/* -*- mode: c; c-basic-offset: 8; -*-
 * vim: noexpandtab sw=8 ts=8 sts=0:
 *
 * options.c - options and stuff
 */

#define _LARGEFILE64_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netdb.h>
#include <fcntl.h>
#include <unistd.h>
#include <getopt.h>
#include <libgen.h>
#include <inttypes.h>
#include <errno.h>

#include "kernel-list.h"
#include "rdstool.h"


/* This gets changed in parse_options() */
char *progname = "rds-generic-tool";
unsigned int verbose = 1;


/*
 * Take "address:port" and return a sockaddr(_in) that describes it.
 * Since RDS is IPv4 only, we don't worry about PF_INET6.
 *
 * XXX: Should we try a default IP or default port?  RDS is very
 * endpoint-oriented; right now we require explicitness.
 *
 * Since getaddrinfo(3) returns multiple addresses, we simply find the
 * first SOCK_DGRAM AF_INET result.  Note that RDS actually uses
 * SOCK_SEQPACKET, but we're lying to getaddrinfo(3).
 */
static int parse_endpoint(struct rds_endpoint *nep)
{
	int rc;
	char *host, *port;
	struct addrinfo *list, *try;
	struct addrinfo hint = {
		.ai_family	= PF_INET,
		.ai_socktype	= SOCK_DGRAM,
	};

	host = strdup(nep->re_name);
	if (!host) {
		rc = -ENOMEM;
		verbosef(0, stderr, "%s: Unable to allocate memory\n",
			 progname);
		goto out;
	}

	port = strchr(host, ':');
	if (!port) {
		rc = -EINVAL;
		verbosef(0, stderr, "%s: Invalid endpoint: %s\n",
			 progname, nep->re_name);
		goto out;
	}

	*port = '\0';
	port++;

	rc = getaddrinfo(host, port, &hint, &list);
	if (rc) {
		verbosef(0, stderr, "%s: Unable to resolve \"%s\": %s\n",
			 progname, nep->re_name, gai_strerror(rc));
		goto out;
	}

	for (try = list; try; try = try->ai_next) {
		if ((try->ai_family == PF_INET) &&
		    (try->ai_socktype == SOCK_DGRAM))
			break;
	}

	if (try) {
		if (try->ai_addrlen != sizeof(struct sockaddr_in))
			verbosef(0, stderr,
				 "%s: OMG WTF BBQ!  try->ai_addrlen = %d, sizeof(struct sockaddr_in) = %zd\n",
				 progname, try->ai_addrlen,
				 sizeof(struct sockaddr_in));

		memcpy(&nep->re_addr, try->ai_addr, try->ai_addrlen);
	}

	if (list)
		freeaddrinfo(list);

out:
	return rc;
}

static int add_endpoint(const char *endpoint, struct list_head *list)
{
	int rc;
	struct rds_endpoint *nep;

	nep = malloc(sizeof(struct rds_endpoint));
	if (!nep)
		return -ENOMEM;

	nep->re_name = strdup(endpoint);
	if (!nep->re_name) {
		free(nep);
		return -ENOMEM;
	}

	rc = parse_endpoint(nep);
	if (!rc) {
		list_add_tail(&nep->re_item, list);
	} else {
		free(nep->re_name);
		free(nep);
	}

	return rc;
}

static int get_number(char *arg, uint64_t *res)
{
	char *ptr = NULL;
	uint64_t num;

	num = strtoull(arg, &ptr, 0);

	if ((ptr == arg) || (num == UINT64_MAX))
		return(-EINVAL);

	switch (*ptr) {
	case '\0':
		break;

	case 'g':
	case 'G':
		num *= 1024;
		/* FALL THROUGH */

	case 'm':
	case 'M':
		num *= 1024;
		/* FALL THROUGH */

	case 'k':
	case 'K':
		num *= 1024;
		/* FALL THROUGH */

	case 'b':
	case 'B':
		break;

	default:
		return -EINVAL;
	}

	*res = num;

	return 0;
}

extern char *optarg;
extern int optopt;
extern int optind;
extern int opterr;
int parse_options(int argc, char *argv[], const char *opts,
		  struct rds_context *ctxt)
{
	int c, rc = 0;
	uint64_t val;
	ssize_t sndbuf = 0;
	struct list_head saddrs;

	if (argc && argv[0])
		progname = basename(argv[0]);

	INIT_LIST_HEAD(&saddrs);
	opterr = 0;
	while ((c = getopt(argc, argv, opts)) != EOF) {
		switch (c) {
			case 's':
				if (!list_empty(&saddrs)) {
					verbosef(0, stderr,
						 "%s: Only one source address allowed\n",
						 progname);
					rc = -EINVAL;
				} else
					rc = add_endpoint(optarg, &saddrs);
				break;

			case 'd':
				rc = add_endpoint(optarg, &ctxt->rc_daddrs);
				break;

			case 'm':
				rc = get_number(optarg, &val);
				if (rc) {
					verbosef(0, stderr,
						 "%s: Invalid number: %s\n",
						 progname, optarg);
					break;
				}

				if (val > UINT32_MAX) {
					rc = -EINVAL;
					verbosef(0, stderr,
						 "%s: Message size too large: %"PRIu64"\n",
						 progname, val);
				} else
					ctxt->rc_msgsize = (uint32_t)val;
				break;

			case 'b':
				rc = get_number(optarg, &val);
				if (rc) {
					verbosef(0, stderr,
						 "%s: Invalid number: %s\n",
						 progname, optarg);
					break;
				}

				if (val > SSIZE_MAX) {
					rc = -EINVAL;
					verbosef(0, stderr,
						 "%s: Socket buffer too large: %"PRIu64"\n",
						 progname, val);
				} else
					sndbuf = (ssize_t)val;
				break;

			case 'l':
				rc = get_number(optarg, &ctxt->rc_total);
				if (rc) {
					verbosef(0, stderr,
						 "%s: Invalid number: %s\n",
						 progname, optarg);
				}
				break;

			case 'f':
				ctxt->rc_filename = optarg;
				break;

			case 'v':
				verbose++;
				break;

			case 'q':
				if (verbose)
					verbose--;
				break;

			case 'V':
				print_version();
				break;

			case 'h':
				print_usage(0);
				break;

			case '?':
				verbosef(0, stderr,
					 "%s: Invalid option \'-%c\'\n",
					 progname, optopt);
				rc = -EINVAL;
				break;
				
			case ':':
				verbosef(0, stderr,
					 "%s: Option \'-%c\' requires an argument\n",
					 progname, optopt);
				rc = -EINVAL;
				break;
				
			default:
				verbosef(0, stderr,
					 "%s: Shouldn't get here %c %c\n",
					 progname, optopt, c);
				rc = -EINVAL;
				break;
		}

		if (rc)
			goto out;
	}

	if (list_empty(&saddrs)) {
		verbosef(0, stderr, "%s: Source endpoint address required\n",
			 progname);
		rc = -EINVAL;
		goto out;
	}

	ctxt->rc_saddr = list_entry(saddrs.prev, struct rds_endpoint,
				    re_item);
	ctxt->rc_saddr->re_sndbuf = sndbuf;

out:
	return rc;
}

int rds_bind(struct rds_context *ctxt)
{
	int rc;
	struct rds_endpoint *e = ctxt->rc_saddr;

	rc = socket(PF_RDS, SOCK_SEQPACKET, 0);
	if (rc < 0) {
		rc = -errno;
		verbosef(0, stderr, "%s: Unable to create socket: %s\n",
			 progname, strerror(-rc));
		goto out;
	}

	e->re_fd = rc;
	rc = bind(e->re_fd, (struct sockaddr *)&e->re_addr,
		  sizeof(struct sockaddr_in));
	if (rc) {
		rc = -errno;
		verbosef(0, stderr, "%s: Unable to bind socket: %s\n",
			 progname, strerror(-rc));

		close(e->re_fd);
		e->re_fd = -1;
		goto out;
	}

	if (e->re_sndbuf) {
		rc = setsockopt(e->re_fd, SOL_RDS, RDS_SNDBUF,
				&e->re_sndbuf, sizeof(e->re_sndbuf));
		if (rc) {
			rc = -errno;
			verbosef(0, stderr, "%s: Unable to set send buffer size: %s\n", 
				 progname, strerror(-rc));
		}
	}

out:
	return rc;
}

int dup_file(struct rds_context *ctxt, int fd, int flags)
{
	int tmp_fd, rc = 0;
	char *type;

	tmp_fd = open64(ctxt->rc_filename, flags);
	if (tmp_fd < 0) {
		rc = -errno;
		verbosef(0, stderr, "%s: Unable to open file \"%s\": %s\n",
			 progname, ctxt->rc_filename, strerror(-rc));
		goto out;
	}

	if (tmp_fd != fd) {
		rc = dup2(tmp_fd, fd);
		if (rc) {
			rc = -errno;
			switch (fd) {
				case STDIN_FILENO:
					type = "stdin";
					break;

				case STDOUT_FILENO:
					type = "stdout";
					break;

				case STDERR_FILENO:
					type = "stderr";
					break;

				default:
					type = "random fd";
					break;
			}

			verbosef(0, stderr,
				 "%s: Unable to set file \"%s\" as %s: %s\n",
				 progname, ctxt->rc_filename, type,
				 strerror(-rc));
		}
	}

out:
	return rc;
}
