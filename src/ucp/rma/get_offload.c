/**
 * Copyright (c) NVIDIA CORPORATION & AFFILIATES, 2020. ALL RIGHTS RESERVED.
 * Copyright (C) Advanced Micro Devices, Inc. 2024. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "rma.inl"

#include <ucp/core/ucp_request.inl>
#include <ucp/dt/datatype_iter.inl>
#include <ucp/proto/proto_init.h>
#include <ucp/proto/proto_multi.inl>


static void ucp_proto_get_offload_bcopy_unpack(void *arg, const void *data,
                                               size_t length)
{
    void *dest = arg;
    ucs_memcpy_relaxed(dest, data, length, UCS_ARCH_MEMCPY_NT_SOURCE, length);
}

static UCS_F_ALWAYS_INLINE ucs_status_t
ucp_proto_get_offload_bcopy_send_func(ucp_request_t *req,
                                      const ucp_proto_multi_lane_priv_t *lpriv,
                                      ucp_datatype_iter_t *next_iter,
                                      ucp_lane_index_t *lane_shift)
{
    uct_rkey_t tl_rkey = ucp_rkey_get_tl_rkey(req->send.rma.rkey,
                                              lpriv->super.rkey_index);
    size_t max_length, length;
    void *dest;

    max_length = ucp_proto_multi_max_payload(req, lpriv, 0);
    length     = ucp_datatype_iter_next_ptr(&req->send.state.dt_iter,
                                            max_length, next_iter, &dest);
    return uct_ep_get_bcopy(ucp_ep_get_lane(req->send.ep, lpriv->super.lane),
                            ucp_proto_get_offload_bcopy_unpack, dest, length,
                            req->send.rma.remote_addr +
                                    req->send.state.dt_iter.offset,
                            tl_rkey, &req->send.state.uct_comp);
}

static void ucp_proto_get_offload_bcopy_completion(uct_completion_t *self)
{
    ucp_request_t *req = ucs_container_of(self, ucp_request_t,
                                          send.state.uct_comp);
    ucp_datatype_iter_cleanup(&req->send.state.dt_iter, 0,
                              UCS_BIT(UCP_DATATYPE_CONTIG));
    ucp_request_complete_send(req, req->send.state.uct_comp.status);
}

static ucs_status_t ucp_proto_get_offload_bcopy_progress(uct_pending_req_t *self)
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

        ucp_proto_completion_init(&req->send.state.uct_comp,
                                  ucp_proto_get_offload_bcopy_completion);
        req->flags |= UCP_REQUEST_FLAG_PROTO_INITIALIZED;
    }

    /* coverity[tainted_data_downcast] */
    return ucp_proto_multi_progress(req, req->send.proto_config->priv,
                                    ucp_proto_get_offload_bcopy_send_func,
                                    ucp_request_invoke_uct_completion_success,
                                    UCS_BIT(UCP_DATATYPE_CONTIG));
}

static void
ucp_proto_get_offload_bcopy_probe(const ucp_proto_init_params_t *init_params)
{
    ucp_context_t *context               = init_params->worker->context;
    ucp_proto_multi_init_params_t params = {
        .super.super         = *init_params,
        .super.latency       = 0,
        .super.overhead      = 0,
        .super.cfg_thresh    = context->config.ext.bcopy_thresh,
        .super.cfg_priority  = 20,
        .super.min_length    = 0,
        .super.max_length    = SIZE_MAX,
        .super.min_iov       = 0,
        .super.min_frag_offs = UCP_PROTO_COMMON_OFFSET_INVALID,
        .super.max_frag_offs = ucs_offsetof(uct_iface_attr_t,
                                            cap.get.max_bcopy),
        .super.max_iov_offs  = UCP_PROTO_COMMON_OFFSET_INVALID,
        .super.hdr_size      = 0,
        .super.send_op       = UCT_EP_OP_GET_BCOPY,
        .super.memtype_op    = UCT_EP_OP_LAST,
        .super.flags         = UCP_PROTO_COMMON_INIT_FLAG_RECV_ZCOPY |
                               UCP_PROTO_COMMON_INIT_FLAG_REMOTE_ACCESS |
                               UCP_PROTO_COMMON_INIT_FLAG_RESPONSE |
                               UCP_PROTO_COMMON_INIT_FLAG_ERR_HANDLING,
        .super.exclude_map   = 0,
        .super.reg_mem_info  = ucp_mem_info_unknown,
        .max_lanes           = UCP_PROTO_RMA_MAX_BCOPY_LANES,
        .min_chunk           = context->config.ext.min_rma_chunk_size,
        .initial_reg_md_map  = 0,
        .first.tl_cap_flags  = UCT_IFACE_FLAG_GET_BCOPY,
        .first.lane_type     = UCP_LANE_TYPE_RMA_BW,
        .middle.tl_cap_flags = UCT_IFACE_FLAG_GET_BCOPY,
        .middle.lane_type    = UCP_LANE_TYPE_RMA_BW,
        .opt_align_offs      = UCP_PROTO_COMMON_OFFSET_INVALID
    };

    if ((init_params->select_param->dt_class != UCP_DATATYPE_CONTIG) ||
        !ucp_proto_init_check_op(init_params, UCS_BIT(UCP_OP_ID_GET))) {
        return;
    }

    ucp_proto_multi_probe(&params);
}

