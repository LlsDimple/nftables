/*
 * Copyright (c) 2013-2017 Pablo Neira Ayuso <pablo@netfilter.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Development of this code funded by Astaro AG (http://www.astaro.com/)
 */

#include <libmnl/libmnl.h>
#include <libnftnl/common.h>
#include <libnftnl/ruleset.h>
#include <libnftnl/table.h>
#include <libnftnl/chain.h>
#include <libnftnl/rule.h>
#include <libnftnl/expr.h>
#include <libnftnl/set.h>
#include <libnftnl/object.h>
#include <libnftnl/batch.h>

#include <linux/netfilter/nfnetlink.h>
#include <linux/netfilter/nf_tables.h>

#include <mnl.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <errno.h>
#include <utils.h>
#include <nftables.h>

uint32_t mnl_seqnum_alloc(unsigned int *seqnum)
{
	return (*seqnum)++;
}

/* The largest nf_tables netlink message is the set element message, which
 * contains the NFTA_SET_ELEM_LIST_ELEMENTS attribute. This attribute is
 * a nest that describes the set elements. Given that the netlink attribute
 * length (nla_len) is 16 bits, the largest message is a bit larger than
 * 64 KBytes.
 */
#define NFT_NLMSG_MAXSIZE (UINT16_MAX + getpagesize())

static int
nft_mnl_recv(struct netlink_ctx *ctx, uint32_t portid,
	     int (*cb)(const struct nlmsghdr *nlh, void *data), void *cb_data)
{
	char buf[NFT_NLMSG_MAXSIZE];
	int ret;

	ret = mnl_socket_recvfrom(ctx->nf_sock, buf, sizeof(buf));
	while (ret > 0) {
		ret = mnl_cb_run(buf, ret, ctx->seqnum, portid, cb, cb_data);
		if (ret <= 0)
			goto out;

		ret = mnl_socket_recvfrom(ctx->nf_sock, buf, sizeof(buf));
	}
out:
	if (ret < 0 && errno == EAGAIN)
		return 0;

	return ret;
}

static int
nft_mnl_talk(struct netlink_ctx *ctx, const void *data, unsigned int len,
	     int (*cb)(const struct nlmsghdr *nlh, void *data), void *cb_data)
{
	uint32_t portid = mnl_socket_get_portid(ctx->nf_sock);

	if (ctx->debug_mask & NFT_DEBUG_MNL)
		mnl_nlmsg_fprintf(ctx->octx->output_fp, data, len,
				  sizeof(struct nfgenmsg));

	if (mnl_socket_sendto(ctx->nf_sock, data, len) < 0)
		return -1;

	return nft_mnl_recv(ctx, portid, cb, cb_data);
}

/*
 * Rule-set consistency check across several netlink dumps
 */
static uint16_t nft_genid;

static int genid_cb(const struct nlmsghdr *nlh, void *data)
{
	struct nfgenmsg *nfh = mnl_nlmsg_get_payload(nlh);

	nft_genid = ntohs(nfh->res_id);

	return MNL_CB_OK;
}

uint16_t mnl_genid_get(struct netlink_ctx *ctx)
{
	char buf[MNL_SOCKET_BUFFER_SIZE];
	struct nlmsghdr *nlh;

	nlh = nftnl_nlmsg_build_hdr(buf, NFT_MSG_GETGEN, AF_UNSPEC, 0, ctx->seqnum);
	/* Skip error checking, old kernels sets res_id field to zero. */
	nft_mnl_talk(ctx, nlh, nlh->nlmsg_len, genid_cb, NULL);

	return nft_genid;
}

static int check_genid(const struct nlmsghdr *nlh)
{
	struct nfgenmsg *nfh = mnl_nlmsg_get_payload(nlh);

	if (nft_genid != ntohs(nfh->res_id)) {
		errno = EINTR;
		return -1;
	}
	return 0;
}

/*
 * Batching
 */

/* selected batch page is 256 Kbytes long to load ruleset of
 * half a million rules without hitting -EMSGSIZE due to large
 * iovec.
 */
#define BATCH_PAGE_SIZE getpagesize() * 32

struct nftnl_batch *mnl_batch_init(void)
{
	struct nftnl_batch *batch;

	batch = nftnl_batch_alloc(BATCH_PAGE_SIZE, NFT_NLMSG_MAXSIZE);
	if (batch == NULL)
		memory_allocation_error();

	return batch;
}

static void mnl_nft_batch_continue(struct nftnl_batch *batch)
{
	if (nftnl_batch_update(batch) < 0)
		memory_allocation_error();
}

uint32_t mnl_batch_begin(struct nftnl_batch *batch, uint32_t seqnum)
{
	nftnl_batch_begin(nftnl_batch_buffer(batch), seqnum);
	mnl_nft_batch_continue(batch);

	return seqnum;
}

void mnl_batch_end(struct nftnl_batch *batch, uint32_t seqnum)
{
	nftnl_batch_end(nftnl_batch_buffer(batch), seqnum);
	mnl_nft_batch_continue(batch);
}

