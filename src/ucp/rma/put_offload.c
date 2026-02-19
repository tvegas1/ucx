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

/* Track copy-in */
typedef struct ucp_proto_put_ppln {
    uct_completion_t comp;
    ucp_mem_desc_t   *mem_desc;
    ucp_request_t    *req;
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

static void
ucp_proto_put_ppln_copy_in_complete(uct_completion_t *self)
{
    ucs_trace_req("put ppln copy-in complete comp=%p", self);
}

UCS_PROFILE_FUNC(ucs_status_t, ucp_am_handler_rts_ppln,
                 (am_arg, am_data, am_length, am_flags), void *am_arg,
                 void *am_data, size_t am_length, unsigned am_flags)
{
    ucp_worker_h worker  = am_arg;
    ucp_rts_ppln_t *rts_ppln;
    int i;
    ucp_memory_info_t mem_info;
    union {
        char payload[1024];
        ucp_rts_ppln_resp_t rts_ppln_resp;
    } u;
    void *p;
    ucp_mem_desc_t *mem_desc;
    ssize_t packed_rkey_size;
    size_t size;
    ucp_ep_h ep = NULL;

    rts_ppln = UCS_PTR_BYTE_OFFSET(am_data, 8);

    UCP_WORKER_GET_EP_BY_ID(&ep, worker, rts_ppln->ep_id, {
                            ucs_error("rts ppln handler: failed to get ep=%lx",
                                      rts_ppln->ep_id);
                            return UCS_ERR_NO_ELEM; }, "rts ppln received");

    ucs_trace_req("put ppln rts ppln received am_length=%zu "
                  "ep_id=%lx ep=%p frag_count=%d md_map=0x%lx req=%p",
                  am_length, rts_ppln->ep_id, ep,
                  rts_ppln->count, rts_ppln->md_map,
                  rts_ppln->req);

    u.rts_ppln_resp.rts_ppln = *rts_ppln;
    p = (void*)(&u.rts_ppln_resp + 1);
    for (i = 0; i < rts_ppln->count; ++i) {
        mem_desc = ucp_rma_mpool_get(worker);
        if (mem_desc == NULL) {
            ucs_error("rts ppln handler: rma mpool get failed");
            return UCS_ERR_NO_RESOURCE;
        }

        *(void **)p = mem_desc;
        p += sizeof(mem_desc);

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

        p += packed_rkey_size;
    }

    size = (char *)p - (char *)&u.rts_ppln_resp;
    ucs_assertv_always(size <= sizeof(u), "size=%zu max_ppln_resp=%zu",
                       size, sizeof(u));
    ucs_trace("put ppln rts ppln received: size=%zu", size);


    return UCS_OK;
}

UCS_PROFILE_FUNC(ucs_status_t, ucp_am_handler_rts_ppln_resp,
                 (am_arg, am_data, am_length, am_flags), void *am_arg,
                 void *am_data, size_t am_length, unsigned am_flags)
{
    ucp_worker_h worker                = am_arg;
    ucp_rts_ppln_resp_t *rts_ppln_resp = UCS_PTR_BYTE_OFFSET(am_data, 8);
    ucp_rts_ppln_t *rts_ppln           = &rts_ppln_resp->rts_ppln;

    (void)worker;
    ucs_trace_req("put ppln rts ppln response received am_length=%zu "
                  "ep_id=%lx frag_count=%d md_map=0x%lx req=%p",
                  am_length, rts_ppln->ep_id,
                  rts_ppln->count, rts_ppln->md_map,
                  rts_ppln->req);

    return UCS_OK;
}

UCP_DEFINE_AM_WITH_PROXY(UCP_FEATURE_AM | UCP_FEATURE_RMA, UCP_AM_ID_RTS_PPLN,
                         ucp_am_handler_rts_ppln, NULL, 0);
UCP_DEFINE_AM_WITH_PROXY(UCP_FEATURE_AM | UCP_FEATURE_RMA, UCP_AM_ID_RTS_PPLN_RESP,
                         ucp_am_handler_rts_ppln_resp, NULL, 0);

static void
ucp_proto_put_ppln_completion(uct_completion_t *self)
{
    ucs_trace_req("put ppln rts ppln request completed");
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
    uct_iov_t iov[1];
    size_t iovcnt;
    ucp_lane_index_t mem_type_rma_lane;
    ucp_proto_put_ppln_ctx_t *ctx;
    ucp_rts_ppln_t rts_ppln;

    frag_size  = ucp_rma_mpool_frag_size(worker);
    frag_count = (dt_iter->length + frag_size - 1) / frag_size;

    if (!(req->flags & UCP_REQUEST_FLAG_PROTO_INITIALIZED)) {
        /* Make sure buffers are registered for read */
        status = ucp_proto_request_zcopy_init(req, mpriv->reg_md_map,
                                              ucp_proto_put_ppln_completion,
                                              UCT_MD_MEM_ACCESS_LOCAL_READ,
                                              UCP_DT_MASK_CONTIG_IOV);
        if (status != UCS_OK) {
            goto out_abort;
        }

        ctx = ucs_malloc(sizeof(*ctx) * frag_count, "");
        if (ctx == NULL) {
            ucs_fatal("failed to allocat copy-in context");
        }

        /* Lookup memtype EP and lane */
        mem_type_rma_lane = ucp_ep_config(mem_type_ep)->key.rma_bw_lanes[0];

        ucs_debug("put ppln for buffer=%p len=%zu frag_size=%zu frag_count=%zu"
                  "mem_type_ep=%p lane=%u",
                  dt_iter->type.contig.buffer, dt_iter->length, frag_size, frag_count,
                  mem_type_ep, mem_type_rma_lane);

        /* Start all copy-in */
        offset = 0;
        for (i = 0; i < frag_count; i++, offset += frag_size) {
            iov[0].buffer = dt_iter->type.contig.buffer + offset;
            iov[0].length = ucs_min(frag_size, dt_iter->length - offset);
            iovcnt        = 1;

            ctx[i].mem_desc    = ucp_rma_mpool_get(worker);
            ctx[i].comp.func   = ucp_proto_put_ppln_copy_in_complete;
            ctx[i].comp.count  = 1;
            ctx[i].comp.status = UCS_OK;
            ctx[i].req         = req;

            ucs_assertv(iov[0].length <= frag_size,
                        "frag_size=%zu iov_length=%zu",
                        frag_size, iov[0].length);

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
    status          = uct_ep_am_short(ucp_ep_get_am_uct_ep(ep),
                                      UCP_AM_ID_RTS_PPLN, 0,
                                      &rts_ppln, sizeof(rts_ppln));

    /* Request for buffer while copy-in is being done */
    if (status == UCS_OK) {
        ucp_proto_request_set_stage(req, UCP_PROTO_PUT_PPLN_WRITE);
        ucs_trace_req("req=%p moving to put_ppln_write stage", req);
    } else {
        ucs_trace_req("req=%p put ppln RTS_PPLN status=%d", req, status);
    }

    return status;

out_abort:
    ucp_proto_request_abort(req, status);
    return UCS_OK;
}

static ucs_status_t
ucp_proto_put_offload_zcopy_ppln_write_progress(uct_pending_req_t *self)
{
    return UCS_OK;
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
