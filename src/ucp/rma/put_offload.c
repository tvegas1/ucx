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
    .probe    = ucp_proto_put_offload_zcopy_probe,
    .query    = ucp_proto_multi_query,
    .progress = {ucp_proto_put_offload_zcopy_progress},
    .abort    = ucp_proto_request_zcopy_abort,
    .reset    = ucp_proto_offload_zcopy_reset
};

ucp_mem_desc_t *
ucp_rma_mpool_get(ucp_worker_h worker)
{
    return ucp_rndv_mpool_get(worker, UCS_MEMORY_TYPE_HOST, 0);
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

static void
ucp_proto_put_ppln_copy_in_complete(uct_completion_t *self)
{
    (void)self;
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
    ucp_mem_h memh;

    if (!(req->flags & UCP_REQUEST_FLAG_PROTO_INITIALIZED)) {
        /* Make sure buffers are registered for read */
        status = ucp_proto_request_zcopy_init(req, mpriv->reg_md_map,
                                              NULL,
                                              UCT_MD_MEM_ACCESS_LOCAL_READ,
                                              UCP_DT_MASK_CONTIG_IOV);
        if (status != UCS_OK) {
            goto out_abort;
        }

        frag_size  = ucp_rma_mpool_frag_size(worker);
        frag_count = (dt_iter->length + frag_size - 1) / frag_size;

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
            memh               = ctx[i].mem_desc->memh;

            ctx[i].comp.func   = ucp_proto_put_ppln_copy_in_complete;
            ctx[i].comp.count  = 1;
            ctx[i].comp.status = UCS_OK;
            ctx[i].req         = req;

            ucs_assertv(ucp_memh_length(memh) <= iov[0].length,
                        "memh_length=%zu length=%zu",
                        ucp_memh_length(memh), iov[0].length);

            status = uct_ep_put_zcopy(ucp_ep_get_lane(mem_type_ep,
                                                      mem_type_rma_lane),
                                      iov, iovcnt,
                                      (uint64_t)ucp_memh_address(memh),
                                      UCT_INVALID_RKEY, &ctx[i].comp);
            ucs_assertv_always(status == UCS_OK, "copy-in failed status=%d",
                               status);
        }

        req->flags |= UCP_REQUEST_FLAG_PROTO_INITIALIZED;
    }

    /* Request for buffer while copy-in is being done */
    status = UCS_OK;
    if (status == UCS_OK) {
        ucp_proto_request_set_stage(req, UCP_PROTO_PUT_PPLN_WRITE);
        ucs_trace_req("req=%p moving to put_ppln_write stage", req);
    }

    return status;

out_abort:
    ucp_proto_request_abort(req, status);
    return UCS_OK;
}

static ucs_status_t
ucp_proto_put_offload_zcopy_ppln_write_progress(uct_pending_req_t *self)
{
    return UCS_INPROGRESS;
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