bool mnl_batch_ready(struct nftnl_batch *batch)
{
	/* Check if the batch only contains the initial and trailing batch
	 * messages. In that case, the batch is empty.
	 */
	return nftnl_batch_buffer_len(batch) !=
	       (NLMSG_HDRLEN + sizeof(struct nfgenmsg)) * 2;
}

void mnl_batch_reset(struct nftnl_batch *batch)
{
	nftnl_batch_free(batch);
}

static void mnl_err_list_node_add(struct list_head *err_list, int error,
				  int seqnum)
{
	struct mnl_err *err = xmalloc(sizeof(struct mnl_err));

	err->seqnum = seqnum;
	err->err = error;
	list_add_tail(&err->head, err_list);
}

void mnl_err_list_free(struct mnl_err *err)
{
	list_del(&err->head);
	xfree(err);
}

static int nlbuffsiz;

static void mnl_set_sndbuffer(const struct mnl_socket *nl,
			      struct nftnl_batch *batch)
{
	int newbuffsiz;

	if (nftnl_batch_iovec_len(batch) * BATCH_PAGE_SIZE <= nlbuffsiz)
		return;

	newbuffsiz = nftnl_batch_iovec_len(batch) * BATCH_PAGE_SIZE;

	/* Rise sender buffer length to avoid hitting -EMSGSIZE */
	if (setsockopt(mnl_socket_get_fd(nl), SOL_SOCKET, SO_SNDBUFFORCE,
		       &newbuffsiz, sizeof(socklen_t)) < 0)
		return;

	nlbuffsiz = newbuffsiz;
}

static ssize_t mnl_nft_socket_sendmsg(const struct netlink_ctx *ctx)
{
	static const struct sockaddr_nl snl = {
		.nl_family = AF_NETLINK
	};
	uint32_t iov_len = nftnl_batch_iovec_len(ctx->batch);
	struct iovec iov[iov_len];
	struct msghdr msg = {
		.msg_name	= (struct sockaddr *) &snl,
		.msg_namelen	= sizeof(snl),
		.msg_iov	= iov,
		.msg_iovlen	= iov_len,
	};
	uint32_t i;

	mnl_set_sndbuffer(ctx->nf_sock, ctx->batch);
	nftnl_batch_iovec(ctx->batch, iov, iov_len);

	for (i = 0; i < iov_len; i++) {
		if (ctx->debug_mask & NFT_DEBUG_MNL) {
			mnl_nlmsg_fprintf(ctx->octx->output_fp,
					  iov[i].iov_base, iov[i].iov_len,
					  sizeof(struct nfgenmsg));
		}
	}

	return sendmsg(mnl_socket_get_fd(ctx->nf_sock), &msg, 0);
}

int mnl_batch_talk(struct netlink_ctx *ctx, struct list_head *err_list)
{
	struct mnl_socket *nl = ctx->nf_sock;
	int ret, fd = mnl_socket_get_fd(nl), portid = mnl_socket_get_portid(nl);
	char rcv_buf[MNL_SOCKET_BUFFER_SIZE];
	fd_set readfds;
	struct timeval tv = {
		.tv_sec		= 0,
		.tv_usec	= 0
	};
	int err = 0;

	ret = mnl_nft_socket_sendmsg(ctx);
	if (ret == -1)
		return -1;

	FD_ZERO(&readfds);
	FD_SET(fd, &readfds);

	/* receive and digest all the acknowledgments from the kernel. */
	ret = select(fd+1, &readfds, NULL, NULL, &tv);
	if (ret == -1)
		return -1;

	while (ret > 0 && FD_ISSET(fd, &readfds)) {
		struct nlmsghdr *nlh = (struct nlmsghdr *)rcv_buf;

		ret = mnl_socket_recvfrom(nl, rcv_buf, sizeof(rcv_buf));
		if (ret == -1)
			return -1;

		ret = mnl_cb_run(rcv_buf, ret, 0, portid, &netlink_echo_callback, ctx);
		/* Continue on error, make sure we get all acknowledgments */
		if (ret == -1) {
			mnl_err_list_node_add(err_list, errno, nlh->nlmsg_seq);
			err = -1;
		}

		ret = select(fd+1, &readfds, NULL, NULL, &tv);
		if (ret == -1)
			return -1;

		FD_ZERO(&readfds);
		FD_SET(fd, &readfds);
	}
	return err;
}

int mnl_nft_rule_batch_add(struct nftnl_rule *nlr, struct nftnl_batch *batch,
			   unsigned int flags, uint32_t seqnum)
{
	struct nlmsghdr *nlh;

	nlh = nftnl_nlmsg_build_hdr(nftnl_batch_buffer(batch),
				    NFT_MSG_NEWRULE,
				    nftnl_rule_get_u32(nlr, NFTNL_RULE_FAMILY),
				    NLM_F_CREATE | flags, seqnum);
	nftnl_rule_nlmsg_build_payload(nlh, nlr);
	mnl_nft_batch_continue(batch);

	return 0;
}

