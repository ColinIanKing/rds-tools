/* -*- mode: c; c-basic-offset: 8; -*-
 * vim: noexpandtab sw=8 ts=8 sts=0:
 *
 * rds-sink.c: Collect some RDS packets.
 */
#define _LARGEFILE64_SOURCE

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>

#include "kernel-list.h"
#include "rdstool.h"

void print_usage(int rc)
{
	int namelen = strlen(progname);
	FILE *output = rc ? stderr : stdout;

	verbosef(0, output,
		 "Usage: %s -s <source_ip>:<source_port>\n"
		 "       %*s [-f <output_file>] [-m <msg_size>]\n"
		 "       %*s [-v ...] [-q ...]\n"
		 "       %s -h\n"
		 "       %s -V\n",
		 progname, namelen, "", namelen, "", progname, progname);

	exit(rc);
}

void print_version()
{
	verbosef(0, stdout, "%s version VERSION\n", progname);

	exit(0);
}

static int wli_do_recv(struct rds_context *ctxt)
{
	char peek_bytes[0]; 
	char *ptr;
	ssize_t len, ret = 0;
	struct rds_endpoint *e = ctxt->rc_saddr;
	struct iovec peek_iov = {
		.iov_base = peek_bytes,
		.iov_len = 0,
	};
	struct iovec iov;
	struct msghdr peek_msg = {
		.msg_name = &e->re_addr,
		.msg_namelen = sizeof(struct sockaddr_in),
		.msg_iov = &peek_iov,
		.msg_iovlen = 1,
	};
	struct msghdr msg = {
		.msg_name = &e->re_addr,
		.msg_namelen = sizeof(struct sockaddr_in),
		.msg_iov = &iov,
		.msg_iovlen = 1,
	};

	verbosef(2, stderr, "Starting receive loop\n");
	while (1) {
		ret = recvmsg(e->re_fd, &peek_msg, MSG_PEEK|MSG_TRUNC);
		if (!ret)
			break;
		if (ret < 0) {
			ret = -errno;
			verbosef(0, stderr,
				 "%s: Error from recvmsg: %s\n",
				 progname, strerror(-ret));
			break;
		}

		if (ret > iov.iov_len) {
			verbosef(3, stderr,
				 "Growing buffer to %zd bytes\n",
				 ret);
			iov.iov_len = ret;
			iov.iov_base = malloc(sizeof(char) *
					      iov.iov_len);
		}

		ret = recvmsg(e->re_fd, &msg, 0);
		if (!ret)
			break;
		if (ret < 0) {
			ret = -errno;
			verbosef(0, stderr,
				 "%s: Error from recvmsg: %s\n",
				 progname, strerror(-ret));
			break;
		}

		len = ret;
		ptr = iov.iov_base;
		while (len) {
			ret = write(STDOUT_FILENO, ptr, len);
			if (!ret) {
				verbosef(0, stderr,
					 "%s: Unexpected end of file writing to %s\n",
					 progname, ctxt->rc_filename);
				break;
			}
			if (ret < 0) {
				ret = -errno;
				verbosef(0, stderr,
					 "%s: Error writing to %s: %s\n",
					 progname, ctxt->rc_filename,
					 strerror(-ret));
				break;
			}

			ptr += ret;
			len -= ret;
		}

		if (len)
			break;
	}
	verbosef(2, stderr, "Stopping receive loop\n");

	return ret;
}

int main(int argc, char *argv[])
{
	int rc;
	char ipbuf[INET_ADDRSTRLEN];
	struct rds_context ctxt = {
		.rc_msgsize = RDS_DEFAULT_MSG_SIZE,
	};

	INIT_LIST_HEAD(&ctxt.rc_daddrs);

	rc = parse_options(argc, argv, RDS_TOOL_BASE_OPTS RDS_SINK_OPTS,
			   &ctxt);
	if (rc)
		print_usage(rc);

	inet_ntop(PF_INET, &ctxt.rc_saddr->re_addr.sin_addr, ipbuf,
		  INET_ADDRSTRLEN);
	verbosef(2, stderr, "Binding endpoint %s:%d\n",
		 ipbuf, ntohs(ctxt.rc_saddr->re_addr.sin_port));

	rc = rds_bind(&ctxt);
	if (rc)
		goto out;

	if (ctxt.rc_filename) {
		rc = dup_file(&ctxt, STDOUT_FILENO, O_CREAT|O_WRONLY);
		if (rc)
			goto out;
	} else
		ctxt.rc_filename = "<standard output>";

	rc = wli_do_recv(&ctxt);

out:
	free(ctxt.rc_saddr->re_name);
	free(ctxt.rc_saddr);

	return rc;
}
