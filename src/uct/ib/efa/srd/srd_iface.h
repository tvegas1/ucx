/**
 * Copyright (c) NVIDIA CORPORATION & AFFILIATES, 2024. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifndef UCT_SRD_IFACE_H
#define UCT_SRD_IFACE_H

#include <uct/ib/base/ib_iface.h>
#include <uct/ib/ud/base/ud_iface_common.h>

#include <ucs/datastruct/ptr_array.h>
#include <ucs/datastruct/conn_match.h>


BEGIN_C_DECLS


/** @file srd_iface.h */

/* TODO: optimize endpoint memory footprint */
enum {
    UCT_SRD_EP_FLAG_DISCONNECTED      = UCS_BIT(0),  /* EP was disconnected */
    UCT_SRD_EP_FLAG_PRIVATE           = UCS_BIT(1),  /* EP was created as internal */
    UCT_SRD_EP_FLAG_HAS_PENDING       = UCS_BIT(2),  /* EP has some pending requests */
    UCT_SRD_EP_FLAG_CONNECTED         = UCS_BIT(3),  /* EP was connected to the peer */
    UCT_SRD_EP_FLAG_ON_CEP            = UCS_BIT(4),  /* EP was inserted to connection
                                                        matching context */

    /* ep should piggy-back a credit grant on the next outgoing AM */
    UCT_SRD_EP_FLAG_FC_GRANT          = UCS_BIT(5),

    /* debug flags */
    UCT_SRD_EP_FLAG_CREQ_RCVD         = UCS_BIT(6),  /* CREQ message was received */
    UCT_SRD_EP_FLAG_CREP_RCVD         = UCS_BIT(7),  /* CREP message was received */
    UCT_SRD_EP_FLAG_CREQ_SENT         = UCS_BIT(8),  /* CREQ message was sent */
    UCT_SRD_EP_FLAG_CREP_SENT         = UCS_BIT(9),  /* CREP message was sent */
    UCT_SRD_EP_FLAG_CREQ_NOTSENT      = UCS_BIT(10), /* CREQ message is NOT sent, because
                                                        connection establishment process
                                                        is driven by remote side. */

    /* Endpoint is currently executing the pending queue */
#if UCS_ENABLE_ASSERT
    UCT_SRD_EP_FLAG_IN_PENDING        = UCS_BIT(11)
#else
    UCT_SRD_EP_FLAG_IN_PENDING        = 0
#endif
};

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
    ucs_ptr_array_t            eps;

#ifdef HAVE_DECL_EFA_DV_RDMA_READ
    struct ibv_qp_ex           *qp_ex;
#endif

    struct {
        unsigned               tx_qp_len;
        unsigned               max_inline;
        size_t                 max_send_sge;
        size_t                 max_get_bcopy;
        size_t                 max_get_zcopy;

        /* Threshold to send "soft" FC credit request. The peer will try to
         * piggy-back credits grant to the counter AM, if any. */
        int16_t              fc_soft_thresh;

        /* Threshold to sent "hard" credits request. The peer will grant
         * credits in a separate ctrl message as soon as it handles this request. */
        int16_t              fc_hard_thresh;

        uint16_t             fc_wnd_size;
    } config;

    ucs_conn_match_ctx_t       conn_match_ctx;
} uct_srd_iface_t;

struct uct_srd_ep;

void uct_srd_iface_add_ep(uct_srd_iface_t *iface, struct uct_srd_ep *ep);

void uct_srd_iface_remove_ep(uct_srd_iface_t *iface, struct uct_srd_ep *ep);

uct_srd_ep_conn_sn_t
uct_srd_iface_cep_get_conn_sn(uct_srd_iface_t *iface,
                              const uct_ib_address_t *ib_addr,
                              const uct_srd_iface_addr_t *if_addr,
                              int path_index);

uct_srd_ep_t *uct_srd_iface_cep_get_ep(uct_srd_iface_t *iface,
                                       const uct_ib_address_t *ib_addr,
                                       const uct_srd_iface_addr_t *if_addr,
                                       int path_index,
                                       uct_srd_ep_conn_sn_t conn_sn,
                                       int is_private);

ucs_status_t
uct_srd_iface_unpack_peer_address(uct_srd_iface_t *iface,
                                  const uct_ib_address_t *ib_addr,
                                  const uct_srd_iface_addr_t *if_addr,
                                  int path_index, void *address_p);
void uct_srd_iface_cep_insert_ep(uct_srd_iface_t *iface,
                                 const uct_ib_address_t *ib_addr,
                                 const uct_srd_iface_addr_t *if_addr,
                                 int path_index, uct_srd_ep_conn_sn_t conn_sn,
                                 uct_srd_ep_t *ep);

uct_srd_ep_conn_sn_t
uct_srd_iface_cep_get_conn_sn(uct_srd_iface_t *iface,
                             const uct_ib_address_t *ib_addr,
                             const uct_srd_iface_addr_t *if_addr,
                             int path_index);
void *
uct_srd_iface_cep_get_peer_address(uct_srd_iface_t *iface,
                                   const uct_ib_address_t *ib_addr,
                                   const uct_srd_iface_addr_t *if_addr,
                                   int path_index, void *address_p);
#endif
