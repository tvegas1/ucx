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

typedef struct uct_srd_peer_name {
    char name[16];
    int  pid;
} uct_srd_peer_name_t;

typedef struct uct_srd_ep_peer_address {
    uint32_t                          dest_qpn;
    struct ibv_ah                     *ah;
} uct_srd_ep_peer_address_t;


typedef struct uct_srd_ep_pending_op {
    ucs_arbiter_group_t   group;
    uint8_t               ops;    /* bitmask that describes what
                                     control ops are sceduled */
    ucs_arbiter_elem_t    elem;
} uct_srd_ep_pending_op_t;

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

ucs_status_t uct_srd_ep_flush(uct_ep_h ep, unsigned flags,
                              uct_completion_t *comp);

ucs_status_t uct_srd_ep_create(const uct_ep_params_t *params, uct_ep_h *ep_p);

void uct_srd_ep_destroy(uct_ep_h ep);

UCS_CLASS_DECLARE_NEW_FUNC(uct_srd_ep_t, uct_ep_t, const uct_ep_params_t *);
UCS_CLASS_DECLARE_DELETE_FUNC(uct_srd_ep_t, uct_ep_t);
#endif
