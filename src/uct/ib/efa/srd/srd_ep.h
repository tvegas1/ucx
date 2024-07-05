/**
 * Copyright (c) NVIDIA CORPORATION & AFFILIATES, 2024. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifndef UCT_SRD_EP_H
#define UCT_SRD_EP_H

#include <ucs/type/status.h>
#include <uct/ib/base/ib_device.h>

#include "srd_iface.h"

/** @file srd_ep.h */


#define UCT_SRD_EP_NULL_ID     ((1 << 24) - 1)
#define UCT_SRD_EP_ID_MAX      UCT_SRD_EP_NULL_ID
#define UCT_SRD_EP_CONN_ID_MAX UCT_SRD_EP_ID_MAX

#define UCT_SRD_IFACE_CEP_CONN_SN_MAX ((uct_srd_ep_conn_sn_t)-1)


/*
 * Endpoint pending control operations. The operations
 * are executed in time of progress along with
 * pending requests added by uct user.
 */
enum {
    UCT_SRD_EP_OP_NONE      = 0,
    UCT_SRD_EP_OP_CREP      = UCS_BIT(0),  /* send connection reply */
    UCT_SRD_EP_OP_CREQ      = UCS_BIT(1),  /* send connection request */
    UCT_SRD_EP_OP_FC_PGRANT = UCS_BIT(2),  /* send pure fc grant */
};


typedef struct uct_srd_ep_pending_op {
    ucs_arbiter_group_t   group;
    uint8_t               ops;    /* bitmask that describes what
                                     control ops are sceduled */
    ucs_arbiter_elem_t    elem;
} uct_srd_ep_pending_op_t;


enum {
    UCT_SRD_FC_STAT_NO_CRED,
    UCT_SRD_FC_STAT_TX_GRANT,
    UCT_SRD_FC_STAT_TX_PURE_GRANT,
    UCT_SRD_FC_STAT_TX_SOFT_REQ,
    UCT_SRD_FC_STAT_TX_HARD_REQ,
    UCT_SRD_FC_STAT_RX_GRANT,
    UCT_SRD_FC_STAT_RX_PURE_GRANT,
    UCT_SRD_FC_STAT_RX_SOFT_REQ,
    UCT_SRD_FC_STAT_RX_HARD_REQ,
    UCT_SRD_FC_STAT_FC_WND,
    UCT_SRD_FC_STAT_LAST
};

typedef struct uct_srd_ep {
    uct_base_ep_t                     super;

    ucs_conn_match_elem_t             conn_match;
    uct_srd_ep_conn_sn_t              conn_sn;

    uint16_t                          flags;
    uint32_t                          ep_id;
    uint32_t                          dest_ep_id;
    uint8_t                           path_index;
    uct_srd_ep_peer_address_t         peer_address;

    struct {
        int16_t                       fc_wnd; /* Not more than fc_wnd active messages
                                                 can be sent without acknowledgment */
        UCS_STATS_NODE_DECLARE(stats)
    } fc;
    struct {
        uct_srd_psn_t                 psn;     /* Next PSN to send */
        uct_srd_ep_pending_op_t       pending; /* pending */
        ucs_queue_head_t              outstanding_q; /* queue of outstanding send ops*/
        UCS_STATS_NODE_DECLARE(stats)
    } tx;
    struct {
        ucs_frag_list_t               ooo_pkts; /* Out of order packets that
                                                   can not be processed yet */
        UCS_STATS_NODE_DECLARE(stats)
    } rx;

    /* connection sequence number. assigned in connect_to_iface() */
    uint8_t                           rx_creq_count;

#if ENABLE_DEBUG_DATA
    uct_srd_peer_name_t               peer;
#endif
} uct_srd_ep_t;


/**
 * SRD pending request private data
 */
typedef struct {
    uct_pending_req_priv_arb_t arb;
    unsigned                   flags;
} uct_srd_pending_req_priv_t;


static UCS_F_ALWAYS_INLINE uct_srd_pending_req_priv_t *
uct_srd_pending_req_priv(uct_pending_req_t *req)
{
    return (uct_srd_pending_req_priv_t *)&(req)->priv;
}

ucs_status_t uct_srd_ep_flush(uct_ep_h ep, unsigned flags,
                              uct_completion_t *comp);
ucs_status_t uct_srd_ep_am_short(uct_ep_h tl_ep, uint8_t id, uint64_t hdr,
                                 const void *buffer, unsigned length);


static UCS_F_ALWAYS_INLINE void
uct_srd_ep_ctl_op_del(uct_srd_ep_t *ep, uint32_t ops)
{
    ep->tx.pending.ops &= ~ops;
}

static UCS_F_ALWAYS_INLINE int
uct_srd_ep_ctl_op_check(uct_srd_ep_t *ep, uint32_t op)
{
    return ep->tx.pending.ops & op;
}

static UCS_F_ALWAYS_INLINE int
uct_srd_ep_ctl_op_isany(uct_srd_ep_t *ep)
{
    return ep->tx.pending.ops;
}

static UCS_F_ALWAYS_INLINE int
uct_srd_ep_ctl_op_check_ex(uct_srd_ep_t *ep, uint32_t ops)
{
    /* check that at least one the given ops is set and
     *      * all ops not given are not set */
    return (ep->tx.pending.ops & ops) &&
        ((ep->tx.pending.ops & ~ops) == 0);
}

ucs_status_t uct_srd_ep_create(const uct_ep_params_t *params, uct_ep_h *ep_p);

ucs_status_t uct_srd_ep_pending_add(uct_ep_h ep_h, uct_pending_req_t *req,
                                    unsigned flags);
ssize_t uct_srd_ep_am_bcopy(uct_ep_h tl_ep, uint8_t id,
                            uct_pack_callback_t pack_cb, void *arg,
                            unsigned flags);
void uct_srd_ep_pending_purge(uct_ep_h ep_h, uct_pending_purge_callback_t cb,
                              void *arg);
void uct_srd_ep_destroy(uct_ep_h ep);


static UCS_F_ALWAYS_INLINE int uct_srd_ep_is_connected(uct_srd_ep_t *ep)
{
    ucs_assert((ep->dest_ep_id == UCT_SRD_EP_NULL_ID) ==
               !(ep->flags & UCT_SRD_EP_FLAG_CONNECTED));
    return ep->flags & UCT_SRD_EP_FLAG_CONNECTED;
}

    static UCS_F_ALWAYS_INLINE int
uct_srd_ep_is_connected_and_no_pending(uct_srd_ep_t *ep)
{
    return (ep->flags & (UCT_SRD_EP_FLAG_CONNECTED |
                         UCT_SRD_EP_FLAG_HAS_PENDING))
        == UCT_SRD_EP_FLAG_CONNECTED;
}

    static UCS_F_ALWAYS_INLINE int
uct_srd_ep_has_fc_resources(const uct_srd_ep_t *ep)
{
    return ep->fc.fc_wnd > 0;
}

UCS_CLASS_DECLARE_NEW_FUNC(uct_srd_ep_t, uct_ep_t, const uct_ep_params_t *);
UCS_CLASS_DECLARE_DELETE_FUNC(uct_srd_ep_t, uct_ep_t);
#endif