static void ucp_proto_get_offload_reset(ucp_request_t *req)
{
    /* get_am protocol will use this field after reset, so it must be
     * initialized */
    req->send.state.completed_size = 0;
}

static ucs_status_t ucp_proto_get_offload_bcopy_reset(ucp_request_t *req)
{
    ucp_proto_get_offload_reset(req);
    return ucp_proto_request_bcopy_reset(req);
}

ucp_proto_t ucp_get_offload_bcopy_proto = {
    .name     = "get/bcopy",
    .desc     = UCP_PROTO_COPY_OUT_DESC,
    .flags    = 0,
    .probe    = ucp_proto_get_offload_bcopy_probe,
    .query    = ucp_proto_multi_query,
    .progress = {ucp_proto_get_offload_bcopy_progress},
    .abort    = ucp_proto_request_bcopy_abort,
    .reset    = ucp_proto_get_offload_bcopy_reset
};

static UCS_F_ALWAYS_INLINE ucs_status_t
ucp_proto_get_offload_zcopy_send_func(ucp_request_t *req,
                                      const ucp_proto_multi_lane_priv_t *lpriv,
                                      ucp_datatype_iter_t *next_iter,
                                      ucp_lane_index_t *lane_shift)
{
    uct_rkey_t tl_rkey = ucp_rkey_get_tl_rkey(req->send.rma.rkey,
                                              lpriv->super.rkey_index);
    size_t offset      = req->send.state.dt_iter.offset;
    const ucp_proto_multi_priv_t *mpriv;
    uct_iov_t iov;

    ucp_datatype_iter_next_iov(&req->send.state.dt_iter,
                               ucp_proto_multi_max_payload(req, lpriv, 0),
                               lpriv->super.md_index, UCP_DT_MASK_CONTIG_IOV,
                               next_iter, &iov, 1);

    mpriv = req->send.proto_config->priv;
    ucp_proto_common_zcopy_adjust_min_frag(req, mpriv->min_frag, iov.length,
                                           &iov, 1, &offset);
    return uct_ep_get_zcopy(ucp_ep_get_lane(req->send.ep, lpriv->super.lane),
                            &iov, 1, req->send.rma.remote_addr + offset,
                            tl_rkey, &req->send.state.uct_comp);
}

static ucs_status_t ucp_proto_get_offload_zcopy_progress(uct_pending_req_t *self)
{
    ucp_request_t *req = ucs_container_of(self, ucp_request_t, send.uct);

    /* coverity[tainted_data_downcast] */
    return ucp_proto_multi_zcopy_progress(
            req, req->send.proto_config->priv, ucp_proto_multi_rma_init_func,
            UCT_MD_MEM_ACCESS_LOCAL_WRITE, UCP_DT_MASK_CONTIG_IOV,
            ucp_proto_get_offload_zcopy_send_func,
            ucp_request_invoke_uct_completion_success,
            ucp_proto_request_zcopy_completion);
}

