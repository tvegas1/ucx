/**
* Copyright (c) NVIDIA CORPORATION & AFFILIATES, 2023. ALL RIGHTS RESERVED.
*
* See file LICENSE for terms.
*/

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "perftest_mad.h"
#include "perftest.h"

#include <ucs/sys/string.h>
#include <ucs/sys/sys.h>
#include <ucs/sys/sock.h>
#include <ucs/debug/log.h>
#include <ucs/sys/iovec.inl>

#include <sys/socket.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/poll.h>

#include <infiniband/mad.h>
#include <infiniband/umad.h>

/* TODO: Pick a good one to not interfere */
#define PERFTEST_RTE_CLASS (IB_VENDOR_RANGE2_START_CLASS + 0x10)
#define PERFTEST_RTE_OPENIB_OUI IB_OPENIB_OUI

typedef struct perftest_mad_rte_group {
    struct ibmad_port *mad_port;
    ib_portid_t dst_port;
    int is_server;
} perftest_mad_rte_group_t;

static void perftest_mad_sendv(perftest_mad_rte_group_t *mad,
                               const struct iovec *iovec,
                               int iovcnt)
{
    if (mad->is_server) {
        /* wait for a get */
        /* respond */
    } else {
        /* send get */
    }
}

static void perftest_mad_send(perftest_mad_rte_group_t *mad,
                              void *buffer,
                              size_t size)
{
    struct iovec iovec;
    int iovcnt = 1;

    iovec.iov_base = buffer;
    iovec.iov_len  = size;

    perftest_mad_sendv(mad, &iovec, iovcnt);
}

static void perftest_mad_recv(perftest_mad_rte_group_t *mad,
                              void *buffer,
                              size_t size)
{
}

static unsigned mad_rte_group_size(void *rte_group)
{
    return 2; /* lookup would use sock rte on 127.0.0.1 instead */
}

static unsigned mad_rte_group_index(void *rte_group)
{
    return ((perftest_mad_rte_group_t *)rte_group)->is_server;
}

static void mad_rte_barrier(void *rte_group, void (*progress)(void *arg),
                            void *arg)
{
    perftest_mad_rte_group_t *mad_group = rte_group;

#pragma omp barrier

#pragma omp master
  {
    const unsigned magic = 0xdeadbeef;
    unsigned snc = magic;

    perftest_mad_send(mad_group, &snc, sizeof(snc));

    snc = 0;

    perftest_mad_recv(mad_group, &snc, sizeof(snc));
    ucs_assert(snc == magic);
  }
#pragma omp barrier
}

static void mad_rte_post_vec(void *rte_group, const struct iovec *iovec,
                             int iovcnt, void **req)
{
    perftest_mad_rte_group_t *mad_group = rte_group;
    perftest_mad_sendv(mad_group, iovec, iovcnt);
}

static void mad_rte_recv(void *rte_group, unsigned src, void *buffer,
                          size_t max, void *req)
{
    perftest_mad_rte_group_t *mad_group = rte_group;
    ucs_assert(src != mad_group->is_server);
    perftest_mad_recv(mad_group, buffer, max);
}

static void mad_rte_report(void *rte_group, const ucx_perf_result_t *result,
                            void *arg, const char *extra_info, int is_final,
                            int is_multi_thread)
{
    struct perftest_context *ctx = arg;
    print_progress(ctx->test_names, ctx->num_batch_files, result, extra_info,
                   ctx->flags, is_final, ctx->server_addr == NULL,
                   is_multi_thread);
}

static ucx_perf_rte_t mad_rte = {
    .group_size    = mad_rte_group_size,
    .group_index   = mad_rte_group_index,
    .barrier       = mad_rte_barrier,
    .post_vec      = mad_rte_post_vec,
    .recv          = mad_rte_recv,
    .exchange_vec  = (ucx_perf_rte_exchange_vec_func_t)ucs_empty_function,
    .report        = mad_rte_report,
};

