/**
 * Copyright (c) NVIDIA CORPORATION & AFFILIATES, 2020. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "rma.h"
#include "rma.inl"

#include <ucp/core/ucp_request.inl>
#include <ucp/dt/datatype_iter.inl>
#include <ucp/proto/proto_init.h>
#include <ucp/proto/proto_multi.inl>
#include <ucp/proto/proto_single.inl>

static ucs_status_t ucp_proto_put_offload_short_progress(uct_pending_req_t *self)
{
    ucp_request_t *req                   = ucs_container_of(self, ucp_request_t,
                                                            send.uct);
    ucp_ep_t *ep                         = req->send.ep;
    const ucp_proto_single_priv_t *spriv = req->send.proto_config->priv;
    ucs_status_t status;
    uct_rkey_t tl_rkey;

    if (!(req->flags & UCP_REQUEST_FLAG_PROTO_INITIALIZED)) {
        status = ucp_ep_rma_handle_fence(ep, req, UCS_BIT(spriv->super.lane));
        if (status != UCS_OK) {
            ucp_proto_request_abort(req, status);
            return UCS_OK;
        }

        req->flags |= UCP_REQUEST_FLAG_PROTO_INITIALIZED;
    }

    tl_rkey = ucp_rkey_get_tl_rkey(req->send.rma.rkey, spriv->super.rkey_index);
    status  = uct_ep_put_short(ucp_ep_get_fast_lane(ep, spriv->super.lane),
                               req->send.state.dt_iter.type.contig.buffer,
                               req->send.state.dt_iter.length,
                               req->send.rma.remote_addr, tl_rkey);
    if (ucs_unlikely(status == UCS_ERR_NO_RESOURCE)) {
        req->send.lane = spriv->super.lane; /* for pending add */
        return status;
    }

    /* UCS_INPROGRESS is not expected */
    ucs_assert((status == UCS_OK) || UCS_STATUS_IS_ERR(status));

    ucp_datatype_iter_cleanup(&req->send.state.dt_iter, 0,
                              UCS_BIT(UCP_DATATYPE_CONTIG));
    ucp_request_complete_send(req, status);
    return UCS_OK;
}

static void
ucp_proto_put_offload_short_probe(const ucp_proto_init_params_t *init_params)
{
    ucp_proto_single_init_params_t params = {
        .super.super         = *init_params,
        .super.latency       = 0,
        .super.overhead      = 0,
        .super.cfg_thresh    = UCS_MEMUNITS_AUTO,
        .super.cfg_priority  = 0,
        .super.min_length    = 0,
        .super.max_length    = SIZE_MAX,
        .super.min_iov       = 0,
        .super.min_frag_offs = UCP_PROTO_COMMON_OFFSET_INVALID,
        .super.max_frag_offs = ucs_offsetof(uct_iface_attr_t,
                                            cap.put.max_short),
        .super.max_iov_offs  = UCP_PROTO_COMMON_OFFSET_INVALID,
        .super.hdr_size      = 0,
        .super.send_op       = UCT_EP_OP_PUT_SHORT,
        .super.memtype_op    = UCT_EP_OP_LAST,
        .super.flags         = UCP_PROTO_COMMON_INIT_FLAG_RECV_ZCOPY    |
                               UCP_PROTO_COMMON_INIT_FLAG_REMOTE_ACCESS |
                               UCP_PROTO_COMMON_INIT_FLAG_SINGLE_FRAG   |
                               UCP_PROTO_COMMON_INIT_FLAG_ERR_HANDLING,
        .super.exclude_map   = 0,
        .super.reg_mem_info  = ucp_mem_info_unknown,
        .lane_type           = UCP_LANE_TYPE_RMA,
        .tl_cap_flags        = UCT_IFACE_FLAG_PUT_SHORT
    };

    if (!ucp_proto_init_check_op(init_params, UCS_BIT(UCP_OP_ID_PUT)) ||
        !ucp_proto_is_short_supported(init_params->select_param)) {
        return;
    }

    if ((init_params->rkey_config_key != NULL) &&
        ucp_rkey_need_remote_flush(init_params->rkey_config_key)) {
        return;
    }

    ucp_proto_single_probe(&params);
}

ucp_proto_t ucp_put_offload_short_proto = {
    .name     = "put/offload/short",
    .desc     = UCP_PROTO_SHORT_DESC,
    .flags    = UCP_PROTO_FLAG_PUT_SHORT,
    .probe    = ucp_proto_put_offload_short_probe,
    .query    = ucp_proto_single_query,
    .progress = {ucp_proto_put_offload_short_progress},
    .abort    = ucp_proto_request_bcopy_abort,
    .reset    = ucp_proto_request_bcopy_reset
};

static size_t ucp_proto_put_offload_bcopy_pack(void *dest, void *arg)
{
    ucp_proto_multi_pack_ctx_t *pack_ctx = arg;

    return ucp_proto_multi_data_pack(pack_ctx, dest);
}

static UCS_F_ALWAYS_INLINE void
ucp_proto_put_offload_update_remote_flush(ucp_ep_h ep,
                                          ucp_sys_dev_map_t flush_sys_dev_mask,
                                          uct_rkey_t tl_rkey, uct_ep_h uct_ep,
                                          uint64_t address)
{
    if (ucs_test_all_flags(ep->ext->flush_sys_dev_map, flush_sys_dev_mask)) {
        return;
    }

    ucp_worker_remote_flush_hash_put(&ep->worker->remote_flush_hash, ep,
                                     ucs_ffs64_safe(flush_sys_dev_mask),
                                     tl_rkey, uct_ep, address);
    ep->ext->flush_sys_dev_map |= flush_sys_dev_mask;
}

