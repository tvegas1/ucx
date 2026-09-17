/**
* Copyright (c) NVIDIA CORPORATION & AFFILIATES, 2020. ALL RIGHTS RESERVED.
*
* See file LICENSE for terms.
*/

#include <common/test.h>
#include <common/mem_buffer.h>

#include "test_ucp_tag.h"
#include "ucp_datatype.h"

extern "C" {
#include <ucp/core/ucp_ep.inl>
#include <ucp/core/ucp_worker.h>
#include <ucs/datastruct/mpool.inl>
#include <ucs/datastruct/queue.h>
}

#include <iostream>
#include <map>
#include <memory>


class test_ucp_tag_mem_type: public test_ucp_tag {
public:
    enum {
        VARIANT_GDR_OFF            = UCS_BIT(0),
        VARIANT_TAG_OFFLOAD        = UCS_BIT(1),
        VARIANT_PROTO_V1           = UCS_BIT(2),
        VARIANT_CONNECT_ALL_TO_ALL = UCS_BIT(3),
        VARIANT_MAX                = UCS_BIT(4)
    };

    void init()
    {
        int variant_flags = get_variant_value() / m_mem_type_pairs.size();

        if (variant_flags & VARIANT_GDR_OFF) {
            if (!has_any_transport({"dc_x", "ud_v", "ud_x", "rc_v", "rc_x",
                                    "srd", "ib"})) {
                UCS_TEST_SKIP_R("No GPU direct RDMA");
            }

            m_env.push_back(
                    new ucs::scoped_setenv("UCX_IB_GPU_DIRECT_RDMA", "n"));
            m_env.push_back(
                    new ucs::scoped_setenv("UCX_GGA_GPU_DIRECT_RDMA", "n"));
        }

        if (variant_flags & VARIANT_TAG_OFFLOAD) {
            if (!has_any_transport({"rc_x", "dc_x", "ib"})) {
                UCS_TEST_SKIP_R("No tag offload");
            }

            enable_tag_mp_offload();

            if (RUNNING_ON_VALGRIND) {
                if (variant_flags & VARIANT_PROTO_V1) {
                    UCS_TEST_SKIP_R("Skip proto v1 with valgrind");
                }
                m_env.push_back(
                        new ucs::scoped_setenv("UCX_RC_TM_SEG_SIZE", "8k"));
                m_env.push_back(
                        new ucs::scoped_setenv("UCX_TCP_RX_SEG_SIZE", "8k"));
                m_env.push_back(
                        new ucs::scoped_setenv("UCX_RC_RX_QUEUE_LEN", "1024"));
            }
        }

        if (variant_flags & VARIANT_PROTO_V1) {
            modify_config("PROTO_ENABLE", "n");
        } else {
            modify_config("PROTO_REQUEST_RESET", "y");
        }

        int mem_type_pair_index = get_variant_value() % m_mem_type_pairs.size();
        m_send_mem_type         = m_mem_type_pairs[mem_type_pair_index][0];
        m_recv_mem_type         = m_mem_type_pairs[mem_type_pair_index][1];

        if (variant_flags & VARIANT_CONNECT_ALL_TO_ALL) {
            modify_config("CONNECT_ALL_TO_ALL", "y");
        }

        modify_config("MAX_EAGER_LANES", "2");
        modify_config("MAX_RNDV_LANES", "2");

        test_ucp_tag::init();
    }

    static void
    add_mem_type_test_variant(std::vector<ucp_test_variant> &variants,
                              int variant_value,
                              ucs_memory_type_t send_mem_type,
                              ucs_memory_type_t recv_mem_type)
    {
        std::string name = ucs_memory_type_names[send_mem_type] +
                           std::string(":") +
                           ucs_memory_type_names[recv_mem_type];

        int variant_flags = variant_value / m_mem_type_pairs.size();

        if (variant_flags & VARIANT_GDR_OFF) {
            if ((send_mem_type != UCS_MEMORY_TYPE_CUDA) &&
                (send_mem_type != UCS_MEMORY_TYPE_ROCM) &&
                (recv_mem_type != UCS_MEMORY_TYPE_CUDA) &&
                (recv_mem_type != UCS_MEMORY_TYPE_ROCM)) {
                /* No need to disable GPU-direct if the memory type does not
                   support it anyway */
                return;
            }
            name += ",nogdr";
        }

        if (variant_flags & VARIANT_TAG_OFFLOAD) {
            name += ",offload";
        }

        if (variant_flags & VARIANT_PROTO_V1) {
            name += ",proto_v1";
        }

        if (variant_flags & VARIANT_CONNECT_ALL_TO_ALL) {
            name += ",connect_all_to_all";
        }

        add_variant_with_value(variants, get_ctx_params(), variant_value, name);
    }

