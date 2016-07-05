/*
 * transaction.c
 *
 * Copyright (C) 2008-2015 Aerospike, Inc.
 *
 * Portions may be licensed to Aerospike, Inc. under one or more contributor
 * license agreements.
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Affero General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option) any
 * later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for more
 * details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see http://www.gnu.org/licenses/
 */

/*
 * Operations on transactions
 */

#include "base/transaction.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "citrusleaf/alloc.h"
#include "citrusleaf/cf_atomic.h"
#include "citrusleaf/cf_clock.h"
#include "citrusleaf/cf_digest.h"

#include "fault.h"

#include "base/batch.h"
#include "base/datamodel.h"
#include "base/proto.h"
#include "base/scan.h"
#include "base/security.h"
#include "base/thr_demarshal.h"
#include "base/thr_proxy.h"
#include "base/udf_rw.h"


void
as_transaction_init_head(as_transaction *tr, cf_digest *keyd, cl_msg *msgp)
{
	tr->msgp				= msgp;
	tr->msg_fields			= 0;

	tr->origin				= 0;
	tr->from_flags			= 0;

	tr->microbenchmark_is_resolve = false; // will soon be gone

	tr->from.any			= NULL;
	tr->from_data.any		= 0;

	tr->keyd				= keyd ? *keyd : cf_digest_zero;

	tr->start_time			= 0;
	tr->microbenchmark_time	= 0;
}

void
as_transaction_init_body(as_transaction *tr)
{
	AS_PARTITION_RESERVATION_INIT(tr->rsv);

	tr->end_time			= 0;
	tr->result_code			= AS_PROTO_RESULT_OK;
	tr->flags				= 0;
	tr->generation			= 0;
	tr->void_time			= 0;
	tr->last_update_time	= 0;
}

void
as_transaction_copy_head(as_transaction *to, const as_transaction *from)
{
	to->msgp				= from->msgp;
	to->msg_fields			= from->msg_fields;

	to->origin				= from->origin;
	to->from_flags			= from->from_flags;

	to->microbenchmark_is_resolve = false; // will soon be gone

	to->from.any			= from->from.any;
	to->from_data.any		= from->from_data.any;

	to->keyd				= from->keyd;

	to->start_time			= from->start_time;
	to->microbenchmark_time	= from->microbenchmark_time;
}

bool
as_transaction_set_msg_field_flag(as_transaction *tr, uint8_t type)
{
	switch (type) {
	case AS_MSG_FIELD_TYPE_NAMESPACE:
		tr->msg_fields |= AS_MSG_FIELD_BIT_NAMESPACE;
		break;
	case AS_MSG_FIELD_TYPE_SET:
		tr->msg_fields |= AS_MSG_FIELD_BIT_SET;
		break;
	case AS_MSG_FIELD_TYPE_KEY:
		tr->msg_fields |= AS_MSG_FIELD_BIT_KEY;
		break;
	case AS_MSG_FIELD_TYPE_DIGEST_RIPE:
		tr->msg_fields |= AS_MSG_FIELD_BIT_DIGEST_RIPE;
		break;
	case AS_MSG_FIELD_TYPE_DIGEST_RIPE_ARRAY:
		tr->msg_fields |= AS_MSG_FIELD_BIT_DIGEST_RIPE_ARRAY;
		break;
	case AS_MSG_FIELD_TYPE_TRID:
		tr->msg_fields |= AS_MSG_FIELD_BIT_TRID;
		break;
	case AS_MSG_FIELD_TYPE_SCAN_OPTIONS:
		tr->msg_fields |= AS_MSG_FIELD_BIT_SCAN_OPTIONS;
		break;
	case AS_MSG_FIELD_TYPE_INDEX_NAME:
		tr->msg_fields |= AS_MSG_FIELD_BIT_INDEX_NAME;
		break;
	case AS_MSG_FIELD_TYPE_INDEX_RANGE:
		tr->msg_fields |= AS_MSG_FIELD_BIT_INDEX_RANGE;
		break;
	case AS_MSG_FIELD_TYPE_INDEX_TYPE:
		tr->msg_fields |= AS_MSG_FIELD_BIT_INDEX_TYPE;
		break;
	case AS_MSG_FIELD_TYPE_UDF_FILENAME:
		tr->msg_fields |= AS_MSG_FIELD_BIT_UDF_FILENAME;
		break;
	case AS_MSG_FIELD_TYPE_UDF_FUNCTION:
		tr->msg_fields |= AS_MSG_FIELD_BIT_UDF_FUNCTION;
		break;
	case AS_MSG_FIELD_TYPE_UDF_ARGLIST:
		tr->msg_fields |= AS_MSG_FIELD_BIT_UDF_ARGLIST;
		break;
	case AS_MSG_FIELD_TYPE_UDF_OP:
		tr->msg_fields |= AS_MSG_FIELD_BIT_UDF_OP;
		break;
	case AS_MSG_FIELD_TYPE_QUERY_BINLIST:
		tr->msg_fields |= AS_MSG_FIELD_BIT_QUERY_BINLIST;
		break;
	case AS_MSG_FIELD_TYPE_BATCH: // shouldn't get here - batch parent handles this
		tr->msg_fields |= AS_MSG_FIELD_BIT_BATCH;
		break;
	case AS_MSG_FIELD_TYPE_BATCH_WITH_SET: // shouldn't get here - batch parent handles this
		tr->msg_fields |= AS_MSG_FIELD_BIT_BATCH_WITH_SET;
		break;
	default:
		return false;
	}

	return true;
}