static UCS_F_ALWAYS_INLINE ucs_status_t
ucp_proto_put_offload_bcopy_send_func(ucp_request_t *req,
                                      const ucp_proto_multi_lane_priv_t *lpriv,
                                      ucp_datatype_iter_t *next_iter,
                                      ucp_lane_index_t *lane_shift)
{
    ucp_ep_h ep        = req->send.ep;
    uct_ep_h uct_ep    = ucp_ep_get_lane(ep, lpriv->super.lane);
    uint64_t address   = req->send.rma.remote_addr +
                         req->send.state.dt_iter.offset;
    uct_rkey_t tl_rkey = ucp_rkey_get_tl_rkey(req->send.rma.rkey,
                                              lpriv->super.rkey_index);

    ucp_proto_multi_pack_ctx_t pack_ctx = {
        .req         = req,
        .max_payload = ucp_proto_multi_max_payload(req, lpriv, 0),
        .next_iter   = next_iter
    };
    ssize_t packed_size;
    ucs_status_t status;

    packed_size = uct_ep_put_bcopy(uct_ep, ucp_proto_put_offload_bcopy_pack,
                                   &pack_ctx, address, tl_rkey);
    status      = ucp_proto_bcopy_send_func_status(packed_size);
    if (!UCS_STATUS_IS_ERR(status)) {
        ucp_proto_put_offload_update_remote_flush(ep, lpriv->flush_sys_dev_mask,
                                                  tl_rkey, uct_ep, address);
    }

    return status;
}

static ucs_status_t ucp_proto_put_offload_bcopy_progress(uct_pending_req_t *self)
{
    ucp_request_t *req                  = ucs_container_of(self, ucp_request_t,
                                                           send.uct);
    const ucp_proto_multi_priv_t *mpriv = req->send.proto_config->priv;
    ucs_status_t status;

    if (!(req->flags & UCP_REQUEST_FLAG_PROTO_INITIALIZED)) {
        ucp_proto_multi_request_init(req);

        status = ucp_ep_rma_handle_fence(req->send.ep, req, mpriv->lane_map);
        if (status != UCS_OK) {
            ucp_proto_request_abort(req, status);
            return UCS_OK;
        }

        req->flags |= UCP_REQUEST_FLAG_PROTO_INITIALIZED;
    }

    /* coverity[tainted_data_downcast] */
    return ucp_proto_multi_progress(req, req->send.proto_config->priv,
                                    ucp_proto_put_offload_bcopy_send_func,
                                    ucp_proto_request_bcopy_complete_success,
                                    UCP_DT_MASK_ALL);
}

static void
ucp_proto_put_offload_bcopy_probe(const ucp_proto_init_params_t *init_params)
{
    ucp_context_t *context               = init_params->worker->context;
    ucp_proto_multi_init_params_t params = {
        .super.super         = *init_params,
        .super.latency       = 0,
        .super.overhead      = context->config.ext.proto_overhead_multi,
        .super.cfg_thresh    = context->config.ext.bcopy_thresh,
        .super.cfg_priority  = 20,
        .super.min_length    = 0,
        .super.max_length    = SIZE_MAX,
        .super.min_iov       = 0,
        .super.min_frag_offs = UCP_PROTO_COMMON_OFFSET_INVALID,
        .super.max_frag_offs = ucs_offsetof(uct_iface_attr_t,
                                           cap.put.max_bcopy),
        .super.max_iov_offs  = UCP_PROTO_COMMON_OFFSET_INVALID,
        .super.hdr_size      = 0,
        .super.send_op       = UCT_EP_OP_PUT_BCOPY,
        .super.memtype_op    = UCT_EP_OP_LAST,
        .super.flags         = UCP_PROTO_COMMON_INIT_FLAG_RECV_ZCOPY    |
                               UCP_PROTO_COMMON_INIT_FLAG_REMOTE_ACCESS |
                               UCP_PROTO_COMMON_INIT_FLAG_ERR_HANDLING,
        .super.exclude_map   = 0,
        .super.reg_mem_info  = ucp_mem_info_unknown,
        .max_lanes           = UCP_PROTO_RMA_MAX_BCOPY_LANES,
        .min_chunk           = context->config.ext.min_rma_chunk_size,
        .initial_reg_md_map  = 0,
        .first.tl_cap_flags  = UCT_IFACE_FLAG_PUT_BCOPY,
        .first.lane_type     = UCP_LANE_TYPE_RMA_BW,
        .middle.tl_cap_flags = UCT_IFACE_FLAG_PUT_BCOPY,
        .middle.lane_type    = UCP_LANE_TYPE_RMA_BW,
        .opt_align_offs      = UCP_PROTO_COMMON_OFFSET_INVALID
    };

    if (!ucp_proto_init_check_op(init_params, UCS_BIT(UCP_OP_ID_PUT))) {
        return;
    }

    ucp_proto_multi_probe(&params);
}

ucp_proto_t ucp_put_offload_bcopy_proto = {
    .name     = "put/offload/bcopy",
    .desc     = UCP_PROTO_COPY_IN_DESC,
    .flags    = 0,
    .probe    = ucp_proto_put_offload_bcopy_probe,
    .query    = ucp_proto_multi_query,
    .progress = {ucp_proto_put_offload_bcopy_progress},
    .abort    = ucp_proto_request_bcopy_abort,
    .reset    = ucp_proto_request_bcopy_reset
};

static UCS_F_ALWAYS_INLINE ucs_status_t
ucp_proto_put_offload_zcopy_send_func(ucp_request_t *req,
                                      const ucp_proto_multi_lane_priv_t *lpriv,
                                      ucp_datatype_iter_t *next_iter,
                                      ucp_lane_index_t *lane_shift)
{
    ucp_ep_h ep        = req->send.ep;
    uct_ep_h uct_ep    = ucp_ep_get_lane(ep, lpriv->super.lane);
    uint64_t address   = req->send.rma.remote_addr +
                         req->send.state.dt_iter.offset;
    uct_rkey_t tl_rkey = ucp_rkey_get_tl_rkey(req->send.rma.rkey,
                                              lpriv->super.rkey_index);
    uct_iov_t iov;
    ucs_status_t status;

    ucp_datatype_iter_next_iov(&req->send.state.dt_iter,
                               ucp_proto_multi_max_payload(req, lpriv, 0),
                               lpriv->super.md_index, UCP_DT_MASK_CONTIG_IOV,
                               next_iter, &iov, 1);
    status = uct_ep_put_zcopy(uct_ep, &iov, 1, address, tl_rkey,
                              &req->send.state.uct_comp);
    if (!UCS_STATUS_IS_ERR(status)) {
        ucp_proto_put_offload_update_remote_flush(ep, lpriv->flush_sys_dev_mask,
                                                  tl_rkey, uct_ep, address);
    }

    return status;
}