int mnl_nft_rule_batch_replace(struct nftnl_rule *nlr, struct nftnl_batch *batch,
			       unsigned int flags, uint32_t seqnum)
{
	struct nlmsghdr *nlh;

	nlh = nftnl_nlmsg_build_hdr(nftnl_batch_buffer(batch),
				    NFT_MSG_NEWRULE,
				    nftnl_rule_get_u32(nlr, NFTNL_RULE_FAMILY),
				    NLM_F_REPLACE | flags, seqnum);
	nftnl_rule_nlmsg_build_payload(nlh, nlr);
	mnl_nft_batch_continue(batch);

	return 0;
}

int mnl_nft_rule_batch_del(struct nftnl_rule *nlr, struct nftnl_batch *batch,
			   unsigned int flags, uint32_t seqnum)
{
	struct nlmsghdr *nlh;

	nlh = nftnl_nlmsg_build_hdr(nftnl_batch_buffer(batch),
				    NFT_MSG_DELRULE,
				    nftnl_rule_get_u32(nlr, NFTNL_RULE_FAMILY),
				    0, seqnum);
	nftnl_rule_nlmsg_build_payload(nlh, nlr);
	mnl_nft_batch_continue(batch);

	return 0;
}

/*
 * Rule
 */

static int rule_cb(const struct nlmsghdr *nlh, void *data)
{
	struct nftnl_rule_list *nlr_list = data;
	struct nftnl_rule *r;

	if (check_genid(nlh) < 0)
		return MNL_CB_ERROR;

	r = nftnl_rule_alloc();
	if (r == NULL)
		memory_allocation_error();

	if (nftnl_rule_nlmsg_parse(nlh, r) < 0)
		goto err_free;

	nftnl_rule_list_add_tail(r, nlr_list);
	return MNL_CB_OK;

err_free:
	nftnl_rule_free(r);
	return MNL_CB_OK;
}

struct nftnl_rule_list *mnl_nft_rule_dump(struct netlink_ctx *ctx,
					  int family)
{
	char buf[MNL_SOCKET_BUFFER_SIZE];
	struct nftnl_rule_list *nlr_list;
	struct nlmsghdr *nlh;
	int ret;

	nlr_list = nftnl_rule_list_alloc();
	if (nlr_list == NULL)
		memory_allocation_error();

	nlh = nftnl_nlmsg_build_hdr(buf, NFT_MSG_GETRULE, family,
				    NLM_F_DUMP, ctx->seqnum);

	ret = nft_mnl_talk(ctx, nlh, nlh->nlmsg_len, rule_cb, nlr_list);
	if (ret < 0)
		goto err;

	return nlr_list;
err:
	nftnl_rule_list_free(nlr_list);
	return NULL;
}

/*
 * Chain
 */
int mnl_nft_chain_add(struct netlink_ctx *ctx, struct nftnl_chain *nlc,
		      unsigned int flags)

{
	char buf[MNL_SOCKET_BUFFER_SIZE];
	struct nlmsghdr *nlh;

	nlh = nftnl_nlmsg_build_hdr(buf, NFT_MSG_NEWCHAIN,
				    nftnl_chain_get_u32(nlc, NFTNL_CHAIN_FAMILY),
				    NLM_F_CREATE | NLM_F_ACK | flags, ctx->seqnum);
	nftnl_chain_nlmsg_build_payload(nlh, nlc);

	return nft_mnl_talk(ctx, nlh, nlh->nlmsg_len, NULL, NULL);
}

int mnl_nft_chain_batch_add(struct nftnl_chain *nlc, struct nftnl_batch *batch,
			    unsigned int flags, uint32_t seqnum)
{
	struct nlmsghdr *nlh;

	nlh = nftnl_nlmsg_build_hdr(nftnl_batch_buffer(batch),
				    NFT_MSG_NEWCHAIN,
				    nftnl_chain_get_u32(nlc, NFTNL_CHAIN_FAMILY),
				    NLM_F_CREATE | flags, seqnum);
	nftnl_chain_nlmsg_build_payload(nlh, nlc);
	mnl_nft_batch_continue(batch);

	return 0;
}

int mnl_nft_chain_delete(struct netlink_ctx *ctx, struct nftnl_chain *nlc,
			 unsigned int flags)
{
	char buf[MNL_SOCKET_BUFFER_SIZE];
	struct nlmsghdr *nlh;

	nlh = nftnl_nlmsg_build_hdr(buf, NFT_MSG_DELCHAIN,
				    nftnl_chain_get_u32(nlc, NFTNL_CHAIN_FAMILY),
				    NLM_F_ACK, ctx->seqnum);
	nftnl_chain_nlmsg_build_payload(nlh, nlc);

	return nft_mnl_talk(ctx, nlh, nlh->nlmsg_len, NULL, NULL);
}

int mnl_nft_chain_batch_del(struct nftnl_chain *nlc, struct nftnl_batch *batch,
			    unsigned int flags, uint32_t seqnum)
{
	struct nlmsghdr *nlh;

	nlh = nftnl_nlmsg_build_hdr(nftnl_batch_buffer(batch),
				    NFT_MSG_DELCHAIN,
				    nftnl_chain_get_u32(nlc, NFTNL_CHAIN_FAMILY),
				    NLM_F_ACK, seqnum);
	nftnl_chain_nlmsg_build_payload(nlh, nlc);
	mnl_nft_batch_continue(batch);

	return 0;
}

