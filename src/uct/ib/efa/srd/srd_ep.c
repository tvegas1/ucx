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

#include "srd_inl.h"



#define UCT_SRD_CHECK_FC_WND(_fc, _stats)\
    if ((_fc)->fc_wnd == 0) { \
        UCS_STATS_UPDATE_COUNTER((_fc)->stats, UCT_SRD_FC_STAT_NO_CRED, 1); \
        UCS_STATS_UPDATE_COUNTER(_stats, UCT_EP_STAT_NO_RES, 1); \
        return UCS_ERR_NO_RESOURCE; \
    } \

#define UCT_SRD_CHECK_FC(_iface, _ep, _fc_hdr) \
    { \
        if (ucs_unlikely((_ep)->fc.fc_wnd <= (_iface)->config.fc_soft_thresh)) { \
            UCT_SRD_CHECK_FC_WND(&(_ep)->fc, (_ep)->super.stats); \
            (_fc_hdr) = uct_srd_ep_fc_get_request(_ep, _iface); \
        } \
        if ((_ep)->flags & UCT_SRD_EP_FLAG_FC_GRANT) { \
            (_fc_hdr) |= UCT_SRD_PACKET_FLAG_FC_GRANT; \
        } \
    }

#define UCT_SRD_UPDATE_FC_WND(_iface, _fc) \
    { \
        ucs_assert((_fc)->fc_wnd > 0); \
        (_fc)->fc_wnd--; \
        UCS_STATS_SET_COUNTER((_fc)->stats, UCT_SRD_FC_STAT_FC_WND, (_fc)->fc_wnd); \
    }

#define UCT_SRD_UPDATE_FC(_iface, _ep, _fc_hdr) \
    { \
        if ((_fc_hdr) & UCT_SRD_PACKET_FLAG_FC_GRANT) { \
            UCS_STATS_UPDATE_COUNTER((_ep)->fc.stats, UCT_SRD_FC_STAT_TX_GRANT, 1); \
        } \
        if ((_fc_hdr) & UCT_SRD_PACKET_FLAG_FC_SREQ) { \
            UCS_STATS_UPDATE_COUNTER((_ep)->fc.stats, UCT_SRD_FC_STAT_TX_SOFT_REQ, 1); \
        } else if ((_fc_hdr) & UCT_SRD_PACKET_FLAG_FC_HREQ) { \
            UCS_STATS_UPDATE_COUNTER((_ep)->fc.stats, UCT_SRD_FC_STAT_TX_HARD_REQ, 1); \
        } \
        \
        (_ep)->flags &= ~UCT_SRD_EP_FLAG_FC_GRANT; \
        \
        UCT_SRD_UPDATE_FC_WND(_iface, &(_ep)->fc) \
    }

#define UCT_SRD_EP_ASSERT_PENDING(_ep) \
    ucs_assertv(((_ep)->flags & UCT_SRD_EP_FLAG_IN_PENDING) ||  \
                !uct_srd_ep_has_pending(_ep),                   \
                "out-of-order send detected for"                \
                " ep %p ep_pending %d arbelem %p",              \
                _ep, ((_ep)->flags & UCT_SRD_EP_FLAG_IN_PENDING), \
                &(_ep)->tx.pending.elem);

#define UCT_SRD_AM_COMMON(_iface, _ep, _id, _neth) \
    { \
        /* either we are executing pending operations, \
         * or there are no pending elements. */ \
        UCT_SRD_EP_ASSERT_PENDING(_ep); \
        \
        UCT_SRD_CHECK_FC(_iface, _ep, (_neth)->fc); \
        uct_srd_neth_set_type_am(_ep, _neth, _id); \
        uct_srd_neth_set_psn(_ep, _neth); \
    }

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


static UCS_F_ALWAYS_INLINE uint8_t
uct_srd_ep_fc_get_request(uct_srd_ep_t *ep, uct_srd_iface_t *iface)
{
    return (ep->fc.fc_wnd == iface->config.fc_hard_thresh) ?
            UCT_SRD_PACKET_FLAG_FC_HREQ:
           (ep->fc.fc_wnd == iface->config.fc_soft_thresh) ?
            UCT_SRD_PACKET_FLAG_FC_SREQ: 0;
}

static void uct_srd_peer_name(uct_srd_peer_name_t *peer)
{
    ucs_strncpy_zero(peer->name, ucs_get_host_name(), sizeof(peer->name));
    peer->pid = getpid();
}

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

    uct_srd_ep_reset(self);
    uct_srd_iface_add_ep(iface, self);
    ucs_arbiter_group_init(&self->tx.pending.group);
    ucs_arbiter_elem_init(&self->tx.pending.elem);
    ucs_debug("created ep ep=%p iface=%p id=%d", self, iface, self->ep_id);

    return UCS_OK;
}