static ucs_status_t
ucp_proto_put_offload_zcopy_progress(uct_pending_req_t *self)
{
    ucp_request_t *req = ucs_container_of(self, ucp_request_t, send.uct);

    /* coverity[tainted_data_downcast] */
    return ucp_proto_multi_zcopy_progress(
            req, req->send.proto_config->priv, ucp_proto_multi_rma_init_func,
            UCT_MD_MEM_ACCESS_LOCAL_READ, UCP_DT_MASK_CONTIG_IOV,
            ucp_proto_put_offload_zcopy_send_func,
            ucp_request_invoke_uct_completion_success,
            ucp_proto_request_zcopy_completion);
}

static void
ucp_proto_put_offload_zcopy_disable_probe(const ucp_proto_init_params_t *init_params)
{
    (void)init_params;
}

static void
ucp_proto_put_offload_zcopy_probe(const ucp_proto_init_params_t *init_params)
{
    ucp_context_t *context               = init_params->worker->context;
    ucp_proto_multi_init_params_t params = {
        .super.super         = *init_params,
        .super.latency       = 0,
        .super.overhead      = context->config.ext.proto_overhead_multi,
        .super.cfg_thresh    = context->config.ext.zcopy_thresh,
        .super.cfg_priority  = 30,
        .super.min_length    = 0,
        .super.max_length    = SIZE_MAX,
        .super.min_iov       = 1,
        .super.min_frag_offs = ucs_offsetof(uct_iface_attr_t,
                                           cap.put.min_zcopy),
        .super.max_frag_offs = ucs_offsetof(uct_iface_attr_t,
                                            cap.put.max_zcopy),
        .super.max_iov_offs  = ucs_offsetof(uct_iface_attr_t, cap.put.max_iov),
        .super.hdr_size      = 0,
        .super.send_op       = UCT_EP_OP_PUT_ZCOPY,
        .super.memtype_op    = UCT_EP_OP_LAST,
        .super.flags         = UCP_PROTO_COMMON_INIT_FLAG_SEND_ZCOPY    |
                               UCP_PROTO_COMMON_INIT_FLAG_RECV_ZCOPY    |
                               UCP_PROTO_COMMON_INIT_FLAG_REMOTE_ACCESS |
                               UCP_PROTO_COMMON_INIT_FLAG_ERR_HANDLING,
        .super.exclude_map   = 0,
        .super.reg_mem_info  = ucp_proto_common_select_param_mem_info(
                                                     init_params->select_param),
        .max_lanes           = context->config.ext.max_rma_lanes,
        .min_chunk           = context->config.ext.min_rma_chunk_size,
        .initial_reg_md_map  = 0,
        .first.tl_cap_flags  = UCT_IFACE_FLAG_PUT_ZCOPY,
        .first.lane_type     = UCP_LANE_TYPE_RMA_BW,
        .middle.tl_cap_flags = UCT_IFACE_FLAG_PUT_ZCOPY,
        .middle.lane_type    = UCP_LANE_TYPE_RMA_BW,
        .opt_align_offs      = UCP_PROTO_COMMON_OFFSET_INVALID,
    };

    if (!ucp_proto_init_check_op(init_params, UCS_BIT(UCP_OP_ID_PUT))) {
        return;
    }

    ucp_proto_multi_probe(&params);
}

ucp_proto_t ucp_put_offload_zcopy_proto = {
    .name     = "put/offload/zcopy",
    .desc     = UCP_PROTO_ZCOPY_DESC,
    .flags    = 0,
    .probe    = ucp_proto_put_offload_zcopy_disable_probe,
    .query    = ucp_proto_multi_query,
    .progress = {ucp_proto_put_offload_zcopy_progress},
    .abort    = ucp_proto_request_zcopy_abort,
    .reset    = ucp_proto_offload_zcopy_reset
};

ucp_mem_desc_t *
ucp_rma_mpool_get(ucp_worker_h worker)
{
    return ucp_rndv_mpool_get(worker, UCS_MEMORY_TYPE_HOST,
                              UCS_SYS_DEVICE_ID_UNKNOWN);
}

static size_t
ucp_rma_mpool_frag_size(ucp_worker_h worker)
{
    return worker->context->config.ext.rndv_frag_size[UCS_MEMORY_TYPE_HOST];
}

enum {
    UCP_PROTO_PUT_PPLN_START = UCP_PROTO_STAGE_START,
    UCP_PROTO_PUT_PPLN_WRITE
};

/* State tracking for the request */
enum {
    UCP_PROTO_PUT_PPLN_SENT    = UCS_BIT(0),
    UCP_PROTO_PUT_PPLN_AM_SENT = UCS_BIT(1),
    UCP_PROTO_PUT_PPLN_PENDING = UCS_BIT(2),
};

/* Track copy-in */
typedef struct ucp_proto_put_ppln {
    uct_completion_t comp;      /* copy-in completion */
    uct_completion_t send_comp; /* remote send completion */
    int              idx;
    int              overall;   /* First fragment tracks overall */
    ucp_mem_desc_t   *mem_desc;
    size_t           size;      /* Size of this fragment */
    ucp_request_t    *req;
    ucp_mem_desc_t   *remote_mem_desc;
    ucp_lane_index_t lane_idx;  /* Index where the RDMA was performed */
    unsigned         flags;
    uint64_t         rva;       /* Remote bounce-buffer address */
    ucp_rkey_h       rkey;      /* Remote bounce-buffer for RDMA */
} ucp_proto_put_ppln_ctx_t;

/* Request for remote buffers */
typedef struct ucp_rts_ppln {
    uint64_t      ep_id;
    ucp_request_t *req;
    int           count;
    ucp_md_map_t  md_map;
} ucp_rts_ppln_t;

typedef struct ucp_rts_ppln_resp {
    ucp_rts_ppln_t rts_ppln;
    char           packed[];
} ucp_rts_ppln_resp_t;

/* Signal copy-out ready */
typedef struct ucp_atp_pppln {
    uint64_t       ep_id;       /* Final ack destination */

    ucp_request_t  *req;        /* Pointer of the sender request */
    int            frag_id;
    int            frag_count;

    ucp_mem_desc_t *mem_desc;   /* Source for copy-out */
    uint64_t       address;     /* Destination for copy-out */
    size_t         length;
} ucp_atp_ppln_t;

/* Signal sender all copy-out are ready */
typedef struct {
    uint64_t      ep_id;        /* Make it invalid */
    ucp_request_t *request;     /* Sender request */
} ucp_atp_ppln_final_t;