static struct ibmad_port *
perftest_mad_open(char *ca, int ca_port, int is_server)
{
    int mgmt_classes[] = { IB_SA_CLASS };
    int mgmt_classes_size = 1;
    struct ibmad_port *port;
    int perftest_rte_class = PERFTEST_RTE_CLASS;
    int oui = PERFTEST_RTE_OPENIB_OUI;


    #if 0
        oui = IB_OPENIB_OUI;
        perftest_rte_class = IB_VENDOR_OPENIB_PING_CLASS;
    #endif
    if (!ca || ca_port < 0) {
        ucs_error("MAD: Missing CA or CA Port");
        return NULL;
    }

    port = mad_rpc_open_port(ca, ca_port, mgmt_classes, mgmt_classes_size);
    if (!port) {
        ucs_error("MAD: Failed to open '%s:%d'", ca, ca_port);
        return NULL;
    }

    if (1 || is_server) {
        if (mad_register_server_via(perftest_rte_class,
                                        0, NULL, oui, port) < 0) {
            ucs_error("MAD: Cannot serve perftest RTE class 0x%02x on"
                      " '%s:%d'", perftest_rte_class, ca, ca_port);
            goto fail;
        }
    } else {
        if (mad_register_client_via(perftest_rte_class, 0, port) < 0) {
            ucs_error("MAD: Cannot register client for perftest RTE class 0x%02x"
                      " on '%s:%d'", perftest_rte_class, ca, ca_port);
            goto fail;
        }

    }
    return port;
fail:
    mad_rpc_close_port(port);
    return NULL;
}

void perftest_mad_close(struct ibmad_port *port)
{
    mad_rpc_close_port(port);
}

static int 
perftest_mad_resolve_portid_str(const char *ca,
                                int ca_port,
                                const char *addr,
                                const struct ibmad_port *mad_port,
                                ib_portid_t *dst_port)
{
    int lid;
    uint64_t guid;
    ib_portid_t sm_id; /* the GUID to LID resolver */
    uint8_t buf[IB_SA_DATA_SIZE] = { 0 };
    __be64 prefix;
    ibmad_gid_t selfgid;
    enum MAD_DEST addr_type;
    int ret;
    umad_port_t port = {};
    uint64_t port_guid;
    uint64_t gid_prefix;

    memset(dst_port, 0, sizeof(*dst_port));

    /* Setup address and address type */
    if (!strncmp(addr, "guid:", strlen("guid:"))) {
        addr += strlen("guid:");
        addr_type = IB_DEST_GUID;
    } else if (!strncmp(addr, "lid:", strlen("lid:"))) {
        addr += strlen("lid:");
        addr_type = IB_DEST_LID;
    } else {
        ucs_error("MAC: Invalid remote address (guid:<>|lid:<>)");
        return -1;
    }

    switch (addr_type) {
    case IB_DEST_LID:
        lid = strtol(addr, NULL, 0);
        if (!IB_LID_VALID(lid)) {
            errno = EINVAL;
            return -1;
        }
        return ib_portid_set(dst_port, lid, 0, 0);

    case IB_DEST_GUID:
        if (!(guid = strtoull(addr, NULL, 0))) {
            errno = EINVAL;
            return -1;
        }

        if ((ret = umad_get_port(ca, ca_port, &port)) < 0) {
            ucs_error("MAD: Could not get SM LID");
            return ret;
        }
        memset(&sm_id, 0, sizeof(sm_id));
        sm_id.lid = port.sm_lid;
        sm_id.sl = port.sm_sl;

        memset(selfgid, 0, sizeof(selfgid));
        gid_prefix = be64toh(port.gid_prefix);
        port_guid = be64toh(port.port_guid);
        mad_encode_field(selfgid, IB_GID_PREFIX_F, &gid_prefix);
        mad_encode_field(selfgid, IB_GID_GUID_F, &port_guid);

        umad_release_port(&port);

        memcpy(&prefix, selfgid, sizeof(prefix));
        mad_set_field64(dst_port->gid, 0, IB_GID_PREFIX_F,
                        prefix ? be64toh(prefix) : IB_DEFAULT_SUBN_PREFIX);
        mad_set_field64(dst_port->gid, 0, IB_GID_GUID_F, guid);

        if ((dst_port->lid =
             ib_path_query_via(mad_port, selfgid, dst_port->gid, &sm_id, buf)) < 0) {
            ucs_error("MAD: GUID Query failed");
            return -1;
        }

        mad_decode_field(buf, IB_SA_PR_SL_F, &dst_port->sl);
        return 0;
    default:
        return -1;
    }
}