    static void get_test_variants(std::vector<ucp_test_variant> &variants)
    {
        int count = 0;
        for (int i = 0; i < VARIANT_MAX; i++) {
            for (const auto &mem_type_pair : m_mem_type_pairs) {
                add_mem_type_test_variant(variants, count, mem_type_pair[0],
                                          mem_type_pair[1]);
                ++count;
            }
        }
    }

    void do_basic_xfer(mem_buffer &send_buffer, mem_buffer &recv_buffer,
                       size_t length, ucs::detail::message_stream &ms)
    {
        const ucp_datatype_t type = ucp_dt_make_contig(1);

        ms << length << " " << std::flush;
        recv_buffer.pattern_fill(1, length);
        send_buffer.pattern_fill(2, length);
        size_t recvd = do_xfer(send_buffer.ptr(), recv_buffer.ptr(), length,
                               type, type, true, false, false);
        ASSERT_EQ(length, recvd);
        recv_buffer.pattern_check(2, length);
    }

    size_t max_test_length(unsigned exp) const
    {
        return static_cast<size_t>(pow(10.0, exp));
    }

    size_t test_length(unsigned exp) const
    {
        return (ucs::rand() % max_test_length(exp)) + 1;
    }

    static const
    std::vector<std::vector<ucs_memory_type_t> >& m_mem_type_pairs;

protected:

    size_t do_xfer(const void *sendbuf, void *recvbuf, size_t count,
                   ucp_datatype_t send_dt, ucp_datatype_t recv_dt,
                   bool expected, bool truncated, bool extended);

    ucs_memory_type_t m_send_mem_type;
    ucs_memory_type_t m_recv_mem_type;

private:

    static const uint64_t SENDER_TAG = 0x111337;
    static const uint64_t RECV_MASK  = 0xffff;
    static const uint64_t RECV_TAG   = 0x1337;
};

const std::vector<std::vector<ucs_memory_type_t> >&
test_ucp_tag_mem_type::m_mem_type_pairs = ucs::supported_mem_type_pairs();

size_t test_ucp_tag_mem_type::do_xfer(const void *sendbuf, void *recvbuf,
                                  size_t count, ucp_datatype_t send_dt,
                                  ucp_datatype_t recv_dt, bool expected,
                                  bool truncated, bool extended)
{
    size_t recv_count = count;
    size_t send_count = count;
    size_t recvd      = 0;
    request *rreq, *sreq;

    if (truncated) {
        recv_count /= 2;
    }

    if (extended) {
        send_count /= 2;
    }

    if (expected) {
        rreq = recv_nb(recvbuf, recv_count, recv_dt, RECV_TAG, RECV_MASK);
        sreq = send_nb(sendbuf, send_count, send_dt, SENDER_TAG);
    } else {
        sreq = send_nb(sendbuf, send_count, send_dt, SENDER_TAG);

        wait_for_unexpected_msg(receiver().worker(), 10.0);

        rreq = recv_nb(recvbuf, recv_count, recv_dt, RECV_TAG, RECV_MASK);
    }

    /* progress both sender and receiver */
    wait(rreq);
    if (sreq != NULL) {
        wait(sreq);
        request_free(sreq);
    }

    recvd = rreq->info.length;
    if (!truncated) {
        EXPECT_UCS_OK(rreq->status);
        EXPECT_EQ((ucp_tag_t)SENDER_TAG, rreq->info.sender_tag);
    } else {
        EXPECT_EQ(UCS_ERR_MESSAGE_TRUNCATED, rreq->status);
    }

    request_free(rreq);
    return recvd;
};