static int chain_cb(const struct nlmsghdr *nlh, void *data)
{
	struct nftnl_chain_list *nlc_list = data;
	struct nftnl_chain *c;

	if (check_genid(nlh) < 0)
		return MNL_CB_ERROR;

	c = nftnl_chain_alloc();
	if (c == NULL)
		memory_allocation_error();

	if (nftnl_chain_nlmsg_parse(nlh, c) < 0)
		goto err_free;

	nftnl_chain_list_add_tail(c, nlc_list);
	return MNL_CB_OK;

err_free:
	nftnl_chain_free(c);
	return MNL_CB_OK;
}

struct nftnl_chain_list *mnl_nft_chain_dump(struct netlink_ctx *ctx,
					    int family)
{
	char buf[MNL_SOCKET_BUFFER_SIZE];
	struct nftnl_chain_list *nlc_list;
	struct nlmsghdr *nlh;
	int ret;

	nlc_list = nftnl_chain_list_alloc();
	if (nlc_list == NULL)
		memory_allocation_error();

	nlh = nftnl_nlmsg_build_hdr(buf, NFT_MSG_GETCHAIN, family,
				    NLM_F_DUMP, ctx->seqnum);

	ret = nft_mnl_talk(ctx, nlh, nlh->nlmsg_len, chain_cb, nlc_list);
	if (ret < 0)
		goto err;

	return nlc_list;
err:
	nftnl_chain_list_free(nlc_list);
	return NULL;
}

/*
 * Table
 */
int mnl_nft_table_add(struct netlink_ctx *ctx, struct nftnl_table *nlt,
		      unsigned int flags)
{
	char buf[MNL_SOCKET_BUFFER_SIZE];
	struct nlmsghdr *nlh;

	nlh = nftnl_nlmsg_build_hdr(buf, NFT_MSG_NEWTABLE,
				    nftnl_table_get_u32(nlt, NFTNL_TABLE_FAMILY),
				    NLM_F_ACK | flags, ctx->seqnum);
	nftnl_table_nlmsg_build_payload(nlh, nlt);

	return nft_mnl_talk(ctx, nlh, nlh->nlmsg_len, NULL, NULL);
}

int mnl_nft_table_batch_add(struct nftnl_table *nlt, struct nftnl_batch *batch,
			    unsigned int flags, uint32_t seqnum)
{
	struct nlmsghdr *nlh;

	nlh = nftnl_nlmsg_build_hdr(nftnl_batch_buffer(batch),
				    NFT_MSG_NEWTABLE,
				    nftnl_table_get_u32(nlt, NFTNL_TABLE_FAMILY),
				    flags, seqnum);
	nftnl_table_nlmsg_build_payload(nlh, nlt);
	mnl_nft_batch_continue(batch);

	return 0;
}

int mnl_nft_table_delete(struct netlink_ctx *ctx, struct nftnl_table *nlt,
			 unsigned int flags)
{
	char buf[MNL_SOCKET_BUFFER_SIZE];
	struct nlmsghdr *nlh;

	nlh = nftnl_nlmsg_build_hdr(buf, NFT_MSG_DELTABLE,
				    nftnl_table_get_u32(nlt, NFTNL_TABLE_FAMILY),
				    NLM_F_ACK, ctx->seqnum);
	nftnl_table_nlmsg_build_payload(nlh, nlt);

	return nft_mnl_talk(ctx, nlh, nlh->nlmsg_len, NULL, NULL);
}

int mnl_nft_table_batch_del(struct nftnl_table *nlt, struct nftnl_batch *batch,
			    unsigned int flags, uint32_t seqnum)
{
	struct nlmsghdr *nlh;

	nlh = nftnl_nlmsg_build_hdr(nftnl_batch_buffer(batch),
				    NFT_MSG_DELTABLE,
				    nftnl_table_get_u32(nlt, NFTNL_TABLE_FAMILY),
				    NLM_F_ACK, seqnum);
	nftnl_table_nlmsg_build_payload(nlh, nlt);
	mnl_nft_batch_continue(batch);

	return 0;
}

static int table_cb(const struct nlmsghdr *nlh, void *data)
{
	struct nftnl_table_list *nlt_list = data;
	struct nftnl_table *t;

	if (check_genid(nlh) < 0)
		return MNL_CB_ERROR;

	t = nftnl_table_alloc();
	if (t == NULL)
		memory_allocation_error();

	if (nftnl_table_nlmsg_parse(nlh, t) < 0)
		goto err_free;

	nftnl_table_list_add_tail(t, nlt_list);
	return MNL_CB_OK;

err_free:
	nftnl_table_free(t);
	return MNL_CB_OK;
}

