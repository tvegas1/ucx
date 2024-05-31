/**
* Copyright (c) NVIDIA CORPORATION & AFFILIATES, 2024. ALL RIGHTS RESERVED.
*
* See file LICENSE for terms.
*/

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <uct/base/uct_iface.h>
#include <uct/ib/base/ib_md.h>
#include <uct/ib/mlx5/accel/rc_mlx5.h>


typedef struct {
    uct_ib_md_packed_mkey_t packed_mkey;
    uct_ib_mlx5_devx_mem_t  *memh;
    uct_ib_mlx5_md_t        *md;
    uct_rkey_bundle_t       rkey_ob;
} uct_gga_mlx5_rkey_handle_t;

typedef struct {
    uct_rc_mlx5_iface_common_t  super;
} uct_gga_mlx5_iface_t;

typedef struct uct_gga_mlx5_iface_config {
    uct_rc_iface_config_t             super;
    uct_rc_mlx5_iface_common_config_t rc_mlx5_common;
} uct_gga_mlx5_iface_config_t;

extern ucs_config_field_t uct_ib_md_config_table[];

ucs_config_field_t uct_gga_mlx5_iface_config_table[] = {
  {"GGA_", "", NULL,
   ucs_offsetof(uct_gga_mlx5_iface_config_t, super),
   UCS_CONFIG_TYPE_TABLE(uct_rc_iface_config_table)},

  {"GGA_", "", NULL,
   ucs_offsetof(uct_gga_mlx5_iface_config_t, rc_mlx5_common),
   UCS_CONFIG_TYPE_TABLE(uct_rc_mlx5_common_config_table)},

  {NULL}
};

static ucs_status_t
uct_ib_mlx5_gga_mkey_pack(uct_md_h uct_md, uct_mem_h uct_memh,
                          void *address, size_t length,
                          const uct_md_mkey_pack_params_t *params,
                          void *mkey_buffer)
{
    uct_md_mkey_pack_params_t gga_params = *params;

    if (gga_params.field_mask & UCT_MD_MKEY_PACK_FIELD_FLAGS) {
        gga_params.flags      |= UCT_MD_MKEY_PACK_FLAG_EXPORT;
    } else {
        gga_params.field_mask |= UCT_MD_MKEY_PACK_FIELD_FLAGS;
        gga_params.flags       = UCT_MD_MKEY_PACK_FLAG_EXPORT;
    }

    return uct_ib_mlx5_devx_mkey_pack(uct_md, uct_memh, address, length,
                                      &gga_params, mkey_buffer);
}

static ucs_status_t
uct_gga_mlx5_rkey_unpack(uct_component_t *component, const void *rkey_buffer,
                         uct_rkey_t *rkey_p, void **handle_p)
{
    const uct_ib_md_packed_mkey_t *mkey = rkey_buffer;
    uct_gga_mlx5_rkey_handle_t *rkey_handle;

    rkey_handle = ucs_malloc(sizeof(*rkey_handle), "gga_rkey_handle");
    if (rkey_handle == NULL) {
        return UCS_ERR_NO_MEMORY;
    }

    rkey_handle->packed_mkey    = *mkey;
    /* memh and rkey_ob will be initialized on demand */
    rkey_handle->memh           = NULL;
    rkey_handle->md             = NULL;
    rkey_handle->rkey_ob.rkey   = UCT_INVALID_RKEY;
    rkey_handle->rkey_ob.handle = NULL;
    rkey_handle->rkey_ob.type   = NULL;

    *rkey_p   = (uintptr_t)rkey_handle;
    *handle_p = NULL;
    return UCS_OK;
}

static void
uct_gga_mlx5_rkey_handle_dereg(uct_gga_mlx5_rkey_handle_t *rkey_handle)
{
    uct_md_mem_dereg_params_t params = {
        .field_mask = UCT_MD_MEM_DEREG_FIELD_MEMH,
        .memh       = rkey_handle->memh
    };
    ucs_status_t status;

    if (rkey_handle->memh == NULL) {
        return;
    }

    status = uct_ib_mlx5_devx_mem_dereg(&rkey_handle->md->super.super, &params);
    if (status != UCS_OK) {
        ucs_warn("md %p: failed to deregister GGA memh %p", rkey_handle->md,
                 rkey_handle->memh);
    }

    rkey_handle->memh = NULL;
    rkey_handle->md   = NULL;
}

static ucs_status_t uct_gga_mlx5_rkey_release(uct_component_t *component,
                                              uct_rkey_t rkey, void *handle)
{
    uct_gga_mlx5_rkey_handle_t *rkey_handle = (uct_gga_mlx5_rkey_handle_t*)rkey;

    uct_gga_mlx5_rkey_handle_dereg(rkey_handle);
    ucs_free(handle);
    return UCS_OK;
}