static void
ucp_proto_get_offload_zcopy_probe(const ucp_proto_init_params_t *init_params)
{
    ucp_context_t *context               = init_params->worker->context;
    ucp_proto_multi_init_params_t params = {
        .super.super         = *init_params,
        .super.latency       = 0,
        .super.overhead      = 0,
        .super.cfg_thresh    = context->config.ext.zcopy_thresh,
        .super.cfg_priority  = 30,
        .super.min_length    = 0,
        .super.max_length    = SIZE_MAX,
        .super.min_iov       = 1,
        .super.min_frag_offs = ucs_offsetof(uct_iface_attr_t,
                                            cap.get.min_zcopy),
        .super.max_frag_offs = ucs_offsetof(uct_iface_attr_t,
                                            cap.get.max_zcopy),
        .super.max_iov_offs  = ucs_offsetof(uct_iface_attr_t, cap.get.max_iov),
        .super.hdr_size      = 0,
        .super.send_op       = UCT_EP_OP_GET_ZCOPY,
        .super.memtype_op    = UCT_EP_OP_LAST,
        .super.flags         = UCP_PROTO_COMMON_INIT_FLAG_SEND_ZCOPY |
                               UCP_PROTO_COMMON_INIT_FLAG_RECV_ZCOPY |
                               UCP_PROTO_COMMON_INIT_FLAG_REMOTE_ACCESS |
                               UCP_PROTO_COMMON_INIT_FLAG_RESPONSE |
                               UCP_PROTO_COMMON_INIT_FLAG_MIN_FRAG |
                               UCP_PROTO_COMMON_INIT_FLAG_ERR_HANDLING,
        .super.exclude_map   = 0,
        .super.reg_mem_info  = ucp_proto_common_select_param_mem_info(
                                                     init_params->select_param),
        .max_lanes           = context->config.ext.max_rma_lanes,
        .min_chunk           = context->config.ext.min_rma_chunk_size,
        .initial_reg_md_map  = 0,
        .first.tl_cap_flags  = UCT_IFACE_FLAG_GET_ZCOPY,
        .first.lane_type     = UCP_LANE_TYPE_RMA_BW,
        .middle.tl_cap_flags = UCT_IFACE_FLAG_GET_ZCOPY,
        .middle.lane_type    = UCP_LANE_TYPE_RMA_BW,
        .opt_align_offs      = UCP_PROTO_COMMON_OFFSET_INVALID
    };

    if (!ucp_proto_init_check_op(init_params, UCS_BIT(UCP_OP_ID_GET))) {
        return;
    }

#if GTEST_MODE == 1
    return;
#endif
    ucp_proto_multi_probe(&params);
}

static void
ucp_proto_get_offload_zcopy_ppln_probe(const ucp_proto_init_params_t *init_params)
{
    ucp_context_t *context               = init_params->worker->context;
    ucp_proto_multi_init_params_t params = {
        .super.super         = *init_params,
        .super.latency       = 0,
        .super.overhead      = 0,
        .super.cfg_thresh    = 0,
        .super.cfg_priority  = 60,
        .super.min_length    = 0,
        .super.max_length    = SIZE_MAX,
        .super.min_iov       = 1,
        .super.min_frag_offs = ucs_offsetof(uct_iface_attr_t,
                                            cap.get.min_zcopy),
        .super.max_frag_offs = ucs_offsetof(uct_iface_attr_t,
                                            cap.get.max_zcopy),
        .super.max_iov_offs  = ucs_offsetof(uct_iface_attr_t, cap.get.max_iov),
        .super.hdr_size      = 0,
        .super.send_op       = UCT_EP_OP_GET_ZCOPY,
        .super.memtype_op    = UCT_EP_OP_LAST,
        .super.flags         = UCP_PROTO_COMMON_INIT_FLAG_SEND_ZCOPY |
                               UCP_PROTO_COMMON_INIT_FLAG_RECV_ZCOPY |
                               UCP_PROTO_COMMON_INIT_FLAG_REMOTE_ACCESS |
                               UCP_PROTO_COMMON_INIT_FLAG_RESPONSE |
                               UCP_PROTO_COMMON_INIT_FLAG_MIN_FRAG |
                               UCP_PROTO_COMMON_INIT_FLAG_ERR_HANDLING,
        .super.exclude_map   = 0,
        .super.reg_mem_info  = ucp_proto_common_select_param_mem_info(
                                                     init_params->select_param),
        .max_lanes           = context->config.ext.max_rma_lanes,
        .min_chunk           = context->config.ext.min_rma_chunk_size,
        .initial_reg_md_map  = 0,
        .first.tl_cap_flags  = UCT_IFACE_FLAG_PUT_ZCOPY /* TODO: PUT should be actually requested on the other side */
            | UCT_IFACE_FLAG_AM_BCOPY | UCT_IFACE_FLAG_AM_SHORT,
        .first.lane_type     = UCP_LANE_TYPE_RMA_BW,
        .middle.tl_cap_flags = UCT_IFACE_FLAG_PUT_ZCOPY /* TODO: Same here */
            | UCT_IFACE_FLAG_AM_BCOPY | UCT_IFACE_FLAG_AM_SHORT,
        .middle.lane_type    = UCP_LANE_TYPE_RMA_BW,
        .opt_align_offs      = UCP_PROTO_COMMON_OFFSET_INVALID
    };

    if (!ucp_proto_rma_ppln_env_and_check(init_params)) {
        return;
    }

    if (!ucp_proto_init_check_op(init_params, UCS_BIT(UCP_OP_ID_GET))) {
        return;
    }

    ucp_proto_multi_probe(&params);
}

static ucs_status_t ucp_proto_get_offload_zcopy_reset(ucp_request_t *req)
{
    ucp_proto_get_offload_reset(req);
    return ucp_proto_offload_zcopy_reset(req);
}