struct nftnl_table_list *mnl_nft_table_dump(struct netlink_ctx *ctx,
					    int family)
{
	char buf[MNL_SOCKET_BUFFER_SIZE];
	struct nftnl_table_list *nlt_list;
	struct nlmsghdr *nlh;
	int ret;

	nlt_list = nftnl_table_list_alloc();
	if (nlt_list == NULL)
		return NULL;

	nlh = nftnl_nlmsg_build_hdr(buf, NFT_MSG_GETTABLE, family,
				    NLM_F_DUMP, ctx->seqnum);

	ret = nft_mnl_talk(ctx, nlh, nlh->nlmsg_len, table_cb, nlt_list);
	if (ret < 0)
		goto err;

	return nlt_list;
err:
	nftnl_table_list_free(nlt_list);
	return NULL;
}

/*
 * Set
 */
static int set_add_cb(const struct nlmsghdr *nlh, void *data)
{
	nftnl_set_nlmsg_parse(nlh, data);
	return MNL_CB_OK;
}

int mnl_nft_set_add(struct netlink_ctx *ctx, struct nftnl_set *nls, unsigned int flags)
{
	char buf[MNL_SOCKET_BUFFER_SIZE];
	struct nlmsghdr *nlh;

	nlh = nftnl_nlmsg_build_hdr(buf, NFT_MSG_NEWSET,
				    nftnl_set_get_u32(nls, NFTNL_SET_FAMILY),
				    NLM_F_CREATE | NLM_F_ACK | flags,
				    ctx->seqnum);
	nftnl_set_nlmsg_build_payload(nlh, nls);

	return nft_mnl_talk(ctx, nlh, nlh->nlmsg_len, set_add_cb, nls);
}

int mnl_nft_set_delete(struct netlink_ctx *ctx, struct nftnl_set *nls,
		       unsigned int flags)
{
	char buf[MNL_SOCKET_BUFFER_SIZE];
	struct nlmsghdr *nlh;

	nlh = nftnl_nlmsg_build_hdr(buf, NFT_MSG_DELSET,
				    nftnl_set_get_u32(nls, NFTNL_SET_FAMILY),
				    flags | NLM_F_ACK, ctx->seqnum);
	nftnl_set_nlmsg_build_payload(nlh, nls);

	return nft_mnl_talk(ctx, nlh, nlh->nlmsg_len, NULL, NULL);
}

int mnl_nft_set_batch_add(struct nftnl_set *nls, struct nftnl_batch *batch,
			  unsigned int flags, uint32_t seqnum)
{
	struct nlmsghdr *nlh;

	nlh = nftnl_nlmsg_build_hdr(nftnl_batch_buffer(batch),
				    NFT_MSG_NEWSET,
				    nftnl_set_get_u32(nls, NFTNL_SET_FAMILY),
				    NLM_F_CREATE | flags, seqnum);
	nftnl_set_nlmsg_build_payload(nlh, nls);
	mnl_nft_batch_continue(batch);

	return 0;
}

int mnl_nft_set_batch_del(struct nftnl_set *nls, struct nftnl_batch *batch,
			  unsigned int flags, uint32_t seqnum)
{
	struct nlmsghdr *nlh;

	nlh = nftnl_nlmsg_build_hdr(nftnl_batch_buffer(batch),
				    NFT_MSG_DELSET,
				    nftnl_set_get_u32(nls, NFTNL_SET_FAMILY),
				    flags, seqnum);
	nftnl_set_nlmsg_build_payload(nlh, nls);
	mnl_nft_batch_continue(batch);

	return 0;
}

static int set_cb(const struct nlmsghdr *nlh, void *data)
{
	struct nftnl_set_list *nls_list = data;
	struct nftnl_set *s;

	if (check_genid(nlh) < 0)
		return MNL_CB_ERROR;

	s = nftnl_set_alloc();
	if (s == NULL)
		memory_allocation_error();

	if (nftnl_set_nlmsg_parse(nlh, s) < 0)
		goto err_free;

	nftnl_set_list_add_tail(s, nls_list);
	return MNL_CB_OK;

err_free:
	nftnl_set_free(s);
	return MNL_CB_OK;
}

struct nftnl_set_list *
mnl_nft_set_dump(struct netlink_ctx *ctx, int family, const char *table)
{
	char buf[MNL_SOCKET_BUFFER_SIZE];
	struct nftnl_set_list *nls_list;
	struct nlmsghdr *nlh;
	struct nftnl_set *s;
	int ret;

	s = nftnl_set_alloc();
	if (s == NULL)
		memory_allocation_error();

	nlh = nftnl_nlmsg_build_hdr(buf, NFT_MSG_GETSET, family,
				    NLM_F_DUMP | NLM_F_ACK, ctx->seqnum);
	if (table != NULL)
		nftnl_set_set(s, NFTNL_SET_TABLE, table);
	nftnl_set_nlmsg_build_payload(nlh, s);
	nftnl_set_free(s);

	nls_list = nftnl_set_list_alloc();
	if (nls_list == NULL)
		memory_allocation_error();

	ret = nft_mnl_talk(ctx, nlh, nlh->nlmsg_len, set_cb, nls_list);
	if (ret < 0)
		goto err;

	return nls_list;
err:
	nftnl_set_list_free(nls_list);
	return NULL;
}

