/*
 * Copyright (c) 2016 Intel Corporation, Inc.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdlib.h>
#include <string.h>

#include <fi_util.h>
#include "rxm.h"

int rxm_msg_ep_open(struct rxm_ep *rxm_ep, struct fi_info *msg_info,
		struct rxm_conn *rxm_conn)
{
	struct rxm_domain *rxm_domain;
	struct rxm_fabric *rxm_fabric;
	struct fid_ep *msg_ep;
	int ret;

	rxm_domain = container_of(rxm_ep->util_ep.domain, struct rxm_domain,
			util_domain);
	rxm_fabric = container_of(rxm_domain->util_domain.fabric, struct rxm_fabric,
			util_fabric);
	ret = fi_endpoint(rxm_domain->msg_domain, msg_info, &msg_ep, rxm_conn);
	if (ret) {
		FI_WARN(&rxm_prov, FI_LOG_EP_CTRL, "Unable to create msg_ep\n");
		return ret;
	}

	ret = fi_ep_bind(msg_ep, &rxm_fabric->msg_eq->fid, 0);
	if (ret) {
		FI_WARN(&rxm_prov, FI_LOG_FABRIC, "Unable to bind msg EP to EQ\n");
		goto err;
	}

	ret = fi_ep_bind(msg_ep, &rxm_ep->srx_ctx->fid, 0);
	if (ret) {
		FI_WARN(&rxm_prov, FI_LOG_FABRIC, "Unable to bind msg EP to shared RX ctx\n");
		goto err;
	}

	// TODO add other completion flags
	ret = fi_ep_bind(msg_ep, &rxm_ep->msg_cq->fid, FI_TRANSMIT | FI_RECV);
	if (ret) {
		FI_WARN(&rxm_prov, FI_LOG_EP_CTRL,
				"Unable to bind msg_ep to msg_cq\n");
		goto err;
	}

	ret = fi_enable(msg_ep);
	if (ret) {
		FI_WARN(&rxm_prov, FI_LOG_EP_CTRL, "Unable to enable msg_ep\n");
		goto err;
	}

	rxm_conn->msg_ep = msg_ep;
	return 0;
err:
	fi_close(&msg_ep->fid);
	return ret;
}

void rxm_conn_close(void *arg)
{
	struct util_cmap_handle *handle = (struct util_cmap_handle *)arg;
	struct rxm_conn *rxm_conn = container_of(handle, struct rxm_conn, handle);
	int ret;

	if ((rxm_conn->handle.state == CMAP_UNSPEC) || !rxm_conn->msg_ep)
		goto out;

	if (rxm_conn->handle.state == CMAP_CONNECTED) {
		ret = fi_shutdown(rxm_conn->msg_ep, 0);
		if (ret)
			FI_WARN(&rxm_prov, FI_LOG_EP_CTRL,
					"Unable to close connection\n");
	}
	ret = fi_close(&rxm_conn->msg_ep->fid);
	if (ret)
		FI_WARN(&rxm_prov, FI_LOG_EP_CTRL,
				"Unable to close msg_ep\n");
out:
	free(rxm_conn);
}

int rxm_msg_process_connreq(struct rxm_ep *rxm_ep, struct fi_info *msg_info,
		void *data)
{
	struct rxm_conn *rxm_conn;
	struct rxm_cm_data *remote_cm_data = data;
	struct rxm_cm_data cm_data;
	int ret;

	if  (!(rxm_conn = calloc(1, sizeof(*rxm_conn)))) {
		ret = -FI_ENOMEM;
		goto err1;
	}

	ret = ofi_cmap_add_handle(rxm_ep->cmap, &rxm_conn->handle, CMAP_CONNECTING,
			FI_ADDR_UNSPEC, &remote_cm_data->name,
			sizeof(remote_cm_data->name));
	if (ret) {
		FI_WARN(&rxm_prov, FI_LOG_FABRIC, "Unable to add handle/peer\n");
		goto err2;
	}

	rxm_conn->handle.remote_key = remote_cm_data->conn_id;

	ret = rxm_msg_ep_open(rxm_ep, msg_info, rxm_conn);
	if (ret)
		goto err2;

	cm_data.conn_id = rxm_conn->handle.key;

	ret = fi_accept(rxm_conn->msg_ep, &cm_data, sizeof(cm_data));
	if (ret) {
		FI_WARN(&rxm_prov, FI_LOG_FABRIC,
				"Unable to accept incoming connection\n");
		goto err2;
	}
	return ret;
err2:
	ofi_cmap_del_handle(&rxm_conn->handle);
err1:
	if (fi_reject(rxm_ep->msg_pep, msg_info->handle, NULL, 0))
		FI_WARN(&rxm_prov, FI_LOG_EP_CTRL,
				"Unable to reject incoming connection\n");
	return ret;
}

static void rxm_msg_process_connect_event(fid_t fid, void *data, size_t datalen)
{
	struct rxm_conn *rxm_conn = (struct rxm_conn *)fid->context;
	struct rxm_cm_data *cm_data;
	ofi_cmap_update_state(&rxm_conn->handle, CMAP_CONNECTED);
	if (datalen) {
		cm_data = data;
		rxm_conn->handle.remote_key = cm_data->conn_id;
	}
}
static void rxm_msg_process_shutdown_event(fid_t fid)
{
	// TODO process shutdown - need to diff local and remote shutdown
	return;
}

void *rxm_msg_listener(void *arg)
{
	struct fi_eq_cm_entry *entry;
	struct fi_eq_err_entry err_entry;
	size_t datalen = sizeof(struct rxm_cm_data);
	size_t len = sizeof(*entry) + datalen;
	struct rxm_fabric *rxm_fabric = (struct rxm_fabric *)arg;
	uint32_t event;
	ssize_t rd;
	int ret;

	entry = calloc(1, len);
	if (!entry) {
		FI_WARN(&rxm_prov, FI_LOG_FABRIC, "Unable to allocate memory!\n");
		return NULL;
	}

	FI_DBG(&rxm_prov, FI_LOG_FABRIC, "Starting MSG listener thread\n");
	while (1) {
		rd = fi_eq_sread(rxm_fabric->msg_eq, &event, entry, len, -1, 0);
		/* We would receive more bytes than sizeof *entry during CONNREQ */
		if (rd < 0) {
			if (rd == -FI_EAVAIL)
				OFI_EQ_READERR(&rxm_prov, FI_LOG_FABRIC,
						rxm_fabric->msg_eq, rd, err_entry);
			else
				FI_WARN(&rxm_prov, FI_LOG_FABRIC,
						"msg: unable to fi_eq_sread\n");
			continue;
		}

		switch(event) {
		case FI_NOTIFY:
			FI_WARN(&rxm_prov, FI_LOG_FABRIC, "Closing rxm msg listener\n");
			return NULL;
		case FI_CONNREQ:
			if (rd != len)
				goto err;
			FI_DBG(&rxm_prov, FI_LOG_FABRIC, "Got new connection\n");
			ret = rxm_msg_process_connreq(entry->fid->context, entry->info,
					entry->data);
			if (ret)
				FI_WARN(&rxm_prov, FI_LOG_FABRIC,
						"Unable to process connection request\n");
			break;
		case FI_CONNECTED:
			FI_DBG(&rxm_prov, FI_LOG_FABRIC, "Connected\n");
			rxm_msg_process_connect_event(entry->fid, entry->data,
					rd - sizeof(*entry));
			break;
		case FI_SHUTDOWN:
			FI_DBG(&rxm_prov, FI_LOG_FABRIC, "Received connection shutdown\n");
			rxm_msg_process_shutdown_event(entry->fid);
			break;
		default:
			FI_WARN(&rxm_prov, FI_LOG_FABRIC, "Unknown event: %u\n", event);
		}
		continue;