static void
ucp_put_ppln_complete(ucp_request_t *req)
{
    ucp_proto_put_ppln_ctx_t *ctx = req->ctx;
    int i;

    ctx[0].overall++;
    if (ctx[0].overall <= req->frag_count) {
        return;
    }

    for (i = 0; i < req->frag_count; i++) {
        ucp_rkey_destroy(ctx[i].rkey);
    }

    ucs_free(req->ctx);
    ucs_debug("PUT PPLN complete req=%p", req);
    ucp_proto_request_zcopy_complete(req, UCS_OK);
}

static void
ucp_proto_put_ppln_send_zcopy_complete(uct_completion_t *self)
{
    ucp_proto_put_ppln_ctx_t *ctx =
        ucs_container_of(self, ucp_proto_put_ppln_ctx_t, send_comp);

    ucs_debug("put ppln send zcopy complete ctx=%p idx=%d req=%p status=%d",
                  ctx, ctx->idx, ctx->req, self->status);

    ucs_mpool_put(ctx->mem_desc);
    ctx->mem_desc = NULL;
    ucp_put_ppln_complete(ctx->req);
}

static void
ucp_proto_put_ppln_copy_in_complete(uct_completion_t *self)
{
    ucp_proto_put_ppln_ctx_t *ctx =
        ucs_container_of(self, ucp_proto_put_ppln_ctx_t, comp);
    ucp_proto_put_ppln_ctx_t *first_ctx = ctx->req->ctx;

    ucs_debug("put ppln copy-in complete ctx=%p idx=%d req=%p status=%d",
                  ctx, ctx->idx, ctx->req, self->status);

    if (!(first_ctx[0].flags & UCP_PROTO_PUT_PPLN_PENDING)) {
        ucp_request_send(ctx->req);
    }
}

static size_t
put_ppln_pack_cb(void *dest, void *arg)
{
    ucp_request_t *req = arg;

    memcpy(dest, req->send.buffer, req->send.length);
    return req->send.length;
}

#define UCP_PUT_PPLN_BCOPY_LIMIT 120

static ucs_status_t
ucp_msg_send_progress(uct_pending_req_t *self)
{
    ucp_request_t *req = ucs_container_of(self, ucp_request_t, send.uct);
    ucp_ep_t *ep       = req->send.ep;
    ucs_status_t status;
    uct_ep_h uct_ep;
    ssize_t packed_len;

    uct_ep = ucp_ep_get_fast_lane(ep, req->send.lane);

    if (req->send.length > UCP_PUT_PPLN_BCOPY_LIMIT) {
        packed_len = uct_ep_am_bcopy(uct_ep, req->send.proto.am_id,
                                     put_ppln_pack_cb, req, 0);
        if (packed_len > 0) {
            ucs_assertv_always(packed_len == req->send.length,
                               "packed_len=%zu req->send.length=%zu",
                               packed_len, req->send.length);
            status = UCS_OK;
        } else {
            status = (ucs_status_t)packed_len;
        }
    } else {
        status = uct_ep_am_short(uct_ep,
                                 req->send.proto.am_id, 0xffffffffffffffff,
                                 req->send.buffer, req->send.length);
    }
    ucs_debug("put ppln send progress req=%p length=%zu status=%d", req,
                  req->send.length, status);
    if (status == UCS_OK) {
        ucs_free(req->send.buffer);
        ucp_request_put(req);
    } else if (status != UCS_ERR_NO_RESOURCE) {
        ucs_error("put ppln send progress failed: length=%zu status=%d",
                  req->send.length, status);
    }

    return status;
}

static ucs_status_t
ucp_msg_send(ucp_worker_h worker, ucp_ep_h ep, uint16_t am_id,
             void *payload, size_t length)
{
    ucp_request_t *req;

    req = ucp_request_get(worker);
    if (req == NULL) {
        ucs_error("Reply allocation failure");
        return UCS_ERR_NO_MEMORY;
    }

    ucp_request_send_state_init(req, ucp_dt_make_contig(1), length);

    req->flags                     = 0;
    req->send.lane                 = ucp_ep_get_am_lane(ep);
    req->send.ep                   = ep;
    req->send.buffer               = payload;
    req->send.length               = length;
    req->send.state.dt_iter.offset = 0;
    req->send.proto.am_id          = am_id;
    req->send.uct.func             = ucp_msg_send_progress;
    req->send.mem_type             = UCS_MEMORY_TYPE_HOST;

    ucp_request_send(req);
    return UCS_OK;
}

static void
ucp_proto_put_ppln_copy_out_complete(uct_completion_t *self)
{
    ucp_ep_rma_ppln_data_entry_t *entry =
        ucs_container_of(self, ucp_ep_rma_ppln_data_entry_t, comp);
    ucp_ep_rma_ppln_data_t *data        = entry->data;
    ucp_ep_h ep                         = data->ep;

    ucs_status_t status;
    ucp_atp_ppln_final_t *atp_final;

    ucs_mpool_put(entry->mem_desc);
    entry->mem_desc = NULL;

    data->frag_done++;
    ucs_debug("put ppln copy-out completed frag_done=%d/%d",
                  data->frag_done, data->frag_count);
    if (data->frag_done < data->frag_count) {
        return;
    }

    atp_final = ucs_malloc(sizeof(*atp_final), "atp_final");
    if (atp_final == NULL) {
        ucs_fatal("ppln copy out alloc failed");
    }

    atp_final->ep_id   = 0;
    atp_final->request = data->request;
    status = ucp_msg_send(ep->worker, ep, UCP_AM_ID_ATP_PPLN,
                          atp_final, sizeof(*atp_final));
    if (status != UCS_OK) {
        ucs_fatal("ppln copy out complete failure status=%d", status);
    }

    ucp_ep_rma_ppln_data_remove(ep, data->request);
}

static ucs_status_t
ucp_am_handler_atp_ppln_final(ucp_atp_ppln_final_t *ppln_final)
{
    ucp_request_t *request        = ppln_final->request;
    ucs_status_t status           = UCS_OK;

    ucs_debug("put ppln: atp ppln_final request=%p", request);
    ucp_put_ppln_complete(request);
    return status;
}