static void
ucp_proto_get_ppln_completion(uct_completion_t *self)
{
    ucs_debug("get ppln rts ppln request completed");
}

static ucs_status_t ucp_proto_get_offload_zcopy_ppln_progress(uct_pending_req_t *self)
{
    ucp_request_t *req                  = ucs_container_of(self, ucp_request_t, send.uct);
    const ucp_proto_multi_priv_t *mpriv = req->send.proto_config->priv;
    ucs_status_t status                 = UCS_OK;
    ucp_datatype_iter_t *dt_iter        = &req->send.state.dt_iter;
    ucp_rts_ppln_t rts_ppln;
    ucp_ep_h ep;

    if (!(req->flags & UCP_REQUEST_FLAG_PROTO_INITIALIZED)) {
        status = ucp_proto_request_zcopy_init(req, mpriv->reg_md_map,
                                              ucp_proto_get_ppln_completion,
                                              UCT_MD_MEM_ACCESS_LOCAL_WRITE,
                                              UCP_DT_MASK_CONTIG_IOV);
        if (status != UCS_OK) {
            goto out_abort;
        }

        ucp_proto_multi_request_init(req);

	req->frag_count = ucp_proto_rma_ppln_get_frag_count(req, dt_iter->length);
        req->ctx        = NULL;

        req->flags |= UCP_REQUEST_FLAG_PROTO_INITIALIZED;
    }

    req->send.lane = ucp_ep_get_am_lane(req->send.ep);

    /* Forge a self-initiated response with a non-null ep_id */
    rts_ppln.ep_id  = ucp_ep_remote_id(req->send.ep);
    if (rts_ppln.ep_id == 0) {
        return UCS_ERR_NO_RESOURCE;
    }
    rts_ppln.req    = req;
    rts_ppln.count  = req->frag_count;
    rts_ppln.md_map = mpriv->reg_md_map;

    rts_ppln.sys_dev = dt_iter->mem_info.sys_dev;

    rts_ppln.get.buffer       = (void *)req->send.rma.remote_addr;
    rts_ppln.get.mem_type     = req->send.rma.rkey->mem_type;
    rts_ppln.get.sys_dev      = ucp_rkey_config(req->send.ep->worker,
                                                req->send.rma.rkey)->key.sys_dev;

    rts_ppln.get.final_buffer = (void *)dt_iter->type.contig.buffer;
    rts_ppln.get.length       = req->send.state.dt_iter.length;

    ucs_debug("get ppln rts_resp get_req=%p ep=%p final_va=%p final_va_mem_type=%u "
              "final_va_sys_dev=%u final_rva=%p final_rva_sys_dev=%u length=%zu "
              "frag_count=%zu",
              req, req->send.ep,
              rts_ppln.get.final_buffer, rts_ppln.get.mem_type,
              rts_ppln.sys_dev,
              (void*)req->send.rma.remote_addr,
              rts_ppln.get.sys_dev,
              req->send.state.dt_iter.length, req->frag_count);

    /* Allocate the bounce buffers and trigger the message */
    ep     = req->send.ep;
    ucp_worker_flush_ops_count_add(ep->worker, +1);
    status = ucp_proto_rma_ppln_send_rts_resp(ep->worker, ep, &rts_ppln);
    ucs_assertv_always(status == UCS_OK, "GET sending rts resp status=%d", status);
    ucp_ep_rma_remote_request_sent(ep); // Force flush to wait
    return status;

out_abort:
    ucp_proto_request_abort(req, status);
    return UCS_OK; /* Removes from pending */
}

ucp_proto_t ucp_get_offload_zcopy_proto = {
    .name     = "get/zcopy",
    .desc     = UCP_PROTO_ZCOPY_DESC,
    .flags    = 0,
    .probe    = ucp_proto_get_offload_zcopy_probe,
    .query    = ucp_proto_multi_query,
    .progress = {ucp_proto_get_offload_zcopy_progress},
    .abort    = ucp_proto_request_zcopy_abort,
    .reset    = ucp_proto_get_offload_zcopy_reset
};

ucp_proto_t ucp_get_offload_zcopy_ppln_proto = {
    .name     = "get/zcopy/ppln",
    .desc     = UCP_PROTO_ZCOPY_PPLN_DESC,
    .flags    = 0,
    .probe    = ucp_proto_get_offload_zcopy_ppln_probe,
    .query    = ucp_proto_multi_query,
    .progress = {ucp_proto_get_offload_zcopy_ppln_progress},
    .abort    = ucp_proto_request_zcopy_abort,
    .reset    = ucp_proto_get_offload_zcopy_reset
};