err:
		FI_WARN(&rxm_prov, FI_LOG_FABRIC,
				"Received size (%d) not matching expected (%d)\n", rd, len);
	}
}

static int rxm_prepare_cm_data(struct fid_pep *pep, struct util_cmap_handle *handle,
		struct rxm_cm_data *cm_data)
{
	size_t cm_data_size = 0;
	size_t name_size = sizeof(cm_data->name);
	size_t opt_size = sizeof(cm_data_size);
	int ret;

	ret = fi_getopt(&pep->fid, FI_OPT_ENDPOINT, FI_OPT_CM_DATA_SIZE,
			&cm_data_size, &opt_size);
	if (ret) {
		FI_WARN(&rxm_prov, FI_LOG_EP_CTRL, "fi_getopt failed\n");
		return ret;
	}

	if (cm_data_size < sizeof(*cm_data)) {
		FI_WARN(&rxm_prov, FI_LOG_EP_CTRL, "MSG EP CM data size too small\n");
		return -FI_EOTHER;
	}

	ret = fi_getname(&pep->fid, &cm_data->name, &name_size);
	if (ret) {
		FI_WARN(&rxm_prov, FI_LOG_EP_CTRL, "Unable to get msg pep name\n");
		return ret;
	}

	cm_data->conn_id = handle->key;
	return 0;
}

