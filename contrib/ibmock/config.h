/**
 * Copyright (c) 2024, NVIDIA CORPORATION. All rights reserved.
 *
 * See file LICENSE for terms.
 */

#ifndef __CONFIG_H
#define __CONFIG_H

#define NUM_DEVS 2

#include <infiniband/efadv.h>
#include <infiniband/verbs.h>

static inline int conf_num_devices(void)
{
    return NUM_DEVS;
}

static inline const char *conf_sys_path(void)
{
    return "/tmp/fake";
}

extern struct efadv_device_attr efa_dev_attr;
extern struct ibv_device_attr efa_ibv_dev_attr;
extern struct ibv_port_attr efa_ib_port_attr;
extern struct ibv_qp_attr efa_ib_qp_attr;
extern struct ibv_qp_init_attr efa_ib_qp_init_attr;

#endif
