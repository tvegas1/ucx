/**
 * Copyright (c) NVIDIA CORPORATION & AFFILIATES, 2024. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "srd_def.h"
#include "srd_iface.h"
#include "srd_ep.h"

#include "../ib_efa.h"

#include <uct/ib/base/ib_log.h>


static uct_iface_ops_t uct_srd_iface_tl_ops;


uct_srd_ep_t *uct_srd_iface_cep_get_ep(uct_srd_iface_t *iface,
                                       const uct_ib_address_t *ib_addr,
                                       const uct_srd_iface_addr_t *if_addr,
                                       int path_index,
                                       uct_srd_ep_conn_sn_t conn_sn,
                                       int is_private)
{
    uct_srd_ep_t *ep                        = NULL;
    ucs_conn_match_queue_type_t queue_type = is_private ?
                                             UCS_CONN_MATCH_QUEUE_UNEXP :
                                             UCS_CONN_MATCH_QUEUE_ANY;
    ucs_conn_match_elem_t *conn_match;
    void *peer_address;

    peer_address = ucs_alloca(iface->conn_match_ctx.address_length);
    uct_srd_iface_cep_get_peer_address(iface, ib_addr, if_addr,
                                       path_index, peer_address);

    conn_match = ucs_conn_match_get_elem(&iface->conn_match_ctx, peer_address,
                                         conn_sn, queue_type, is_private);
    if (conn_match == NULL) {
        return NULL;
    }

    ep = ucs_container_of(conn_match, uct_srd_ep_t, conn_match);
    ucs_assert(ep->flags & UCT_SRD_EP_FLAG_ON_CEP);

    if (is_private) {
        ep->flags &= ~UCT_SRD_EP_FLAG_ON_CEP;
    }

    return ep;
}

static UCS_F_ALWAYS_INLINE ucs_conn_match_queue_type_t
uct_srd_iface_cep_ep_queue_type(uct_srd_ep_t *ep)
{
    return (ep->flags & UCT_SRD_EP_FLAG_PRIVATE) ?
           UCS_CONN_MATCH_QUEUE_UNEXP :
           UCS_CONN_MATCH_QUEUE_EXP;
}


ucs_status_t
uct_srd_iface_unpack_peer_address(uct_srd_iface_t *iface,
                                  const uct_ib_address_t *ib_addr,
                                  const uct_srd_iface_addr_t *if_addr,
                                  int path_index, void *address_p)
{
    uct_ib_iface_t *ib_iface                = &iface->super;
    uct_srd_ep_peer_address_t *peer_address =
        (uct_srd_ep_peer_address_t*)address_p;
    struct ibv_ah_attr ah_attr;
    enum ibv_mtu path_mtu;
    ucs_status_t status;

    memset(peer_address, 0, sizeof(*peer_address));

    uct_ib_iface_fill_ah_attr_from_addr(ib_iface, ib_addr, path_index,
                                        &ah_attr, &path_mtu);
    status = uct_ib_iface_create_ah(ib_iface, &ah_attr, "SRD connect",
                                    &peer_address->ah);
    if (status != UCS_OK) {
        return status;
    }

    peer_address->dest_qpn = uct_ib_unpack_uint24(if_addr->qp_num);

    return UCS_OK;
}

void *
uct_srd_iface_cep_get_peer_address(uct_srd_iface_t *iface,
                                   const uct_ib_address_t *ib_addr,
                                   const uct_srd_iface_addr_t *if_addr,
                                   int path_index, void *address_p)
{
    ucs_status_t status = uct_srd_iface_unpack_peer_address(iface, ib_addr,
                                                            if_addr, path_index,
                                                            address_p);

    if (status != UCS_OK) {
        ucs_fatal("iface %p: failed to get peer address", iface);
    }

    return address_p;
}

void uct_srd_iface_cep_insert_ep(uct_srd_iface_t *iface,
                                 const uct_ib_address_t *ib_addr,
                                 const uct_srd_iface_addr_t *if_addr,
                                 int path_index, uct_srd_ep_conn_sn_t conn_sn,
                                 uct_srd_ep_t *ep)
{
    ucs_conn_match_queue_type_t queue_type;
    void *peer_address;

    queue_type   = uct_srd_iface_cep_ep_queue_type(ep);
    peer_address = ucs_alloca(iface->conn_match_ctx.address_length);
    uct_srd_iface_cep_get_peer_address(iface, ib_addr, if_addr, path_index,
                                       peer_address);

    ucs_assert(!(ep->flags & UCT_SRD_EP_FLAG_ON_CEP));
    ucs_conn_match_insert(&iface->conn_match_ctx, peer_address,
                          conn_sn, &ep->conn_match, queue_type);
    ep->flags |= UCT_SRD_EP_FLAG_ON_CEP;
}

ucs_status_t
uct_srd_iface_get_address(uct_iface_h tl_iface, uct_iface_addr_t *iface_addr)
{
    uct_srd_iface_t *iface = ucs_derived_of(tl_iface, uct_srd_iface_t);
    uct_srd_iface_addr_t *addr = (uct_srd_iface_addr_t *)iface_addr;

    uct_ib_pack_uint24(addr->qp_num, iface->qp->qp_num);

    return UCS_OK;
}

static void uct_srd_iface_vfs_refresh(uct_iface_h iface)
{
}

static ucs_status_t uct_srd_ep_invalidate(uct_ep_h tl_ep, unsigned flags)
{
    return UCS_ERR_UNSUPPORTED;
}

static uct_ib_iface_ops_t uct_srd_iface_ops = {
    .super = {
        .iface_estimate_perf = uct_base_iface_estimate_perf,
        .iface_vfs_refresh   = uct_srd_iface_vfs_refresh,
        .ep_query            = (uct_ep_query_func_t)
            ucs_empty_function_return_unsupported,
        .ep_invalidate       = uct_srd_ep_invalidate,
        .iface_is_reachable_v2 = uct_ib_iface_is_reachable_v2,
    },
    .create_cq      = uct_ib_verbs_create_cq,
    .destroy_cq     = uct_ib_verbs_destroy_cq,
    .event_cq       = (uct_ib_iface_event_cq_func_t)ucs_empty_function,
    .handle_failure = (uct_ib_iface_handle_failure_func_t)
        ucs_empty_function_do_assert
};

static ucs_status_t
uct_srd_query_tl_devices(uct_md_h md, uct_tl_device_resource_t **tl_devices_p,
                         unsigned *num_tl_devices_p)
{
    uct_ib_md_t *ib_md = ucs_derived_of(md, uct_ib_md_t);

    if (!uct_ib_efadv_is_supported(ib_md->dev.ibv_context->device)) {
        return UCS_ERR_UNSUPPORTED;
    }

    return uct_ib_device_query_ports(&ib_md->dev, 0, tl_devices_p,
                                     num_tl_devices_p);
}



ucs_status_t uct_srd_iface_flush(uct_iface_h tl_iface, unsigned flags,
                                 uct_completion_t *comp)
{
    uct_srd_iface_t *iface = ucs_derived_of(tl_iface, uct_srd_iface_t);
    uct_srd_ep_t *ep;
    ucs_status_t status;
    int i, count;

    ucs_trace_func("");

    if (comp != NULL) {
        return UCS_ERR_UNSUPPORTED;
    }

    count = 0;
    ucs_ptr_array_for_each(ep, i, &iface->eps) {
        /* srd ep flush returns either ok or in progress */
        status = uct_srd_ep_flush_nolock(iface, ep, NULL);
        if ((status == UCS_INPROGRESS) || (status == UCS_ERR_NO_RESOURCE)) {
            ++count;
        }
    }

    if (count != 0) {
        UCT_TL_IFACE_STAT_FLUSH_WAIT(&iface->super.super);
        return UCS_INPROGRESS;
    }

    UCT_TL_IFACE_STAT_FLUSH(&iface->super.super);
    return UCS_OK;
}