int rxm_msg_connect(struct rxm_ep *rxm_ep, fi_addr_t fi_addr,
		struct fi_info *msg_hints)
{
	struct rxm_conn *rxm_conn;
	struct fi_info *msg_info;
	struct rxm_cm_data cm_data;
	int ret;

	assert(!msg_hints->dest_addr);

	msg_hints->dest_addrlen = rxm_ep->util_ep.av->addrlen;
	msg_hints->dest_addr = mem_dup(ofi_av_get_addr(rxm_ep->util_ep.av,
				fi_addr), msg_hints->dest_addrlen);

	ret = fi_getinfo(rxm_prov.version, NULL, NULL, 0, msg_hints, &msg_info);
	if (ret)
		return ret;

	if  (!(rxm_conn = calloc(1, sizeof(*rxm_conn)))) {
		ret = -FI_ENOMEM;
		goto err1;
	}

	ret = ofi_cmap_add_handle(rxm_ep->cmap, &rxm_conn->handle,
			CMAP_CONNECTING, fi_addr, NULL, 0);
	if (ret)
		goto err2;

	ret = rxm_msg_ep_open(rxm_ep, msg_info, rxm_conn);
	if (ret)
		goto err2;

	/* We have to send passive endpoint's address to the server since the
	 * address from which connection request would be sent would have a
	 * different port. */
	ret = rxm_prepare_cm_data(rxm_ep->msg_pep, &rxm_conn->handle, &cm_data);
	if (ret)
		goto err2;

	ret = fi_connect(rxm_conn->msg_ep, msg_info->dest_addr, &cm_data, sizeof(cm_data));
	if (ret) {
		FI_WARN(&rxm_prov, FI_LOG_EP_CTRL, "Unable to connect msg_ep\n");
		goto err2;
	}
	fi_freeinfo(msg_info);
	return 0;
err2:
	ofi_cmap_del_handle(&rxm_conn->handle);
err1:
	fi_freeinfo(msg_info);
	return ret;
}

int rxm_get_conn(struct rxm_ep *rxm_ep, fi_addr_t fi_addr,
		struct rxm_conn **rxm_conn)
{
	struct util_cmap_handle *handle;

	if (fi_addr > rxm_ep->util_ep.av->count) {
		FI_WARN(&rxm_prov, FI_LOG_EP_CTRL, "Invalid fi_addr\n");
		return -FI_EINVAL;
	}

	handle = ofi_cmap_get_handle(rxm_ep->cmap, fi_addr);
	if (!handle)
		goto connect;

	switch (handle->state) {
	case CMAP_CONNECTING:
		return -FI_EAGAIN;
	case CMAP_CONNECTED:
		*rxm_conn = container_of(handle, struct rxm_conn, handle);
		return 0;
	default:
		/* We shouldn't be here */
		assert(0);
	}

connect:
	if (rxm_msg_connect(rxm_ep, fi_addr, rxm_ep->msg_info)) {
		FI_WARN(&rxm_prov, FI_LOG_EP_DATA, "Unable to connect\n");
		return -FI_EOTHER;
	}
	return -FI_EAGAIN;
}