UCS_PROFILE_FUNC(ucs_status_t, ucp_am_handler_atp_ppln,
                 (am_arg, am_data, am_length, am_flags), void *am_arg,
                 void *am_data, size_t am_length, unsigned am_flags)
{
    ucp_atp_ppln_t *atp_ppln = UCS_PTR_BYTE_OFFSET(am_data, 8);
    ucp_worker_h worker      = am_arg;
    ucp_ep_h mem_type_ep     = worker->mem_type_ep[UCS_MEMORY_TYPE_CUDA];
    ucp_lane_index_t mem_type_rma_lane;
    ucp_ep_h ep;
    ucp_ep_rma_ppln_data_t *ppln_data;
    uct_iov_t iov[1] = {};
    size_t iovcnt;
    ucs_status_t status;
    ucp_ep_rma_ppln_data_entry_t *entry;
    ucp_atp_ppln_final_t *atp_ppln_final;

    if (atp_ppln->ep_id == 0) {
        ucs_assert_always((sizeof(*atp_ppln_final) + 8) == am_length);
        atp_ppln_final = (ucp_atp_ppln_final_t *)atp_ppln;
        return ucp_am_handler_atp_ppln_final(atp_ppln_final);
    } 

    ucs_assert_always((sizeof(*atp_ppln) + 8) == am_length);

    /* What to copy from/to */
    ucs_debug("put ppln atp_ppln received am_length=%zu "
                  "ep_id=0x%lx req=%p mem_desc=%p address=0x%lx frag_id=%u "
                  "frag_count=%u",
                  am_length, atp_ppln->ep_id, atp_ppln->req, atp_ppln->mem_desc,
                  atp_ppln->address, atp_ppln->frag_id, atp_ppln->frag_count);

    /* Find the corresponding endpoint */
    UCP_WORKER_GET_EP_BY_ID(&ep, worker, atp_ppln->ep_id, {
                            ucs_error("atp_ppln handler: failed to get ep=%lx",
                                      atp_ppln->ep_id);
                            return UCS_ERR_NO_ELEM; }, "atp ppln received");

    ppln_data = ucp_ep_rma_ppln_data_get(ep, atp_ppln->req,
                                         atp_ppln->frag_count);
    if (ppln_data->frag_count == 0) {
        /* First ATP arrived */
        ppln_data->frag_count  = atp_ppln->frag_count;
        ppln_data->frag_done   = 0;
        ppln_data->ep          = ep;
        ppln_data->request     = atp_ppln->req;
    }

    entry              = &ppln_data->entry[atp_ppln->frag_id];
    entry->mem_desc    = atp_ppln->mem_desc;
    entry->data        = ppln_data;
    entry->comp.func   = ucp_proto_put_ppln_copy_out_complete;
    entry->comp.count  = 1;
    entry->comp.status = UCS_OK;

    iov[0].buffer = atp_ppln->mem_desc->ptr;
    iov[0].length = atp_ppln->length;
    iovcnt        = 1;

    /* Start the copy-out */
    mem_type_rma_lane = ucp_ep_config(mem_type_ep)->key.rma_bw_lanes[0];
    status            = uct_ep_put_zcopy(ucp_ep_get_lane(mem_type_ep,
                                                         mem_type_rma_lane),
                                         iov, iovcnt, atp_ppln->address, 
                                         UCT_INVALID_RKEY, &entry->comp);
    ucs_debug("put ppln atp_ppln copy-out src=%p dst=%p size=%zu req=%p "
                  "ep=%p", 
                  iov[0].buffer, (void *)atp_ppln->address, iov[0].length,
                  ppln_data->request, ppln_data->ep);
    if (status == UCS_OK) {
        ucp_proto_put_ppln_copy_out_complete(&entry->comp);
    } else if (status != UCS_INPROGRESS) {
        ucs_fatal("put ppln copy-out failed status=%d", status);
    }

    return UCS_OK;
}

UCS_PROFILE_FUNC(ucs_status_t, ucp_am_handler_rts_ppln,
                 (am_arg, am_data, am_length, am_flags), void *am_arg,
                 void *am_data, size_t am_length, unsigned am_flags)
{
    size_t alloc_size = 32 * 1024;
    ucp_worker_h worker  = am_arg;
    ucp_rts_ppln_t *rts_ppln;
    int i;
    ucp_memory_info_t mem_info;
    ucp_rts_ppln_resp_t *rts_ppln_resp;
    void *p;
    ucp_mem_desc_t *mem_desc;
    ssize_t packed_rkey_size;
    size_t size;
    ucp_ep_h ep;
    uint8_t *size_p;

    rts_ppln = UCS_PTR_BYTE_OFFSET(am_data, 8);

    UCP_WORKER_GET_EP_BY_ID(&ep, worker, rts_ppln->ep_id, {
                            ucs_error("rts ppln handler: failed to get ep=%lx",
                                      rts_ppln->ep_id);
                            return UCS_ERR_NO_ELEM; }, "rts ppln received");

    rts_ppln_resp = ucs_malloc(alloc_size, "rts_ppln_resp");
    if (rts_ppln_resp == NULL) {
        ucs_error("Failed to allocate rts_ppln_resp");
        return UCS_ERR_NO_MEMORY;
    }

    ucs_debug("put ppln rts ppln received am_length=%zu "
                  "ep_id=%lx ep=%p frag_count=%d md_map=0x%lx req=%p",
                  am_length, rts_ppln->ep_id, ep,
                  rts_ppln->count, rts_ppln->md_map,
                  rts_ppln->req);

    rts_ppln_resp->rts_ppln = *rts_ppln;
    p = (void*)(rts_ppln_resp + 1);
    for (i = 0; i < rts_ppln->count; ++i) {
        mem_desc = ucp_rma_mpool_get(worker);
        if (mem_desc == NULL) {
            ucs_error("rts ppln handler: rma mpool get failed");
            return UCS_ERR_NO_RESOURCE;
        }

        *(void **)p = mem_desc;
        p += sizeof(mem_desc);
        *(uint64_t **)p = mem_desc->ptr;
        p += sizeof(uint64_t);
        size_p = p;
        p += sizeof(*size_p);

        mem_info.type    = UCS_MEMORY_TYPE_HOST;
        mem_info.sys_dev = UCS_SYS_DEVICE_ID_UNKNOWN;
        packed_rkey_size = ucp_rkey_pack_memh(
                                      worker->context,
                                      rts_ppln->md_map,
                                      mem_desc->memh,
                                      mem_desc->ptr,
                                      ucp_rma_mpool_frag_size(worker),
                                      &mem_info,
                                      0, NULL,
                                      0,
                                      0,
                                      p);
        if (packed_rkey_size < 0) {
            ucs_error("rts ppln handler: rkey pack failed size=%zd",
                      packed_rkey_size);
            return UCS_ERR_NO_RESOURCE;
        }

        ucs_debug("put ppln rts: req=%p pack memh: mem_desc=%p rva=%p packed_size=%zd",
                      rts_ppln->req, mem_desc, mem_desc->ptr, packed_rkey_size);
        ucs_assertv_always(packed_rkey_size <= UCHAR_MAX, "Bad packed size!");
        *size_p = packed_rkey_size;
        p      += packed_rkey_size;
    }

    size = (char *)p - (char *)rts_ppln_resp;
    ucs_assertv_always(size <= alloc_size,
                       "size=%zu max_ppln_resp=%zu", size, alloc_size);
    ucs_trace("put ppln rts ppln received: sending size=%zu", size);

    return ucp_msg_send(worker, ep, UCP_AM_ID_RTS_PPLN_RESP,
                        rts_ppln_resp, size);
}