int
perftest_mad_get_portid(const char *ca,
                        int ca_port,
                        const char *addr, 
                        const struct ibmad_port *mad_port,
                        ib_portid_t *dst_port)
{
    /* GUID needs SM port id for lookup but API also finds it */
    const char *orig_addr = addr;

#if 1
    int ret;
    ret = perftest_mad_resolve_portid_str(ca, ca_port, addr, mad_port, dst_port);
#else
    enum MAD_DEST addr_type;

    int ret;
    /* Setup address and address type */
    if (!strncmp(addr, "guid:", strlen("guid:"))) {
        addr += strlen("guid:");
        addr_type = IB_DEST_GUID;
    } else if (!strncmp(addr, "lid:", strlen("lid:"))) {
        addr += strlen("lid:");
        addr_type = IB_DEST_LID;
    } else {
        ucs_error("MAC: Invalid remote address (guid:<>|lid:<>)");
        return -1;
    }

    ret = ib_resolve_portid_str(dst_port, (char *)addr, addr_type, sm_id);
#endif
    if (ret < 0) {
        ucs_error("MAD: Failed to resolve address '%s'",
                  orig_addr);
        return -1;
    }
    return 0;
}

static void perftest_mad_accept(struct perftest_context *ctx)
{
    perftest_mad_rte_group_t *mad = ctx->params.super.rte_group;
    uint8_t buf[1024] = {};
    int len = sizeof(buf);
    int timeout = 1000000;
    int fd;
    int status;
    unsigned magic;

    void *data = umad_get_mad(buf);
    data = (uint8_t *)data + IB_VENDOR_RANGE2_DATA_OFFS;

    fd = mad_rpc_portid(mad->mad_port);

    for (;;) {
        status = umad_recv(fd, buf, &len, timeout);
        if (status == -EAGAIN) {
            ucs_error("MAD: accept eagain...");
            continue;
        }
        if (status == -ETIMEDOUT) {
            ucs_error("MAD: accept etimeout...");
            continue;
        }
 
        if (status < 0) {
            ucs_error("MAD: accept() failed status:%d", status);
            return;
        }
        break;
    }

    status = umad_status(buf);
    if (status) {
        ucs_error("MAD: accept() got error status: %d", status);
        return;
    }

    memcpy(&magic, data, sizeof(magic));
    if (magic != 0xdeadbeef) {
        ucs_error("MAD: Bad magic");
    } else {
        ucs_error("MAD: Good magic");
    }
}