/* Forward declaration */
static ucs_status_t
uct_ib_mlx5_gga_md_open(uct_component_t *component, const char *md_name,
                        const uct_md_config_t *uct_md_config, uct_md_h *md_p);

static uct_component_t uct_gga_component = {
    .query_md_resources = uct_ib_query_md_resources,
    .md_open            = uct_ib_mlx5_gga_md_open,
    .cm_open            = ucs_empty_function_return_unsupported,
    .rkey_unpack        = uct_gga_mlx5_rkey_unpack,
    .rkey_ptr           = ucs_empty_function_return_unsupported,
    .rkey_release       = uct_gga_mlx5_rkey_release,
    .rkey_compare       = uct_base_rkey_compare,
    .name               = "gga",
    .md_config          = {
        .name           = "GGA memory domain",
        .prefix         = "GGA_",
        .table          = uct_ib_md_config_table,
        .size           = sizeof(uct_ib_md_config_t),
    },
    .cm_config          = UCS_CONFIG_EMPTY_GLOBAL_LIST_ENTRY,
    .tl_list            = UCT_COMPONENT_TL_LIST_INITIALIZER(&uct_gga_component),
    .flags              = 0,
    .md_vfs_init        = (uct_component_md_vfs_init_func_t)ucs_empty_function
};

/* TODO: the function will be used on data path */
static UCS_F_MAYBE_UNUSED ucs_status_t
uct_gga_mlx5_rkey_resolve(uct_ib_mlx5_md_t *md, uct_rkey_t rkey)
{
    uct_md_h uct_md                         = &md->super.super;
    uct_gga_mlx5_rkey_handle_t *rkey_handle = (uct_gga_mlx5_rkey_handle_t*)rkey;
    uct_md_mem_attach_params_t atach_params = { 0 };
    uct_md_mkey_pack_params_t repack_params = { 0 };
    uint64_t repack_mkey;
    ucs_status_t status;

    if (ucs_likely(rkey_handle->memh != NULL)) {
        return UCS_OK;
    }

    status = uct_ib_mlx5_devx_mem_attach(uct_md, &rkey_handle->packed_mkey,
                                         &atach_params,
                                         (uct_mem_h *)&rkey_handle->memh);
    if (status != UCS_OK) {
        goto err_out;
    }

    rkey_handle->md = md;

    status = uct_ib_mlx5_devx_mkey_pack(uct_md, (uct_mem_h)rkey_handle->memh,
                                        NULL, 0, &repack_params, &repack_mkey);
    if (status != UCS_OK) {
        goto err_dereg;
    }

    return uct_ib_rkey_unpack(NULL, &repack_mkey,
                              &rkey_handle->rkey_ob.rkey,
                              &rkey_handle->rkey_ob.handle);
err_dereg:
    uct_gga_mlx5_rkey_handle_dereg(rkey_handle);
err_out:
    return status;
}

static UCS_CLASS_DECLARE_DELETE_FUNC(uct_gga_mlx5_iface_t, uct_iface_t);

static uct_iface_ops_t uct_gga_mlx5_iface_tl_ops = {
    .ep_put_short             = ucs_empty_function_return_unsupported,
    .ep_put_bcopy             = (uct_ep_put_bcopy_func_t)ucs_empty_function_return_unsupported,
    .ep_put_zcopy             = ucs_empty_function_return_unsupported,
    .ep_get_bcopy             = ucs_empty_function_return_unsupported,
    .ep_get_zcopy             = ucs_empty_function_return_unsupported,
    .ep_am_short              = (uct_ep_am_short_func_t)ucs_empty_function_return_unsupported,
    .ep_am_short_iov          = (uct_ep_am_short_iov_func_t)ucs_empty_function_return_unsupported,
    .ep_am_bcopy              = (uct_ep_am_bcopy_func_t)ucs_empty_function_return_unsupported,
    .ep_am_zcopy              = (uct_ep_am_zcopy_func_t)ucs_empty_function_return_unsupported,
    .ep_atomic_cswap64        = ucs_empty_function_return_unsupported,
    .ep_atomic_cswap32        = ucs_empty_function_return_unsupported,
    .ep_atomic64_post         = ucs_empty_function_return_unsupported,
    .ep_atomic32_post         = ucs_empty_function_return_unsupported,
    .ep_atomic64_fetch        = ucs_empty_function_return_unsupported,
    .ep_atomic32_fetch        = ucs_empty_function_return_unsupported,
    .ep_pending_add           = ucs_empty_function_return_unsupported,
    .ep_pending_purge         = ucs_empty_function_do_assert_void,
    .ep_flush                 = ucs_empty_function_return_unsupported,
    .ep_fence                 = ucs_empty_function_return_unsupported,
    .ep_check                 = ucs_empty_function_return_unsupported,
    .ep_create                = ucs_empty_function_return_unsupported,
    .ep_destroy               = ucs_empty_function_do_assert_void,
    .ep_get_address           = ucs_empty_function_return_unsupported,
    .ep_connect_to_ep         = ucs_empty_function_return_unsupported,
    .iface_flush              = ucs_empty_function_return_unsupported,
    .iface_fence              = (uct_iface_fence_func_t)ucs_empty_function_do_assert,
    .iface_progress_enable    = ucs_empty_function_do_assert_void,
    .iface_progress_disable   = ucs_empty_function_do_assert_void,
    .iface_progress           = (uct_iface_progress_func_t)ucs_empty_function_do_assert,
    .iface_event_fd_get       = ucs_empty_function_return_unsupported,
    .iface_event_arm          = ucs_empty_function_return_unsupported,
    .iface_close              = uct_gga_mlx5_iface_t_delete,
    .iface_query              = (uct_iface_query_func_t)ucs_empty_function_do_assert,
    .iface_get_address        = ucs_empty_function_return_success,
    .iface_get_device_address = ucs_empty_function_return_unsupported,
    .iface_is_reachable       = uct_base_iface_is_reachable
};

