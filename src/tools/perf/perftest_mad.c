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
#include <infiniband/umad_types.h>

/* TODO: How to pick a good one to not interfere? */
#define PERFTEST_RTE_CLASS (IB_VENDOR_RANGE2_START_CLASS + 0x10)
#define PERFTEST_RTE_OPENIB_OUI IB_OPENIB_OUI

typedef struct perftest_mad_rte_group {
    struct ibmad_port           *mad_port;
    ib_portid_t                  dst_port;
    int                          is_server;
} perftest_mad_rte_group_t;

static int
perftest_mad_get_remote_port(void *umad, ib_portid_t *remote_port)
{
    ib_mad_addr_t *mad_addr;

    if (!(mad_addr = umad_get_mad_addr(umad))) {
        return -1;
    }
    return ib_portid_set(remote_port, ntohs(mad_addr->lid), 0, 0)? -1 : 0;
}

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

static ucs_status_t perftest_mad_send(perftest_mad_rte_group_t *mad,
                                      void *buffer,
                                      size_t size)
{
#if 0
    size_t umad_size = umad_size() + IB_VENDOR_RANGE2_DATA_OFFS + size;
    void *umad = malloc(umad_size);
    if (!umad) {
        return UCS_ERR_NO_MEMORY;
    }
#endif
    return UCS_OK;
}

static ucs_status_t
perftest_mad_recv(perftest_mad_rte_group_t *rte_group,
                  void *buffer,
                  int *avail,
                  ib_portid_t *remote_port)
{
    int ret;
    void *umad;
    uint8_t *data;

    int timeout = 1000 * 1000;
    int fd = mad_rpc_portid(rte_group->mad_port);
    int len; /* cannot use 'size_t' here */

    len = *avail + IB_VENDOR_RANGE2_DATA_OFFS;
    umad = calloc(1, len + umad_size());
    if (!umad) {
        return UCS_ERR_NO_MEMORY;
    }

retry:
    ret = umad_recv(fd, umad, &len, timeout);
    if (ret < 0) {
        if (errno == ENOSPC) {
            umad = realloc(umad, umad_size() + len);
            goto retry;
        }
        ucs_error("MAD: failed to receive umad len:%d, ret:%d", len, ret);
        free(umad);
        return UCS_ERR_IO_ERROR;
    }

    if (perftest_mad_get_remote_port(umad, remote_port) < 0) {
        ucs_error("MAD: failed to get remote port from received MAD");
        free(umad);
        return UCS_ERR_IO_ERROR;
    }

    if (len <= 0) {
        free(umad);
        return UCS_ERR_OUT_OF_RANGE;
    }

    ret = umad_status(umad);
    if (ret) {
        ucs_error("MAD: umad received failure: %d", ret);
        free(umad);
        return UCS_ERR_REJECTED;
    }
    ret = UCS_OK;

    data = (uint8_t *)umad_get_mad(umad) + IB_VENDOR_RANGE2_DATA_OFFS;
    len -= IB_VENDOR_RANGE2_DATA_OFFS;
    if (len > *avail) {
        ret = UCS_ERR_MESSAGE_TRUNCATED;
        len = *avail;
    }
    memcpy(buffer, data, len);
    *avail = len;
    free(umad);
    return ret;
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

    /*perftest_mad_recv(mad_group, &snc, sizeof(snc)); */
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
    /*perftest_mad_recv(mad_group, buffer, max);*/
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
    int mgmt_classes[] = { IB_SA_CLASS }; /* needed to activate RMPP */
    int mgmt_classes_size = 1;
    struct ibmad_port *port;
    int perftest_rte_class = PERFTEST_RTE_CLASS;
    int oui = PERFTEST_RTE_OPENIB_OUI;
    int rmpp_version = UMAD_RMPP_VERSION;

    if (!ca || ca_port < 0) {
        ucs_error("MAD: Missing CA or CA Port");
        return NULL;
    }

    port = mad_rpc_open_port(ca, ca_port, mgmt_classes, mgmt_classes_size);
    if (!port) {
        ucs_error("MAD: Failed to open '%s:%d'", ca, ca_port);
        return NULL;
    }

    if (mad_register_server_via(perftest_rte_class,
                                rmpp_version,
                                NULL,
                                oui,
                                port) < 0) {
        ucs_error("MAD: Cannot serve perftest RTE class 0x%02x on"
                  " '%s:%d'", perftest_rte_class, ca, ca_port);
        goto fail;
    }
    return port;
fail:
    mad_rpc_close_port(port);
    return NULL;
}

void perftest_mad_close(struct ibmad_port *port)
{
    if (port) {
        mad_rpc_close_port(port);
    }
}

static ucs_status_t
perftest_mad_sm_query(const char *ca,
                      int ca_port,
                      const struct ibmad_port *mad_port,
                      uint64_t guid,
                      ib_portid_t *dst_port)
{
    uint8_t buf[IB_SA_DATA_SIZE] = { 0 };
    umad_port_t port = {};
    __be64 prefix;
    ibmad_gid_t selfgid;
    uint64_t port_guid;
    uint64_t gid_prefix;
    int ret;
    ib_portid_t sm_id; /* SM: the GUID to LID resolver */

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
}

