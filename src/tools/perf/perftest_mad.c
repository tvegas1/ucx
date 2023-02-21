/**
* Copyright (c) NVIDIA CORPORATION & AFFILIATES, 2023. ALL RIGHTS RESERVED.
*
* See file LICENSE for terms.
*/

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

#define PERFTEST_RTE_CLASS      (IB_VENDOR_RANGE2_START_CLASS + 0x10)
#define PERFTEST_RTE_OPENIB_OUI IB_OPENIB_OUI

typedef struct perftest_mad_rte_group {
    struct ibmad_port *mad_port;
    ib_portid_t       dst_port;
    int               is_server;
} perftest_mad_rte_group_t;

static unsigned mad_magic = 0xdeadbeef;

static ucs_status_t perftest_mad_get_remote_port(void *umad, ib_portid_t *remote_port)
{
    ib_mad_addr_t *mad_addr = umad_get_mad_addr(umad);

    if (!mad_addr) {
        return UCS_ERR_INVALID_PARAM;
    }
    if (ib_portid_set(remote_port, ntohs(mad_addr->lid), 0, 0)) {
        return UCS_ERR_NO_DEVICE;
    }
    return UCS_OK;
}

static size_t perftest_mad_iov_size(const struct iovec *iovec, int iovcnt)
{
    size_t size = 0;
    while (iovcnt-- > 0) {
        size += iovec->iov_len;
        iovec++;
    }
    return size;
}

static ucs_status_t perftest_mad_sendv(perftest_mad_rte_group_t *mad,
                                       const struct iovec *iovec, int iovcnt)
{
    int len;
    int fd;
    int agent;
    int ret;
    int i;
    ib_portid_t *portid;
    uint8_t *data;

    int oui            = PERFTEST_RTE_OPENIB_OUI;
    int timeout        = 0;
    ib_rmpp_hdr_t rmpp = {
        /* Always active, even when data_size <= IB_VENDOR_RANGE2_DATA_SIZE */
        .flags = IB_RMPP_FLAG_ACTIVE,
    };
    ib_rpc_t rpc       = {};

    size_t data_size = perftest_mad_iov_size(iovec, iovcnt);
    size_t size      = umad_size() + IB_VENDOR_RANGE2_DATA_OFFS + data_size;
    void *umad;

    if (data_size > INT_MAX - IB_VENDOR_RANGE2_DATA_OFFS) {
        return UCS_ERR_INVALID_PARAM;
    }

    umad = calloc(1, size);
    if (!umad) {
        return UCS_ERR_NO_MEMORY;
    }

    data = umad_get_mad(umad) + IB_VENDOR_RANGE2_DATA_OFFS;
    for (i = 0; i < iovcnt; i++) {
        memcpy(data, iovec[i].iov_base, iovec[i].iov_len);
        data += iovec[i].iov_len;
    }

    rpc.mgtclass = PERFTEST_RTE_CLASS;
    rpc.method   = IB_MAD_METHOD_TRAP;
    rpc.attr.id  = 0;
    rpc.attr.mod = 0;
    rpc.oui      = oui;
    rpc.timeout  = 0;
    rpc.dataoffs = 0;
    rpc.datasz   = 0; /* ok: mad_build_pkt() is passed NULL pointer */

    portid     = &mad->dst_port;
    portid->qp = 1;
    if (!portid->qkey) {
        portid->qkey = IB_DEFAULT_QP1_QKEY;
    }

    len = mad_build_pkt(umad, &rpc, &mad->dst_port, &rmpp, NULL);
    if (len < 0) {
        free(umad);
        return UCS_ERR_INVALID_PARAM;
    }

    agent = mad_rpc_class_agent(mad->mad_port, rpc.mgtclass);
    fd    = mad_rpc_portid(mad->mad_port);
    len   = IB_VENDOR_RANGE2_DATA_OFFS + data_size;

    ret = umad_send(fd, agent, umad, len, timeout, 0);
    if (ret < 0) {
        goto failure;
    }
    free(umad);
    return UCS_OK;
failure:
    free(umad);
    return UCS_ERR_IO_ERROR;
}