void uct_srd_iface_add_ep(uct_srd_iface_t *iface, uct_srd_ep_t *ep)
{
    ep->ep_id = ucs_ptr_array_insert(&iface->eps, ep);
}

void uct_srd_iface_remove_ep(uct_srd_iface_t *iface, uct_srd_ep_t *ep)
{
    if (ep->ep_id != UCT_SRD_EP_NULL_ID) {
        ucs_trace("iface(%p) remove ep: %p id %d", iface, ep, ep->ep_id);
        ucs_ptr_array_remove(&iface->eps, ep->ep_id);
    }
}

static ucs_status_t
uct_srd_iface_create_qp(uct_srd_iface_t *iface,
                        const uct_srd_iface_config_t *config)
{
    uct_ib_efadv_md_t *efadv_md =
        ucs_derived_of(uct_ib_iface_md(&iface->super), uct_ib_efadv_md_t);
    const uct_ib_efadv_t *efadv = &efadv_md->efadv;
    struct ibv_pd *pd           = efadv_md->super.pd;
    struct ibv_qp_attr qp_attr;
    int ret;

#ifdef HAVE_DECL_EFA_DV_RDMA_READ
    struct efadv_qp_init_attr  efa_qp_init_attr  = { 0 };
    struct ibv_qp_init_attr_ex qp_init_attr      = { 0 };
#else
    struct ibv_qp_init_attr    qp_init_attr      = { 0 };
#endif

    qp_init_attr.qp_type             = IBV_QPT_DRIVER;
    qp_init_attr.sq_sig_all          = 1;
    qp_init_attr.send_cq             = iface->super.cq[UCT_IB_DIR_TX];
    qp_init_attr.recv_cq             = iface->super.cq[UCT_IB_DIR_RX];
    qp_init_attr.cap.max_send_wr     = ucs_min(config->super.tx.queue_len,
                                               uct_ib_efadv_max_sq_wr(efadv));
    qp_init_attr.cap.max_recv_wr     = ucs_min(config->super.rx.queue_len,
                                               uct_ib_efadv_max_rq_wr(efadv));
    qp_init_attr.cap.max_send_sge    = 1 + ucs_min(config->super.tx.min_sge,
                                                   (uct_ib_efadv_max_sq_sge(efadv) - 1));
    qp_init_attr.cap.max_recv_sge    = 1;
    qp_init_attr.cap.max_inline_data = uct_ib_efadv_inline_buf_size(efadv);

#ifdef HAVE_DECL_EFA_DV_RDMA_READ
    qp_init_attr.pd                  = efadv_md->super.pd;
    qp_init_attr.comp_mask           = IBV_QP_INIT_ATTR_PD |
                                       IBV_QP_INIT_ATTR_SEND_OPS_FLAGS;
    qp_init_attr.send_ops_flags      = IBV_QP_EX_WITH_SEND;
    if (uct_ib_efadv_has_rdma_read(efadv)) {
        qp_init_attr.send_ops_flags |= IBV_QP_EX_WITH_RDMA_READ;
    }
    efa_qp_init_attr.driver_qp_type  = EFADV_QP_DRIVER_TYPE_SRD;

    iface->qp    = efadv_create_qp_ex(pd->context, &qp_init_attr,
                                      &efa_qp_init_attr,
                                      sizeof(efa_qp_init_attr));
    iface->qp_ex = ibv_qp_to_qp_ex(iface->qp);
#else
    iface->qp = efadv_create_driver_qp(pd, &qp_init_attr,
                                       EFADV_QP_DRIVER_TYPE_SRD);
#endif

    if (iface->qp == NULL) {
        ucs_error("iface=%p: failed to create %s QP on "UCT_IB_IFACE_FMT
                  " TX wr:%d sge:%d inl:%d resp:%d RX wr:%d sge:%d resp:%d: %m",
                  iface, uct_ib_qp_type_str(UCT_IB_QPT_SRD),
                  UCT_IB_IFACE_ARG(&iface->super),
                  qp_init_attr.cap.max_send_wr,
                  qp_init_attr.cap.max_send_sge,
                  qp_init_attr.cap.max_inline_data,
                  iface->super.config.max_inl_cqe[UCT_IB_DIR_TX],
                  qp_init_attr.cap.max_recv_wr,
                  qp_init_attr.cap.max_recv_sge,
                  iface->super.config.max_inl_cqe[UCT_IB_DIR_RX]);
        return UCS_ERR_IO_ERROR;
    }

    /* TODO: Use common configuration */
    iface->config.max_inline = qp_init_attr.cap.max_inline_data;
    iface->config.tx_qp_len  = qp_init_attr.cap.max_send_wr;
    iface->tx.available      = qp_init_attr.cap.max_send_wr;
    iface->rx.available      = qp_init_attr.cap.max_recv_wr;

    ucs_debug("iface=%p: created %s QP 0x%x on "UCT_IB_IFACE_FMT
              " TX wr:%d sge:%d inl:%d resp:%d RX wr:%d sge:%d resp:%d",
              iface, uct_ib_qp_type_str(UCT_IB_QPT_SRD),
              iface->qp->qp_num, UCT_IB_IFACE_ARG(&iface->super),
              qp_init_attr.cap.max_send_wr,
              qp_init_attr.cap.max_send_sge,
              qp_init_attr.cap.max_inline_data,
              iface->super.config.max_inl_cqe[UCT_IB_DIR_TX],
              qp_init_attr.cap.max_recv_wr,
              qp_init_attr.cap.max_recv_sge,
              iface->super.config.max_inl_cqe[UCT_IB_DIR_RX]);


    memset(&qp_attr, 0, sizeof(qp_attr));
    /* Modify QP to INIT state */
    qp_attr.qp_state   = IBV_QPS_INIT;
    qp_attr.pkey_index = iface->super.pkey_index;
    qp_attr.port_num   = iface->super.config.port_num;
    qp_attr.qkey       = UCT_IB_KEY;
    ret = ibv_modify_qp(iface->qp, &qp_attr,
                        IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_QKEY);
    if (ret) {
        ucs_error("Failed to modify SRD QP to INIT: %m");
        goto err_destroy_qp;
    }

    /* Modify to RTR */
    qp_attr.qp_state = IBV_QPS_RTR;
    ret = ibv_modify_qp(iface->qp, &qp_attr, IBV_QP_STATE);
    if (ret) {
        ucs_error("Failed to modify SRD QP to RTR: %m");
        goto err_destroy_qp;
    }

    /* Modify to RTS */
    qp_attr.qp_state = IBV_QPS_RTS;
    qp_attr.sq_psn = 0;
    ret = ibv_modify_qp(iface->qp, &qp_attr, IBV_QP_STATE | IBV_QP_SQ_PSN);
    if (ret) {
        ucs_error("Failed to modify SRD QP to RTS: %m");
        goto err_destroy_qp;
    }

    return UCS_OK;
err_destroy_qp:
    uct_ib_destroy_qp(iface->qp);
    return UCS_ERR_INVALID_PARAM;
}


