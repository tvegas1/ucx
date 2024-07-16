/**
 * Copyright (c) NVIDIA CORPORATION & AFFILIATES, 2001-2019. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifndef UD_VERBS_H
#define UD_VERBS_H

#include <uct/ib/base/ib_verbs.h>

#include <uct/ib/ud/base/ud_iface.h>
#include <uct/ib/ud/base/ud_ep.h>
#include <uct/ib/ud/base/ud_def.h>


typedef struct {
    uint32_t                          dest_qpn;
    struct ibv_ah                     *ah;
} uct_ud_verbs_ep_peer_address_t;


typedef struct {
    uct_ud_ep_t                       super;
    uct_ud_verbs_ep_peer_address_t    peer_address;
} uct_ud_verbs_ep_t;


typedef struct {
    uct_ud_iface_t                    super;
    struct {
        struct ibv_sge                sge[UCT_IB_MAX_IOV];
        struct ibv_send_wr            wr_inl;
        struct ibv_send_wr            wr_skb;
        uint16_t                      send_sn;
        uint16_t                      comp_sn;
    } tx;
    struct {
        size_t                        max_send_sge;
    } config;
} uct_ud_verbs_iface_t;


UCS_CLASS_DECLARE(uct_ud_verbs_ep_t, const uct_ep_params_t *)


ucs_status_t uct_ud_verbs_qp_max_send_sge(uct_ud_verbs_iface_t *iface,
                                          size_t *max_send_sge);

int uct_ud_verbs_ep_is_connected(const uct_ep_h tl_ep,
                                 const uct_ep_is_connected_params_t *params);

UCS_CLASS_DECLARE(uct_ud_verbs_iface_t, uct_md_h md,
                  uct_ud_iface_ops_t *iface_ops,
                  uct_iface_ops_t *iface_tl_ops,
                  uct_worker_h worker,
                  const uct_iface_params_t *params,
                  const uct_iface_config_t *tl_config);
unsigned uct_ud_verbs_iface_async_progress(uct_ud_iface_t *ud_iface);
unsigned uct_ud_verbs_iface_progress(uct_iface_h tl_iface);
void uct_ud_verbs_iface_destroy_qp(uct_ud_iface_t *ud_iface);
uint16_t
uct_ud_verbs_ep_send_ctl(uct_ud_ep_t *ud_ep, uct_ud_send_skb_t *skb,
                         const uct_ud_iov_t *iov, uint16_t iovcnt, int flags,
                         int max_log_sge);
ucs_status_t
uct_ud_verbs_iface_unpack_peer_address(uct_ud_iface_t *iface,
                                       const uct_ib_address_t *ib_addr,
                                       const uct_ud_iface_addr_t *if_addr,
                                       int path_index, void *address_p);
void *uct_ud_verbs_ep_get_peer_address(uct_ud_ep_t *ud_ep);
size_t uct_ud_verbs_get_peer_address_length();
const char*
uct_ud_verbs_iface_peer_address_str(const uct_ud_iface_t *iface,
                                    const void *address,
                                    char *str, size_t max_size);
ucs_status_t uct_ud_verbs_ep_put_short(uct_ep_h tl_ep,
                                       const void *buffer, unsigned length,
                                       uint64_t remote_addr, uct_rkey_t rkey);
ucs_status_t
uct_ud_verbs_ep_am_zcopy(uct_ep_h tl_ep, uint8_t id, const void *header,
                         unsigned header_length, const uct_iov_t *iov,
                         size_t iovcnt, unsigned flags, uct_completion_t *comp);
ucs_status_t uct_ud_verbs_ep_am_short(uct_ep_h tl_ep, uint8_t id, uint64_t hdr,
                                      const void *buffer, unsigned length);
ucs_status_t uct_ud_verbs_ep_am_short_iov(uct_ep_h tl_ep, uint8_t id,
                                                 const uct_iov_t *iov, size_t iovcnt);
ssize_t uct_ud_verbs_ep_am_bcopy(uct_ep_h tl_ep, uint8_t id,
                                        uct_pack_callback_t pack_cb, void *arg,
                                        unsigned flags);
ucs_status_t
uct_ud_verbs_iface_event_arm(uct_iface_h tl_iface, unsigned events);
ucs_status_t
uct_ud_verbs_iface_query(uct_iface_h tl_iface, uct_iface_attr_t *iface_attr);
UCS_CLASS_DECLARE_NEW_FUNC(uct_ud_verbs_ep_t, uct_ep_t,
                           const uct_ep_params_t *);
UCS_CLASS_DECLARE_DELETE_FUNC(uct_ud_verbs_ep_t, uct_ep_t);
UCS_CLASS_DECLARE_DELETE_FUNC(uct_ud_verbs_iface_t, uct_iface_t);
#endif