#if 1
static void perftest_mad_connect(struct perftest_context *ctx)
{
    perftest_mad_rte_group_t *mad = ctx->params.super.rte_group;
    uint8_t buf[2048] = {};
    void *data = umad_get_mad(buf) + IB_VENDOR_RANGE2_DATA_OFFS;
    int len;
    int fd;
    int agent;
    ib_rpc_t rpc = {};
    unsigned magic = 0xdeadbeef;
    int oui = PERFTEST_RTE_OPENIB_OUI;
    int ret;
    int timeout = 1000;
    ib_portid_t *portid;

    memcpy(data, &magic, sizeof(magic));

    rpc.mgtclass = PERFTEST_RTE_CLASS;
    rpc.method = IB_MAD_METHOD_GET;
    rpc.attr.id = 0;
    rpc.attr.mod = 0;
    rpc.oui = oui;
    rpc.timeout = 0;
    rpc.dataoffs = IB_VENDOR_RANGE2_DATA_OFFS;
    rpc.datasz = IB_VENDOR_RANGE2_DATA_SIZE;

    portid = &mad->dst_port;
    portid->qp = 1;
    if (!portid->qkey)
        portid->qkey = IB_DEFAULT_QP1_QKEY;

    len = mad_build_pkt(buf, &rpc, &mad->dst_port, NULL, NULL);
    if (len < 0){ 
        ucs_error("MAD: cannot build connect packet");
        return;
    }
    agent = mad_rpc_class_agent(mad->mad_port, rpc.mgtclass);

    fd = mad_rpc_portid(mad->mad_port);

    ret = umad_send(fd, agent, buf, len, timeout, 0);
    if (ret < 0) {
        ucs_error("MAD: cannot send connect_packet");
        return;
    }
    ucs_error("MAD: connect sent packet");
    
    //
    //fd = mad_rpc_portid(mad->mad_port);
    //agent = mad_rpc_class_agent(mad->mad_port, rpc.mgtclass);
}
#else
static void perftest_mad_connect(struct perftest_context *ctx)
{
    perftest_mad_rte_group_t *mad = ctx->params.super.rte_group;
    int oui = PERFTEST_RTE_OPENIB_OUI;
    ib_vendor_call_t call;
    void *ptr;
    unsigned magic;
    uint8_t data[2048];

#if 0
    oui = IB_OPENIB_OUI;
#endif


    call.method = IB_MAD_METHOD_GET;
#if 0
    call.mgmt_class = IB_VENDOR_OPENIB_PING_CLASS;
#else
    call.mgmt_class = PERFTEST_RTE_CLASS;
#endif
    call.attrid = 0;
    call.mod = 0;
    call.oui = oui;
    call.timeout = 0;
    memset(&call.rmpp, 0, sizeof(call.rmpp));


    magic = 0xdeadbeef;
    memcpy(data, &magic, sizeof(magic));

    ptr = ib_vendor_call_via(data, &mad->dst_port, &call, mad->mad_port);
    if (!ptr) {
        ucs_error("Failed!");
        return;
    }

}
#endif

ucs_status_t setup_mad_rte(struct perftest_context *ctx)
{
    int ret;
    int is_server = !ctx->server_addr;

    perftest_mad_rte_group_t *rte_group = calloc(1, sizeof(*rte_group));
    if (!rte_group) {
        return UCS_ERR_NO_MEMORY;
    }
    ibdebug = 10;
    umad_debug(10);

    rte_group->mad_port = perftest_mad_open(ctx->ib.ca,
                                            ctx->ib.ca_port,
                                            is_server);
    if (!rte_group->mad_port) {
        goto fail;
    }

    if (!is_server) {
        ret = perftest_mad_get_portid(ctx->ib.ca,
                                      ctx->ib.ca_port, 
                                      ctx->server_addr,
                                      rte_group->mad_port,
                                      &rte_group->dst_port);
        if (ret < 0) {
            ucs_error("MAD: Client: Cannot get port as: '%s:%d' -> '%s'",
                      ctx->ib.ca, ctx->ib.ca_port, ctx->server_addr);
            /* TODO: release dst port */
            goto fail;
        }
    }
    rte_group->is_server = is_server;

    ctx->params.super.rte_group  = rte_group;
    ctx->params.super.rte        = &mad_rte;
    ctx->params.super.report_arg = ctx;

    if (!is_server) {
        /* client sends initial ping */
        perftest_mad_connect(ctx);
    } else {
        perftest_mad_accept(ctx);
    }
    return UCS_OK;
fail:
    free(rte_group);
    return UCS_ERR_NO_DEVICE;

}

ucs_status_t cleanup_mad_rte(struct perftest_context *ctx)
{
    perftest_mad_rte_group_t *group = ctx->params.super.rte_group;
    perftest_mad_close(group->mad_port);
    return UCS_OK;
}