static const char*
uct_srd_iface_peer_address_str(const uct_srd_iface_t *iface,
                               const void *address,
                               char *str, size_t max_size)
{
    const uct_srd_ep_peer_address_t *peer_address =
        (const uct_srd_ep_peer_address_t*)address;

    ucs_snprintf_zero(str, max_size, "ah=%p dest_qpn=%u",
                      peer_address->ah, peer_address->dest_qpn);
    return str;
}

static const void *
uct_srd_ep_get_conn_address(const ucs_conn_match_elem_t *elem)
{
    uct_srd_ep_t *ep = ucs_container_of(elem, uct_srd_ep_t, conn_match);

    return &ep->peer_address;
}

static ucs_conn_sn_t
uct_srd_iface_conn_match_get_conn_sn(const ucs_conn_match_elem_t *elem)
{
    uct_srd_ep_t *ep = ucs_container_of(elem, uct_srd_ep_t, conn_match);
    return ep->conn_sn;
}

static const char *
uct_srd_iface_conn_match_peer_address_str(const ucs_conn_match_ctx_t *conn_match_ctx,
                                          const void *address,
                                          char *str, size_t max_size)
{
    uct_srd_iface_t *iface = ucs_container_of(conn_match_ctx,
                                              uct_srd_iface_t,
                                              conn_match_ctx);
    return uct_srd_iface_peer_address_str(iface, address, str, max_size);
}