static UCS_F_ALWAYS_INLINE int
uct_srd_ep_is_last_pending_elem(uct_srd_ep_t *ep, ucs_arbiter_elem_t *elem)
{
    return (/* this is the only one pending element in the group */
            (ucs_arbiter_elem_is_only(elem)) ||
            (/* the next element in the group is control operation */
             (elem->next == &ep->tx.pending.elem) &&
             /* only two elements are in the group (the 1st element is the
              * current one, the 2nd (or the last) element is the control one) */
             (ucs_arbiter_group_tail(&ep->tx.pending.group) == &ep->tx.pending.elem)));
}


ucs_status_t uct_srd_ep_pending_add(uct_ep_h ep_h, uct_pending_req_t *req,
                                    unsigned flags)
{
    uct_srd_ep_t *ep       = ucs_derived_of(ep_h, uct_srd_ep_t);
    uct_srd_iface_t *iface = ucs_derived_of(ep->super.super.iface,
                                            uct_srd_iface_t);

    if (uct_srd_iface_has_all_tx_resources(iface)  &&
        uct_srd_ep_is_connected_and_no_pending(ep) &&
        uct_srd_ep_has_fc_resources(ep)) {

        return UCS_ERR_BUSY;
    }

    UCS_STATIC_ASSERT(sizeof(uct_srd_pending_req_priv_t) <=
                      UCT_PENDING_REQ_PRIV_LEN);
    uct_srd_pending_req_priv(req)->flags = flags;
    uct_srd_ep_set_has_pending_flag(ep);
    uct_pending_req_arb_group_push(&ep->tx.pending.group, req);
    ucs_arbiter_group_schedule(&iface->tx.pending_q, &ep->tx.pending.group);
    ucs_trace_data("srd ep %p: added pending req %p tx_psn %d",
                   ep, req, ep->tx.psn);
    UCT_TL_EP_STAT_PEND(&ep->super);

    return UCS_OK;
}

static ucs_arbiter_cb_result_t
uct_srd_ep_pending_purge_cb(ucs_arbiter_t *arbiter, ucs_arbiter_group_t *group,
                            ucs_arbiter_elem_t *elem, void *arg)
{
    uct_srd_ep_t *ep                = ucs_container_of(group, uct_srd_ep_t,
                                                       tx.pending.group);
    uct_purge_cb_args_t *cb_args    = arg;
    uct_pending_purge_callback_t cb = cb_args->cb;
    uct_pending_req_t *req;
    int is_last_pending_elem;

    if (&ep->tx.pending.elem == elem) {
        /* return ignored by arbiter */
        return UCS_ARBITER_CB_RESULT_REMOVE_ELEM;
    }

    is_last_pending_elem = uct_srd_ep_is_last_pending_elem(ep, elem);

    req = ucs_container_of(elem, uct_pending_req_t, priv);
    if (cb) {
        cb(req, cb_args->arg);
    } else {
        ucs_debug("ep=%p cancelling user pending request %p", ep, req);
    }

    if (is_last_pending_elem) {
        uct_srd_ep_remove_has_pending_flag(ep);
    }

    /* return ignored by arbiter */
    return UCS_ARBITER_CB_RESULT_REMOVE_ELEM;
}