UCS_TEST_P(test_ucp_tag_mem_type, realloc_buffers)
{
    std::vector<size_t> sizes =
            {0, 1, 16, 128, 1048512, 1011439, UCS_MBYTE + 4, 4194324};
    const size_t max_iter     = RUNNING_ON_VALGRIND ? 3 : 7;
    const size_t multiplier   = RUNNING_ON_VALGRIND ? 2 : 1;
    for (unsigned i = 0; i < max_iter; ++i) {
        sizes.push_back((i * multiplier));
    }

    ucs::detail::message_stream ms("INFO");
    for (auto length : sizes) {
        mem_buffer recv_mem_buf(length, m_recv_mem_type);
        mem_buffer send_mem_buf(length, m_send_mem_type);
        do_basic_xfer(send_mem_buf, recv_mem_buf, length, ms);
    }
}

// Set NUM_PATHS to 2 to allow multi-rail
UCS_TEST_P(test_ucp_tag_mem_type, reuse_buffers_mrail, "IB_NUM_PATHS?=2")
{
    const size_t max_length = max_test_length(7);
    mem_buffer recv_mem_buf(max_length, m_recv_mem_type);
    mem_buffer send_mem_buf(max_length, m_send_mem_type);

    // Test few specific sizes that expose corner cases, plush a few random ones
    std::vector<size_t> sizes = {0, 1, 16, 128, 1048512, UCS_MBYTE + 4, 4194324};
    const size_t max_iter     = RUNNING_ON_VALGRIND ? 1 : 4;
    for (unsigned i = 0; i < max_iter; ++i) {
        sizes.push_back(test_length(7));
    }

    ucs::detail::message_stream ms("INFO");
    for (auto length : sizes) {
        do_basic_xfer(send_mem_buf, recv_mem_buf, length, ms);
    }
}

UCS_TEST_P(test_ucp_tag_mem_type, rndv_4mb, "RNDV_THRESH=0")
{
    ucp_datatype_t type = ucp_dt_make_contig(1);
    const size_t length = 4 * UCS_MBYTE;

    mem_buffer recv_mem_buf(length, m_recv_mem_type, 1);
    mem_buffer send_mem_buf(length, m_send_mem_type, 2);

    size_t recvd = do_xfer(send_mem_buf.ptr(), recv_mem_buf.ptr(), length, type,
                           type, true, false, false);
    ASSERT_EQ(length, recvd);

    recv_mem_buf.pattern_check(2);
}

UCS_TEST_P(test_ucp_tag_mem_type, xfer_mismatch_length)
{
    ucp_datatype_t type = ucp_dt_make_contig(1);
    size_t length       = test_length(7);

    UCS_TEST_MESSAGE << "TEST: "
                     << ucs_memory_type_names[m_send_mem_type] << " <-> "
                     << ucs_memory_type_names[m_recv_mem_type] << " length: "
                     << length;

    mem_buffer m_recv_mem_buf(length, m_recv_mem_type);
    mem_buffer m_send_mem_buf(length, m_send_mem_type);

    mem_buffer::pattern_fill(m_recv_mem_buf.ptr(), m_recv_mem_buf.size(),
                             1, m_recv_mem_buf.mem_type());

    mem_buffer::pattern_fill(m_send_mem_buf.ptr(), m_send_mem_buf.size(),
                             2, m_send_mem_buf.mem_type());

    /* truncated */
    do_xfer(m_send_mem_buf.ptr(), m_recv_mem_buf.ptr(),
            length, type, type, true, true, false);

    /* extended recv buffer */
    size_t recvd = do_xfer(m_send_mem_buf.ptr(), m_recv_mem_buf.ptr(),
                           length, type, type, true, false, true);
    ASSERT_EQ(length / 2,  recvd);

}


UCP_INSTANTIATE_TEST_CASE_GPU_AWARE(test_ucp_tag_mem_type);


/*
 * Rendezvous mtype flow control keeps the throttled requests on pending queues
 * which are shared by the whole worker, while the fragment quota is per
 * staging mpool, and a mpool is created per memory type and system device. A
 * released fragment wakes up only the head of the queue, even when that
 * request is waiting for a different mpool, and the wakeup is dropped when the
 * request is queued again. Transfers which stage through different mpools
 * therefore steal each other wakeups.
 */
class test_ucp_rndv_mtype_fc : public ucp_test {
public:
    static void get_test_variants(variant_vec_t &variants)
    {
        if (!mem_buffer::is_gpu_supported()) {
            return;
        }

        add_variant(variants, UCP_FEATURE_TAG);
    }