static void
uct_srd_iface_conn_match_purge_cb(ucs_conn_match_ctx_t *conn_match_ctx,
                                  ucs_conn_match_elem_t *elem)
{
    uct_srd_ep_t *ep = ucs_container_of(elem, uct_srd_ep_t, conn_match);

    ep->flags &= ~UCT_SRD_EP_FLAG_ON_CEP;
    return UCS_CLASS_DELETE_FUNC_NAME(uct_srd_ep_t)(&ep->super.super);
}

static ucs_conn_match_ops_t conn_match_ops = {
    .get_address = uct_srd_ep_get_conn_address,
    .get_conn_sn = uct_srd_iface_conn_match_get_conn_sn,
    .address_str = uct_srd_iface_conn_match_peer_address_str,
    .purge_cb    = uct_srd_iface_conn_match_purge_cb
};

void uct_srd_iface_release_recv_desc(uct_recv_desc_t *self, void *desc)
{
    uct_ib_iface_release_desc(self, desc);
}

void uct_srd_iface_send_op_release(uct_srd_send_op_t *send_op)
{
    ucs_assert(!(send_op->flags & UCT_SRD_SEND_OP_FLAG_INVALID));
    send_op->flags = UCT_SRD_SEND_OP_FLAG_INVALID;
    ucs_mpool_put(send_op);
}

void uct_srd_iface_send_op_ucomp_release(uct_srd_send_op_t *send_op)
{
    ucs_assert(!(send_op->flags & UCT_SRD_SEND_OP_FLAG_INVALID));
    if (!(send_op->flags & UCT_SRD_SEND_OP_FLAG_PURGED)) {
        uct_invoke_completion(send_op->user_comp, UCS_OK);
    }
    uct_srd_iface_send_op_release(send_op);
}


static void uct_srd_iface_send_op_init(ucs_mpool_t *mp, void *obj, void *chunk)
{
    uct_srd_send_op_t *send_op = obj;

    send_op->flags = UCT_SRD_SEND_OP_FLAG_INVALID;
}

static void uct_srd_iface_send_desc_init(uct_iface_h tl_iface, void *obj,
                                         uct_mem_h memh)
{
    uct_srd_send_desc_t *desc = obj;

    desc->lkey        = uct_ib_memh_get_lkey(memh);
    desc->super.flags = UCT_SRD_SEND_OP_FLAG_INVALID;
}


static ucs_mpool_ops_t uct_srd_send_op_mpool_ops = {
    .chunk_alloc   = ucs_mpool_chunk_malloc,
    .chunk_release = ucs_mpool_chunk_free,
    .obj_init      = uct_srd_iface_send_op_init,
    .obj_cleanup   = NULL
};

static UCS_F_NOINLINE void
uct_srd_iface_post_recv_always(uct_srd_iface_t *iface, int max)
{
    struct ibv_recv_wr *bad_wr;
    uct_ib_recv_wr_t *wrs;
    unsigned count;
    int ret;

    wrs  = ucs_alloca(sizeof *wrs  * max);

    count = uct_ib_iface_prepare_rx_wrs(&iface->super, &iface->rx.mp, wrs, max);
    if (count == 0) {
        return;
    }

    ret = ibv_post_recv(iface->qp, &wrs[0].ibwr, &bad_wr);
    if (ret != 0) {
        ucs_fatal("ibv_post_recv() returned %d: %m", ret);
    }
    iface->rx.available -= count;
}

