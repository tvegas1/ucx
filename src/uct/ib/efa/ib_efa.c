/**
 * Copyright (c) 2024, NVIDIA CORPORATION. All rights reserved.
 *
 * See file LICENSE for terms.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <uct/ib/efa/ib_efa.h>
#include <ucs/sys/compiler_def.h>


ucs_status_t uct_ib_efadv_query(struct ibv_context *ctx,
                                struct efadv_device_attr *attr)
{
    if (efadv_query_device(ctx, attr, sizeof(*attr))) {
        ucs_error("efadv_query_device failed for EFA device %s %m",
                  ibv_get_device_name(ctx->device));
        return UCS_ERR_IO_ERROR;
    }

    return UCS_OK;
}

int uct_ib_efadv_is_supported(struct ibv_device *ibv_device)
{
    struct ibv_context *ctx;
    struct efadv_device_attr attr;
    int ret;

    ctx = ibv_open_device(ibv_device);
    if (ctx != NULL) {
        ucs_diag("ibv_open_device(%s) failed: %m",
                 ibv_get_device_name(ibv_device));
        return 0;
    }

    ret = efadv_query_device(ctx, &attr, sizeof(attr));
    ibv_close_device(ctx);
    return !ret;
}

void UCS_F_CTOR uct_efa_init(void)
{
    ucs_list_add_head(&uct_ib_ops, &UCT_IB_MD_OPS_NAME(efa).list);
}

void UCS_F_DTOR uct_efa_cleanup(void)
{
    ucs_list_del(&UCT_IB_MD_OPS_NAME(efa).list);
}
