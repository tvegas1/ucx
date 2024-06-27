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


static ucs_status_t
uct_srd_iface_get_address(uct_iface_h tl_iface, uct_iface_addr_t *iface_addr)
{
    uct_srd_iface_t *iface = ucs_derived_of(tl_iface, uct_srd_iface_t);
    uct_srd_iface_addr_t *addr = (uct_srd_iface_addr_t *)iface_addr;

    uct_ib_pack_uint24(addr->qp_num, iface->qp->qp_num);

    return UCS_OK;
}

static ucs_status_t
uct_srd_iface_estimate_perf(uct_iface_h iface, uct_perf_attr_t *perf_attr)
{
    return UCS_ERR_UNSUPPORTED;
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
        .iface_estimate_perf = uct_srd_iface_estimate_perf,
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

static void uct_srd_iface_release_recv_desc(uct_recv_desc_t *self, void *desc)
{
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

#if 0
    /* TODO: Use common configuration */
    iface->config.max_inline = qp_init_attr.cap.max_inline_data;
    iface->config.tx_qp_len  = qp_init_attr.cap.max_send_wr;
    iface->tx.available      = qp_init_attr.cap.max_send_wr;
    iface->rx.available      = qp_init_attr.cap.max_recv_wr;
#endif

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

static UCS_CLASS_INIT_FUNC(uct_srd_iface_t, uct_md_h md, uct_worker_h worker,
                           const uct_iface_params_t *params,
                           const uct_iface_config_t *tl_config)
{
    uct_srd_iface_config_t *config      = ucs_derived_of(tl_config,
                                                    uct_srd_iface_config_t);
//    uct_ib_efadv_md_t *efa_md           = ucs_derived_of(md, uct_ib_efadv_md_t);
    uct_ib_iface_init_attr_t init_attr;
    ucs_status_t status;
    int mtu;

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

    return UCS_OK;
}

static UCS_CLASS_CLEANUP_FUNC(uct_srd_iface_t)
{
    /* TODO Cleanups here and valgrind checks */
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

    {NULL}
};

static ucs_status_t uct_srd_iface_flush(uct_iface_h tl_iface, unsigned flags,
                                        uct_completion_t *comp)
{
    return UCS_ERR_UNSUPPORTED;
}

static void uct_srd_iface_progress_enable(uct_iface_h tl_iface, unsigned flags)
{
}

static unsigned uct_srd_iface_progress(uct_iface_h tl_iface)
{
    return 0;
}

static ucs_status_t
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

    iface_attr->cap.am.max_bcopy = iface->super.config.seg_size - sizeof(uct_srd_neth_t);
    iface_attr->cap.am.min_zcopy = 0;
    iface_attr->cap.am.max_zcopy = max_rdma_size;
    iface_attr->cap.am.max_iov   = md->dev.max_sq_sge;
    iface_attr->cap.am.max_hdr   = uct_ib_iface_hdr_size(iface->super.config.seg_size,
                                                         sizeof(uct_srd_neth_t));

    /* GET */
    iface_attr->cap.get.max_bcopy = iface->super.config.seg_size;
    iface_attr->cap.get.min_zcopy = iface->super.config.max_inl_cqe[UCT_IB_DIR_TX] + 1;
    iface_attr->cap.am.max_zcopy  = max_rdma_size; /* TODO wrapper */
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
    .ep_destroy               = uct_srd_ep_destroy,
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