static UCS_F_ALWAYS_INLINE void
uct_srd_iface_post_recv(uct_srd_iface_t *iface)
{
    unsigned batch = iface->super.config.rx_max_batch;

    if (iface->rx.available < batch) {
        return;
    }

    uct_srd_iface_post_recv_always(iface, batch);
}

static ucs_status_t
uct_srd_iface_init_fc_thresh(uct_srd_iface_t *iface,
                             uct_srd_iface_config_t *config)
{
    iface->config.fc_wnd_size = ucs_min(config->fc.wnd_size,
                                        config->super.rx.queue_len);

    if (config->fc.hard_thresh >= 1) {
        ucs_error("The factor for hard FC threshold should be less than 1");
        return UCS_ERR_INVALID_PARAM;
    }

    iface->config.fc_hard_thresh = iface->config.fc_wnd_size *
        config->fc.hard_thresh;

    if ((config->fc.soft_thresh <= config->fc.hard_thresh) ||
        (config->fc.soft_thresh >= 1)) {
        ucs_error("The factor for soft FC threshold should be bigger"
                  " than FC_HARD_THRESH value and less than 1 (s=%f, h=%f)",
                  config->fc.soft_thresh, config->fc.hard_thresh);
        return UCS_ERR_INVALID_PARAM;
    }

    iface->config.fc_soft_thresh = iface->config.fc_wnd_size *
        config->fc.soft_thresh;
    return UCS_OK;
}

static UCS_CLASS_INIT_FUNC(uct_srd_iface_t, uct_md_h md, uct_worker_h worker,
                           const uct_iface_params_t *params,
                           const uct_iface_config_t *tl_config)
{
    uct_srd_iface_config_t *config      = ucs_derived_of(tl_config,
                                                    uct_srd_iface_config_t);
    uct_ib_efadv_md_t *efa_md           = ucs_derived_of(md, uct_ib_efadv_md_t);
    uct_ib_iface_init_attr_t init_attr;
    ucs_status_t status;
    int mtu;
    size_t data_size;
    ucs_mpool_params_t mp_params;

    UCT_CHECK_PARAM(params->field_mask & UCT_IFACE_PARAM_FIELD_OPEN_MODE,
                    "UCT_IFACE_PARAM_FIELD_OPEN_MODE not set");
    if (!(params->open_mode & UCT_IFACE_OPEN_MODE_DEVICE)) {
        ucs_error("only UCT_IFACE_OPEN_MODE_DEVICE is supported");
        return UCS_ERR_UNSUPPORTED;
    }

    status = uct_ib_device_mtu(params->mode.device.dev_name, md, &mtu);
    if (status != UCS_OK) {
        return status;
    }

    init_attr.cq_len[UCT_IB_DIR_TX] = config->super.tx.queue_len;
    init_attr.cq_len[UCT_IB_DIR_RX] = config->super.rx.queue_len;
    init_attr.rx_priv_len           = sizeof(uct_srd_recv_desc_t) -
                                      sizeof(uct_ib_iface_recv_desc_t);
    init_attr.rx_hdr_len            = sizeof(uct_srd_neth_t);
    init_attr.seg_size              = ucs_min(mtu, config->super.seg_size);
    init_attr.qp_type               = IBV_QPT_DRIVER;

    UCS_CLASS_CALL_SUPER_INIT(uct_ib_iface_t, &uct_srd_iface_tl_ops,
                              &uct_srd_iface_ops, md, worker,
                              params, &config->super, &init_attr);

    self->super.release_desc.cb = uct_srd_iface_release_recv_desc;

    status = uct_srd_iface_create_qp(self, config);
    if (status != UCS_OK) {
        return status;
    }


    ucs_mpool_params_reset(&mp_params);
    mp_params.name            = "srd_send_op";
    mp_params.elem_size       = sizeof(uct_srd_send_op_t);
    mp_params.align_offset    = 0;
    mp_params.alignment       = UCT_SRD_SEND_OP_ALIGN;
    mp_params.elems_per_chunk = 256;
    mp_params.ops             = &uct_srd_send_op_mpool_ops;
    
    status = ucs_mpool_init(&mp_params, &self->tx.send_op_mp);
    if (status != UCS_OK) {
        goto err_tx_desc_mpool;
    }

    ucs_arbiter_init(&self->tx.pending_q);
    ucs_ptr_array_init(&self->eps, "srd_eps");

    status = uct_ib_iface_recv_mpool_init(&self->super, &config->super, params,
                                          "srd_recv_desc", &self->rx.mp);
    if (status != UCS_OK) {
        goto err_qp;
    }

    self->rx.quota =
        self->rx.available - ucs_min(self->rx.available,
                                     config->ud_common.rx_queue_len_init);
    self->rx.available -= self->rx.quota;

    ucs_mpool_grow(&self->rx.mp, self->rx.available);

    memset(&self->tx.wr_inl, 0, sizeof(self->tx.wr_inl));
    self->tx.wr_inl.opcode               = IBV_WR_SEND;
    self->tx.wr_inl.wr_id                = 0xBEEBBEEB;
    self->tx.wr_inl.wr.ud.remote_qkey    = UCT_IB_KEY;
    self->tx.wr_inl.imm_data             = 0;
    self->tx.wr_inl.next                 = 0;
    self->tx.wr_inl.sg_list              = self->tx.sge;

    memset(&self->tx.wr_desc, 0, sizeof(self->tx.wr_desc));
    self->tx.wr_desc.opcode              = IBV_WR_SEND;
    self->tx.wr_desc.wr_id               = 0xFAAFFAAF;
    self->tx.wr_desc.wr.ud.remote_qkey   = UCT_IB_KEY;
    self->tx.wr_desc.imm_data            = 0;
    self->tx.wr_desc.next                = 0;
    self->tx.wr_desc.sg_list             = self->tx.sge;
    self->tx.wr_desc.num_sge             = 1;

    self->tx.desc                        = NULL;
    self->tx.send_op                     = NULL;

    data_size = self->super.config.seg_size;
    status = uct_iface_mpool_init(&self->super.super, &self->tx.desc_mp,
                                  sizeof(uct_srd_send_desc_t) + data_size,
                                  sizeof(uct_srd_send_desc_t),
                                  UCT_SRD_SEND_DESC_ALIGN,
                                  &config->super.tx.mp,
                                  self->config.tx_qp_len,
                                  uct_srd_iface_send_desc_init, "srd_send_desc");
    if (status != UCS_OK) {
        goto err_tx_desc_mpool;
    }

    self->super.config.sl = uct_ib_iface_config_select_sl(&config->super);
    ucs_conn_match_init(&self->conn_match_ctx,
                        sizeof(uct_srd_ep_peer_address_t),
                        UCT_SRD_IFACE_CEP_CONN_SN_MAX, &conn_match_ops);

    self->config.max_send_sge = uct_ib_efadv_max_sq_sge(&efa_md->efadv);

    self->config.max_get_zcopy = efa_md->efadv.attr.max_rdma_size;

    /* Check and set FC parameters */
    status = uct_srd_iface_init_fc_thresh(self, config);
    if (status != UCS_OK) {
        goto err_rx_mpool;
    }

    while (self->rx.available >= self->super.config.rx_max_batch) {
        uct_srd_iface_post_recv(self);
    }

    return UCS_OK;

    /* TODO Fix goto labels */
err_rx_mpool:
    ucs_mpool_cleanup(&self->rx.mp, 1);
err_tx_desc_mpool:
    ucs_mpool_cleanup(&self->tx.desc_mp, 1);
err_tx_send_op_mpool:
    ucs_mpool_cleanup(&self->tx.send_op_mp, 1);
err_qp:
    uct_ib_destroy_qp(self->qp);
    ucs_ptr_array_cleanup(&self->eps, 1);
    return status;
}