// TODO - check m->n_fields against PROTO_NFIELDS_MAX_WARNING?
bool
as_transaction_demarshal_prepare(as_transaction *tr)
{
	uint64_t size = tr->msgp->proto.sz;

	if (size < sizeof(as_msg)) {
		cf_warning(AS_PROTO, "proto body size %lu smaller than as_msg", size);
		return false;
	}

	// The proto data is not smaller than an as_msg - safe to swap header.
	as_msg *m = &tr->msgp->msg;

	as_msg_swap_header(m);

	uint8_t* p_end = (uint8_t*)m + size;
	uint8_t* p_read = m->data;

	// Parse and swap fields first.
	for (uint16_t n = 0; n < m->n_fields; n++) {
		if (p_read + sizeof(as_msg_field) > p_end) {
			cf_warning(AS_PROTO, "incomplete as_msg_field");
			return false;
		}

		as_msg_field* p_field = (as_msg_field*)p_read;

		as_msg_swap_field(p_field);
		p_read = as_msg_field_skip(p_field);

		if (! p_read) {
			cf_warning(AS_PROTO, "bad as_msg_field");
			return false;
		}

		if (p_read > p_end) {
			cf_warning(AS_PROTO, "incomplete as_msg_field value");
			return false;
		}

		// Store which message fields are present - prevents lots of re-parsing.
		if (! as_transaction_set_msg_field_flag(tr, p_field->type)) {
			cf_debug(AS_PROTO, "skipping as_msg_field type %u", p_field->type);
		}
	}

	// Parse and swap bin-ops, if any.
	for (uint16_t n = 0; n < m->n_ops; n++) {
		if (p_read + sizeof(as_msg_op) > p_end) {
			cf_warning(AS_PROTO, "incomplete as_msg_op");
			return false;
		}

		as_msg_op* op = (as_msg_op*)p_read;

		as_msg_swap_op(op);
		p_read = as_msg_op_skip(op);

		if (! p_read) {
			cf_warning(AS_PROTO, "bad as_msg_op");
			return false;
		}

		if (p_read > p_end) {
			cf_warning(AS_PROTO, "incomplete as_msg_op data");
			return false;
		}
	}

	// Temporarily skip the check for extra message bytes, for compatibility
	// with legacy clients.

//	if (p_read != p_end) {
//		cf_warning(AS_PROTO, "extra bytes follow fields and bin-ops");
//		return false;
//	}

	return true;
}

void
as_transaction_proxyee_prepare(as_transaction *tr)
{
	as_msg *m = &tr->msgp->msg;
	as_msg_field* p_field = (as_msg_field*)m->data;

	// Store which message fields are present - prevents lots of re-parsing.
	// Proto header, field sizes already swapped to host order by proxyer.
	for (uint16_t n = 0; n < m->n_fields; n++) {
		if (! as_transaction_set_msg_field_flag(tr, p_field->type)) {
			cf_debug(AS_PROTO, "skipping as_msg_field type %u", p_field->type);
		}

		p_field = as_msg_field_get_next(p_field);
	}
}

// Initialize an internal UDF transaction (for a UDF scan/query). Allocates a
// message with namespace and digest - no set for now, since these transactions
// won't get security checked, and they can't create a record.
int
as_transaction_init_iudf(as_transaction *tr, as_namespace *ns, cf_digest *keyd)
{
	size_t msg_sz = sizeof(cl_msg);
	int ns_len = strlen(ns->name);

	msg_sz += sizeof(as_msg_field) + ns_len;
	msg_sz += sizeof(as_msg_field) + sizeof(cf_digest);

	cl_msg *msgp = (cl_msg *)cf_malloc(msg_sz);

	if (! msgp) {
		return -1;
	}

	uint8_t *b = (uint8_t *)msgp;

	b = as_msg_write_header(b, msg_sz, 0, AS_MSG_INFO2_WRITE, 0, 0, 0, 0, 2, 0);
	b = as_msg_write_fields(b, ns->name, ns_len, NULL, 0, keyd, 0, 0, 0, 0);

	as_transaction_init_head(tr, NULL, msgp);

	as_transaction_set_msg_field_flag(tr, AS_MSG_FIELD_TYPE_NAMESPACE);
	as_transaction_set_msg_field_flag(tr, AS_MSG_FIELD_TYPE_DIGEST_RIPE);

	tr->origin = FROM_IUDF;
	// Caller must set tr->from.iudf_orig immediately afterwards...

	// Do this last, to exclude the setup time in this function.
	tr->start_time = cf_getns();
	MICROBENCHMARK_SET_TO_START_P();

	return 0;
}