static ucs_status_t perftest_mad_send(perftest_mad_rte_group_t *rte_group,
                                      void *buffer, size_t size)
{
    const struct iovec iovec = {
        .iov_base = buffer,
        .iov_len  = size,
    };
    return perftest_mad_sendv(rte_group, &iovec, 1);
}

static void *realloc_or_free(void *old, size_t size)
{
    void *ptr = realloc(old, size);
    if (!ptr) {
        free(old);
    }
    return ptr;
}

static ucs_status_t perftest_mad_recv(perftest_mad_rte_group_t *rte_group,
                                      void *buffer, size_t *avail,
                                      ib_portid_t *remote_port)
{
    int ret;
    void *umad;
    uint8_t *data;
    int len; /* cannot use 'size_t' here */
    size_t ref_len;
    struct ib_user_mad *user_mad;

    int timeout = 3 * 1000;
    int fd      = mad_rpc_portid(rte_group->mad_port);

    ref_len = *avail + IB_VENDOR_RANGE2_DATA_OFFS;
    umad    = calloc(1, ref_len + umad_size());
    if (!umad) {
        return UCS_ERR_NO_MEMORY;
    }

    ucs_debug("MAD: receive: Allocated len:%zu", ref_len);
retry:
    if (ref_len > INT_MAX - umad_size()) {
        free(umad);
        return UCS_ERR_INVALID_PARAM;
    }

    user_mad = umad;
    len      = ref_len;

    ret = umad_recv(fd, umad, &len, timeout);
    if (ret < 0) {
        if (errno == ETIMEDOUT || errno == EAGAIN) {
            goto retry;
        }
        if (errno != EINVAL && errno != ENOSPC) {
            ucs_error("MAD: receive: Failed, umad len:%d, ret:%d, errno:%d",
                      len, ret, errno);
            free(umad);
            return UCS_ERR_IO_ERROR;
        }
        /* EINVAL: no length info, ENOSPC: needed length is set */
        if (errno == EINVAL) {
            ref_len *= 2; /* 'len' has become invalid */
        } else {
            ref_len = len;
        }
        umad = realloc_or_free(umad, umad_size() + ref_len);
        if (!umad) {
            return UCS_ERR_NO_MEMORY;
        }
        goto retry;
    }

    if (perftest_mad_get_remote_port(umad, remote_port) != UCS_OK) {
        free(umad);
        return UCS_ERR_IO_ERROR;
    }

    len -= IB_VENDOR_RANGE2_DATA_OFFS;
    if (len <= 0) {
        if (user_mad->status == ETIMEDOUT) {
            ucs_info("MAD: receive: Remote unreachable");
            free(umad);
            return UCS_ERR_UNREACHABLE;
        }
        free(umad);
        return UCS_ERR_OUT_OF_RANGE;
    }

    ret = umad_status(umad);
    if (ret) {
        ucs_info("MAD: receive: Status failure: %d", ret);
        free(umad);
        return UCS_ERR_REJECTED;
    }
    ret = UCS_OK;

    data = (uint8_t*)umad_get_mad(umad) + IB_VENDOR_RANGE2_DATA_OFFS;
    if (len > *avail) {
        ret = UCS_ERR_MESSAGE_TRUNCATED;
        len = *avail;
    }
    memcpy(buffer, data, len);
    *avail = len;
    free(umad);
    return ret;
}

static ucs_status_t
perftest_mad_recv_from_remote(perftest_mad_rte_group_t *rte_group, void *buffer,
                              size_t *avail, const ib_portid_t *target_port)
{
    ucs_status_t ret        = UCS_ERR_IO_ERROR;
    ib_portid_t remote_port = {
        .lid = 0
    };
    size_t size;

    while (!remote_port.lid || remote_port.lid != target_port->lid) {
        size            = *avail;
        remote_port.lid = 0;
        ret = perftest_mad_recv(rte_group, buffer, &size, &remote_port);
    }
    ucs_info("MAD: recv packet size:%zu/%zu", size, *avail);
    *avail = size;
    return ret;
}

static unsigned rte_mad_group_size(void *rte_group)
{
    return 2;
}

static unsigned rte_mad_group_index(void *rte_group)
{
    return !((perftest_mad_rte_group_t*)rte_group)->is_server;
}

