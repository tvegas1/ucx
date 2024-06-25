/**
 * Copyright (c) NVIDIA CORPORATION & AFFILIATES, 2024. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifndef SRD_DEF_H_
#define SRD_DEF_H_

#include <uct/ib/base/ib_iface.h>

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


typedef struct uct_srd_iface_addr {
    uct_ib_uint24_t     qp_num;
} uct_srd_iface_addr_t;

typedef struct uct_srd_ep_addr {
    uct_srd_iface_addr_t iface_addr;
    uct_ib_uint24_t      ep_id;
} uct_srd_ep_addr_t;

#endif