    void init() override
    {
        modify_config("RNDV_THRESH", "128");
        modify_config("RNDV_SCHEME", "put_ppln");
        /* Stage the fragments on the sender instead of using 2-stage
         * pipeline */
        modify_config("RNDV_PIPELINE_SHM_ENABLE", "n");
        modify_config("RNDV_PIPELINE_ERROR_HANDLING", "y");

        ucp_test::init();
        sender().connect(&receiver(), get_ep_params());
        receiver().connect(&sender(), get_ep_params());

        if (!is_proto_enabled() || !sender().is_rndv_put_ppln_supported()) {
            cleanup();
            UCS_TEST_SKIP_R("rndv mtype pipeline is not supported");
        }
    }

protected:
    /* Fragment size of both memory types, so that RNDV_FRAG_WORKER_MAX_MEM
     * gives a quota of one fragment per staging mpool */
    static const size_t FRAG_SIZE = 8 * UCS_KBYTE;
    static const size_t NUM_FRAGS = 16;
    /* Transfers posted while the other staging mpool is exhausted */
    static const size_t NUM_XFERS = 4;

    struct xfer {
        xfer(size_t size, uint64_t pattern, ucp_worker_h dst_worker) :
            src(size, UCS_MEMORY_TYPE_CUDA, pattern),
            dst(size, UCS_MEMORY_TYPE_CUDA), sreq(NULL), rreq(NULL),
            dst_worker(dst_worker), pattern(pattern)
        {
        }

        mem_buffer   src;
        mem_buffer   dst;
        void         *sreq;
        void         *rreq;
        ucp_worker_h dst_worker;
        uint64_t     pattern;
    };

    typedef std::vector<std::unique_ptr<xfer>>  xfer_vec_t;
    typedef std::vector<ucp_worker_mpool_key_t> mpool_key_vec_t;
    typedef std::vector<ucp_mem_desc_t*>        mdesc_vec_t;
    typedef std::map<ucs_memory_type_t, size_t> frag_mem_type_map_t;

    ucp_ep_params_t get_ep_params() override
    {
        ucp_ep_params_t ep_params = ucp_test::get_ep_params();

        /* Endpoint purge is the only way out of a lost fragment wakeup */
        ep_params.field_mask |= UCP_EP_PARAM_FIELD_ERR_HANDLING_MODE;
        ep_params.err_mode    = UCP_ERR_HANDLING_MODE_PEER;
        return ep_params;
    }

    void start_xfer(ucp_tag_t tag, size_t size, entity &src_entity,
                    entity &dst_entity, xfer_vec_t &xfers)
    {
        ucp_request_param_t param;

        param.op_attr_mask = 0;

        xfers.emplace_back(new xfer(size, tag, dst_entity.worker()));
        xfer &x = *xfers.back();

        x.rreq = ucp_tag_recv_nbx(dst_entity.worker(), x.dst.ptr(), x.dst.size(),
                                  tag, UINT64_MAX, &param);
        ASSERT_FALSE(UCS_PTR_IS_ERR(x.rreq));

        x.sreq = ucp_tag_send_nbx(src_entity.ep(), x.src.ptr(), x.src.size(),
                                  tag, &param);
        ASSERT_FALSE(UCS_PTR_IS_ERR(x.sreq));
    }

    static bool xfers_completed(const xfer_vec_t &xfers)
    {
        for (const auto &x : xfers) {
            for (void *req : {x->sreq, x->rreq}) {
                if ((req != NULL) &&
                    (ucp_request_check_status(req) == UCS_INPROGRESS)) {
                    return false;
                }
            }
        }

        return true;
    }

    void wait_xfers(const xfer_vec_t &xfers)
    {
        ucs_time_t deadline = ucs::get_deadline();

        while (!xfers_completed(xfers) && (ucs_get_time() < deadline)) {
            progress();
            m_max_pending = std::max(m_max_pending,
                                     fc_pending_length(sender().worker()) +
                                     fc_pending_length(receiver().worker()));
        }
    }

    /* Endpoint purge is what aborts the requests which wait for a fragment */
    void abort_xfers(xfer_vec_t &xfers)
    {
        scoped_log_handler err_handler(wrap_errors_logger);

        for (auto &x : xfers) {
            if ((x->rreq != NULL) &&
                (ucp_request_check_status(x->rreq) == UCS_INPROGRESS)) {
                ucp_request_cancel(x->dst_worker, x->rreq);
            }
        }

        sender().close_all_eps(*this, 0, UCP_EP_CLOSE_FLAG_FORCE);
        receiver().close_all_eps(*this, 0, UCP_EP_CLOSE_FLAG_FORCE);
        wait_xfers(xfers);
    }