static ucs_status_t
perftest_mad_recv_magic(perftest_mad_rte_group_t *group, unsigned value)
{
    unsigned snc;
    size_t size = sizeof(snc);
    ucs_status_t status;

    status = perftest_mad_recv_from_remote(group, &snc, &size,
                                           &group->dst_port);

    if (status == UCS_OK && size == sizeof(snc) && snc == value) {
        return UCS_OK;
    }
    return UCS_ERR_REJECTED;
}

static ucs_status_t perftest_mad_barrier(perftest_mad_rte_group_t *group)
{
    unsigned value;
    ucs_status_t status;
    ucs_status_t result = UCS_OK;

#pragma omp barrier

#pragma omp master
    {
        value = ++mad_magic;
        if (group->is_server) {
            value = ~value;
        }

        status = perftest_mad_send(group, &value, sizeof(value));
        ucs_assert(status == UCS_OK);

        result = perftest_mad_recv_magic(group, ~value);
    }

#pragma omp barrier
    return result;
}

static void
rte_mad_barrier(void *rte_group, void (*progress)(void *arg), void *arg)
{
    ucs_status_t status;

    status = perftest_mad_barrier(rte_group);
    ucs_assert(status == UCS_OK);
}

static void rte_mad_post_vec(void *rte_group, const struct iovec *iovec,
                             int iovcnt, void **req)
{
    ucs_status_t status = perftest_mad_sendv(rte_group, iovec, iovcnt);
    ucs_assert(status == UCS_OK);
}

static void
rte_mad_recv(void *rte_group, unsigned src, void *buffer, size_t max, void *req)
{
    perftest_mad_rte_group_t *group = rte_group;
    ucs_status_t status;
    size_t size = max;

    if (src != group->is_server) {
        return;
    }

    status = perftest_mad_recv_from_remote(group, buffer, &size,
                                           &group->dst_port);
    ucs_assert(status == UCS_OK);
}

static void rte_mad_report(void *rte_group, const ucx_perf_result_t *result,
                           void *arg, const char *extra_info, int is_final,
                           int is_multi_thread)
{
    struct perftest_context *ctx = arg;
    print_progress(ctx->test_names, ctx->num_batch_files, result, extra_info,
                   ctx->flags, is_final, ctx->server_addr == NULL,
                   is_multi_thread);
}

static ucx_perf_rte_t mad_rte = {
    .group_size   = rte_mad_group_size,
    .group_index  = rte_mad_group_index,
    .barrier      = rte_mad_barrier,
    .post_vec     = rte_mad_post_vec,
    .recv         = rte_mad_recv,
    .exchange_vec = (ucx_perf_rte_exchange_vec_func_t)ucs_empty_function,
    .report       = rte_mad_report,
};