UCS_PROFILE_FUNC(ucs_status_t, ucp_am_handler_rts_ppln_resp,
                 (am_arg, am_data, am_length, am_flags), void *am_arg,
                 void *am_data, size_t am_length, unsigned am_flags)
{
    ucp_rts_ppln_resp_t *rts_ppln_resp;
    ucp_rts_ppln_t *rts_ppln;
    int i;
    ucp_mem_desc_t *remote_mem_desc;
    char *p;
    uint8_t size;
    ucp_request_t *req;
    ucs_status_t status;
    ucp_proto_put_ppln_ctx_t *ctx;
    ucp_ep_h ep;
    uint64_t rva;

    if (*(uint64_t*)am_data == 0xffffffffffffffff) {
        rts_ppln_resp = UCS_PTR_BYTE_OFFSET(am_data, 8);
    } else {
        rts_ppln_resp = UCS_PTR_BYTE_OFFSET(am_data, 0);
    }

    rts_ppln = &rts_ppln_resp->rts_ppln;

    ucs_debug("put ppln rts ppln response received am_length=%zu "
                  "ep_id=%lx frag_count=%d md_map=0x%lx req=%p ctx=%p",
                  am_length, rts_ppln->ep_id,
                  rts_ppln->count, rts_ppln->md_map,
                  rts_ppln->req, rts_ppln->req->ctx);

    p   = (char *)rts_ppln_resp->packed;
    req = rts_ppln->req;
    ep  = req->send.ep;
    ctx = req->ctx;

    for (i = 0; i < rts_ppln->count; i++) {
        remote_mem_desc = *(ucp_mem_desc_t **)p;
        p              += sizeof(remote_mem_desc);
        rva             = *(uint64_t*)p;
        p              += sizeof(rva);
        size            = *(unsigned char*)p;
        p              += sizeof(size);

        ctx[i].remote_mem_desc = remote_mem_desc;
        ctx[i].rva             = rva;

        ucs_debug("req=%p i=%u ctx=%p mem_desc=%p",
                      req, i, &ctx[i], ctx[i].mem_desc);

        status = ucp_ep_rkey_unpack(ep, p, &ctx[i].rkey);
        if (status != UCS_OK) {
            ucs_fatal("failed to unpack rendezvous remote key received from %s: %s",
                      ucp_ep_peer_name(ep), ucs_status_string(status));
        }

        ucs_debug("put ppln rts ppln response: unpacking "
                      "req=%p remote_mem_desc=%p rva=%lx size=%u ctx->mem_desc=%p",
                      req, remote_mem_desc, ctx[i].rva, size, ctx[i].mem_desc);

        p += size;
    }

    ucs_assertv_always(am_length == (p - (char*)am_data),
                       "mismatched rts ppln resp am_length=%zu "
                       "final_size=%zu", am_length, (p - (char*)am_data));

    ucp_request_send(req);
    return UCS_OK;
}

UCP_DEFINE_AM_WITH_PROXY(UCP_FEATURE_AM | UCP_FEATURE_RMA, UCP_AM_ID_ATP_PPLN,
                         ucp_am_handler_atp_ppln, NULL, 0);
UCP_DEFINE_AM_WITH_PROXY(UCP_FEATURE_AM | UCP_FEATURE_RMA, UCP_AM_ID_RTS_PPLN,
                         ucp_am_handler_rts_ppln, NULL, 0);
UCP_DEFINE_AM_WITH_PROXY(UCP_FEATURE_AM | UCP_FEATURE_RMA, UCP_AM_ID_RTS_PPLN_RESP,
                         ucp_am_handler_rts_ppln_resp, NULL, 0);

static void
ucp_proto_put_ppln_completion(uct_completion_t *self)
{
    ucs_debug("put ppln rts ppln request completed");
}

static ucp_md_map_t
ucp_proto_multi_remote_md_map_req(const ucp_request_t *req)
{
    ucp_worker_h worker                    = req->send.ep->worker;
    const ucp_proto_config_t *proto_config = req->send.proto_config;
    const ucp_ep_config_key_t *ep_config_key;
    const ucp_proto_multi_priv_t *mpriv;
    ucp_md_map_t remote_md_map = 0;
    ucp_lane_index_t i, lane;

    ep_config_key = &ucs_array_elem(&worker->ep_config,
                                    proto_config->ep_cfg_index).key;
    mpriv         = proto_config->priv;

    for (i = 0; i < mpriv->num_lanes; i++) {
        lane = mpriv->lanes[i].super.lane;
        remote_md_map |= UCS_BIT(ep_config_key->lanes[lane].dst_md_index);
    }

    return remote_md_map;
}