void uct_srd_ep_pending_purge(uct_ep_h ep_h, uct_pending_purge_callback_t cb,
                              void *arg)
{
    uct_srd_ep_t *ep         = ucs_derived_of(ep_h, uct_srd_ep_t);
    uct_srd_iface_t *iface   = ucs_derived_of(ep->super.super.iface,
                                              uct_srd_iface_t);
    uct_purge_cb_args_t args = {cb, arg};

    ucs_arbiter_group_purge(&iface->tx.pending_q, &ep->tx.pending.group,
                            uct_srd_ep_pending_purge_cb, &args);
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


ucs_status_t uct_srd_ep_get_address(uct_ep_h tl_ep, uct_ep_addr_t *addr)
{
    uct_srd_ep_t *ep = ucs_derived_of(tl_ep, uct_srd_ep_t);
    uct_srd_iface_t *iface = ucs_derived_of(ep->super.super.iface, uct_srd_iface_t);
    uct_srd_ep_addr_t *ep_addr = (uct_srd_ep_addr_t *)addr;

    uct_ib_pack_uint24(ep_addr->iface_addr.qp_num, iface->qp->qp_num);
    uct_ib_pack_uint24(ep_addr->ep_id, ep->ep_id);
    return UCS_OK;
}

static uct_srd_send_desc_t *uct_srd_ep_prepare_creq(uct_srd_ep_t *ep)
{
    uct_srd_iface_t *iface = ucs_derived_of(ep->super.super.iface, uct_srd_iface_t);
    uct_srd_ctl_hdr_t *creq;
    uct_srd_send_desc_t *desc;
    uct_srd_neth_t *neth;
    ucs_status_t status;

    ucs_assert_always(ep->dest_ep_id == UCT_SRD_EP_NULL_ID);
    ucs_assert_always(ep->ep_id != UCT_SRD_EP_NULL_ID);

    /* CREQ should not be sent if CREP for the counter CREQ is scheduled
     *      * (or sent already) */
    ucs_assertv_always(!uct_srd_ep_ctl_op_check(ep, UCT_SRD_EP_OP_CREP) &&
                       !(ep->flags & UCT_SRD_EP_FLAG_CREP_SENT),
                       "iface=%p ep=%p conn_sn=%d rx_psn=%u ep_flags=0x%x "
                       "ctl_ops=0x%x rx_creq_count=%d",
                       iface, ep, ep->conn_sn, ep->rx.ooo_pkts.head_sn,
                       ep->flags, ep->tx.pending.ops, ep->rx_creq_count);

    desc = uct_srd_iface_get_send_desc(iface);
    if (!desc) {
        return NULL;
    }

    neth               = uct_srd_send_desc_neth(desc);
    neth->packet_type  = UCT_SRD_EP_NULL_ID;
    neth->packet_type |= UCT_SRD_PACKET_FLAG_CTLX;
    uct_srd_neth_set_psn(ep, neth);

    creq                      = (uct_srd_ctl_hdr_t *)(neth + 1);
    creq->type                = UCT_SRD_PACKET_CREQ;
    creq->conn_req.conn_sn    = ep->conn_sn;
    creq->conn_req.path_index = ep->path_index;

    status = uct_srd_ep_get_address(&ep->super.super,
                                    (void*)&creq->conn_req.ep_addr);
    if (status != UCS_OK) {
        return NULL;
    }

    status = uct_ib_iface_get_device_address(&iface->super.super.super,
                                             (uct_device_addr_t*)uct_srd_creq_ib_addr(creq));
    if (status != UCS_OK) {
        return NULL;
    }

    uct_srd_peer_name(ucs_unaligned_ptr(&creq->peer));

    desc->super.ep           = ep;
    desc->super.len          = sizeof(*neth) + sizeof(*creq) + iface->super.addr_size;
    desc->super.comp_handler = uct_srd_iface_send_op_release;

    return desc;
}

uct_srd_send_desc_t *uct_srd_ep_prepare_crep(uct_srd_ep_t *ep)
{
    uct_srd_iface_t *iface = ucs_derived_of(ep->super.super.iface, uct_srd_iface_t);
    uct_srd_ctl_hdr_t *crep;
    uct_srd_send_desc_t *desc;
    uct_srd_neth_t *neth;

    ucs_assert_always(ep->dest_ep_id != UCT_SRD_EP_NULL_ID);
    ucs_assert_always(ep->ep_id != UCT_SRD_EP_NULL_ID);

    /* Check that CREQ is not sheduled */
    ucs_assertv_always(!uct_srd_ep_ctl_op_check(ep, UCT_SRD_EP_OP_CREQ),
                       "iface=%p ep=%p conn_sn=%d ep_id=%d, dest_ep_id=%d "
                       "rx_psn=%u ep_flags=0x%x ctl_ops=0x%x rx_creq_count=%d",
                       iface, ep, ep->conn_sn, ep->ep_id, ep->dest_ep_id,
                       ep->rx.ooo_pkts.head_sn, ep->flags, ep->tx.pending.ops,
                       ep->rx_creq_count);

    desc = uct_srd_iface_get_send_desc(iface);
    if (!desc) {
        return NULL;
    }

    neth               = uct_srd_send_desc_neth(desc);
    neth->packet_type  = ep->dest_ep_id;
    neth->packet_type |= UCT_SRD_PACKET_FLAG_CTLX;
    uct_srd_neth_set_psn(ep, neth);

    crep                     = (uct_srd_ctl_hdr_t *)(neth + 1);
    crep->type               = UCT_SRD_PACKET_CREP;
    crep->conn_rep.src_ep_id = ep->ep_id;

    uct_srd_peer_name(ucs_unaligned_ptr(&crep->peer));

    uct_srd_ep_ctl_op_del(ep, UCT_SRD_EP_OP_CREP);

    desc->super.ep           = ep;
    desc->super.len          = sizeof(*neth) + sizeof(*crep);
    desc->super.comp_handler = uct_srd_iface_send_op_release;

    return desc;
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
    uct_srd_send_desc_t *desc;


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

    desc = uct_srd_ep_prepare_creq(ep);
    if (desc) {
        uct_srd_ep_tx_desc(iface, ep, desc, 0, 1);
        uct_srd_iface_complete_tx_desc(iface, ep, desc);
        uct_srd_ep_set_state(ep, UCT_SRD_EP_FLAG_CREQ_SENT);
    } else {
        uct_srd_ep_ctl_op_add(iface, ep, UCT_SRD_EP_OP_CREQ);
    }

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


ucs_status_t uct_srd_ep_am_short(uct_ep_h tl_ep, uint8_t id, uint64_t hdr,
                                 const void *buffer, unsigned length)
{
    uct_srd_ep_t *ep           = ucs_derived_of(tl_ep, uct_srd_ep_t);
    uct_srd_iface_t *iface     = ucs_derived_of(tl_ep->iface, uct_srd_iface_t);
    uct_srd_am_short_hdr_t *am = &iface->tx.am_inl_hdr;
    uct_srd_send_op_t *send_op;

    UCT_SRD_CHECK_AM_SHORT(iface, id, sizeof(am->hdr), length, "am_short");

    /* Because send completions can be out of order on EFA, we
     * need a dummy send op for am short to track completion order */
    send_op = uct_srd_ep_get_send_op(iface, ep);
    if (!send_op) {
        return UCS_ERR_NO_RESOURCE;
    }

    UCT_SRD_AM_COMMON(iface, ep, id, &am->neth);

    am->hdr = hdr;

    send_op->comp_handler = uct_srd_iface_send_op_release;

    iface->tx.sge[0].addr    = (uintptr_t)am;
    iface->tx.sge[0].length  = sizeof(*am);
    iface->tx.sge[1].addr    = (uintptr_t)buffer;
    iface->tx.sge[1].length  = length;
    iface->tx.wr_inl.num_sge = 2;
    iface->tx.wr_inl.wr_id   = (uintptr_t)send_op;

    uct_srd_post_send(iface, ep, &iface->tx.wr_inl, IBV_SEND_INLINE, 2);
    ucs_queue_push(&ep->tx.outstanding_q, &send_op->out_queue);
    uct_srd_iface_complete_tx_op(iface, ep, send_op);

    UCT_SRD_UPDATE_FC(iface, ep, am->neth.fc);

    UCT_TL_EP_STAT_OP(&ep->super, AM, SHORT, sizeof(hdr) + length);
    return UCS_OK;
}

ssize_t uct_srd_ep_am_bcopy(uct_ep_h tl_ep, uint8_t id,
                            uct_pack_callback_t pack_cb, void *arg,
                            unsigned flags)
{
    uct_srd_ep_t *ep       = ucs_derived_of(tl_ep, uct_srd_ep_t);
    uct_srd_iface_t *iface = ucs_derived_of(tl_ep->iface, uct_srd_iface_t);
    uct_srd_send_desc_t *desc;
    uct_srd_neth_t *neth;
    size_t length;

    desc = uct_srd_ep_get_send_desc(iface, ep);
    if (!desc) {
        return UCS_ERR_NO_RESOURCE;
    }

    neth = uct_srd_send_desc_neth(desc);

    UCT_SRD_AM_COMMON(iface, ep, id, neth);

    length = pack_cb(neth + 1, arg);

    UCT_SRD_CHECK_AM_BCOPY(iface, id, length);

    desc->super.comp_handler = uct_srd_iface_send_op_release;
    desc->super.len          = sizeof(*neth) + length;

    ucs_assert(iface->tx.wr_desc.num_sge == 1);
    uct_srd_ep_tx_desc(iface, ep, desc, 0, INT_MAX);
    uct_srd_iface_complete_tx_desc(iface, ep, desc);

    UCT_SRD_UPDATE_FC(iface, ep, neth->fc);

    UCT_TL_EP_STAT_OP(&ep->super, AM, BCOPY, length);
    return length;
}

void uct_srd_ep_destroy(uct_ep_h ep)
{
}