static struct ibmad_port *
perftest_mad_open(char *ca, int ca_port, int is_server)
{
    struct ibmad_port *port;

    int mgmt_classes[]     = {IB_SA_CLASS}; /* needed to activate RMPP */
    int mgmt_classes_size  = 1;
    int perftest_rte_class = PERFTEST_RTE_CLASS;
    int oui                = PERFTEST_RTE_OPENIB_OUI;
    int rmpp_version       = UMAD_RMPP_VERSION;

    if (!ca || ca_port < 0) {
        ucs_error("MAD: Missing CA or CA Port");
        return NULL;
    }

    port = mad_rpc_open_port(ca, ca_port, mgmt_classes, mgmt_classes_size);
    if (!port) {
        ucs_error("MAD: Failed to open '%s:%d'", ca, ca_port);
        return NULL;
    }

    if (mad_register_server_via(perftest_rte_class, rmpp_version, NULL, oui,
                                port) < 0) {
        ucs_error("MAD: Cannot serve perftest RTE class 0x%02x on '%s:%d'",
                  perftest_rte_class, ca, ca_port);
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

static ucs_status_t perftest_mad_sm_query(const char *ca, int ca_port,
                                          const struct ibmad_port *mad_port,
                                          uint64_t guid, ib_portid_t *dst_port)
{
    __be64 prefix;
    ibmad_gid_t selfgid;
    uint64_t port_guid;
    uint64_t gid_prefix;
    int ret;
    ib_portid_t sm_id; /* SM: the GUID to LID resolver */

    uint8_t buf[IB_SA_DATA_SIZE] = {};
    umad_port_t port             = {};

    ret = umad_get_port(ca, ca_port, &port);
    if (ret < 0) {
        ucs_error("MAD: Could not get SM LID");
        return UCS_ERR_INVALID_PARAM;
    }

    memset(&sm_id, 0, sizeof(sm_id));
    sm_id.lid = port.sm_lid;
    sm_id.sl  = port.sm_sl;

    memset(selfgid, 0, sizeof(selfgid)); /* uint8_t[] */
    gid_prefix = be64toh(port.gid_prefix);
    port_guid  = be64toh(port.port_guid);

    umad_release_port(&port);

    mad_encode_field(selfgid, IB_GID_PREFIX_F, &gid_prefix);
    mad_encode_field(selfgid, IB_GID_GUID_F, &port_guid);

    memcpy(&prefix, selfgid, sizeof(prefix));
    mad_set_field64(dst_port->gid, 0, IB_GID_PREFIX_F,
                    prefix ? be64toh(prefix) : IB_DEFAULT_SUBN_PREFIX);
    mad_set_field64(dst_port->gid, 0, IB_GID_GUID_F, guid);

    dst_port->lid = ib_path_query_via(mad_port, selfgid, dst_port->gid, &sm_id,
                                      buf);
    if (dst_port->lid < 0) {
        ucs_error("MAD: GUID Query failed");
        return UCS_ERR_UNREACHABLE;
    }
    mad_decode_field(buf, IB_SA_PR_SL_F, &dst_port->sl);
    return UCS_OK;
}

static ucs_status_t perftest_mad_get_portid(const char *ca, int ca_port,
                                            const char *addr,
                                            const struct ibmad_port *mad_port,
                                            ib_portid_t *dst_port)
{
    static const char guid_str[] = "guid:";
    static const char lid_str[]  = "lid:";

    int lid;
    uint64_t guid;
    enum MAD_DEST addr_type;

    memset(dst_port, 0, sizeof(*dst_port));

    /* Setup address and address type */
    if (!strncmp(addr, guid_str, strlen(guid_str))) {
        addr     += strlen(guid_str);
        addr_type = IB_DEST_GUID;
    } else if (!strncmp(addr, lid_str, strlen(lid_str))) {
        addr     += strlen(lid_str);
        addr_type = IB_DEST_LID;
    } else {
        ucs_error("MAD: Invalid dst address, use '%s' or '%s' prefix", guid_str,
                  lid_str);
        return UCS_ERR_INVALID_PARAM;
    }

    switch (addr_type) {
    case IB_DEST_LID:
        lid = strtol(addr, NULL, 0);
        if (!IB_LID_VALID(lid)) {
            return UCS_ERR_INVALID_PARAM;
        }
        return ib_portid_set(dst_port, lid, 0, 0)? UCS_ERR_NO_DEVICE : UCS_OK;

    case IB_DEST_GUID:
        guid = strtoull(addr, NULL, 0);
        if (!guid) {
            return UCS_ERR_INVALID_PARAM;
        }
        return perftest_mad_sm_query(ca, ca_port, mad_port, guid, dst_port);

    default:
        break;
    }
    return UCS_ERR_INVALID_PARAM;
}

static int perftest_mad_accept_is_valid(void *buf, size_t size)
{
    size_t array_size;
    perftest_params_t *params = buf;

    if (size < sizeof(*params)) {
        return 0;
    }

    array_size  = sizeof(*params->super.msg_size_list);
    array_size *= params->super.msg_size_cnt;

    return size == sizeof(*params) + array_size;
}

static ucs_status_t perftest_mad_accept(perftest_mad_rte_group_t *rte_group,
                                        struct perftest_context *ctx)
{
    size_t size;
    ucs_status_t status;
    void *ptr;
    uint8_t buf[4096];
    int lid;

    free(ctx->params.super.msg_size_list);
    ctx->params.super.msg_size_list = NULL;

    do {
        size   = sizeof(buf);
        status = perftest_mad_recv(rte_group, buf, &size, &rte_group->dst_port);

        ucs_debug("MAD: Accept: receive got status:%d, size:%zu/%zu", status, size,
                  sizeof(buf));

    } while (status != UCS_OK || !perftest_mad_accept_is_valid(buf, size));

    lid = rte_group->dst_port.lid;
    ucs_debug("MAD: Accept: remote lid:%d/0x%02x", lid, lid);

    /* Import received message size list */
    size = sizeof(*ctx->params.super.msg_size_list) *
           ctx->params.super.msg_size_cnt;
    ptr  = calloc(1, size);
    if (!ptr) {
        return UCS_ERR_NO_MEMORY;
    }
    memcpy(&ctx->params, buf, sizeof(ctx->params));
    ctx->params.super.msg_size_list = ptr;

    memcpy(ctx->params.super.msg_size_list, buf + sizeof(ctx->params), size);

    return perftest_mad_send(rte_group, &mad_magic, sizeof(mad_magic));
}

static ucs_status_t perftest_mad_connect(perftest_mad_rte_group_t *rte_group,
                                         struct perftest_context *ctx)
{
    ucs_status_t status;
    size_t size;
    struct iovec iov[2];
    int iovcnt = 1;

    iov[0].iov_base = &ctx->params;
    iov[0].iov_len  = sizeof(ctx->params);

    size  = sizeof(*ctx->params.super.msg_size_list);
    size *= ctx->params.super.msg_size_cnt;

    if (size) {
        iov[1].iov_base = ctx->params.super.msg_size_list;
        iov[1].iov_len  = size;
        iovcnt++;
    }
    status = perftest_mad_sendv(rte_group, iov, iovcnt);
    if (status != UCS_OK) {
        return status;
    }
    return perftest_mad_recv_magic(rte_group, mad_magic);
}

static void perftest_mad_set_logging(void)
{
#define IB_DEBUG_LEVEL 10
    if (ucs_log_is_enabled(UCS_LOG_LEVEL_DEBUG)) {
        ibdebug = IB_DEBUG_LEVEL; /* extern variable from mad headers */
        umad_debug(IB_DEBUG_LEVEL);
    }
}

ucs_status_t setup_mad_rte(struct perftest_context *ctx)
{
    int is_server = !ctx->server_addr;
    int ret;

    perftest_mad_rte_group_t *rte_group = calloc(1, sizeof(*rte_group));
    if (!rte_group) {
        return UCS_ERR_NO_MEMORY;
    }

    perftest_mad_set_logging();

    rte_group->is_server = is_server;
    rte_group->mad_port  = perftest_mad_open(ctx->ib.ca, ctx->ib.ca_port,
                                             is_server);
    if (!rte_group->mad_port) {
        ucs_error("MAD: %s: Cannot open port '%s:%d'",
                  is_server ? "Server" : "Client", ctx->ib.ca, ctx->ib.ca_port);
        goto fail;
    }

    if (is_server) {
        ret = perftest_mad_accept(rte_group, ctx);
    } else {
        /* Lookup server if needed */
        ret = perftest_mad_get_portid(ctx->ib.ca, ctx->ib.ca_port,
                                      ctx->server_addr, rte_group->mad_port,
                                      &rte_group->dst_port);
        if (ret != UCS_OK) {
            ucs_error("MAD: Client: Cannot get port as: '%s:%d' -> '%s'",
                      ctx->ib.ca, ctx->ib.ca_port, ctx->server_addr);
            goto fail;
        }

        /* Try to connect to it */
        ret = perftest_mad_connect(rte_group, ctx);
    }

    if (ret != UCS_OK) {
        goto fail;
    }

    ctx->params.super.rte_group  = rte_group;
    ctx->params.super.rte        = &mad_rte;
    ctx->params.super.report_arg = ctx;

    if (rte_group->is_server) {
        ctx->flags |= TEST_FLAG_PRINT_TEST;
    } else {
        ctx->flags |= TEST_FLAG_PRINT_RESULTS;
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
    ctx->params.super.rte_group     = NULL;
    if (group) {
        perftest_mad_close(group->mad_port);
        free(group);
    }
    return UCS_OK;
}
