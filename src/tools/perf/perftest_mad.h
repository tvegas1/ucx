/**
* Copyright (c) NVIDIA CORPORATION & AFFILIATES, 2023. ALL RIGHTS RESERVED.
*
* See file LICENSE for terms.
*/

#ifndef UCX_PERFTEST_MAD_H
#define UCX_PERFTEST_MAD_H

#include <ucs/type/status.h>

struct perftest_context;

ucs_status_t setup_mad_rte(struct perftest_context *ctx);
ucs_status_t cleanup_mad_rte(struct perftest_context *ctx);
#endif /* UCX_PERFTEST_MAD_H */
