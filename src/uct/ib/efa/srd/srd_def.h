/**
 * Copyright (c) NVIDIA CORPORATION & AFFILIATES, 2024. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifndef SRD_DEF_H_
#define SRD_DEF_H_

#include <uct/ib/base/ib_iface.h>

#include <ucs/datastruct/frag_list.h>



#define UCT_SRD_INITIAL_PSN      1
#define UCT_SRD_RX_BATCH_MIN     8
#define UCT_SRD_SEND_DESC_ALIGN  UCS_SYS_CACHE_LINE_SIZE
#define UCT_SRD_SEND_OP_ALIGN    UCS_SYS_CACHE_LINE_SIZE


typedef ucs_frag_list_sn_t          uct_srd_psn_t;
typedef struct uct_srd_iface        uct_srd_iface_t;
typedef struct uct_srd_iface_addr   uct_srd_iface_addr_t;
typedef struct uct_srd_iface_peer   uct_srd_iface_peer_t;
typedef struct uct_srd_ep           uct_srd_ep_t;
typedef struct uct_srd_ep_addr      uct_srd_ep_addr_t;
typedef struct uct_srd_send_op      uct_srd_send_op_t;
typedef struct uct_srd_send_desc    uct_srd_send_desc_t;
typedef struct uct_srd_ctl_hdr      uct_srd_ctl_hdr_t;

typedef uint32_t uct_srd_ep_conn_sn_t;


enum {
    UCT_SRD_PACKET_DEST_ID_SHIFT   = 24,
    UCT_SRD_PACKET_AM_ID_SHIFT     = 27,
};

typedef struct uct_srd_neth {
    uint32_t             packet_type;
    uint8_t              fc;
} UCS_S_PACKED uct_srd_neth_t;

typedef struct uct_srd_recv_desc {
    uct_ib_iface_recv_desc_t super;

} uct_srd_recv_desc_t;


typedef struct uct_srd_am_short_hdr {
    uct_srd_neth_t neth;
    uint64_t       hdr;
} UCS_S_PACKED uct_srd_am_short_hdr_t;


typedef struct uct_srd_put_hdr {
    uct_srd_neth_t neth;
    uint64_t       rva;
} UCS_S_PACKED uct_srd_put_hdr_t;

typedef void (*uct_srd_send_op_comp_handler_t)(uct_srd_send_op_t *send_op);

/*
 * Send descriptor
 * - used for any send op (including RDMA READ) that
 *   requires some form of handling after completion
 */
struct uct_srd_send_op {
    /* link in ep outstanding send queue */
    ucs_queue_elem_t                 out_queue;

    /* number of bytes that should be sent/received */
    uint32_t                         len;
    uint16_t                         flags;

    /* ep that does the send */
    uct_srd_ep_t                     *ep;

    uct_completion_t                 *user_comp;

    /* handler that is called at send completion time */
    uct_srd_send_op_comp_handler_t   comp_handler;
} UCS_V_ALIGNED(UCT_SRD_SEND_OP_ALIGN);

/*
 * - for both am bcopy and am zcopy, network comes after the descriptor
 * - for am zcopy, the am header comes after the network header
 * - for am bcopy, the copied data comes after the network header
 * - for CREQ/CREP, control header comes after the network header
 * - for get bcopy, the copied data comes after the descriptor
 *   am short and get zcopy do not use this struct. They use send_op only
 */
struct uct_srd_send_desc {
    uct_srd_send_op_t                super;
    uct_unpack_callback_t            unpack_cb;
    void                             *unpack_arg;
    uint32_t                         lkey;
} UCS_S_PACKED UCS_V_ALIGNED(UCT_SRD_SEND_DESC_ALIGN);

typedef struct uct_srd_iface_addr {
    uct_ib_uint24_t     qp_num;
} uct_srd_iface_addr_t;

typedef struct uct_srd_ep_addr {
    uct_srd_iface_addr_t iface_addr;
    uct_ib_uint24_t      ep_id;
} uct_srd_ep_addr_t;

#endif
