/**
 * Copyright (c) NVIDIA CORPORATION & AFFILIATES, 2024. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifndef UCT_SRD_EP_H
#define UCT_SRD_EP_H

#include <ucs/type/status.h>
#include <uct/ib/base/ib_device.h>


/** @file srd_ep.h */

ucs_status_t uct_srd_ep_flush(uct_ep_h ep, unsigned flags,
                              uct_completion_t *comp);

void uct_srd_ep_destroy(uct_ep_h ep);
#endif