int mnl_nft_obj_batch_add(struct nftnl_obj *nln, struct nftnl_batch *batch,
			  unsigned int flags, uint32_t seqnum)
{
	struct nlmsghdr *nlh;

	nlh = nftnl_nlmsg_build_hdr(nftnl_batch_buffer(batch),
				    NFT_MSG_NEWOBJ,
				    nftnl_obj_get_u32(nln, NFTNL_OBJ_FAMILY),
				    NLM_F_CREATE | flags, seqnum);
	nftnl_obj_nlmsg_build_payload(nlh, nln);
	mnl_nft_batch_continue(batch);

	return 0;
}

int mnl_nft_obj_batch_del(struct nftnl_obj *nln, struct nftnl_batch *batch,
			  unsigned int flags, uint32_t seqnum)
{
	struct nlmsghdr *nlh;

	nlh = nftnl_nlmsg_build_hdr(nftnl_batch_buffer(batch),
				    NFT_MSG_DELOBJ,
				    nftnl_obj_get_u32(nln, NFTNL_OBJ_FAMILY),
				    flags, seqnum);
	nftnl_obj_nlmsg_build_payload(nlh, nln);
	mnl_nft_batch_continue(batch);

	return 0;
}

static int obj_cb(const struct nlmsghdr *nlh, void *data)
{
	struct nftnl_obj_list *nln_list = data;
	struct nftnl_obj *n;

	if (check_genid(nlh) < 0)
		return MNL_CB_ERROR;

	n = nftnl_obj_alloc();
	if (n == NULL)
		memory_allocation_error();

	if (nftnl_obj_nlmsg_parse(nlh, n) < 0)
		goto err_free;

	nftnl_obj_list_add_tail(n, nln_list);
	return MNL_CB_OK;

err_free:
	nftnl_obj_free(n);
	return MNL_CB_OK;
}


struct nftnl_obj_list *
mnl_nft_obj_dump(struct netlink_ctx *ctx, int family,
		 const char *table, const char *name,  uint32_t type, bool dump,
		 bool reset)
{
	uint16_t nl_flags = dump ? NLM_F_DUMP : 0;
	struct nftnl_obj_list *nln_list;
	char buf[MNL_SOCKET_BUFFER_SIZE];
	struct nlmsghdr *nlh;
	struct nftnl_obj *n;
	int msg_type, ret;

	if (reset)
		msg_type = NFT_MSG_GETOBJ_RESET;
	else
		msg_type = NFT_MSG_GETOBJ;

	n = nftnl_obj_alloc();
	if (n == NULL)
		memory_allocation_error();

	nlh = nftnl_nlmsg_build_hdr(buf, msg_type, family,
				    nl_flags | NLM_F_ACK, ctx->seqnum);
	if (table != NULL)
		nftnl_obj_set_str(n, NFTNL_OBJ_TABLE, table);
	if (name != NULL)
		nftnl_obj_set_str(n, NFTNL_OBJ_NAME, name);
	if (type != NFT_OBJECT_UNSPEC)
		nftnl_obj_set_u32(n, NFTNL_OBJ_TYPE, type);
	nftnl_obj_nlmsg_build_payload(nlh, n);
	nftnl_obj_free(n);

	nln_list = nftnl_obj_list_alloc();
	if (nln_list == NULL)
		memory_allocation_error();

	ret = nft_mnl_talk(ctx, nlh, nlh->nlmsg_len, obj_cb, nln_list);
	if (ret < 0)
		goto err;

	return nln_list;
err:
	nftnl_obj_list_free(nln_list);
	return NULL;
}

/*
 * Set elements
 */
int mnl_nft_setelem_add(struct netlink_ctx *ctx, struct nftnl_set *nls,
			unsigned int flags)
{
	char buf[NFT_NLMSG_MAXSIZE];
	struct nftnl_set_elems_iter *iter;
	struct nlmsghdr *nlh;
	int ret, err = 0;

	iter = nftnl_set_elems_iter_create(nls);
	if (iter == NULL)
		memory_allocation_error();

	while (nftnl_set_elems_iter_cur(iter)) {
		nlh = nftnl_nlmsg_build_hdr(buf, NFT_MSG_NEWSETELEM,
					    nftnl_set_get_u32(nls, NFTNL_SET_FAMILY),
					    NLM_F_CREATE | NLM_F_ACK | flags,
					    ctx->seqnum);
		ret = nftnl_set_elems_nlmsg_build_payload_iter(nlh, iter);
		err = nft_mnl_talk(ctx, nlh, nlh->nlmsg_len, NULL, NULL);
		if (ret <= 0 || err < 0)
			break;
	}

	nftnl_set_elems_iter_destroy(iter);

	return err;
}

int mnl_nft_setelem_delete(struct netlink_ctx *ctx, struct nftnl_set *nls,
			   unsigned int flags)
{
	char buf[NFT_NLMSG_MAXSIZE];
	struct nlmsghdr *nlh;

	nlh = nftnl_nlmsg_build_hdr(buf, NFT_MSG_DELSETELEM,
				    nftnl_set_get_u32(nls, NFTNL_SET_FAMILY),
				    NLM_F_ACK, ctx->seqnum);
	nftnl_set_elems_nlmsg_build_payload(nlh, nls);

	return nft_mnl_talk(ctx, nlh, nlh->nlmsg_len, NULL, NULL);
}