static uct_rc_iface_ops_t uct_gga_mlx5_iface_ops = {
    .super = {
        .super = {
            .iface_estimate_perf   = uct_base_iface_estimate_perf,
            .iface_vfs_refresh     = uct_rc_iface_vfs_refresh,
            .ep_query              = (uct_ep_query_func_t)ucs_empty_function,
            .ep_invalidate         = uct_rc_mlx5_base_ep_invalidate,
            .ep_connect_to_ep_v2   = (uct_ep_connect_to_ep_v2_func_t)ucs_empty_function_do_assert,
            .iface_is_reachable_v2 = ucs_empty_function_do_assert,
            .ep_is_connected       = ucs_empty_function_do_assert
        },
        .create_cq      = uct_rc_mlx5_iface_common_create_cq,
        .destroy_cq     = uct_rc_mlx5_iface_common_destroy_cq,
        .event_cq       = uct_rc_mlx5_iface_common_event_cq,
        .handle_failure = (uct_ib_iface_handle_failure_func_t)ucs_empty_function_do_assert_void,
    },
    .init_rx         = (uct_rc_iface_init_rx_func_t)ucs_empty_function_do_assert,
    .cleanup_rx      = ucs_empty_function_do_assert_void,
    .fc_ctrl         = uct_rc_mlx5_base_ep_fc_ctrl,
    .fc_handler      = uct_rc_iface_fc_handler,
    .cleanup_qp      = ucs_empty_function_do_assert_void,
    .ep_post_check   = uct_rc_mlx5_base_ep_post_check,
    .ep_vfs_populate = uct_rc_mlx5_base_ep_vfs_populate
};

static UCS_CLASS_INIT_FUNC(uct_gga_mlx5_iface_t,
                           uct_md_h tl_md, uct_worker_h worker,
                           const uct_iface_params_t *params,
                           const uct_iface_config_t *tl_config)
{
    uct_gga_mlx5_iface_config_t *config =
            ucs_derived_of(tl_config, uct_gga_mlx5_iface_config_t);
    uct_ib_mlx5_md_t *md                = ucs_derived_of(tl_md, uct_ib_mlx5_md_t);
    uct_ib_iface_init_attr_t init_attr  = {};

    init_attr.qp_type               = IBV_QPT_RC;
    init_attr.cq_len[UCT_IB_DIR_TX] = config->super.tx_cq_len;
    init_attr.max_rd_atomic         = IBV_DEV_ATTR(&md->super.dev,
                                                   max_qp_rd_atom);
    init_attr.tx_moderation         = config->super.tx_cq_moderation;

    UCS_CLASS_CALL_SUPER_INIT(uct_rc_mlx5_iface_common_t,
                              &uct_gga_mlx5_iface_tl_ops,
                              &uct_gga_mlx5_iface_ops, tl_md, worker, params,
                              &config->super.super, &config->rc_mlx5_common,
                              &init_attr);

    config->super.super.fc.enable = 0; /* FC requires AM capability */
    return UCS_OK;
}

static UCS_CLASS_CLEANUP_FUNC(uct_gga_mlx5_iface_t)
{
    uct_base_iface_progress_disable(&self->super.super.super.super.super,
                                    UCT_PROGRESS_SEND | UCT_PROGRESS_RECV);
}

UCS_CLASS_DEFINE(uct_gga_mlx5_iface_t, uct_rc_mlx5_iface_common_t);