static ucs_status_t
ucp_proto_put_offload_zcopy_ppln_start_progress(uct_pending_req_t *self)
{
    ucp_request_t *req           = ucs_container_of(self, ucp_request_t,
                                                    send.uct);
    ucp_ep_h ep                  = req->send.ep;
    ucp_datatype_iter_t *dt_iter = &req->send.state.dt_iter;
    ucp_worker_h worker          = ep->worker;
    ucp_ep_h mem_type_ep         = worker->mem_type_ep[UCS_MEMORY_TYPE_CUDA];
    const ucp_proto_multi_priv_t *mpriv
                                 = req->send.proto_config->priv;
    ucs_status_t status;
    size_t i, offset, frag_count, frag_size;
    uct_iov_t iov[1] = {};
    size_t iovcnt;
    ucp_lane_index_t mem_type_rma_lane;
    ucp_proto_put_ppln_ctx_t *ctx;
    ucp_rts_ppln_t rts_ppln;

    frag_size       = ucp_rma_mpool_frag_size(worker);
    frag_count      = (dt_iter->length + frag_size - 1) / frag_size;
    req->frag_count = frag_count;

    if (!(req->flags & UCP_REQUEST_FLAG_PROTO_INITIALIZED)) {
        req->send.multi_lane_idx = 0;

        /* Make sure buffers are registered for read */
        status = ucp_proto_request_zcopy_init(req, mpriv->reg_md_map,
                                              ucp_proto_put_ppln_completion,
                                              UCT_MD_MEM_ACCESS_LOCAL_READ,
                                              UCP_DT_MASK_CONTIG_IOV);
        if (status != UCS_OK) {
            goto out_abort;
        }

        ctx      = ucs_malloc(sizeof(*ctx) * frag_count, "");
        req->ctx = ctx;
        if (ctx == NULL) {
            ucs_fatal("failed to allocat copy-in context");
        }

        /* Lookup memtype EP and lane */
        mem_type_rma_lane = ucp_ep_config(mem_type_ep)->key.rma_bw_lanes[0];

        ucs_debug("put ppln for buffer=%p len=%zu frag_size=%zu frag_count=%zu "
                  "mem_type_ep=%p lane=%u ctx=%p",
                  dt_iter->type.contig.buffer, dt_iter->length, frag_size, frag_count,
                  mem_type_ep, mem_type_rma_lane, req->ctx);

        /* Start all copy-in */
        offset = 0;
        for (i = 0; i < frag_count; i++, offset += frag_size) {
            iov[0].buffer = dt_iter->type.contig.buffer + offset;
            iov[0].length = ucs_min(frag_size, dt_iter->length - offset);
            iovcnt        = 1;

            ctx[i].idx             = i;
            ctx[i].flags           = 0;
            ctx[i].overall         = 0;
            ctx[i].mem_desc        = ucp_rma_mpool_get(worker);
            ctx[i].remote_mem_desc = NULL;
            ctx[i].comp.func       = ucp_proto_put_ppln_copy_in_complete;
            ctx[i].comp.count      = 1;
            ctx[i].comp.status     = UCS_OK;
            ctx[i].req             = req;
            ctx[i].size            = iov[0].length;

            ucs_assertv(iov[0].length <= frag_size,
                        "frag_size=%zu iov_length=%zu",
                        frag_size, iov[0].length);
            ucs_assert(ctx[i].mem_desc != NULL);

            status = uct_ep_put_zcopy(ucp_ep_get_lane(mem_type_ep,
                                                      mem_type_rma_lane),
                                      iov, iovcnt,
                                      (uint64_t)ctx[i].mem_desc->ptr,
                                      UCT_INVALID_RKEY, &ctx[i].comp);
            ucs_assertv_always(status == UCS_INPROGRESS,
                               "copy-in failed status=%d", status);
        }

        req->flags |= UCP_REQUEST_FLAG_PROTO_INITIALIZED;
    }

    rts_ppln.ep_id  = ucp_ep_remote_id(ep);
    rts_ppln.count  = frag_count;
    rts_ppln.req    = req;
    rts_ppln.md_map = ucp_proto_multi_remote_md_map_req(req);
    req->send.lane  = ucp_ep_get_am_lane(ep);
    status          = uct_ep_am_short(ucp_ep_get_fast_lane(ep, req->send.lane),
                                      UCP_AM_ID_RTS_PPLN, 0,
                                      &rts_ppln, sizeof(rts_ppln));

    /* Request for buffer while copy-in is being done */
    if (status == UCS_OK) {
        ucp_proto_request_set_stage(req, UCP_PROTO_PUT_PPLN_WRITE);
        ucs_debug("req=%p moving to put_ppln_write stage", req);
    } else {
        ucs_debug("req=%p put ppln RTS_PPLN status=%d", req, status);
    }

    ctx = req->ctx;
    if (status == UCS_ERR_NO_RESOURCE) {
        ctx[0].flags |= UCP_PROTO_PUT_PPLN_PENDING;
    } else {
        ctx[0].flags &= ~UCP_PROTO_PUT_PPLN_PENDING;
    }
    return status;

out_abort:
    ucp_proto_request_abort(req, status);
    return UCS_OK;
}

static ucp_md_index_t
ucp_put_ppln_get_rkey_index(ucp_request_t *req, ucp_rkey_h rkey,
                            ucp_lane_index_t lane)
{
    ucp_ep_h ep                 = req->send.ep;
    ucp_ep_config_t *ep_config  = ucp_ep_config(ep);
    ucp_md_index_t md_index     = ep_config->md_index[lane];
    ucp_md_index_t dst_md_index = ep_config->key.lanes[lane].dst_md_index;

    ucs_assertv_always((UCS_BIT(dst_md_index) & rkey->md_map) &&
                       (md_index != UCP_NULL_RESOURCE),
        "dst_md_index=%u rkey->md_map=0x%lx md_index=%u",
        dst_md_index, rkey->md_map, md_index);

    return ucs_bitmap2idx(rkey->md_map, dst_md_index);
}

static ucs_status_t
ucp_put_ppln_send_signal(ucp_request_t *req, int i)
{
    uct_ep_h uct_ep;
    ucp_atp_ppln_t atp_ppln;
    ucp_proto_put_ppln_ctx_t *ctx = req->ctx;
    ucs_status_t status;

    uct_ep = ucp_ep_get_lane(req->send.ep, ctx[i].lane_idx);

    status = uct_ep_fence(uct_ep, 0);
    ucs_assertv_always(status == UCS_OK, "fence status=%d", status);

    /* What to copy from/to */
    atp_ppln.mem_desc   = ctx[i].remote_mem_desc;
    atp_ppln.address    = req->send.rma.remote_addr +
        (i * ucp_rma_mpool_frag_size(req->send.ep->worker));

    /* Where to send back the final ack to */
    atp_ppln.ep_id      = ucp_ep_remote_id(req->send.ep);
    atp_ppln.req        = req;
    atp_ppln.frag_id    = i;
    atp_ppln.frag_count = req->frag_count;
    atp_ppln.length     = ctx[i].size;

    req->send.lane = ctx[i].lane_idx;
    status = uct_ep_am_short(uct_ep, UCP_AM_ID_ATP_PPLN, 0,
                             &atp_ppln, sizeof(atp_ppln));
    if ((status == UCS_OK) || (status == UCS_INPROGRESS)) {
        ucs_debug("put atp ppln req=%p ep_id=0x%lx frag_id=%u frag_count=%u "
                      "address=0x%lx size=%zu mem_desc=%p",
                      req, atp_ppln.ep_id, atp_ppln.frag_id,
                      atp_ppln.frag_count,
                      atp_ppln.address, ctx[i].size, atp_ppln.mem_desc);
    }

    return status;
}