static int set_elem_cb(const struct nlmsghdr *nlh, void *data)
{
	if (check_genid(nlh) < 0)
		return MNL_CB_ERROR;

	nftnl_set_elems_nlmsg_parse(nlh, data);
	return MNL_CB_OK;
}

static int mnl_nft_setelem_batch(struct nftnl_set *nls,
				 struct nftnl_batch *batch,
				 enum nf_tables_msg_types cmd,
				 unsigned int flags, uint32_t seqnum)
{
	struct nlmsghdr *nlh;
	struct nftnl_set_elems_iter *iter;
	int ret;

	iter = nftnl_set_elems_iter_create(nls);
	if (iter == NULL)
		memory_allocation_error();

	if (cmd == NFT_MSG_NEWSETELEM)
		flags |= NLM_F_CREATE;

	while (nftnl_set_elems_iter_cur(iter)) {
		nlh = nftnl_nlmsg_build_hdr(nftnl_batch_buffer(batch), cmd,
					    nftnl_set_get_u32(nls, NFTNL_SET_FAMILY),
					    flags, seqnum);
		ret = nftnl_set_elems_nlmsg_build_payload_iter(nlh, iter);
		mnl_nft_batch_continue(batch);
		if (ret <= 0)
			break;
	}

	nftnl_set_elems_iter_destroy(iter);

	return 0;
}

int mnl_nft_setelem_batch_add(struct nftnl_set *nls, struct nftnl_batch *batch,
			      unsigned int flags, uint32_t seqnum)
{
	return mnl_nft_setelem_batch(nls, batch, NFT_MSG_NEWSETELEM, flags,
				     seqnum);
}

int mnl_nft_setelem_batch_flush(struct nftnl_set *nls, struct nftnl_batch *batch,
				unsigned int flags, uint32_t seqnum)
{
	struct nlmsghdr *nlh;

	nlh = nftnl_nlmsg_build_hdr(nftnl_batch_buffer(batch),
				    NFT_MSG_DELSETELEM,
				    nftnl_set_get_u32(nls, NFTNL_SET_FAMILY),
				    flags, seqnum);
	nftnl_set_elems_nlmsg_build_payload(nlh, nls);
	mnl_nft_batch_continue(batch);

	return 0;
}

int mnl_nft_setelem_batch_del(struct nftnl_set *nls, struct nftnl_batch *batch,
			      unsigned int flags, uint32_t seqnum)
{
	return mnl_nft_setelem_batch(nls, batch, NFT_MSG_DELSETELEM, flags,
				     seqnum);
}

int mnl_nft_setelem_get(struct netlink_ctx *ctx, struct nftnl_set *nls)
{
	char buf[MNL_SOCKET_BUFFER_SIZE];
	struct nlmsghdr *nlh;

	nlh = nftnl_nlmsg_build_hdr(buf, NFT_MSG_GETSETELEM,
				    nftnl_set_get_u32(nls, NFTNL_SET_FAMILY),
				    NLM_F_DUMP|NLM_F_ACK, ctx->seqnum);
	nftnl_set_nlmsg_build_payload(nlh, nls);

	return nft_mnl_talk(ctx, nlh, nlh->nlmsg_len, set_elem_cb, nls);
}

/*
 * ruleset
 */
struct nftnl_ruleset *mnl_nft_ruleset_dump(struct netlink_ctx *ctx,
					   uint32_t family)
{
	struct nftnl_ruleset *rs;
	struct nftnl_table_list *t;
	struct nftnl_chain_list *c;
	struct nftnl_set_list *sl;
	struct nftnl_set_list_iter *i;
	struct nftnl_set *s;
	struct nftnl_rule_list *r;
	int ret = 0;

	rs = nftnl_ruleset_alloc();
	if (rs == NULL)
		memory_allocation_error();

	t = mnl_nft_table_dump(ctx, family);
	if (t == NULL)
		goto err;

	nftnl_ruleset_set(rs, NFTNL_RULESET_TABLELIST, t);

	c = mnl_nft_chain_dump(ctx, family);
	if (c == NULL)
		goto err;

	nftnl_ruleset_set(rs, NFTNL_RULESET_CHAINLIST, c);

	sl = mnl_nft_set_dump(ctx, family, NULL);
	if (sl == NULL)
		goto err;

	i = nftnl_set_list_iter_create(sl);
	s = nftnl_set_list_iter_next(i);
	while (s != NULL) {
		ret = mnl_nft_setelem_get(ctx, s);
		if (ret < 0)
			goto err;

		s = nftnl_set_list_iter_next(i);
	}
	nftnl_set_list_iter_destroy(i);

	nftnl_ruleset_set(rs, NFTNL_RULESET_SETLIST, sl);

	r = mnl_nft_rule_dump(ctx, family);
	if (r == NULL)
		goto err;

	nftnl_ruleset_set(rs, NFTNL_RULESET_RULELIST, r);

	return rs;
err:
	nftnl_ruleset_free(rs);
	return NULL;
}