static UCS_CLASS_CLEANUP_FUNC(uct_srd_iface_t)
{
    /* TODO Cleanups here and valgrind checks */
    ucs_ptr_array_cleanup(&self->eps, 1);
    ucs_conn_match_cleanup(&self->conn_match_ctx);
    ucs_arbiter_cleanup(&self->tx.pending_q);
}

UCS_CLASS_DEFINE(uct_srd_iface_t, uct_ib_iface_t);

static UCS_CLASS_DEFINE_NEW_FUNC(uct_srd_iface_t, uct_iface_t, uct_md_h,
                                 uct_worker_h, const uct_iface_params_t*,
                                 const uct_iface_config_t*);


static UCS_CLASS_DEFINE_DELETE_FUNC(uct_srd_iface_t, uct_iface_t);

ucs_config_field_t uct_srd_iface_config_table[] = {
    {UCT_IB_CONFIG_PREFIX, "", NULL,
     ucs_offsetof(uct_srd_iface_config_t, super),
     UCS_CONFIG_TYPE_TABLE(uct_ib_iface_config_table)},

    {"SRD_", "", NULL,
     ucs_offsetof(uct_srd_iface_config_t, ud_common),
     UCS_CONFIG_TYPE_TABLE(uct_ud_iface_common_config_table)},

    {"FC_WND_SIZE", "512",
        "The size of flow control window per endpoint. limits the number of AM\n"
            "which can be sent w/o acknowledgment.",
        ucs_offsetof(uct_srd_iface_config_t, fc.wnd_size), UCS_CONFIG_TYPE_UINT},

    {"FC_SOFT_THRESH", "0.5",
        "Threshold for sending soft request for FC credits to the peer. This value\n"
            "refers to the percentage of the FC_WND_SIZE value. (must be > HARD_THRESH and < 1)",
        ucs_offsetof(uct_srd_iface_config_t, fc.soft_thresh), UCS_CONFIG_TYPE_DOUBLE},

    {"FC_HARD_THRESH", "0.25",
        "Threshold for sending hard request for FC credits to the peer. This value\n"
            "refers to the percentage of the FC_WND_SIZE value. (must be > 0 and < 1)",
        ucs_offsetof(uct_srd_iface_config_t, fc.hard_thresh), UCS_CONFIG_TYPE_DOUBLE},

    {NULL}
};


