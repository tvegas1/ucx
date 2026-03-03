/**
 * Copyright (c) NVIDIA CORPORATION & AFFILIATES, 2001-2018. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifndef UCP_RMA_H_
#define UCP_RMA_H_

#include <ucp/core/ucp_types.h>
#include <ucp/core/ucp_rkey.h>
#include <ucp/proto/proto_am.h>
#include <uct/api/uct.h>
#include <ucs/datastruct/ptr_map.h>


#define UCP_PROTO_RMA_EMULATION_DESC "software emulation"


/**
 * In current implementation a known bug exists in the process of
 * flushing multiple lanes. The flush operation can be scheduled and
 * completed while an RMA operation executed prior is still pending
 * completion and scheduled on a different lane.
 *
 * To address this, we're using a single bcopy RMA lane to mitigate these
 * issues.
 */
#define UCP_PROTO_RMA_MAX_BCOPY_LANES 1


/**
 * Defines functions for RMA protocol
 */
struct ucp_rma_proto {
    const char                 *name;
    uct_pending_callback_t     progress_put;
    uct_pending_callback_t     progress_get;
};


/**
 * Defines functions for AMO protocol
 */
struct ucp_amo_proto {
    const char                 *name;
    uct_pending_callback_t     progress_fetch;
    uct_pending_callback_t     progress_post;
};


/**
 * Atomic reply data
 */
typedef union {
    uint32_t           reply32; /* 32-bit reply */
    uint64_t           reply64; /* 64-bit reply */
} ucp_atomic_reply_t;


typedef struct {
    uint64_t                  address;
    uint64_t                  ep_id;
    ucs_memory_type_t         mem_type;
} UCS_S_PACKED ucp_put_hdr_t;


typedef struct {
    uint64_t                  ep_id;
} UCS_S_PACKED ucp_cmpl_hdr_t;


typedef struct {
    uint64_t                  address;
    uint64_t                  length;
    ucp_request_hdr_t         req;
    ucs_memory_type_t         mem_type;
} UCS_S_PACKED ucp_get_req_hdr_t;


typedef struct {
    uint64_t                  req_id;
} UCS_S_PACKED ucp_rma_rep_hdr_t;


typedef struct {
    uint64_t                  address;
    ucp_request_hdr_t         req; /* invalid req_id if no reply */
    uint8_t                   length;
    uint8_t                   opcode;
} UCS_S_PACKED ucp_atomic_req_hdr_t;


extern ucp_rma_proto_t ucp_rma_basic_proto;
extern ucp_rma_proto_t ucp_rma_sw_proto;
extern ucp_amo_proto_t ucp_amo_basic_proto;
extern ucp_amo_proto_t ucp_amo_sw_proto;


extern const ucp_rma_proto_t *ucp_rma_proto_list[];
extern const ucp_amo_proto_t *ucp_amo_proto_list[];


ucs_status_t ucp_rma_request_advance(ucp_request_t *req, ssize_t frag_length,
                                     ucs_status_t status,
                                     ucs_ptr_map_key_t req_id);

void ucp_ep_flush_remote_completed(ucp_request_t *req);

void ucp_rma_sw_send_cmpl(ucp_ep_h ep);

ucs_status_t ucp_ep_fence_weak(ucp_ep_h ep);

ucs_status_t ucp_ep_fence_strong(ucp_ep_h ep);

#define GTEST_MODE 0

/* Request for remote buffers */
typedef struct ucp_rts_ppln {
    uint64_t      ep_id;
    ucp_request_t *req;
    int           count;
    ucp_md_map_t  md_map;

    /* Information needed when initializing get-ppln */
    struct {
        void      *final_buffer;
        void      *buffer;
        size_t    length;
        unsigned  mem_type;
        ucs_sys_device_t sys_dev;
    } get;
} ucp_rts_ppln_t;

int ucp_proto_rma_ppln_env_and_check(const ucp_proto_init_params_t *init_params);

size_t ucp_rma_mpool_frag_size(ucp_worker_h worker);

ucs_status_t
ucp_proto_rma_ppln_send_rts_resp(ucp_worker_h worker, ucp_ep_h ep,
                                 ucp_rts_ppln_t *rts_ppln);

#endif