/*
 * events
 */
#define NFTABLES_NLEVENT_BUFSIZ	(1 << 24)

int mnl_nft_event_listener(struct mnl_socket *nf_sock, unsigned int debug_mask,
			   struct output_ctx *octx,
			   int (*cb)(const struct nlmsghdr *nlh, void *data),
			   void *cb_data)
{
	/* Set netlink socket buffer size to 16 Mbytes to reduce chances of
 	 * message loss due to ENOBUFS.
	 */
	unsigned int bufsiz = NFTABLES_NLEVENT_BUFSIZ;
	int fd = mnl_socket_get_fd(nf_sock);
	char buf[NFT_NLMSG_MAXSIZE];
	fd_set readfds;
	int ret;

	ret = setsockopt(fd, SOL_SOCKET, SO_RCVBUFFORCE, &bufsiz,
			 sizeof(socklen_t));
	if (ret < 0) {
		/* If this doesn't work, try to reach the system wide maximum
		 * (or whatever the user requested).
		 */
		ret = setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufsiz,
				 sizeof(socklen_t));
		nft_print(octx, "# Cannot set up netlink socket buffer size to %u bytes, falling back to %u bytes\n",
			  NFTABLES_NLEVENT_BUFSIZ, bufsiz);
	}

	while (1) {
		FD_ZERO(&readfds);
		FD_SET(fd, &readfds);

		ret = select(fd + 1, &readfds, NULL, NULL, NULL);
		if (ret < 0)
			return -1;

		if (FD_ISSET(fd, &readfds)) {
			ret = mnl_socket_recvfrom(nf_sock, buf, sizeof(buf));
			if (ret < 0) {
				if (errno == ENOBUFS) {
					nft_print(octx, "# ERROR: We lost some netlink events!\n");
					continue;
				}
				nft_print(octx, "# ERROR: %s\n",
					  strerror(errno));
				break;
			}
		}

		if (debug_mask & NFT_DEBUG_MNL) {
			mnl_nlmsg_fprintf(octx->output_fp, buf, sizeof(buf),
					  sizeof(struct nfgenmsg));
		}
		ret = mnl_cb_run(buf, ret, 0, 0, cb, cb_data);
		if (ret <= 0)
			break;
	}
	return ret;
}

static void nft_mnl_batch_put(char *buf, uint16_t type, uint32_t seqnum)
{
	struct nlmsghdr *nlh;
	struct nfgenmsg *nfg;

	nlh = mnl_nlmsg_put_header(buf);
	nlh->nlmsg_type = type;
	nlh->nlmsg_flags = NLM_F_REQUEST;
	nlh->nlmsg_seq = seqnum;

	nfg = mnl_nlmsg_put_extra_header(nlh, sizeof(*nfg));
	nfg->nfgen_family = AF_INET;
	nfg->version = NFNETLINK_V0;
	nfg->res_id = NFNL_SUBSYS_NFTABLES;
}

bool mnl_batch_supported(struct mnl_socket *nf_sock, uint32_t *seqnum)
{
	struct mnl_nlmsg_batch *b;
	char buf[MNL_SOCKET_BUFFER_SIZE];
	int ret;

	b = mnl_nlmsg_batch_start(buf, sizeof(buf));

	nft_mnl_batch_put(mnl_nlmsg_batch_current(b), NFNL_MSG_BATCH_BEGIN,
			  mnl_seqnum_alloc(seqnum));
	mnl_nlmsg_batch_next(b);

	nftnl_nlmsg_build_hdr(mnl_nlmsg_batch_current(b), NFT_MSG_NEWSET,
			      AF_INET, NLM_F_ACK, mnl_seqnum_alloc(seqnum));
	mnl_nlmsg_batch_next(b);

	nft_mnl_batch_put(mnl_nlmsg_batch_current(b), NFNL_MSG_BATCH_END,
			  mnl_seqnum_alloc(seqnum));
	mnl_nlmsg_batch_next(b);

	ret = mnl_socket_sendto(nf_sock, mnl_nlmsg_batch_head(b),
				mnl_nlmsg_batch_size(b));
	if (ret < 0)
		goto err;

	mnl_nlmsg_batch_stop(b);

	ret = mnl_socket_recvfrom(nf_sock, buf, sizeof(buf));
	while (ret > 0) {
		ret = mnl_cb_run(buf, ret, 0, mnl_socket_get_portid(nf_sock),
				 NULL, NULL);
		if (ret <= 0)
			break;

		ret = mnl_socket_recvfrom(nf_sock, buf, sizeof(buf));
	}

	/* We're sending an incomplete message to see if the kernel supports
	 * set messages in batches. EINVAL means that we sent an incomplete
	 * message with missing attributes. The kernel just ignores messages
	 * that we cannot include in the batch.
	 */
	return (ret == -1 && errno == EINVAL) ? true : false;
err:
	mnl_nlmsg_batch_stop(b);
	return ret;
}