static void uct_srd_iface_progress_enable(uct_iface_h tl_iface, unsigned flags)
{
    uct_srd_iface_t *iface = ucs_derived_of(tl_iface, uct_srd_iface_t);

    if (flags & UCT_PROGRESS_RECV) {
        iface->rx.available += iface->rx.quota;
        iface->rx.quota      = 0;
        /* let progress post the missing receives */
    }

    uct_base_iface_progress_enable(tl_iface, flags);
}


static void uct_srd_iface_send_completion(uct_srd_iface_t *iface,
                                          uct_srd_send_op_t *send_op)
{
    ucs_assert(!(send_op->flags & UCT_SRD_SEND_OP_FLAG_INVALID));
    uct_srd_ep_send_completion(send_op);
}


static UCS_F_ALWAYS_INLINE unsigned
uct_srd_iface_poll_rx(uct_srd_iface_t *iface)
{
    unsigned num_wcs = iface->super.config.rx_max_poll;
    struct ibv_wc wc[num_wcs];
    ucs_status_t status;
    void *packet;
    int i;

    status = uct_ib_poll_cq(iface->super.cq[UCT_IB_DIR_RX], &num_wcs, wc);
    if (status != UCS_OK) {
        num_wcs = 0;
        goto out;
    }

    UCT_IB_IFACE_VERBS_FOREACH_RXWQE(&iface->super, i, packet, wc, num_wcs) {
        uct_ib_log_recv_completion(&iface->super, &wc[i], packet,
                                   wc[i].byte_len, uct_srd_dump_packet);
        uct_srd_ep_process_rx(iface, (uct_srd_neth_t *)packet, wc[i].byte_len,
                              (uct_srd_recv_desc_t *)wc[i].wr_id);
    }
    iface->rx.available += num_wcs;
out:
    uct_srd_iface_post_recv(iface);
    return num_wcs;
}

static UCS_F_ALWAYS_INLINE unsigned
uct_srd_iface_poll_tx(uct_srd_iface_t *iface)
{
    unsigned num_wcs = iface->super.config.tx_max_poll;
    struct ibv_wc wc[num_wcs];
    ucs_status_t status;
    int i;

    status = uct_ib_poll_cq(iface->super.cq[UCT_IB_DIR_TX], &num_wcs, wc);
    if (status != UCS_OK) {
        num_wcs = 0;
    }

    for (i = 0; i < num_wcs; i++) {
        if (ucs_unlikely(wc[i].status != IBV_WC_SUCCESS)) {
            UCT_IB_IFACE_VERBS_COMPLETION_ERR("send", &iface->super, i, wc);
            continue;
        }

        uct_srd_iface_send_completion(iface, (uct_srd_send_op_t*)wc[i].wr_id);
    }

    iface->tx.available += num_wcs;
    return num_wcs;
}

static unsigned uct_srd_iface_progress(uct_iface_h tl_iface)
{
    uct_srd_iface_t *iface = ucs_derived_of(tl_iface, uct_srd_iface_t);
    unsigned count;

    count = uct_srd_iface_poll_rx(iface);
    if (count == 0) {
        count = uct_srd_iface_poll_tx(iface);
    }

    uct_srd_iface_progress_pending(iface);
    return count;
}