static int 
perftest_mad_get_portid(const char *ca,
                        int ca_port,
                        const char *addr,
                        const struct ibmad_port *mad_port,
                        ib_portid_t *dst_port)
{
    int lid;
    uint64_t guid;
    enum MAD_DEST addr_type;
    static const char guid_str[] = "guid:";
    static const char lid_str[] = "lid:";

    memset(dst_port, 0, sizeof(*dst_port));

    /* Setup address and address type */
    if (!strncmp(addr, guid_str, strlen(guid_str))) {
        addr += strlen(guid_str);
        addr_type = IB_DEST_GUID;
    } else if (!strncmp(addr, lid_str, strlen(lid_str))) {
        addr += strlen(lid_str);
        addr_type = IB_DEST_LID;
    } else {
        ucs_error("MAD: Invalid dst address, use '%s' or '%s' prefix",
                  guid_str, lid_str);
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
        return perftest_mad_sm_query(ca, ca_port, mad_port, guid, dst_port);

    default:
        break;
    }
    return -1;
}

static void perftest_mad_accept(struct perftest_context *ctx)
{
    perftest_mad_rte_group_t *rte_group = ctx->params.super.rte_group;
    int need;
    int i;
    int ret;

    do {
        need = sizeof(ctx->params);
        ret = perftest_mad_recv(rte_group,
                                &ctx->params,
                                &need,
                                &rte_group->dst_port);

        for (i = 0; i < need; i++) {
            if (i && !(i % 16)) {
                printf("\n");
            }
            printf("%02x ", ((uint8_t *)&ctx->params)[i]);
        }
        printf("\n");
        ucs_error("ACCEPT: got ret:%d, size:%d/%d",
                  ret, need, (int)sizeof(ctx->params));
    } while (ret != UCS_OK || need != (int)sizeof(ctx->params));
}

static void perftest_mad_connect(struct perftest_context *ctx)
{
    perftest_mad_rte_group_t *mad = ctx->params.super.rte_group;
    uint8_t buf[8192*10] = {};
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
    ib_rmpp_hdr_t rmpp = {};
    int data_size;
    int i;

    memcpy(data, &magic, sizeof(magic));

    rpc.mgtclass = PERFTEST_RTE_CLASS;
    rpc.method = IB_MAD_METHOD_GET;
    rpc.attr.id = 0;
    rpc.attr.mod = 0;
    rpc.oui = oui;
    rpc.timeout = 0;
    rpc.dataoffs = IB_VENDOR_RANGE2_DATA_OFFS; /* not used */
    rpc.datasz = IB_VENDOR_RANGE2_DATA_SIZE; /* not used */

    portid = &mad->dst_port;
    portid->qp = 1;
    if (!portid->qkey)
        portid->qkey = IB_DEFAULT_QP1_QKEY;

    data_size = 8192 * 3; /* seems data_size == 8192 * 3 can be problematic on rx size */
    data_size = 8192;
    if (data_size > IB_VENDOR_RANGE2_DATA_SIZE) {
        rmpp.flags = IB_RMPP_FLAG_ACTIVE;
    }

    for (i = 0; i < data_size; i++) {
        if (data_size - 1 - i < 4) {
            break;
        }
        ((char *)data)[data_size - 1 - i] = i & 0xff;
    }
    len = mad_build_pkt(buf, &rpc, &mad->dst_port, &rmpp, NULL);
    if (len < 0){ 
        ucs_error("MAD: cannot build connect packet");
        return;
    }
    agent = mad_rpc_class_agent(mad->mad_port, rpc.mgtclass);

    fd = mad_rpc_portid(mad->mad_port);

    len = IB_VENDOR_RANGE2_DATA_OFFS + data_size;
    ret = umad_send(fd, agent, buf, len, timeout, 0);
    if (ret < 0) {
        ucs_error("MAD: cannot send connect_packet");
        return;
    }
    ucs_error("MAD: connect sent packet");
}

/* TODO: Make optional or from env */
static void
perftest_mad_debug_enable(void)
{
    ibdebug = 10; /* extern variable from mad headers */
    umad_debug(10);
}

ucs_status_t setup_mad_rte(struct perftest_context *ctx)
{
    int ret;
    int is_server = !ctx->server_addr;

    perftest_mad_rte_group_t *rte_group = calloc(1, sizeof(*rte_group));
    if (!rte_group) {
        return UCS_ERR_NO_MEMORY;
    }

    perftest_mad_debug_enable();

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
            /* TODO: release dst port? apparently not needed */
            goto fail;
        }
    }
    rte_group->is_server = is_server;

    ctx->params.super.rte_group  = rte_group;
    ctx->params.super.rte        = &mad_rte;
    ctx->params.super.report_arg = ctx;

    if (is_server) {
        perftest_mad_accept(ctx);
    } else {
        perftest_mad_connect(ctx);
        for (;;);
    }
    return UCS_OK;
fail:
    perftest_mad_close(rte_group->mad_port);
    free(rte_group);
    return UCS_ERR_NO_DEVICE;
}

ucs_status_t cleanup_mad_rte(struct perftest_context *ctx)
{
    perftest_mad_rte_group_t *group = ctx->params.super.rte_group;
    perftest_mad_close(group->mad_port);
    return UCS_OK;
}
