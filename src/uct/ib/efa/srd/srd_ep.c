/**
 * Copyright (c) NVIDIA CORPORATION & AFFILIATES, 2024. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "srd_ep.h"


ucs_status_t uct_srd_ep_flush(uct_ep_h ep, unsigned flags,
                              uct_completion_t *comp)
{
    return UCS_ERR_UNSUPPORTED;
}

void uct_srd_ep_destroy(uct_ep_h ep)
{
}