ucs_status_t
uct_srd_iface_query(uct_iface_h tl_iface, uct_iface_attr_t *iface_attr)
{
    uct_srd_iface_t *iface = ucs_derived_of(tl_iface, uct_srd_iface_t);
    uct_ib_md_t *md        = uct_ib_iface_md(&iface->super);
    uct_ib_efadv_md_t *efa_md = ucs_derived_of(md, uct_ib_efadv_md_t);
    size_t active_mtu      =
        uct_ib_mtu_value(uct_ib_iface_port_attr(&iface->super)->active_mtu);
    ucs_status_t status;
    size_t max_rdma_size;

    /* Common */
    status = uct_ib_iface_query(&iface->super,
                                UCT_IB_DETH_LEN + sizeof(uct_srd_neth_t),
                                iface_attr);

    /* General */
    iface_attr->cap.am.align_mtu        = active_mtu;
    iface_attr->cap.get.align_mtu       = active_mtu;
    iface_attr->cap.am.opt_zcopy_align  = UCS_SYS_PCI_MAX_PAYLOAD;
    iface_attr->cap.get.opt_zcopy_align = UCS_SYS_PCI_MAX_PAYLOAD;

    iface_attr->cap.flags      = UCT_IFACE_FLAG_AM_BCOPY         |
                                 UCT_IFACE_FLAG_AM_ZCOPY         |
                                 UCT_IFACE_FLAG_CONNECT_TO_EP    |
                                 UCT_IFACE_FLAG_CONNECT_TO_IFACE |
                                 UCT_IFACE_FLAG_PENDING          |
                                 UCT_IFACE_FLAG_EP_CHECK         |
                                 UCT_IFACE_FLAG_CB_SYNC          |
                                 UCT_IFACE_FLAG_ERRHANDLE_PEER_FAILURE;
    iface_attr->iface_addr_len = sizeof(uct_srd_iface_addr_t);
    iface_attr->ep_addr_len    = sizeof(uct_srd_ep_addr_t);
    iface_attr->max_conn_priv  = 0;

    iface_attr->latency.c += 30e-9; /* TODO: set the correct values for SRD */
    iface_attr->overhead   = 105e-9;

    /* TODO don't use MD */
    /* AM */
    iface_attr->cap.am.max_short = uct_ib_iface_hdr_size(md->dev.max_inline_data,
                                                         sizeof(uct_srd_neth_t));
    if (iface_attr->cap.am.max_short) {
        iface_attr->cap.flags |= UCT_IFACE_FLAG_AM_SHORT;
    }

    max_rdma_size = efa_md->efadv.attr.max_rdma_size;
    if (max_rdma_size > 2 * UCS_MBYTE) {
        max_rdma_size = 2 * UCS_MBYTE;
    }

        /* AM */
    iface_attr->cap.am.max_short = uct_ib_iface_hdr_size(iface->config.max_inline,
                                                         sizeof(uct_srd_neth_t));
    iface_attr->cap.am.max_bcopy = iface->super.config.seg_size - sizeof(uct_srd_neth_t);
    iface_attr->cap.am.min_zcopy = 0;
    iface_attr->cap.am.max_zcopy = iface->super.config.seg_size - sizeof(uct_srd_neth_t);
    iface_attr->cap.am.max_iov   = iface->config.max_send_sge;
    iface_attr->cap.am.max_hdr   = uct_ib_iface_hdr_size(iface->super.config.seg_size,
                                                         sizeof(uct_srd_neth_t));
    iface_attr->cap.tag.rndv.max_zcopy = max_rdma_size;

    /* GET */
    iface_attr->cap.get.max_bcopy = iface->super.config.seg_size;
    iface_attr->cap.get.min_zcopy = iface->super.config.max_inl_cqe[UCT_IB_DIR_TX] + 1;
    iface_attr->cap.get.max_zcopy = iface->config.max_get_zcopy;
    iface_attr->cap.get.max_iov   = md->dev.max_sq_sge;

    if (iface_attr->cap.get.max_bcopy) {
        iface_attr->cap.flags |= UCT_IFACE_FLAG_GET_BCOPY;
    }
    if (iface_attr->cap.get.max_zcopy) {
        iface_attr->cap.flags |= UCT_IFACE_FLAG_GET_ZCOPY;
    }
    return status;
}

static uct_iface_ops_t uct_srd_iface_tl_ops = {
    .ep_flush                 = uct_srd_ep_flush,
    .ep_fence                 = uct_base_ep_fence,
    .ep_create                = uct_srd_ep_create,
    .ep_destroy               = uct_srd_ep_destroy,
    .ep_am_bcopy              = uct_srd_ep_am_bcopy,
    .ep_am_zcopy              = uct_srd_ep_am_zcopy,
    .ep_get_zcopy             = uct_srd_ep_get_zcopy,
    .ep_am_short              = uct_srd_ep_am_short,
    .ep_pending_add           = uct_srd_ep_pending_add,
    .ep_pending_purge         = uct_srd_ep_pending_purge,
    .iface_flush              = uct_srd_iface_flush,
    .iface_fence              = uct_base_iface_fence,
    .iface_progress_enable    = uct_srd_iface_progress_enable,
    .iface_progress_disable   = uct_base_iface_progress_disable,
    .iface_progress           = uct_srd_iface_progress,
    .iface_query              = uct_srd_iface_query,
    .iface_get_address        = uct_srd_iface_get_address,
    .iface_is_reachable       = uct_base_iface_is_reachable,
    .iface_event_fd_get       = (uct_iface_event_fd_get_func_t)
        ucs_empty_function_return_unsupported,
    .iface_event_arm          = (uct_iface_event_arm_func_t)
        ucs_empty_function_return_unsupported,
    .iface_close              = UCS_CLASS_DELETE_FUNC_NAME(uct_srd_iface_t),
    .iface_get_device_address = uct_ib_iface_get_device_address
};

UCT_TL_DEFINE_ENTRY(&uct_ib_component, srd, uct_srd_query_tl_devices,
                    uct_srd_iface_t,  "SRD_",
                    uct_srd_iface_config_table, uct_srd_iface_config_t);