    void free_xfers(xfer_vec_t &xfers)
    {
        for (auto &x : xfers) {
            for (void *req : {x->sreq, x->rreq}) {
                if (req != NULL) {
                    ucp_request_free(req);
                }
            }
        }

        xfers.clear();
    }

    /* Keys of the staging mpools which exist on the worker */
    static mpool_key_vec_t frag_mpools(ucp_worker_h worker)
    {
        mpool_key_vec_t keys;

        for (khiter_t iter = kh_begin(&worker->mpool_hash);
             iter != kh_end(&worker->mpool_hash); ++iter) {
            if (kh_exist(&worker->mpool_hash, iter)) {
                keys.push_back(kh_key(&worker->mpool_hash, iter));
            }
        }

        return keys;
    }

    static bool
    has_frag_mpool(const mpool_key_vec_t &keys, ucp_worker_mpool_key_t key)
    {
        for (const auto &elem : keys) {
            if ((elem.mem_type == key.mem_type) &&
                (elem.sys_dev == key.sys_dev)) {
                return true;
            }
        }

        return false;
    }

    static size_t fc_pending_length(ucp_worker_h worker)
    {
        size_t length = 0;

        for (unsigned i = 0; i < UCP_WORKER_RNDV_FC_OP_LAST; ++i) {
            length += ucs_queue_length(&worker->rndv_mtype_fc.pending_q[i]);
        }

        return length;
    }

    /*
     * Take all the fragments of the staging mpools of a given memory type, to
     * emulate fragments which are held by transfers that cannot release them
     * before the peer makes progress.
     */
    static mdesc_vec_t
    hold_frags(ucp_worker_h worker, ucs_memory_type_t mem_type)
    {
        mdesc_vec_t held;
        ucp_mem_desc_t *mdesc;

        for (khiter_t iter = kh_begin(&worker->mpool_hash);
             iter != kh_end(&worker->mpool_hash); ++iter) {
            if (!kh_exist(&worker->mpool_hash, iter) ||
                (kh_key(&worker->mpool_hash, iter).mem_type != mem_type)) {
                continue;
            }

            /* Terminates only because the mpool quota is finite, which
             * requires proto v2 */
            while ((mdesc = static_cast<ucp_mem_desc_t*>(ucs_mpool_get_inline(
                                    &kh_val(&worker->mpool_hash, iter)))) !=
                   NULL) {
                held.push_back(mdesc);
            }
        }

        return held;
    }

    static void release_frags(mdesc_vec_t &held)
    {
        /* Return the fragments without rescheduling throttled requests, which
         * must have been purged by now */
        for (auto *mdesc : held) {
            ucs_mpool_put_inline(mdesc);
        }

        held.clear();
    }

    /*
     * Send one message per size and report which message size created each
     * staging mpool on the receiver worker. Protocol selection picks the
     * fragment memory type, so the mapping is discovered instead of assumed.
     */
    frag_mem_type_map_t probe_frag_mem_types()
    {
        static const size_t sizes[] = {FRAG_SIZE / 4, FRAG_SIZE, FRAG_SIZE * 4,
                                       FRAG_SIZE * NUM_FRAGS};
        ucp_worker_h worker = receiver().worker();
        frag_mem_type_map_t frag_mem_types;
        ucp_tag_t tag = 0;

        for (size_t size : sizes) {
            mpool_key_vec_t existing = frag_mpools(worker);
            xfer_vec_t xfers;

            start_xfer(++tag, size, sender(), receiver(), xfers);
            wait_xfers(xfers);
            if (!xfers_completed(xfers)) {
                abort_xfers(xfers);
                free_xfers(xfers);
                UCS_TEST_ABORT("transfer of " << size << " bytes is stuck");
            }

            free_xfers(xfers);

            for (const auto &key : frag_mpools(worker)) {
                if (!has_frag_mpool(existing, key)) {
                    frag_mem_types.insert({key.mem_type, size});
                }
            }
        }

        return frag_mem_types;
    }