static uct_mem_h
ucp_proto_put_ppln_get_memh(ucp_request_t *req,
                            ucp_mem_h memh,
                            const ucp_proto_multi_lane_priv_t *lpriv)
{
    ucp_md_index_t md_index = ucp_ep_md_index(req->send.ep, lpriv->super.lane);

    ucs_assert_always(md_index != UCP_NULL_RESOURCE);
    ucs_assertv(UCS_BIT(md_index) & memh->md_map,
                "md_index=%d md_map=0x%" PRIx64, md_index, memh->md_map);

    return memh->uct[md_index];
}

static ucs_status_t
ucp_proto_put_offload_zcopy_ppln_write_progress(uct_pending_req_t *self)
{
    ucp_lane_index_t lane_shift   = 1;
    ucp_request_t *req            = ucs_container_of(self, ucp_request_t,
                                                     send.uct);
    ucp_ep_h ep                   = req->send.ep;
    ucs_status_t status           = UCS_OK;
    ucp_proto_put_ppln_ctx_t *ctx = req->ctx;
    const ucp_proto_multi_priv_t *mpriv;
    const ucp_proto_multi_lane_priv_t *lpriv;
    ucp_lane_index_t lane_idx;
    int i;
    uct_iov_t iov;
    uct_rkey_t tl_rkey;
    ucp_md_index_t rkey_index;
    uct_ep_h uct_ep;

    ucs_debug("put ppln write req=%p", req);

    mpriv = req->send.proto_config->priv;

    /* Post all ready transfers and retry if transport is not ready */
    for (i = 0; i < req->frag_count; i++) {
        if ((ctx[i].flags & UCP_PROTO_PUT_PPLN_SENT)) {
            continue;
        }

        if (ctx[i].remote_mem_desc == NULL) {
            /* Did not receive the information for remote bounce buf */
            continue;
        }

        lpriv          = &mpriv->lanes[req->send.multi_lane_idx];
        req->send.lane = lpriv->super.lane;  /* For pending queueing */
        lane_idx       = lpriv->super.lane;

        /* Wait for copy-in to complete */
        if (ctx[i].comp.count == 1) {
            continue;
        }

        ucs_assertv_always(ctx[i].comp.count == 0, "copy_in_count=%u",
                           ctx[i].comp.count);

        /* Sender bounce buffer */
        iov.buffer = ctx[i].mem_desc->ptr;
        iov.length = ctx[i].size;
        iov.memh   = ucp_proto_put_ppln_get_memh(req,
                                                 ctx[i].mem_desc->memh, lpriv);

        iov.stride = 0;
        iov.count  = 1;

        ucs_assertv_always(lane_idx == lpriv->super.lane,
                           "lane_idx=%u lpriv_super_lane=%u",
                           lane_idx, lpriv->super.lane);

        /* Receiver bounce buffer with unpacked rkey */
        rkey_index = ucp_put_ppln_get_rkey_index(req, ctx[i].rkey, lane_idx);
        tl_rkey    = ucp_rkey_get_tl_rkey(ctx[i].rkey, rkey_index);
        uct_ep     = ucp_ep_get_lane(ep, lpriv->super.lane);

        /* Release mem_desc after usage */
        ctx[i].send_comp.status = UCS_OK;
        ctx[i].send_comp.count  = 1;
        ctx[i].send_comp.func   = ucp_proto_put_ppln_send_zcopy_complete;

        status = uct_ep_put_zcopy(uct_ep, &iov, 1, ctx[i].rva, tl_rkey,
                                  &ctx[i].send_comp);
        ucs_debug("put ppln write req=%p ctx=%p i=%u status=%d",
                      req, ctx, i, status);
        if ((status != UCS_OK) && (status != UCS_INPROGRESS)) {
            goto done;
        }

        /* Signaling must be done on the same lane */
        ctx[i].lane_idx = lane_idx;
        ctx[i].flags   |= UCP_PROTO_PUT_PPLN_SENT;
        ucs_debug("put ppln write req=%p i=%d lane_idx=%u va=%p rva=0x%lx",
                      req, i, lane_idx, iov.buffer, ctx[i].rva);

        ucp_proto_multi_advance_lane_idx(req, mpriv->num_lanes, lane_shift);
    }

    for (i = 0; i < req->frag_count; i++) {
        if (!(ctx[i].flags & UCP_PROTO_PUT_PPLN_SENT) ||
            (ctx[i].flags & UCP_PROTO_PUT_PPLN_AM_SENT)) {
            continue;
        }

        status = ucp_put_ppln_send_signal(req, i);
        if ((status != UCS_OK) && (status != UCS_INPROGRESS)) {
            ucs_debug("put ppln write req=%p i=%d signal", req, i);
            goto done;
        }

        ctx[i].flags |= UCP_PROTO_PUT_PPLN_AM_SENT;
    }

done:
    if (status == UCS_ERR_NO_RESOURCE) {
        ctx[0].flags |= UCP_PROTO_PUT_PPLN_PENDING;
    } else {
        ctx[0].flags &= ~UCP_PROTO_PUT_PPLN_PENDING;
    }
    return status;
}

ucp_proto_t ucp_put_offload_zcopy_ppln_proto = {
    .name     = "put/offload/zcopy/ppln",
    .desc     = UCP_PROTO_ZCOPY_PPLN_DESC,
    .flags    = 0,
    .probe    = ucp_proto_put_offload_zcopy_probe,
    .query    = ucp_proto_multi_query,
    .progress = {
        [UCP_PROTO_PUT_PPLN_START] =
            ucp_proto_put_offload_zcopy_ppln_start_progress,
        [UCP_PROTO_PUT_PPLN_WRITE] =
            ucp_proto_put_offload_zcopy_ppln_write_progress,
    },
    .abort    = ucp_proto_request_zcopy_abort,
    .reset    = ucp_proto_offload_zcopy_reset
};