static UCS_CLASS_DEFINE_NEW_FUNC(uct_gga_mlx5_iface_t, uct_iface_t, uct_md_h,
                                 uct_worker_h, const uct_iface_params_t*,
                                 const uct_iface_config_t*);

static UCS_CLASS_DEFINE_DELETE_FUNC(uct_gga_mlx5_iface_t, uct_iface_t);

static ucs_status_t
uct_gga_mlx5_query_tl_devices(uct_md_h md,
                              uct_tl_device_resource_t **tl_devices_p,
                              unsigned *num_tl_devices_p)
{
    uct_ib_mlx5_md_t *mlx5_md = ucs_derived_of(md, uct_ib_mlx5_md_t);
    uct_tl_device_resource_t *tl_devices;
    unsigned num_tl_devices;
    ucs_status_t status;

    if (strcmp(mlx5_md->super.name, UCT_IB_MD_NAME(mlx5)) ||
        !ucs_test_all_flags(mlx5_md->flags, UCT_IB_MLX5_MD_FLAG_DEVX           |
                                            UCT_IB_MLX5_MD_FLAG_INDIRECT_XGVMI |
                                            UCT_IB_MLX5_MD_FLAG_MMO_DMA)) {
        return UCS_ERR_NO_DEVICE;
    }

    status = uct_ib_device_query_ports(&mlx5_md->super.dev,
                                       UCT_IB_DEVICE_FLAG_SRQ |
                                       UCT_IB_DEVICE_FLAG_MLX5_PRM,
                                       &tl_devices, &num_tl_devices);
    if (status != UCS_OK) {
        return status;
    }

    /* TODO: del to enable GGA in UCP */
    ucs_free(tl_devices);
    return UCS_ERR_NO_DEVICE;
}

UCT_TL_DEFINE_ENTRY(&uct_gga_component, gga_mlx5, uct_gga_mlx5_query_tl_devices,
                    uct_gga_mlx5_iface_t, "GGA_MLX5_",
                    uct_gga_mlx5_iface_config_table,
                    uct_gga_mlx5_iface_config_t);

UCT_SINGLE_TL_INIT(&uct_gga_component, gga_mlx5, ctor,,)

/* TODO: separate memh since atomic_mr is not relevant for GGA */
static uct_md_ops_t uct_mlx5_gga_md_ops = {
    .close              = uct_ib_mlx5_devx_md_close,
    .query              = uct_ib_mlx5_devx_md_query,
    .mem_alloc          = uct_ib_mlx5_devx_device_mem_alloc,
    .mem_free           = uct_ib_mlx5_devx_device_mem_free,
    .mem_reg            = uct_ib_mlx5_devx_mem_reg,
    .mem_dereg          = uct_ib_mlx5_devx_mem_dereg,
    .mem_attach         = uct_ib_mlx5_devx_mem_attach,
    .mem_advise         = uct_ib_mem_advise,
    .mkey_pack          = uct_ib_mlx5_gga_mkey_pack,
    .detect_memory_type = ucs_empty_function_return_unsupported,
};

static ucs_status_t
uct_ib_mlx5_gga_md_open(uct_component_t *component, const char *md_name,
                        const uct_md_config_t *uct_md_config, uct_md_h *md_p)
{
    const uct_ib_md_config_t *md_config = ucs_derived_of(uct_md_config,
                                                         uct_ib_md_config_t);
    struct ibv_device **ib_device_list, *ib_device;
    int num_devices, fork_init;
    ucs_status_t status;
    uct_ib_md_t *md;

    ucs_trace("opening GGA device %s", md_name);

    if (md_config->devx == UCS_NO) {
        return UCS_ERR_UNSUPPORTED;
    }

    /* Get device list from driver */
    ib_device_list = ibv_get_device_list(&num_devices);
    if (ib_device_list == NULL) {
        ucs_debug("Failed to get GGA device list, assuming no devices are present");
        status = UCS_ERR_NO_DEVICE;
        goto out;
    }

    status = uct_ib_get_device_by_name(ib_device_list, num_devices, md_name,
                                       &ib_device);
    if (status != UCS_OK) {
        goto out_free_dev_list;
    }

    status = uct_ib_fork_init(md_config, &fork_init);
    if (status != UCS_OK) {
        goto out_free_dev_list;
    }

    status = uct_ib_mlx5_devx_md_open(ib_device, md_config, &md);
    if (status != UCS_OK) {
        goto out_free_dev_list;
    }

    md->super.component = &uct_gga_component;
    md->super.ops       = &uct_mlx5_gga_md_ops;
    md->name            = UCT_IB_MD_NAME(gga);
    md->fork_init       = fork_init;
    *md_p               = &md->super;

out_free_dev_list:
    ibv_free_device_list(ib_device_list);
out:
    return status;
}
