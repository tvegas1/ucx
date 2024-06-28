/**
 * Copyright (c) NVIDIA CORPORATION & AFFILIATES, 2024. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "srd_def.h"
#include "srd_ep.h"


static void uct_srd_ep_reset(uct_srd_ep_t *ep)
{
    uct_srd_iface_t *iface = ucs_derived_of(ep->super.super.iface, uct_srd_iface_t);

    ep->tx.psn             = UCT_SRD_INITIAL_PSN;
    ep->tx.pending.ops     = UCT_SRD_EP_OP_NONE;
    ep->rx_creq_count      = 0;
    ep->fc.fc_wnd          = iface->config.fc_wnd_size;
    ucs_queue_head_init(&ep->tx.outstanding_q);
    ucs_frag_list_init(ep->tx.psn - 1, &ep->rx.ooo_pkts, -1
                       UCS_STATS_ARG(ep->super.stats));
}

static ucs_status_t uct_srd_ep_disconnect_from_iface(uct_ep_h tl_ep)
{
    uct_srd_ep_t *ep = ucs_derived_of(tl_ep, uct_srd_ep_t);

    ucs_frag_list_cleanup(&ep->rx.ooo_pkts);
    uct_srd_ep_reset(ep);

    ep->dest_ep_id = UCT_SRD_EP_NULL_ID;
    ep->flags     &= ~UCT_SRD_EP_FLAG_CONNECTED;

    return UCS_OK;
}

void *uct_srd_ep_get_peer_address(uct_srd_ep_t *srd_ep)
{
    uct_srd_ep_t *ep = ucs_derived_of(srd_ep, uct_srd_ep_t);
    return &ep->peer_address;
}

#if 0
static void uct_srd_peer_name(uct_srd_peer_name_t *peer)
{
    ucs_strncpy_zero(peer->name, ucs_get_host_name(), sizeof(peer->name));
    peer->pid = getpid();
}
#endif

static void uct_srd_ep_set_state(uct_srd_ep_t *ep, uint32_t state)
{
    ep->flags |= state;
}

#if 0 //ENABLE_DEBUG_DATA
static void uct_srd_peer_copy(uct_srd_peer_name_t *dst,
                              uct_srd_peer_name_t *src)
{
    memcpy(dst, src, sizeof(*src));
}

#else
#define  uct_srd_peer_copy(dst, src)
#endif

static UCS_CLASS_INIT_FUNC(uct_srd_ep_t, const uct_ep_params_t* params)
{
    uct_srd_iface_t *iface = ucs_derived_of(params->iface, uct_srd_iface_t);

    memset(self, 0, sizeof(*self));
    UCS_CLASS_CALL_SUPER_INIT(uct_base_ep_t, &iface->super.super);

    self->dest_ep_id         = UCT_SRD_EP_NULL_ID;
    self->path_index         = UCT_EP_PARAMS_GET_PATH_INDEX(params);
    self->peer_address.ah    = NULL;

    uct_srd_iface_add_ep(iface, self);

    ucs_debug("created ep ep=%p iface=%p id=%d", self, iface, self->ep_id);

    return UCS_OK;
}

static UCS_CLASS_CLEANUP_FUNC(uct_srd_ep_t)
{
}

UCS_CLASS_DEFINE(uct_srd_ep_t, uct_base_ep_t);
UCS_CLASS_DEFINE_NEW_FUNC(uct_srd_ep_t, uct_ep_t, const uct_ep_params_t *);
UCS_CLASS_DEFINE_DELETE_FUNC(uct_srd_ep_t, uct_ep_t);


ucs_status_t uct_srd_ep_flush(uct_ep_h ep, unsigned flags,
                              uct_completion_t *comp)
{
    return UCS_ERR_UNSUPPORTED;
}

static ucs_status_t uct_srd_ep_connect_to_iface(uct_srd_ep_t *ep,
                                                const uct_ib_address_t *ib_addr,
                                                const uct_srd_iface_addr_t *if_addr)
{
    uct_srd_iface_t *iface = ucs_derived_of(ep->super.super.iface, uct_srd_iface_t);
    uct_ib_device_t UCS_V_UNUSED *dev = uct_ib_iface_device(&iface->super);
    char buf[128];

    uct_srd_ep_reset(ep);

    ucs_debug(UCT_IB_IFACE_FMT" lid %d qpn 0x%x epid %u ep %p connected to "
              "IFACE %s qpn 0x%x", UCT_IB_IFACE_ARG(&iface->super),
              dev->port_attr[iface->super.config.port_num - dev->first_port].lid,
              iface->qp->qp_num, ep->ep_id, ep,
              uct_ib_address_str(ib_addr, buf, sizeof(buf)),
              uct_ib_unpack_uint24(if_addr->qp_num));

    return UCS_OK;
}


uct_srd_ep_conn_sn_t
uct_srd_iface_cep_get_conn_sn(uct_srd_iface_t *iface,
                             const uct_ib_address_t *ib_addr,
                             const uct_srd_iface_addr_t *if_addr,
                             int path_index)
{
    void *peer_address = ucs_alloca(iface->conn_match_ctx.address_length);
    return (uct_srd_ep_conn_sn_t)
           ucs_conn_match_get_next_sn(&iface->conn_match_ctx,
                                      uct_srd_iface_cep_get_peer_address(
                                          iface, ib_addr, if_addr, path_index,
                                          peer_address));
}

static ucs_status_t uct_srd_ep_create_connected(
                                            const uct_ep_params_t *ep_params,
                                            uct_ep_h *new_ep_p)
{
    uct_srd_iface_t *iface              = ucs_derived_of(ep_params->iface,
                                                         uct_srd_iface_t);
    const uct_ib_address_t *ib_addr     = (const uct_ib_address_t*)
        ep_params->dev_addr;
    const uct_srd_iface_addr_t *if_addr = (const uct_srd_iface_addr_t*)
        ep_params->iface_addr;
    int path_index                      = UCT_EP_PARAMS_GET_PATH_INDEX(ep_params);
    void *peer_address;
    uct_srd_ep_conn_sn_t conn_sn;
    uct_ep_params_t params;
    ucs_status_t status;
    uct_srd_ep_t *ep;
    uct_ep_h new_ep_h;


    *new_ep_p = NULL;

    conn_sn = uct_srd_iface_cep_get_conn_sn(iface, ib_addr, if_addr, path_index);
    ep      = uct_srd_iface_cep_get_ep(iface, ib_addr, if_addr, path_index,
                                       conn_sn, 1);
    if (ep != NULL) {
        uct_srd_ep_set_state(ep, UCT_SRD_EP_FLAG_CREQ_NOTSENT);
        ep->flags &= ~UCT_SRD_EP_FLAG_PRIVATE;
        status     = UCS_OK;
        uct_srd_iface_cep_insert_ep(iface, ib_addr, if_addr, path_index,
                                    conn_sn, ep);
        goto out_set_ep;
    }

    params.field_mask = UCT_EP_PARAM_FIELD_IFACE |
        UCT_EP_PARAM_FIELD_PATH_INDEX;
    params.iface      = &iface->super.super.super;
    params.path_index = path_index;

    status = uct_ep_create(&params, &new_ep_h);
    if (status != UCS_OK) {
        goto out;
    }

    ep          = ucs_derived_of(new_ep_h, uct_srd_ep_t);
    ep->conn_sn = conn_sn;

    status = uct_srd_ep_connect_to_iface(ep, ib_addr, if_addr);
    if (status != UCS_OK) {
        goto out;
    }

    uct_srd_iface_cep_insert_ep(iface, ib_addr, if_addr, path_index, conn_sn, ep);
    peer_address = uct_srd_ep_get_peer_address(ep);

    status = uct_srd_iface_unpack_peer_address(iface, ib_addr, if_addr,
                                               ep->path_index, peer_address);
    if (status != UCS_OK) {
        uct_srd_ep_disconnect_from_iface(&ep->super.super);
        goto out;
    }

#if 0
    desc = uct_srd_ep_prepare_creq(ep);
    if (desc) {
        uct_srd_ep_tx_desc(iface, ep, desc, 0, 1);
        uct_srd_iface_complete_tx_desc(iface, ep, desc);
        uct_srd_ep_set_state(ep, UCT_SRD_EP_FLAG_CREQ_SENT);
    } else {
        uct_srd_ep_ctl_op_add(iface, ep, UCT_SRD_EP_OP_CREQ);
    }
#endif

out_set_ep:
    /* cppcheck-suppress autoVariables */
    *new_ep_p = &ep->super.super;
out:
    return status;
}

ucs_status_t uct_srd_ep_create(const uct_ep_params_t *params, uct_ep_h *ep_p)
{
    if (ucs_test_all_flags(params->field_mask, UCT_EP_PARAM_FIELD_DEV_ADDR |
                           UCT_EP_PARAM_FIELD_IFACE_ADDR)) {
        return uct_srd_ep_create_connected(params, ep_p);
    }

    return UCS_CLASS_NEW_FUNC_NAME(uct_srd_ep_t)(params, ep_p);
}

void uct_srd_ep_destroy(uct_ep_h ep)
{
}
