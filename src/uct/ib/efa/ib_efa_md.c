/**
 * Copyright (c) 2024, NVIDIA CORPORATION. All rights reserved.
 *
 * See file LICENSE for terms.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <uct/ib/efa/ib_efa.h>
#include <uct/ib/base/ib_md.h>
#include <ucs/type/status.h>
#include <ucs/debug/log.h>


uint64_t
uct_ib_efadv_access_flags(const uct_ib_efadv_t *efadv)
{
    if (uct_ib_efadv_has_rdma_read(efadv)) {
        return IBV_ACCESS_REMOTE_READ | IBV_ACCESS_LOCAL_WRITE;
    }

    return IBV_ACCESS_LOCAL_WRITE;
}

static uct_ib_md_ops_t uct_ib_efa_md_ops;

static ucs_status_t uct_ib_efa_md_open(struct ibv_device *ibv_device,
                                       const uct_ib_md_config_t *md_config,
                                       uct_ib_md_t **p_md)
{
    struct ibv_context *ctx;
    struct efadv_device_attr attr;
    uct_ib_efadv_md_t *md;
    ucs_status_t status;

    ctx = ibv_open_device(ibv_device);
    if (ctx == NULL) {
        ucs_diag("ibv_open_device(%s) failed: %m",
                 ibv_get_device_name(ibv_device));
        return UCS_ERR_IO_ERROR;
    }

    if (efadv_query_device(ctx, &attr, sizeof(attr))) {
        status = UCS_ERR_UNSUPPORTED;
        goto err_free_context;
    }

    md = ucs_derived_of(uct_ib_md_alloc(sizeof(*md), "ib_efadv_md", ctx),
                        uct_ib_efadv_md_t);
    if (md == NULL) {
        status = UCS_ERR_NO_MEMORY;
        goto err_free_context;
    }

    md->super.super.ops           = &uct_ib_efa_md_ops.super;
    md->efadv.attr                = attr;
    md->super.name                = UCT_IB_MD_NAME(efa);

    status = uct_ib_device_query(&md->super.dev, ibv_device);
    if (status != UCS_OK) {
        goto err_md_free;
    }

    uct_ib_device_configure(&md->super.dev);

    md->super.dev.mr_access_flags   = uct_ib_efadv_access_flags(&md->efadv);
    md->super.dev.max_sq_sge        = md->efadv.attr.max_sq_sge;
    md->super.dev.max_inline_data   = md->efadv.attr.inline_buf_size;
    md->super.dev.ordered_send_comp = 0;

    status = uct_ib_md_open_common(&md->super, ibv_device, md_config);
    if (status != UCS_OK) {
        goto err_md_free;
    }

    *p_md = &md->super;
    return UCS_OK;

err_md_free:
    uct_ib_md_free(&md->super);
err_free_context:
    uct_ib_md_device_context_close(ctx);
    return status;
}

static void uct_ib_efa_md_close(uct_md_h tl_md)
{
    uct_ib_md_t *ib_md      = ucs_derived_of(tl_md, uct_ib_md_t);
    struct ibv_context *ctx = ib_md->dev.ibv_context;

    uct_ib_md_close_common(ib_md);
    uct_ib_md_free(ib_md);
    uct_ib_md_device_context_close(ctx);
}

static uct_ib_md_ops_t uct_ib_efa_md_ops = {
    .super = {
        .close              = uct_ib_efa_md_close,
        .query              = uct_ib_md_query,
        .mem_reg            = uct_ib_verbs_mem_reg,
        .mem_dereg          = uct_ib_verbs_mem_dereg,
        .mem_attach         = ucs_empty_function_return_unsupported,
        .mem_advise         = uct_ib_mem_advise,
        .mkey_pack          = uct_ib_verbs_mkey_pack,
        .detect_memory_type = ucs_empty_function_return_unsupported,
    },
    .open = uct_ib_efa_md_open,
};

UCT_IB_MD_DEFINE_ENTRY(efa, uct_ib_efa_md_ops);