    /*
     * All the transfers are posted by one peer, so the receiver worker stages
     * only RTR fragments and PUT and RTR requests never compete for the same
     * mpool. One staging mpool is kept exhausted, and the transfers which
     * stage through the other mpool must still complete.
     */
    void busy_mpool_test(ucs_memory_type_t busy_mem_type,
                         ucs_memory_type_t xfer_mem_type)
    {
        frag_mem_type_map_t frag_mem_types = probe_frag_mem_types();
        ucp_worker_h worker                = receiver().worker();
        xfer_vec_t xfers, busy_xfers;
        mdesc_vec_t held;
        ucp_tag_t tag = 100;

        if ((frag_mem_types.count(busy_mem_type) == 0) ||
            (frag_mem_types.count(xfer_mem_type) == 0)) {
            UCS_TEST_SKIP_R(
                    std::string("transfers do not stage through both ") +
                    ucs_memory_type_names[busy_mem_type] + " and " +
                    ucs_memory_type_names[xfer_mem_type] + " fragments");
        }

        held = hold_frags(worker, busy_mem_type);
        if (held.empty()) {
            UCS_TEST_SKIP_R("staging mpool quota is not limited");
        }

        /* This receive is throttled on the exhausted mpool, and stays at the
         * head of the pending queue */
        start_xfer(++tag, frag_mem_types[busy_mem_type], sender(), receiver(),
                   busy_xfers);
        wait_for_cond([worker]() { return fc_pending_length(worker) > 0; },
                      [this]() { progress(); });

        const size_t throttled = fc_pending_length(worker);
        EXPECT_EQ(1ul, throttled)
                << "receive of " << frag_mem_types[busy_mem_type]
                << " bytes is not throttled on the "
                << ucs_memory_type_names[busy_mem_type] << " mpool";

        /* These receives stage through the other mpool, and are queued behind
         * the throttled request while its single fragment is taken */
        for (size_t i = 0; i < NUM_XFERS; ++i) {
            start_xfer(++tag, frag_mem_types[xfer_mem_type], sender(),
                       receiver(), xfers);
        }

        wait_xfers(xfers);

        const bool completed = xfers_completed(xfers);
        const size_t pending = fc_pending_length(worker);

        if (completed) {
            for (const auto &x : xfers) {
                x->dst.pattern_check(x->pattern);
            }
        }

        /* The throttled receive completes only when the endpoint is purged */
        for (auto &x : xfers) {
            busy_xfers.push_back(std::move(x));
        }

        xfers.clear();
        abort_xfers(busy_xfers);
        free_xfers(busy_xfers);
        release_frags(held);

        UCS_TEST_MESSAGE << "busy " << ucs_memory_type_names[busy_mem_type]
                         << " size " << frag_mem_types[busy_mem_type]
                         << ", transfer "
                         << ucs_memory_type_names[xfer_mem_type] << " size "
                         << frag_mem_types[xfer_mem_type] << ", max pending "
                         << m_max_pending;

        EXPECT_TRUE(completed)
                << pending << " requests are still waiting for a fragment";
    }

    size_t m_max_pending{0};
};

#define UCP_RNDV_MTYPE_FC_CONFIG \
    "RNDV_FRAG_MEM_TYPES=host,cuda", "RNDV_FRAG_SIZE=host:8K,cuda:8K", \
            "RNDV_FRAG_ALLOC_COUNT=host:1,cuda:1", \
            "RNDV_FRAG_WORKER_MAX_MEM=8K"

/* Report which message size stages through which fragment memory type */
UCS_TEST_P(test_ucp_rndv_mtype_fc, frag_mem_type_by_size,
           UCP_RNDV_MTYPE_FC_CONFIG)
{
    for (const auto &elem : probe_frag_mem_types()) {
        UCS_TEST_MESSAGE << "frag " << ucs_memory_type_names[elem.first]
                         << " is first used by message size " << elem.second;
    }
}

UCS_TEST_P(test_ucp_rndv_mtype_fc, one_sided_busy_cuda_mpool,
           UCP_RNDV_MTYPE_FC_CONFIG)
{
    busy_mpool_test(UCS_MEMORY_TYPE_CUDA, UCS_MEMORY_TYPE_HOST);
}

UCS_TEST_P(test_ucp_rndv_mtype_fc, one_sided_busy_host_mpool,
           UCP_RNDV_MTYPE_FC_CONFIG)
{
    busy_mpool_test(UCS_MEMORY_TYPE_HOST, UCS_MEMORY_TYPE_CUDA);
}

UCP_INSTANTIATE_TEST_CASE_GPU_AWARE(test_ucp_rndv_mtype_fc);