void
as_transaction_demarshal_error(as_transaction* tr, uint32_t error_code)
{
	as_msg_send_reply(tr->from.proto_fd_h, error_code, 0, 0, NULL, NULL, 0, NULL, NULL, 0, NULL);
	tr->from.proto_fd_h = NULL;

	cf_free(tr->msgp);
	tr->msgp = NULL;
}

void
as_transaction_error(as_transaction* tr, uint32_t error_code)
{
	if (error_code == 0) {
		cf_warning(AS_PROTO, "converting error code 0 to 1 (unknown)");
		error_code = AS_PROTO_RESULT_FAIL_UNKNOWN;
	}

	// The 'from' checks below should not be necessary, but there's a known race
	// between duplicate-resolution's cluster-key-mismatch handler (which
	// re-queues transactions) and retransmit thread timeouts which can allow a
	// null 'from' to get here. That race will be fixed in a future release, but
	// for now these checks keep us safe.

	switch (tr->origin) {
	case FROM_CLIENT:
		if (tr->from.proto_fd_h) {
			as_msg_send_reply(tr->from.proto_fd_h, error_code, 0, 0, NULL, NULL, 0, NULL, NULL, as_transaction_trid(tr), NULL);
			tr->from.proto_fd_h = NULL; // pattern, not needed
		}
		MICROBENCHMARK_HIST_INSERT_P(error_hist);
		cf_atomic_int_incr(&g_config.err_tsvc_requests);
		if (error_code == AS_PROTO_RESULT_FAIL_TIMEOUT) {
			cf_atomic_int_incr(&g_config.err_tsvc_requests_timeout);
		}
		break;
	case FROM_PROXY:
		if (tr->from.proxy_node != 0) {
			as_proxy_send_response(tr->from.proxy_node, tr->from_data.proxy_tid, error_code, 0, 0, NULL, NULL, 0, NULL, as_transaction_trid(tr), NULL);
			tr->from.proxy_node = 0; // pattern, not needed
		}
		break;
	case FROM_BATCH:
		if (tr->from.batch_shared) {
			as_batch_add_error(tr->from.batch_shared, tr->from_data.batch_index, error_code);
			tr->from.batch_shared = NULL; // pattern, not needed
			tr->msgp = NULL; // pattern, not needed
		}
		break;
	case FROM_IUDF:
		if (tr->from.iudf_orig) {
			tr->from.iudf_orig->cb(tr->from.iudf_orig->udata, error_code);
			tr->from.iudf_orig = NULL; // pattern, not needed
		}
		break;
	case FROM_NSUP:
		break;
	default:
		cf_crash(AS_PROTO, "unexpected transaction origin %u", tr->origin);
		break;
	}
}

// Helper to release transaction file handles.
void
as_release_file_handle(as_file_handle *proto_fd_h)
{
	int rc = cf_rc_release(proto_fd_h);

	if (rc > 0) {
		return;
	}
	else if (rc < 0) {
		cf_warning(AS_PROTO, "release file handle: negative ref-count %d", rc);
		return;
	}

	cf_socket_close(proto_fd_h->sock);
	proto_fd_h->fh_info &= ~FH_INFO_DONOT_REAP;
	SFD(proto_fd_h->sock) = -1;

	if (proto_fd_h->proto)	{
		as_proto *p = proto_fd_h->proto;

		if ((p->version != PROTO_VERSION) || (p->type >= PROTO_TYPE_MAX)) {
			cf_warning(AS_PROTO, "release file handle: bad proto buf, corruption");
		}
		else {
			cf_free(proto_fd_h->proto);
			proto_fd_h->proto = NULL;
		}
	}

	if (proto_fd_h->security_filter) {
		as_security_filter_destroy(proto_fd_h->security_filter);
		proto_fd_h->security_filter = NULL;
	}

	cf_rc_free(proto_fd_h);
	cf_atomic_int_incr(&g_config.proto_connections_closed);
}

void
as_end_of_transaction(as_file_handle *proto_fd_h, bool force_close)
{
	thr_demarshal_resume(proto_fd_h);

	if (force_close) {
		cf_socket_shutdown(proto_fd_h->sock);
	}

	as_release_file_handle(proto_fd_h);
}

void
as_end_of_transaction_ok(as_file_handle *proto_fd_h)
{
	as_end_of_transaction(proto_fd_h, false);
}

void
as_end_of_transaction_force_close(as_file_handle *proto_fd_h)
{
	as_end_of_transaction(proto_fd_h, true);
}
