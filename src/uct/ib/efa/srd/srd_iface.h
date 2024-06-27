/**
 * Copyright (c) NVIDIA CORPORATION & AFFILIATES, 2024. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifndef UCT_SRD_IFACE_H
#define UCT_SRD_IFACE_H

#include <uct/ib/base/ib_iface.h>
#include <uct/ib/ud/base/ud_iface_common.h>


BEGIN_C_DECLS


/** @file srd_iface.h */

typedef struct uct_srd_iface_config {
    uct_ib_iface_config_t         super;
    uct_ud_iface_common_config_t  ud_common;
    struct {
        size_t max_get_zcopy;
    } tx;

    struct {
        double               soft_thresh;
        double               hard_thresh;
        unsigned             wnd_size;
    } fc;

} uct_srd_iface_config_t;

typedef struct uct_srd_iface {
    uct_ib_iface_t             super;
    struct ibv_qp              *qp;
#ifdef HAVE_DECL_EFA_DV_RDMA_READ
    struct ibv_qp_ex           *qp_ex;
#endif
} uct_srd_iface_t;
#endif
