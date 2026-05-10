/* -*- C -*-
 *
 * Copyright (c) 2022 Intel Corporation. All rights reserved.
 *
 * Copyright (c) 2022 Cornelis Networks, Inc. All rights reserved.
 *
 * This software is available to you under the BSD license.
 *
 * This file is part of the Sandia OpenSHMEM software package. For license
 * information, see the LICENSE file in the top level directory of the
 * distribution.
 *
 */

#include "config.h"

#ifdef HAVE_SYS_GETTID
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <sys/syscall.h>
#endif

#include <errno.h>
#include <stdlib.h>
#include <sys/time.h>
#include <sys/param.h>
#include <stdio.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <inttypes.h>
#include <netdb.h>
#include <rdma/fabric.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#if HAVE_FNMATCH_H
#include <fnmatch.h>
#else
#define fnmatch(P, S, F) strcmp(P, S)
#endif

#ifdef USE_HWLOC
#include <hwloc.h>
#endif

#define SHMEM_INTERNAL_INCLUDE
#include "shmem.h"
#include "shmem_internal.h"
#include "shmem_comm.h"
#include "transport_ofi.h"
#include <unistd.h>
#include "runtime.h"
#include "uthash.h"

struct fabric_info {
    struct fi_info *fabrics;
    struct fi_info *p_info;
    char *prov_name;
    char *fabric_name;
    char *domain_name;
    int npes;
};

struct fid_fabric*              shmem_transport_ofi_fabfd;
struct fid_domain*              shmem_transport_ofi_domainfd;
struct fid_av*                  shmem_transport_ofi_avfd;
struct fid_av_set*              shmem_transport_ofi_avset;
struct fid_mc                   *coll_mc = NULL;
fi_addr_t                       shmem_transport_ofi_world_addr;
fi_addr_t                       shmem_transport_ofi_coll_addr;

struct fid_ep*                  shmem_transport_ofi_target_ep;
struct fid_cq*                  shmem_transport_ofi_target_cq;


static struct fabric_info              shmem_transport_ofi_CXI_info = {0};
struct fid_fabric*              shmem_transport_ofi_CXI_fabfd;
struct fid_domain*              shmem_transport_ofi_CXI_domain_fd;
struct fid_av*                  shmem_transport_ofi_CXI_avfd;
struct fid_av_set*              shmem_transport_ofi_CXI_avfd_set;
fi_addr_t                       shmem_transport_ofi_CXI_world_addr;
fi_addr_t                       shmem_transport_ofi_CXI_coll_addr;
fi_addr_t                       shmem_transport_ofi_CXI_my_addr;
fi_addr_t                       *shmem_transport_ofi_CXI_addr_table;
struct fid_ep                   *shmem_transport_ofi_CXI_target_ep;
struct fid_cq                   *shmem_transport_ofi_CXI_target_cq;
struct fid_eq                   *shmem_transport_ofi_CXI_eq;
struct fid_cq                   *shmem_transport_ofi_CXI_recv_cq;
struct fid_mc                   *ofi_coll_mc = NULL;
struct fi_cxi_dom_ops           *cxi_dom_ops = NULL;

struct fid_cntr                 *coll_send_cntr,
                                *coll_recv_cntr,
                                *coll_read_cntr,
                                *coll_write_cntr,
                                *coll_rem_cntr;


#if ENABLE_TARGET_CNTR
struct fid_cntr*                shmem_transport_ofi_target_cntrfd;
#endif
#ifdef ENABLE_MR_SCALABLE
#ifdef ENABLE_REMOTE_VIRTUAL_ADDRESSING
struct fid_mr*                  shmem_transport_ofi_target_mrfd;
#else  /* !ENABLE_REMOTE_VIRTUAL_ADDRESSING */
struct fid_mr*                  shmem_transport_ofi_target_heap_mrfd;
struct fid_mr*                  shmem_transport_ofi_target_data_mrfd;
#endif
#else  /* !ENABLE_MR_SCALABLE */
struct fid_mr*                  shmem_transport_ofi_target_heap_mrfd;
struct fid_mr*                  shmem_transport_ofi_target_data_mrfd;
uint64_t*                       shmem_transport_ofi_target_heap_keys;
uint64_t*                       shmem_transport_ofi_target_data_keys;
#ifdef ENABLE_REMOTE_VIRTUAL_ADDRESSING
int                             shmem_transport_ofi_use_absolute_address;
#else
uint8_t**                       shmem_transport_ofi_target_heap_addrs;
uint8_t**                       shmem_transport_ofi_target_data_addrs;
#endif /* ENABLE_REMOTE_VIRTUAL_ADDRESSING */
#endif /* ENABLE_MR_SCALABLE */

#ifdef USE_FI_HMEM
struct fid_mr*                  shmem_transport_ofi_external_heap_mrfd;
uint64_t*                       shmem_transport_ofi_external_heap_keys;
uint8_t**                       shmem_transport_ofi_external_heap_addrs;
#endif




static ssize_t copy_from_hmem_iov(void *dest, size_t size,
        enum fi_hmem_iface iface, uint64_t device,
        const struct iovec *hmem_iov,
        size_t hmem_iov_count,
        uint64_t hmem_iov_offset)
{
    size_t cpy_size = MIN(size, hmem_iov->iov_len);

    assert(iface == FI_HMEM_SYSTEM);
    assert(hmem_iov_count == 1);
    assert(hmem_iov_offset == 0);

    memcpy(dest, hmem_iov->iov_base, cpy_size);

    return cpy_size;
}

static ssize_t copy_to_hmem_iov(enum fi_hmem_iface iface, uint64_t device,
        const struct iovec *hmem_iov,
        size_t hmem_iov_count,
        uint64_t hmem_iov_offset, const void *src,
        size_t size)
{
    size_t cpy_size = MIN(size, hmem_iov->iov_len);

    assert(iface == FI_HMEM_SYSTEM);
    assert(hmem_iov_count == 1);
    assert(hmem_iov_offset == 0);

    memcpy(hmem_iov->iov_base, src, cpy_size);

    return cpy_size;
}

struct fi_hmem_override_ops cxit_hmem_ops = {
    .copy_from_hmem_iov = copy_from_hmem_iov,
    .copy_to_hmem_iov = copy_to_hmem_iov,
};

/* List of MR descriptors: current support is for heap, data, and one external heap */
struct fid_mr*                  shmem_transport_ofi_mrfd_list[3];
uint64_t                        shmem_transport_ofi_max_poll;
long                            shmem_transport_ofi_put_poll_limit;
long                            shmem_transport_ofi_get_poll_limit;
size_t                          shmem_transport_ofi_max_buffered_send;
size_t                          shmem_transport_ofi_max_msg_size;
size_t                          shmem_transport_ofi_bounce_buffer_size;
long                            shmem_transport_ofi_max_bounce_buffers;
size_t                          shmem_transport_ofi_addrlen;
#ifdef ENABLE_MR_RMA_EVENT
int                             shmem_transport_ofi_mr_rma_event;
#endif
fi_addr_t                       *addr_table;
#ifdef ENABLE_THREADS
shmem_internal_mutex_t          shmem_transport_ofi_lock;
pthread_mutex_t                 shmem_transport_ofi_progress_lock = PTHREAD_MUTEX_INITIALIZER;
#endif /* ENABLE_THREADS */

int shmem_transport_ofi_single_ep;

/* Temporarily redefine SHM_INTERNAL integer types to their FI counterparts to
 * translate the DTYPE_* types (defined by autoconf according to system ABI)
 * into FI types in the table below */
#define SHM_INTERNAL_INT8   FI_INT8
#define SHM_INTERNAL_INT16  FI_INT16
#define SHM_INTERNAL_INT32  FI_INT32
#define SHM_INTERNAL_INT64  FI_INT64
#define SHM_INTERNAL_UINT8  FI_UINT8
#define SHM_INTERNAL_UINT16 FI_UINT16
#define SHM_INTERNAL_UINT32 FI_UINT32
#define SHM_INTERNAL_UINT64 FI_UINT64


static void avset_ary_destroy(struct avset_ary *setary)
{
    int i;

    if (setary->avset) {
        for (i = 0; i < setary->avset_cnt; i++)
            fi_close(&setary->avset[i]->fid);
        free(setary->avset);
    }
    avset_ary_init(setary);
}

static int avset_ary_append(fi_addr_t *fiaddrs, size_t size,
        int mcast_addr, int root_idx,
        struct avset_ary *setary, int start, int stride)
{
    struct cxip_comm_key comm_key = {
        .keytype = COMM_KEY_UNICAST,
        .ucast.mcast_addr = mcast_addr,
        .ucast.hwroot_idx = root_idx
    };
    struct fi_av_set_attr attr = {
        .count = start,
        .start_addr = FI_ADDR_NOTAVAIL,
        .end_addr = FI_ADDR_NOTAVAIL,
        .stride = stride,
        .comm_key_size = sizeof(comm_key),
        .comm_key = (void *)&comm_key,
        .flags = 0,
    };
    struct fid_av_set *setp;
    int i, ret;

    PRINT_DEBUG("cnt %d size %d\n", setary->avset_cnt, setary->avset_siz);
    if (setary->avset_siz <= setary->avset_cnt) {
        void *ptr;
        int siz;

        PRINT_DEBUG("%s expand setary\n", __func__);
        siz = setary->avset_siz + 4;
        ptr = realloc(setary->avset, siz * sizeof(void *));
        if (!ptr) {
            PRINT_ERROR("%s realloc failed\n", __func__);
            ret = -FI_ENOMEM;
            goto quit;
        }
        setary->avset_siz = siz;
        setary->avset = ptr;
    }

    PRINT_DEBUG("Starting fi_av_set with avfd %p\n", shmem_transport_ofi_avfd);

    ret = fi_av_set(shmem_transport_ofi_CXI_avfd, &attr, &setp, NULL);
    if (ret) {
        PRINT_ERROR("%s fi_av_set failed %d\n", __func__, ret);
        goto quit;
    }
    if (setp == NULL){
        PRINT_ERROR("setp %p is NULL\n", setp);
        ret = -FI_EINVAL;
        goto quit;
    }
    PRINT_DEBUG("av_set returned: %p\n", setp);

    for (i = start; i < size; i+= stride) {
        PRINT_DEBUG("Inserting at index %d of %lu into setp %p at addr 0x%lx\n", i, size, setp, fiaddrs[i]);
        ret = fi_av_set_insert(setp, fiaddrs[i]);
        if (ret) {
            PRINT_ERROR("%s fi_av_set_insert failed %d\n", __func__, ret);
            goto quit;
        }
    }
    // add to expanded list
    setary->avset[setary->avset_cnt++] = setp;
    PRINT_DEBUG("avset_cnt %d, setary->avset[setary->avset_cnt-1] = %p\n", setary->avset_cnt, setary->avset[setary->avset_cnt-1]);
    return 0;

quit:
    PRINT_ERROR("%s: FAILED %d %s\n", __func__, ret, fi_strerror(ret));
    if (setp) {
        fi_close(&setp->fid);
        free(setp);
    }
    return ret;
}

static int eq_poll(shmem_transport_ctx_t *ctx){
    int ret = 0;
    struct fid_eq *eq;
    struct fi_eq_err_entry eqd = {};
    join_item_t *jctx = NULL;
    uint32_t event = 0;

    eq = ctx->eq;

    jctx = NULL;
    ret = fi_eq_read(eq, &event, &eqd, sizeof(eqd), 0);
    if (ret == -FI_EAGAIN){
        return -FI_EAGAIN;
    }

    if (ret >= 0){
        if (ret < sizeof(struct fi_eq_entry)) { 
            PRINT_ERROR("Too small: %d versus %lu\n",
                    ret, sizeof(struct fi_eq_entry));
            return -FI_EINVAL;
        }
        if ( (!eqd.context) || (event != FI_JOIN_COMPLETE)){
            PRINT_ERROR("Unexpected eqd response\n");
            return -FI_EINVAL;
        }
        jctx = eqd.context;
        PRINT_DEBUG("Simple response: jctx/context = %p\n", jctx);
        jctx->retval = 0;
        jctx->prov_errno = 0;
        return FI_SUCCESS;
    }
    if (ret == -FI_EAVAIL){
        ret = fi_eq_readerr(eq, &eqd, 0);
        if (ret < sizeof(struct fi_eq_entry)) { 
            PRINT_ERROR("Too small: %d versus %lu\n",
                    ret, sizeof(struct fi_eq_entry));
            return -FI_EINVAL;
        }

        if (!eqd.context){
            PRINT_ERROR("Unexpected eqd response\n");
            return -FI_EINVAL;
        } 

        jctx = eqd.context;
        PRINT_DEBUG("Round 2 response: jctx/context = %p\n", jctx);
        jctx->retval = eqd.err;
        jctx->prov_errno = eqd.prov_errno;
        return FI_SUCCESS;
    }
    return 0;
}


static void *cq_poll(shmem_transport_ctx_t *ctx){
    struct fi_cq_err_entry cq_err = {};
    ssize_t size = 0;

    /* Poll once instead of polling per operation */
    size = fi_cq_read(ctx->rx_cq, &cq_err, 1);
    if (size == -FI_EAVAIL)
        size = fi_cq_readerr(ctx->rx_cq, &cq_err, 1);
    if (size > 0){
        PRINT_DEBUG("rx_cq Success, returning context %p\n", cq_err.op_context);
        return cq_err.op_context;
    }

    size = fi_cq_read(ctx->coll_tx_cq, &cq_err, 1);
    if (size == -FI_EAVAIL)
        size = fi_cq_readerr(ctx->coll_tx_cq, &cq_err, 1);
    if (size > 0){
        PRINT_DEBUG("coll_tx_cq Success, returning context %p\n", cq_err.op_context);
        return cq_err.op_context; 
    }

//    PRINT_DEBUG("Returning NULL\n");
    return NULL;
}


static void cq_wait(shmem_transport_ctx_t *ctx, void *pcontext){
    do {
        if (pcontext == cq_poll(ctx))
            break;
    } while(true);
}

static int coll_multi_join(shmem_transport_ctx_t *ctx, struct avset_ary *setary, struct d_entry *joinlist,
        int limit)
{
    struct join_item *jctx;
    int i, ret = FI_SUCCESS, total, count;
 //   fi_addr_t local_world_addr;

    PRINT_DEBUG("ENTRY %s\n", __func__);
    total = setary->avset_cnt;
    count = 0;
    PRINT_DEBUG("total join count: %d\n", total);
    struct fid_ep *ep = ctx->CXI_ep; 

    for (i = 0; i < total; i++) {
        jctx = calloc(1, sizeof(*jctx));
        if (!jctx) {
            PRINT_ERROR("calloc failed on jctx[%d]\n", i);
            ret = -FI_ENOMEM;
            goto fail;
        }
        d_init(&jctx->entry);
        jctx->join_index = i;
        jctx->avset = setary->avset[i];
        PRINT_DEBUG("join %d of %d initiating with ep %p, avset %p, Pointer mc entry %p, jctx %p, FI_ADDR_NOTAVAIL %ld\n", 
                i, total,
                ep, 
                setary->avset[i], &(jctx->mc), jctx, FI_ADDR_NOTAVAIL);
        ret = fi_join_collective(ep, FI_ADDR_NOTAVAIL,
                setary->avset[i], 0L, &jctx->mc, jctx);

        PRINT_DEBUG("mc after join: 0x%lx\n", jctx->mc);

        if (ret == -FI_ECONNREFUSED) {
            free(jctx);
            continue;
        }
        if (ret != FI_SUCCESS) {
            PRINT_ERROR("join %d FAILED join %d, %s\n", i, ret, fi_strerror(ret));
            free(jctx);
            goto fail;
        }
        PRINT_DEBUG("Actual join succeeded, polling time\n");

        do {
            cq_poll(ctx);
            ret = eq_poll(ctx);
        } while (ret == -FI_EAGAIN);

        OFI_CHECK_RETURN_STR(ret, "Failed to poll EQ\n");

        d_insert_tail(&jctx->entry, joinlist);
        count++;
    }

    PRINT_DEBUG("DONE %s completed %d joins\n", __func__, count);
    return FI_SUCCESS;

fail:
    PRINT_ERROR("MULTIJOIN failed\n");
    //coll_multi_release(joinlist);
    return ret;
}



static struct join_item *coll_single_join(shmem_transport_ctx_t *ctx, fi_addr_t *fiaddrs, size_t size,
        int mcast_addr, int root_idx,
        int exp_retval, int exp_prov_errno,
        struct avset_ary *setary,
        struct d_entry *joinlist,
        const char *msg, int stride, int start)
{
    struct join_item *jctx = NULL;
    int ret;

    avset_ary_init(setary);
    ret = avset_ary_append(fiaddrs, size, mcast_addr, root_idx, setary, stride, start);
    if (ret) {
        PRINT_ERROR("%s JOIN avset_ary_append()=%d\n", msg, ret);
        goto quit;
    }

    d_init(joinlist);
    ret = coll_multi_join(ctx, setary, joinlist, -1);
    if (ret < 0) {
        PRINT_ERROR("%s JOIN coll_multi_join()=%d\n", msg, ret);
        goto quit;
    }

    jctx = d_first_entry_or_null(joinlist, struct join_item, entry);
    if (!jctx) {
        PRINT_ERROR("%s JOIN produced NULL result\n", msg);
        goto quit;
    }

    if (jctx->retval != exp_retval || jctx->prov_errno != exp_prov_errno) {
        PRINT_ERROR("%s JOIN ret=%d,exp=%d prov_errno=%d,exp=%d\n", msg,
                jctx->retval, exp_retval,
                jctx->prov_errno, exp_prov_errno);
        goto quit;
    }

    return jctx;
quit:
    return NULL;
}

static int _simple_join(shmem_transport_ctx_t *ctx, fi_addr_t *fiaddrs, size_t size,
        struct avset_ary *setary,
        struct d_entry *joinlist, int stride, int start)
{
    int ret;

    avset_ary_init(setary);
    ret = avset_ary_append(fiaddrs, size, 0, 1, setary, start, stride);
    OFI_CHECK_RETURN_STR(ret, "Failed to add to the avset\n");
    if (ret)
        return ret;

    d_init(joinlist);
    ret = coll_multi_join(ctx, setary, joinlist, -1);

    OFI_CHECK_RETURN_STR(ret, "Failed to perform a join\n");
    if (ret < 0)
        return ret;

    return 0;
}

static uint64_t _simple_get_mc(struct d_entry *joinlist)
{
    struct join_item *jctx;

    jctx = d_first_entry_or_null(joinlist, struct join_item, entry);
    if (jctx == NULL) {
        PRINT_ERROR("Join item is NULL\n");
        return 0;
    }
    PRINT_DEBUG("jctx->mc: 0x%lx\n", (uint64_t)jctx->mc);
    return (uint64_t)jctx->mc;
}


/* Taken from transport_ofi.h and putting it here */

int shmem_transport_dtype_table[] = {
    FI_INT8,                  /* SHM_INTERNAL_SIGNED_BYTE    */
    DTYPE_CHAR,               /* SHM_INTERNAL_CHAR           */
    DTYPE_SIGNED_CHAR,        /* SHM_INTERNAL_SCHAR           */
    DTYPE_SHORT,              /* SHM_INTERNAL_SHORT          */
    DTYPE_INT,                /* SHM_INTERNAL_INT            */
    DTYPE_LONG,               /* SHM_INTERNAL_LONG           */
    DTYPE_LONG_LONG,          /* SHM_INTERNAL_LONG_LONG      */
    DTYPE_FORTRAN_INTEGER,    /* SHM_INTERNAL_FORTRAN_INT    */
    FI_INT8,                  /* SHM_INTERNAL_INT8           */
    FI_INT16,                 /* SHM_INTERNAL_INT16          */
    FI_INT32,                 /* SHM_INTERNAL_INT32          */
    FI_INT64,                 /* SHM_INTERNAL_INT64          */
    DTYPE_PTRDIFF_T,          /* SHM_INTERNAL_PTRDIFF_T      */
    DTYPE_UNSIGNED_CHAR,      /* SHM_INTERNAL_UCHAR          */
    DTYPE_UNSIGNED_SHORT,     /* SHM_INTERNAL_USHORT         */
    DTYPE_UNSIGNED_INT,       /* SHM_INTERNAL_UINT           */
    DTYPE_UNSIGNED_LONG,      /* SHM_INTERNAL_ULONG          */
    DTYPE_UNSIGNED_LONG_LONG, /* SHM_INTERNAL_ULONG_LONG     */
    FI_UINT8,                 /* SHM_INTERNAL_UINT8          */
    FI_UINT16,                /* SHM_INTERNAL_UINT16         */
    FI_UINT32,                /* SHM_INTERNAL_UINT32         */
    FI_UINT64,                /* SHM_INTERNAL_UINT64         */
    DTYPE_SIZE_T,             /* SHM_INTERNAL_SIZE_T         */
    FI_FLOAT,                 /* SHM_INTERNAL_FLOAT          */
    FI_DOUBLE,                /* SHM_INTERNAL_DOUBLE         */
    FI_LONG_DOUBLE,           /* SHM_INTERNAL_LONG_DOUBLE    */
    FI_FLOAT_COMPLEX,         /* SHM_INTERNAL_FLOAT_COMPLEX  */
    FI_DOUBLE_COMPLEX         /* SHM_INTERNAL_DOUBLE_COMPLEX */
};

#undef SHM_INTERNAL_INT8
#undef SHM_INTERNAL_INT16
#undef SHM_INTERNAL_INT32
#undef SHM_INTERNAL_INT64
#undef SHM_INTERNAL_UINT8
#undef SHM_INTERNAL_UINT16
#undef SHM_INTERNAL_UINT32
#undef SHM_INTERNAL_UINT64

/* Need a syscall to gettid() because glibc doesn't provide a wrapper
 * (see gettid manpage in the NOTES section): */
static inline
struct shmem_internal_tid shmem_transport_ofi_gettid(void)
{
    struct shmem_internal_tid tid;
    memset(&tid, 0, sizeof(struct shmem_internal_tid));

    if (shmem_internal_gettid_fn) {
        tid.tid_t = tid_is_uint64_t;
        tid.val.uint64_val = (*shmem_internal_gettid_fn)();
    } else {
#ifndef __APPLE__
#ifdef HAVE_SYS_GETTID
        tid.tid_t = tid_is_pid_t;
        tid.val.pid_val = syscall(SYS_gettid);
#else
        /* Cannot query the tid with a syscall, so instead assume each tid
         * query corresponds to a unique thread. */
        tid.tid_t = tid_is_uint64_t;
        static uint64_t tid_val = 0;
        static int tid_cnt_start = 0;
        if (!tid_cnt_start)
            tid_cnt_start = 1;
        else
            tid_val++;
        tid.val.uint64_val = tid_val;
#endif /* HAVE_SYS_GETTID */
#else
        tid.tid_t = tid_is_uint64_t;
        int ret;
        ret = pthread_threadid_np(NULL, &tid.val.uint64_val);
        if (ret != 0)
            RAISE_ERROR_MSG("Error getting thread ID: %s\n", strerror(ret));
#endif /* APPLE */
    }
    return tid;
}

#define SHMEM_TRANSPORT_OFI_PROV_SOCKETS "sockets"

static struct fabric_info shmem_transport_ofi_info = {0};

static size_t shmem_transport_ofi_grow_size = 128;

#define SHMEM_TRANSPORT_CTX_DEFAULT_ID -1
shmem_transport_ctx_t shmem_transport_ctx_default;
shmem_ctx_t SHMEM_CTX_DEFAULT = (shmem_ctx_t) &shmem_transport_ctx_default;

size_t SHMEM_Dtsize[FI_DATATYPE_LAST];

static char * SHMEM_DtName[FI_DATATYPE_LAST];
static char * SHMEM_OpName[FI_ATOMIC_OP_LAST];

// Corresponds to Table 5 in the OpenSHMEM standard
#define OSHMEM_STANDARD_len 24

typedef struct shmem_coll_types {
    const char *type;
    enum fi_datatype match;
    int size;
} shmem_coll_types_t;

static shmem_coll_types_t coll_type_arr[] = { 
    { "float", FI_FLOAT, sizeof(float) },
    { "double", FI_DOUBLE, sizeof(double) },
    { "longdouble", FI_LONG_DOUBLE, sizeof (long double) },
    { "char", FI_INT8, sizeof(char) },
    { "schar", FI_INT8, sizeof(signed char) },
    { "short", FI_INT16, sizeof(short) },
    { "int", FI_INT32, sizeof(int) },
    { "long", FI_INT32, sizeof(long) },
    { "longlong", FI_INT64, sizeof(long long) },
    { "uchar", FI_UINT8, sizeof(unsigned char) },
    { "ushort", FI_UINT16, sizeof(unsigned short) },
    { "uint", FI_UINT32, sizeof(unsigned int) },
    { "ulong", FI_UINT32, sizeof(unsigned long) },
    { "ulonglong", FI_UINT32, sizeof(unsigned long long) },
    { "int8", FI_INT8, sizeof(int8_t) },
    { "int16", FI_INT16, sizeof(int16_t) },
    { "int32", FI_INT32, sizeof(int32_t) },
    { "int64", FI_INT64, sizeof(int64_t) },
    { "uint8", FI_UINT8, sizeof(uint8_t) },
    { "uint16", FI_UINT16, sizeof(uint16_t) },
    { "uint32", FI_UINT32, sizeof(uint32_t) },
    { "uint64", FI_UINT64, sizeof(uint64_t) },
    { "size", FI_UINT64, sizeof(size_t) },
    { "ptrdiff", FI_INT64, sizeof(ptrdiff_t) }
};




static inline void init_ofi_tables(void)
{
    SHMEM_Dtsize[FI_INT8]                = sizeof(int8_t);
    SHMEM_Dtsize[FI_UINT8]               = sizeof(uint8_t);
    SHMEM_Dtsize[FI_INT16]               = sizeof(int16_t);
    SHMEM_Dtsize[FI_UINT16]              = sizeof(uint16_t);
    SHMEM_Dtsize[FI_INT32]               = sizeof(int32_t);
    SHMEM_Dtsize[FI_UINT32]              = sizeof(uint32_t);
    SHMEM_Dtsize[FI_INT64]               = sizeof(int64_t);
    SHMEM_Dtsize[FI_UINT64]              = sizeof(uint64_t);
    SHMEM_Dtsize[FI_FLOAT]               = sizeof(float);
    SHMEM_Dtsize[FI_DOUBLE]              = sizeof(double);
    SHMEM_Dtsize[FI_FLOAT_COMPLEX]       = sizeof(float _Complex);
    SHMEM_Dtsize[FI_DOUBLE_COMPLEX]      = sizeof(double _Complex);
    SHMEM_Dtsize[FI_LONG_DOUBLE]         = sizeof(long double);
    SHMEM_Dtsize[FI_LONG_DOUBLE_COMPLEX] = sizeof(long double _Complex);

    SHMEM_DtName[FI_INT8]                = "int8";
    SHMEM_DtName[FI_UINT8]               = "uint8";
    SHMEM_DtName[FI_INT16]               = "int16";
    SHMEM_DtName[FI_UINT16]              = "uint16";
    SHMEM_DtName[FI_INT32]               = "int32";
    SHMEM_DtName[FI_UINT32]              = "uint32";
    SHMEM_DtName[FI_INT64]               = "int64";
    SHMEM_DtName[FI_UINT64]              = "uint64";
    SHMEM_DtName[FI_FLOAT]               = "float";
    SHMEM_DtName[FI_DOUBLE]              = "double";
    SHMEM_DtName[FI_FLOAT_COMPLEX]       = "float _Complex";
    SHMEM_DtName[FI_DOUBLE_COMPLEX]      = "double _Complex";
    SHMEM_DtName[FI_LONG_DOUBLE]         = "long double";
    SHMEM_DtName[FI_LONG_DOUBLE_COMPLEX] = "long double _Complex";

    SHMEM_OpName[FI_MIN]                 = "MIN";
    SHMEM_OpName[FI_MAX]                 = "MAX";
    SHMEM_OpName[FI_SUM]                 = "SUM";
    SHMEM_OpName[FI_PROD]                = "PROD";
    SHMEM_OpName[FI_LOR]                 = "LOR";
    SHMEM_OpName[FI_LAND]                = "LAND";
    SHMEM_OpName[FI_BOR]                 = "BOR";
    SHMEM_OpName[FI_BAND]                = "BAND";
    SHMEM_OpName[FI_LXOR]                = "LXOR";
    SHMEM_OpName[FI_BXOR]                = "BXOR";
    SHMEM_OpName[FI_ATOMIC_READ]         = "ATOMIC_WRITE";
    SHMEM_OpName[FI_ATOMIC_WRITE]        = "ATOMIC_READ";
    SHMEM_OpName[FI_CSWAP]               = "CSWAP";
    SHMEM_OpName[FI_CSWAP_NE]            = "CSWAP_NE";
    SHMEM_OpName[FI_CSWAP_LE]            = "CSWAP_LE";
    SHMEM_OpName[FI_CSWAP_LT]            = "CSWAP_LT";
    SHMEM_OpName[FI_CSWAP_GE]            = "CSWAP_GE";
    SHMEM_OpName[FI_CSWAP_GT]            = "CSWAP_GT";
    SHMEM_OpName[FI_MSWAP]               = "MSWAP";
}

/* Cover OpenSHMEM atomics API */

#define SIZEOF_AMO_DT 5
static int DT_AMO_STANDARD[] = {
    SHM_INTERNAL_INT, SHM_INTERNAL_LONG, SHM_INTERNAL_LONG_LONG,
    SHM_INTERNAL_INT32, SHM_INTERNAL_INT64
};
#define SIZEOF_AMO_OPS 1
static int AMO_STANDARD_OPS[] = {
    SHM_INTERNAL_SUM
};
#define SIZEOF_AMO_FOPS 1
static int FETCH_AMO_STANDARD_OPS[] = {
    SHM_INTERNAL_SUM
};
#define SIZEOF_AMO_COPS 1
static int COMPARE_AMO_STANDARD_OPS[] = {
    FI_CSWAP
};

/* Note: Fortran-specific types should be last so they can be disabled here */
#ifdef ENABLE_FORTRAN
#define SIZEOF_AMO_EX_DT 8
#else
#define SIZEOF_AMO_EX_DT 7
#endif
static int DT_AMO_EXTENDED[] = {
    SHM_INTERNAL_FLOAT, SHM_INTERNAL_DOUBLE, SHM_INTERNAL_INT, SHM_INTERNAL_LONG,
    SHM_INTERNAL_LONG_LONG, SHM_INTERNAL_INT32, SHM_INTERNAL_INT64,
    SHM_INTERNAL_FORTRAN_INTEGER
};
#define SIZEOF_AMO_EX_OPS 1
static int AMO_EXTENDED_OPS[] = {
    FI_ATOMIC_WRITE
};
#define SIZEOF_AMO_EX_FOPS 2
static int FETCH_AMO_EXTENDED_OPS[] = {
    FI_ATOMIC_WRITE, FI_ATOMIC_READ
};


/* Cover one-sided implementation of reduction */
#define SIZEOF_RED_DT 6
static int DT_REDUCE_BITWISE[] = {
    SHM_INTERNAL_SHORT, SHM_INTERNAL_INT, SHM_INTERNAL_LONG,
    SHM_INTERNAL_LONG_LONG, SHM_INTERNAL_INT32, SHM_INTERNAL_INT64
};
#define SIZEOF_RED_OPS 3
static int REDUCE_BITWISE_OPS[] = {
    SHM_INTERNAL_BAND, SHM_INTERNAL_BOR, SHM_INTERNAL_BXOR
};


#define SIZEOF_REDC_DT 9
static int DT_REDUCE_COMPARE[] = {
    SHM_INTERNAL_FLOAT, SHM_INTERNAL_DOUBLE, SHM_INTERNAL_SHORT,
    SHM_INTERNAL_INT, SHM_INTERNAL_LONG, SHM_INTERNAL_LONG_LONG,
    SHM_INTERNAL_INT32, SHM_INTERNAL_INT64, SHM_INTERNAL_LONG_DOUBLE
};
#define SIZEOF_REDC_OPS 2
static int REDUCE_COMPARE_OPS[] = {
    SHM_INTERNAL_MAX, SHM_INTERNAL_MIN
};


#define SIZEOF_REDA_DT 11
static int DT_REDUCE_ARITH[] = {
    SHM_INTERNAL_FLOAT, SHM_INTERNAL_DOUBLE, SHM_INTERNAL_FLOAT_COMPLEX,
    SHM_INTERNAL_DOUBLE_COMPLEX, SHM_INTERNAL_SHORT, SHM_INTERNAL_INT,
    SHM_INTERNAL_LONG, SHM_INTERNAL_LONG_LONG, SHM_INTERNAL_INT32,
    SHM_INTERNAL_INT64, SHM_INTERNAL_LONG_DOUBLE
};
#define SIZEOF_REDA_OPS 2
static int REDUCE_ARITH_OPS[] = {
    SHM_INTERNAL_SUM, SHM_INTERNAL_PROD
};

/* Internal to SHMEM implementation atomic requirement */
/* Locking implementation requirement */
#define SIZEOF_INTERNAL_REQ_DT 1
static int DT_INTERNAL_REQ[] = {
    SHM_INTERNAL_INT
};
#define SIZEOF_INTERNAL_REQ_OPS 1
static int INTERNAL_REQ_OPS[] = {
    FI_MSWAP
};

typedef enum {
    ATOMIC_NO_SUPPORT,
    ATOMIC_WARNINGS,
    ATOMIC_SOFT_SUPPORT,
} atomic_support_lv;


/* default CQ depth */
uint64_t shmem_transport_ofi_max_poll = (1ULL<<30);


enum stx_allocator_t {
    ROUNDROBIN = 0,
    RANDOM
};
typedef enum stx_allocator_t stx_allocator_t;
static stx_allocator_t shmem_transport_ofi_stx_allocator;

static long shmem_transport_ofi_stx_max;
static long shmem_transport_ofi_stx_threshold;

struct shmem_transport_ofi_stx_t {
    struct fid_stx*   stx;
    long              ref_cnt;
    int               is_private;
};
typedef struct shmem_transport_ofi_stx_t shmem_transport_ofi_stx_t;
static shmem_transport_ofi_stx_t* shmem_transport_ofi_stx_pool = NULL;

struct shmem_transport_ofi_stx_kvs_t {
    int                         stx_idx;
    struct shmem_internal_tid   tid;
    UT_hash_handle              hh;
};
typedef struct shmem_transport_ofi_stx_kvs_t shmem_transport_ofi_stx_kvs_t;
static shmem_transport_ofi_stx_kvs_t* shmem_transport_ofi_stx_kvs = NULL;

static inline
void shmem_transport_ofi_dump_stx(void) {
    char stx_str[256];
    int i, offset;

    if (shmem_transport_ofi_stx_max == 0)
        return;

    for (i = offset = 0; i < shmem_transport_ofi_stx_max; i++)
        offset += snprintf(stx_str+offset, 256-offset,
                           (i == shmem_transport_ofi_stx_max-1) ? "%ld%s" : "%ld%s ",
                           shmem_transport_ofi_stx_pool[i].ref_cnt,
                           shmem_transport_ofi_stx_pool[i].is_private ? "P" : "S");

    DEBUG_MSG("STX[%ld] = [ %s ]\n", shmem_transport_ofi_stx_max, stx_str);
}

static inline
int shmem_transport_ofi_is_private(long options) {
    if (!shmem_internal_params.OFI_STX_DISABLE_PRIVATE &&
        (options & SHMEM_CTX_PRIVATE)) {
        return 1;
    } else {
        return 0;
    }
}

static unsigned int rand_pool_seed;

static inline
void shmem_transport_ofi_stx_rand_init(void) {
    rand_pool_seed = shmem_internal_my_pe;
    return;
}

static inline
int shmem_transport_ofi_stx_search_unused(void)
{
    int stx_idx = -1, i;

    for (i = 0; i < shmem_transport_ofi_stx_max; i++) {
        if (shmem_transport_ofi_stx_pool[i].ref_cnt == 0) {
            shmem_internal_assert(!shmem_transport_ofi_stx_pool[i].is_private);
            stx_idx = i;
            break;
        }
    }

    return stx_idx;
}


static inline
int shmem_transport_ofi_stx_search_shared(long threshold)
{
    static int rr_start_idx = 0;
    int stx_idx = -1, i, count;

    switch (shmem_transport_ofi_stx_allocator) {
        case ROUNDROBIN:
            i = rr_start_idx;
            for (count = 0; count < shmem_transport_ofi_stx_max; count++) {
                if (shmem_transport_ofi_stx_pool[i].ref_cnt > 0 &&
                    (shmem_transport_ofi_stx_pool[i].ref_cnt <= threshold || threshold == -1) &&
                    !shmem_transport_ofi_stx_pool[i].is_private) {
                    stx_idx = i;
                    rr_start_idx = (i + 1) % shmem_transport_ofi_stx_max;
                    break;
                }

                i = (i + 1) % shmem_transport_ofi_stx_max;
            }

            break;

        case RANDOM:
            for (i = count = 0; i < shmem_transport_ofi_stx_max; i++) {
                if (shmem_transport_ofi_stx_pool[i].ref_cnt > 0 &&
                    (shmem_transport_ofi_stx_pool[i].ref_cnt <= threshold || threshold == -1) &&
                    !shmem_transport_ofi_stx_pool[i].is_private)
                {
                    ++count;
                    break;
                }
            }

            if (count == 0)
                break;

            /* Probe at random until we select an available STX */
            else {
                do {
                    stx_idx = (int) (rand_r(&rand_pool_seed) / (RAND_MAX + 1.0) * shmem_transport_ofi_stx_max);
                } while (!(shmem_transport_ofi_stx_pool[stx_idx].ref_cnt > 0 &&
                           (shmem_transport_ofi_stx_pool[stx_idx].ref_cnt <= threshold || threshold == -1) &&
                           !shmem_transport_ofi_stx_pool[stx_idx].is_private));
            }

            break;
        default:
            RAISE_ERROR_MSG("Invalid STX allocator (%d)\n",
                            shmem_transport_ofi_stx_allocator);
    }

    return stx_idx;
}



static inline
void shmem_transport_ofi_stx_allocate(shmem_transport_ctx_t *ctx)
{
    if (shmem_transport_ofi_stx_max == 0) {
        ctx->stx_idx = -1;
    } else if (shmem_transport_ofi_is_private(ctx->options)) {
        /* SHMEM contexts that are private to the same thread (i.e. have
         * SHMEM_CTX_PRIVATE option set) share the same STX.  */

        shmem_transport_ofi_stx_kvs_t *f;
        HASH_FIND(hh, shmem_transport_ofi_stx_kvs,
                  &ctx->tid, sizeof(struct shmem_internal_tid), f);

        if (f) {
            shmem_transport_ofi_stx_pool[f->stx_idx].ref_cnt++;
            ctx->stx_idx = f->stx_idx;

        } else {
            /* No STX allocated to the given TID, attempt to allocate one */
            int is_unused = 1;
            int stx_idx;
            shmem_transport_ofi_stx_t *stx = NULL;

            stx_idx = shmem_transport_ofi_stx_search_unused();

            /* Couldn't get new STX, assign a shared one */
            /* Note: When stx_max > 0, shared STX allocation is always successful */
            if (stx_idx < 0) {
                DEBUG_STR("private STX unavailable, falling back to STX sharing");
                is_unused = 0;
                stx_idx = shmem_transport_ofi_stx_search_shared(shmem_transport_ofi_stx_threshold);
                if (stx_idx < 0)
                    stx_idx = shmem_transport_ofi_stx_search_shared(-1);
            }

            shmem_internal_assert(stx_idx >= 0);
            stx = &shmem_transport_ofi_stx_pool[stx_idx];
            ctx->stx_idx = stx_idx;
            stx->ref_cnt++;

            if (is_unused) {
                stx->is_private = 1;
                shmem_transport_ofi_stx_kvs_t *e = calloc(1, sizeof(shmem_transport_ofi_stx_kvs_t));
                if (e == NULL) {
                    RAISE_ERROR_STR("out of memory when allocating STX KVS entry");
                }
                e->tid     = ctx->tid;
                e->stx_idx = ctx->stx_idx;
                HASH_ADD(hh, shmem_transport_ofi_stx_kvs, tid,
                         sizeof(struct shmem_internal_tid), e);
            } else {
                ctx->options &= ~SHMEM_CTX_PRIVATE;
            }
        }
    /* TODO: Optimize this case? else if (ctx->options & SHMEM_CTX_SERIALIZED) */
    } else {
        int stx_idx = shmem_transport_ofi_stx_search_shared(shmem_transport_ofi_stx_threshold);

        if (stx_idx < 0)
            stx_idx = shmem_transport_ofi_stx_search_unused();

        if (stx_idx < 0)
            stx_idx = shmem_transport_ofi_stx_search_shared(-1);

        shmem_internal_assert(stx_idx >= 0);
        ctx->stx_idx = stx_idx;
        shmem_transport_ofi_stx_pool[ctx->stx_idx].ref_cnt++;
    }

    shmem_transport_ofi_dump_stx();

    return;
}

#define OFI_MAJOR_VERSION 1
#define OFI_MINOR_VERSION 5

static
void init_bounce_buffer(shmem_free_list_item_t *item)
{
    shmem_transport_ofi_frag_t *frag =
        (shmem_transport_ofi_frag_t*) item;
    frag->mytype = SHMEM_TRANSPORT_OFI_TYPE_BOUNCE;
}


static inline
int bind_enable_ep_resources(shmem_transport_ctx_t *ctx)
{
    int ret = 0;

    /* If using SOS-managed STXs, bind the STX */
    if (ctx->stx_idx >= 0) {
        ret = fi_ep_bind(ctx->ep, &shmem_transport_ofi_stx_pool[ctx->stx_idx].stx->fid, 0);
        OFI_CHECK_RETURN_STR(ret, "fi_ep_bind STX to endpoint failed");
    }

    /* Put counter captures completions for non-fetching operations (put,
     * atomic, etc.) */
    ret = fi_ep_bind(ctx->ep, &ctx->put_cntr->fid, FI_WRITE);
    OFI_CHECK_RETURN_STR(ret, "fi_ep_bind put CNTR to endpoint failed");

    /* Get counter captures completions for fetching operations (get,
     * fetch-atomic, etc.) */
    ret = fi_ep_bind(ctx->ep, &ctx->get_cntr->fid, FI_READ);
    OFI_CHECK_RETURN_STR(ret, "fi_ep_bind get CNTR to endpoint failed");

    /* In addition to incrementing the put counter, bounce buffered puts and
     * non-fetching AMOs generate a CQ event that is used to reclaim the buffer
     * (pointer is returned in event context) after the operation completes. */

    /* Note: The CQ is bound with FI_RECV even though no receive capabilities
     * are enabled on this EP.  FI_RECV is required to drive progress for this
     * EP using the CQ.  When manual progress is disabled, FI_RECV can be
     * removed below.  However, there aren't currently any cases where removing
     * FI_RECV significantly improves performance or resource usage.  */

    if (ctx->ep != shmem_transport_ofi_target_ep) {
        ret = fi_ep_bind(ctx->ep, &ctx->tx_cq->fid,
                         FI_SELECTIVE_COMPLETION | FI_TRANSMIT | FI_RECV);
        OFI_CHECK_RETURN_STR(ret, "fi_ep_bind CQ to endpoint failed");

        ret = fi_ep_bind(ctx->ep, &shmem_transport_ofi_avfd->fid, 0);
        OFI_CHECK_RETURN_STR(ret, "fi_ep_bind AV to endpoint failed");

        ret = fi_enable(ctx->ep);
        OFI_CHECK_RETURN_STR(ret, "fi_enable on endpoint failed");

  //      ret = fi_enable(ctx->CXI_ep);
  //      OFI_CHECK_RETURN_STR(ret, "fi_enable on coll/CXI endpoint failed");

    } /* In single-endpoint mode, the sockets provider requires re-enabling the EP, but other
         providers require NOT re-enabling the EP (e.g. as of v2.1.0, tcp, verbs, and opx) */
    else if (shmem_transport_ofi_info.p_info->fabric_attr->prov_name != NULL &&
             strncmp(shmem_transport_ofi_info.p_info->fabric_attr->prov_name,
                     SHMEM_TRANSPORT_OFI_PROV_SOCKETS,
                     strlen(SHMEM_TRANSPORT_OFI_PROV_SOCKETS)) == 0) {
        ret = fi_enable(ctx->ep);
        OFI_CHECK_RETURN_STR(ret, "fi_enable on endpoint failed");
  //      ret = fi_enable(ctx->CXI_ep);
  //      OFI_CHECK_RETURN_STR(ret, "fi_enable on coll/CXI endpoint failed");
    }

    return ret;
}

#ifdef USE_FI_HMEM
static inline
int ofi_mr_reg_external_heap(void)
{
    int ret = 0;
    uint64_t key = 2;

    const struct iovec iov = {
                               .iov_base     = shmem_external_heap_base,
                               .iov_len      = shmem_external_heap_length
                             };
    const struct fi_mr_attr mr_attr = {
                                        .mr_iov         = &iov,
                                        .iov_count      = 1,
                                        .access         = FI_REMOTE_READ | FI_REMOTE_WRITE,
                                        .requested_key  = key,
                                        .iface          = (shmem_external_heap_device_type == 
                                                          SHMEMX_EXTERNAL_HEAP_ZE ? FI_HMEM_ZE : FI_HMEM_CUDA),
                                        .device.ze      = shmem_external_heap_device, /* TODO: Need to change to local */
                                        .offset         = 0,
                                        .context        = NULL
                                      };

    ret = fi_mr_regattr(shmem_transport_ofi_domainfd, &mr_attr, 0, &shmem_transport_ofi_external_heap_mrfd);
    OFI_CHECK_RETURN_STR(ret, "fi_mr_regattr (heap) failed");

#if ENABLE_TARGET_CNTR
    ret = fi_mr_bind(shmem_transport_ofi_external_heap_mrfd,
                     &shmem_transport_ofi_target_cntrfd->fid,
                     FI_REMOTE_WRITE);
    OFI_CHECK_RETURN_STR(ret, "target CNTR binding to external heap MR failed");

    if (shmem_transport_ofi_info.p_info->domain_attr->mr_mode & FI_MR_ENDPOINT) {
        ret = fi_ep_bind(shmem_transport_ofi_target_ep,
                         &shmem_transport_ofi_target_cntrfd->fid, FI_REMOTE_WRITE);
        OFI_CHECK_RETURN_STR(ret, "target CNTR binding to target EP failed");
        ret = fi_mr_bind(shmem_transport_ofi_external_heap_mrfd,
                         &shmem_transport_ofi_target_ep->fid, FI_REMOTE_WRITE);
        OFI_CHECK_RETURN_STR(ret, "target EP binding to heap MR failed");

        ret = fi_mr_enable(shmem_transport_ofi_external_heap_mrfd);
        OFI_CHECK_RETURN_STR(ret, "target heap MR enable failed");
    }
#endif

    return ret;
}
#endif /* USE_FI_HMEM */

static inline
int ofi_mr_reg_bind(uint64_t flags)
{
    int ret = 0;

#if defined(ENABLE_MR_SCALABLE) && defined(ENABLE_REMOTE_VIRTUAL_ADDRESSING)
    ret = fi_mr_reg(shmem_transport_ofi_domainfd, 0, UINT64_MAX,
                    FI_REMOTE_READ | FI_REMOTE_WRITE, 0, 0ULL, flags,
                    &shmem_transport_ofi_target_mrfd, NULL);
    OFI_CHECK_RETURN_STR(ret, "target memory (all) registration failed");

    /* Bind counter with target memory region for incoming messages */
#if ENABLE_TARGET_CNTR
    ret = fi_mr_bind(shmem_transport_ofi_target_mrfd,
                     &shmem_transport_ofi_target_cntrfd->fid,
                     FI_REMOTE_WRITE);
    OFI_CHECK_RETURN_STR(ret, "target CNTR binding to MR failed");

#ifdef ENABLE_MR_RMA_EVENT
    if (shmem_transport_ofi_mr_rma_event) {
        ret = fi_mr_enable(shmem_transport_ofi_target_mrfd);
        OFI_CHECK_RETURN_STR(ret, "target MR enable failed");
    }
#endif /* ENABLE_MR_RMA_EVENT */
#endif /* ENABLE_TARGET_CNTR */
    shmem_transport_ofi_mrfd_list[0] = shmem_transport_ofi_target_mrfd;
    shmem_transport_ofi_mrfd_list[1] = NULL;

#else
    /* Register separate data and heap segments using keys 0 and 1,
     * respectively.  In MR_BASIC_MODE, the keys are ignored and selected by
     * the provider. */
    uint64_t key = 1;
    ret = fi_mr_reg(shmem_transport_ofi_domainfd, shmem_internal_heap_base,
                    shmem_internal_heap_length,
                    FI_REMOTE_READ | FI_REMOTE_WRITE, 0, key, flags,
                    &shmem_transport_ofi_target_heap_mrfd, NULL);
    OFI_CHECK_RETURN_STR(ret, "target memory (heap) registration failed");

    key = 0;
    ret = fi_mr_reg(shmem_transport_ofi_domainfd, shmem_internal_data_base,
                    shmem_internal_data_length,
                    FI_REMOTE_READ | FI_REMOTE_WRITE, 0, key, flags,
                    &shmem_transport_ofi_target_data_mrfd, NULL);
    OFI_CHECK_RETURN_STR(ret, "target memory (data) registration failed");

    /* Bind counter with target memory region for incoming messages */
#if ENABLE_TARGET_CNTR
    ret = fi_mr_bind(shmem_transport_ofi_target_heap_mrfd,
                     &shmem_transport_ofi_target_cntrfd->fid,
                     FI_REMOTE_WRITE);
    OFI_CHECK_RETURN_STR(ret, "target CNTR binding to heap MR failed");

    ret = fi_mr_bind(shmem_transport_ofi_target_data_mrfd,
                     &shmem_transport_ofi_target_cntrfd->fid,
                     FI_REMOTE_WRITE);
    OFI_CHECK_RETURN_STR(ret, "target CNTR binding to data MR failed");

#ifdef ENABLE_MR_ENDPOINT
    if (shmem_transport_ofi_info.p_info->domain_attr->mr_mode & FI_MR_ENDPOINT) {
        ret = fi_ep_bind(shmem_transport_ofi_target_ep,
                         &shmem_transport_ofi_target_cntrfd->fid, FI_REMOTE_WRITE);
        OFI_CHECK_RETURN_STR(ret, "target CNTR binding to target EP failed");

        ret = fi_mr_bind(shmem_transport_ofi_target_heap_mrfd,
                         &shmem_transport_ofi_target_ep->fid, FI_REMOTE_WRITE);
        OFI_CHECK_RETURN_STR(ret, "target EP binding to heap MR failed");

        ret = fi_mr_enable(shmem_transport_ofi_target_heap_mrfd);
        OFI_CHECK_RETURN_STR(ret, "target heap MR enable failed");

        ret = fi_mr_bind(shmem_transport_ofi_target_data_mrfd,
                         &shmem_transport_ofi_target_ep->fid, FI_REMOTE_WRITE);
        OFI_CHECK_RETURN_STR(ret, "target EP binding to data MR failed");

        ret = fi_mr_enable(shmem_transport_ofi_target_data_mrfd);
        OFI_CHECK_RETURN_STR(ret, "target data MR enable failed");
    }
#endif

#ifdef ENABLE_MR_RMA_EVENT
    if (shmem_transport_ofi_mr_rma_event) {
        ret = fi_mr_enable(shmem_transport_ofi_target_data_mrfd);
        OFI_CHECK_RETURN_STR(ret, "target data MR enable failed");

        ret = fi_mr_enable(shmem_transport_ofi_target_heap_mrfd);
        OFI_CHECK_RETURN_STR(ret, "target heap MR enable failed");
    }
#endif /* ENABLE_MR_RMA_EVENT */
#endif /* ENABLE_TARGET_CNTR */

    shmem_transport_ofi_mrfd_list[0] = shmem_transport_ofi_target_data_mrfd;
    shmem_transport_ofi_mrfd_list[1] = shmem_transport_ofi_target_heap_mrfd;

#endif

    return ret;
}

static inline
int allocate_recv_cntr_mr(void)
{
    int ret = 0;
    uint64_t flags = 0;

    /* ------------------------------------ */
    /* POST enable resources for to EP      */
    /* ------------------------------------ */

    /* since this is AFTER enable and RMA you must create memory regions for
     * incoming reads/writes and outgoing non-blocking Puts, specifying entire
     * VA range */

#if ENABLE_TARGET_CNTR
    {
        struct fi_cntr_attr cntr_attr = {0};

        /* Create counter for incoming writes */
        cntr_attr.events   = FI_CNTR_EVENTS_COMP;
        cntr_attr.wait_obj = FI_WAIT_UNSPEC;

        ret = fi_cntr_open(shmem_transport_ofi_domainfd, &cntr_attr,
                           &shmem_transport_ofi_target_cntrfd, NULL);
        OFI_CHECK_RETURN_STR(ret, "target CNTR open failed");

#ifdef ENABLE_MR_RMA_EVENT
        if (shmem_transport_ofi_mr_rma_event)
            flags |= FI_RMA_EVENT;
#endif /* ENABLE_MR_RMA_EVENT */
    }
#endif

#ifdef USE_FI_HMEM
    if (shmem_external_heap_pre_initialized) {
        ret = ofi_mr_reg_external_heap();
        OFI_CHECK_RETURN_STR(ret, "OFI MR registration with HMEM failed");
        shmem_transport_ofi_mrfd_list[2] = shmem_transport_ofi_external_heap_mrfd;
    } else {
        shmem_transport_ofi_mrfd_list[2] = NULL;
    }
#else
    shmem_transport_ofi_mrfd_list[2] = NULL;
#endif

    ret = ofi_mr_reg_bind(flags);
    OFI_CHECK_RETURN_STR(ret, "OFI MR registration failed");

    return ret;
}

#ifdef USE_FI_HMEM
static
int publish_external_mr_info(void)
{
    int err;
    uint64_t ext_heap_key;

    if (shmem_transport_ofi_info.p_info->domain_attr->mr_mode & FI_MR_PROV_KEY) {
        ext_heap_key = fi_mr_key(shmem_transport_ofi_external_heap_mrfd);
    } else {
        ext_heap_key = 2;
    }

    err = shmem_runtime_put("fi_ext_heap_key", &ext_heap_key, sizeof(uint64_t));
    if (err) {
        RAISE_WARN_STR("Put of heap key to runtime KVS failed");
        return 1;
    }

    void *ext_heap_base;

    if (shmem_transport_ofi_info.p_info->domain_attr->mr_mode & FI_MR_VIRT_ADDR) {
        ext_heap_base = shmem_external_heap_base;
    } else {
        ext_heap_base = (void *) 0;
    }

    err = shmem_runtime_put("fi_ext_heap_addr", &ext_heap_base, sizeof(uint8_t*));
    if (err) {
        RAISE_WARN_STR("Put of heap address to runtime KVS failed");
        return 1;
    }

    return 0;
}
#endif

static
int publish_mr_info(void)
{
#ifndef ENABLE_MR_SCALABLE
    {
        int err;
        uint64_t heap_key, data_key;

        if (shmem_transport_ofi_info.p_info->domain_attr->mr_mode & FI_MR_PROV_KEY) {
            heap_key = fi_mr_key(shmem_transport_ofi_target_heap_mrfd);
            data_key = fi_mr_key(shmem_transport_ofi_target_data_mrfd);
        } else {
            heap_key = 1;
            data_key = 0;
        }

        err = shmem_runtime_put("fi_heap_key", &heap_key, sizeof(uint64_t));
        if (err) {
            RAISE_WARN_STR("Put of heap key to runtime KVS failed");
            return 1;
        }

        err = shmem_runtime_put("fi_data_key", &data_key, sizeof(uint64_t));
        if (err) {
            RAISE_WARN_STR("Put of data segment key to runtime KVS failed");
            return 1;
        }
    }

#ifdef ENABLE_REMOTE_VIRTUAL_ADDRESSING
    if (shmem_transport_ofi_info.p_info->domain_attr->mr_mode & FI_MR_VIRT_ADDR)
        shmem_transport_ofi_use_absolute_address = 1;
    else
        shmem_transport_ofi_use_absolute_address = 0;
#else /* !ENABLE_REMOTE_VIRTUAL_ADDRESSING */
    {
        int err;
        void *heap_base, *data_base;

        if (shmem_transport_ofi_info.p_info->domain_attr->mr_mode & FI_MR_VIRT_ADDR) {
            heap_base = shmem_internal_heap_base;
            data_base = shmem_internal_data_base;
        } else {
            heap_base = (void *) 0;
            data_base = (void *) 0;
        }

        err = shmem_runtime_put("fi_heap_addr", &heap_base, sizeof(uint8_t*));
        if (err) {
            RAISE_WARN_STR("Put of heap address to runtime KVS failed");
            return 1;
        }

        err = shmem_runtime_put("fi_data_addr", &data_base, sizeof(uint8_t*));
        if (err) {
            RAISE_WARN_STR("Put of data segment address to runtime KVS failed");
            return 1;
        }
    }
#endif /* ENABLE_REMOTE_VIRTUAL_ADDRESSING */
#endif /* !ENABLE_MR_SCALABLE */

#ifdef USE_FI_HMEM
    if (shmem_external_heap_pre_initialized) {
        int err = publish_external_mr_info();
        if (err) {
            RAISE_WARN_STR("Publish of external mr info failed");
            return 1;
        }
    }
#endif

    return 0;
}

#ifdef USE_FI_HMEM
static
int populate_external_mr_tables(void)
{
    int i, err;

    shmem_transport_ofi_external_heap_keys = malloc(sizeof(uint64_t) * shmem_internal_num_pes);
    if (NULL == shmem_transport_ofi_external_heap_keys) {
        RAISE_WARN_STR("Out of memory allocating heap keytable");
        return 1;
    }

    /* Called after the upper layer performs the runtime exchange */
    for (i = 0; i < shmem_internal_num_pes; i++) {
        err = shmem_runtime_get(i, "fi_ext_heap_key",
                                &shmem_transport_ofi_external_heap_keys[i],
                                sizeof(uint64_t));
        if (err) {
            RAISE_WARN_STR("Get of heap key from runtime KVS failed");
            return 1;
        }
    }

    shmem_transport_ofi_external_heap_addrs = malloc(sizeof(uint8_t*) * shmem_internal_num_pes);
    if (NULL == shmem_transport_ofi_external_heap_addrs) {
        RAISE_WARN_STR("Out of memory allocating heap addrtable");
        return 1;
    }

    /* Called after the upper layer performs the runtime exchange */
    for (i = 0; i < shmem_internal_num_pes; i++) {
        err = shmem_runtime_get(i, "fi_ext_heap_addr",
                                &shmem_transport_ofi_external_heap_addrs[i],
                                sizeof(uint8_t*));
        if (err) {
            RAISE_WARN_STR("Get of heap address from runtime KVS failed");
            return 1;
        }
    }

    return 0;
}
#endif

static
int populate_mr_tables(void)
{
#ifndef ENABLE_MR_SCALABLE
    {
        int i, err;

        shmem_transport_ofi_target_heap_keys = malloc(sizeof(uint64_t) * shmem_internal_num_pes);
        if (NULL == shmem_transport_ofi_target_heap_keys) {
            RAISE_WARN_STR("Out of memory allocating heap keytable");
            return 1;
        }

        shmem_transport_ofi_target_data_keys = malloc(sizeof(uint64_t) * shmem_internal_num_pes);
        if (NULL == shmem_transport_ofi_target_data_keys) {
            RAISE_WARN_STR("Out of memory allocating heap keytable");
            return 1;
        }

        /* Called after the upper layer performs the runtime exchange */
        for (i = 0; i < shmem_internal_num_pes; i++) {
            err = shmem_runtime_get(i, "fi_heap_key",
                                    &shmem_transport_ofi_target_heap_keys[i],
                                    sizeof(uint64_t));
            if (err) {
                RAISE_WARN_STR("Get of heap key from runtime KVS failed");
                return 1;
            }
            err = shmem_runtime_get(i, "fi_data_key",
                                    &shmem_transport_ofi_target_data_keys[i],
                                    sizeof(uint64_t));
            if (err) {
                RAISE_WARN_STR("Get of data segment key from runtime KVS failed");
                return 1;
            }
        }
    }

#ifndef ENABLE_REMOTE_VIRTUAL_ADDRESSING
    {
        int i, err;

        shmem_transport_ofi_target_heap_addrs = malloc(sizeof(uint8_t*) * shmem_internal_num_pes);
        if (NULL == shmem_transport_ofi_target_heap_addrs) {
            RAISE_WARN_STR("Out of memory allocating heap addrtable");
            return 1;
        }

        shmem_transport_ofi_target_data_addrs = malloc(sizeof(uint8_t*) * shmem_internal_num_pes);
        if (NULL == shmem_transport_ofi_target_data_addrs) {
            RAISE_WARN_STR("Out of memory allocating data addrtable");
            return 1;
        }

        /* Called after the upper layer performs the runtime exchange */
        for (i = 0; i < shmem_internal_num_pes; i++) {
            err = shmem_runtime_get(i, "fi_heap_addr",
                                    &shmem_transport_ofi_target_heap_addrs[i],
                                    sizeof(uint8_t*));
            if (err) {
                RAISE_WARN_STR("Get of heap address from runtime KVS failed");
                return 1;
            }
            err = shmem_runtime_get(i, "fi_data_addr",
                                    &shmem_transport_ofi_target_data_addrs[i],
                                    sizeof(uint8_t*));
            if (err) {
                RAISE_WARN_STR("Get of data segment address from runtime KVS failed");
                return 1;
            }
        }
    }
#endif /* ENABLE_REMOTE_VIRTUAL_ADDRESSING */
#endif /* !ENABLE_MR_SCALABLE */

#ifdef USE_FI_HMEM
    if (shmem_external_heap_pre_initialized) {
        int err = populate_external_mr_tables();
        if (err) {
            RAISE_WARN_STR("Populate external MR tables failed");
            return 1;
        }
    }
#endif

    return 0;
}

/* SOFT_SUPPORT will not produce warning or error */
static inline
int atomicvalid_rtncheck(int ret, int atomic_size,
                         atomic_support_lv atomic_sup,
                         char *strOP, char *strDT)
{
    if ((ret != 0 || atomic_size == 0) && atomic_sup != ATOMIC_SOFT_SUPPORT) {
        RAISE_WARN_MSG("Provider does not support atomic '%s' "
                       "on type '%s' (%d, %d)\n", strOP, strDT, ret, atomic_size);

        if (atomic_sup != ATOMIC_WARNINGS) {
            return ret ? ret : -1;
        }
    }

    return 0;
}

static inline
int atomicvalid_DTxOP(int DT_MAX, int OPS_MAX, int *DT, int *OPS,
                      atomic_support_lv atomic_sup)
{
    int i, j;
    size_t atomic_size;

    for (i = 0; i < DT_MAX; i++) {
        for (j = 0; j < OPS_MAX; j++) {
            int dt = SHMEM_TRANSPORT_DTYPE(DT[i]);
            int ret = fi_atomicvalid(shmem_transport_ctx_default.ep,
                                     dt, OPS[j], &atomic_size);
            if (atomicvalid_rtncheck(ret, atomic_size, atomic_sup,
                                     SHMEM_OpName[OPS[j]],
                                     SHMEM_DtName[dt]))
                return ret;
        }
    }

    return 0;
}

static inline
int compare_atomicvalid_DTxOP(int DT_MAX, int OPS_MAX, int *DT,
                              int *OPS, atomic_support_lv atomic_sup)
{
    int i, j;
    size_t atomic_size;

    for (i = 0; i < DT_MAX; i++) {
        for (j = 0; j < OPS_MAX; j++) {
            int dt = SHMEM_TRANSPORT_DTYPE(DT[i]);
            int ret = fi_compare_atomicvalid(shmem_transport_ctx_default.ep,
                                             dt, OPS[j], &atomic_size);
            if (atomicvalid_rtncheck(ret, atomic_size, atomic_sup,
                                     SHMEM_OpName[OPS[j]],
                                     SHMEM_DtName[dt]))
                return ret;
        }
    }

    return 0;
}

static inline
int fetch_atomicvalid_DTxOP(int DT_MAX, int OPS_MAX, int *DT, int *OPS,
                            atomic_support_lv atomic_sup)
{
    int i, j;
    size_t atomic_size;

    for (i = 0; i < DT_MAX; i++) {
        for (j = 0; j < OPS_MAX; j++) {
            int dt = SHMEM_TRANSPORT_DTYPE(DT[i]);
            int ret = fi_fetch_atomicvalid(shmem_transport_ctx_default.ep,
                                           dt, OPS[j], &atomic_size);
            if (atomicvalid_rtncheck(ret, atomic_size, atomic_sup,
                                     SHMEM_OpName[OPS[j]],
                                     SHMEM_DtName[dt]))
                return ret;
        }
    }

    return 0;
}

static inline
int atomic_limitations_check(void)
{
    /* Retrieve messaging limitations from OFI
     *
     * NOTE: Currently only have reduction software atomic support. User can
     * optionally request for warnings if other atomic limitations are detected
     */

    int ret = 0;
    atomic_support_lv general_atomic_sup = ATOMIC_NO_SUPPORT;
    atomic_support_lv reduction_sup = ATOMIC_SOFT_SUPPORT;

    if (shmem_internal_params.OFI_ATOMIC_CHECKS_WARN)
        general_atomic_sup = ATOMIC_WARNINGS;

    init_ofi_tables();

    /* Standard OPS check */
    ret = atomicvalid_DTxOP(SIZEOF_AMO_DT, SIZEOF_AMO_OPS, DT_AMO_STANDARD,
                            AMO_STANDARD_OPS, general_atomic_sup);
    if (ret)
        return ret;

    ret = fetch_atomicvalid_DTxOP(SIZEOF_AMO_DT, SIZEOF_AMO_FOPS,
                                  DT_AMO_STANDARD, FETCH_AMO_STANDARD_OPS,
                                  general_atomic_sup);
    if (ret)
        return ret;

    ret = compare_atomicvalid_DTxOP(SIZEOF_AMO_DT, SIZEOF_AMO_COPS,
                                    DT_AMO_STANDARD, COMPARE_AMO_STANDARD_OPS,
                                    general_atomic_sup);
    if (ret)
        return ret;

    /* Extended OPS check */
    ret = atomicvalid_DTxOP(SIZEOF_AMO_EX_DT, SIZEOF_AMO_EX_OPS, DT_AMO_EXTENDED,
                            AMO_EXTENDED_OPS, general_atomic_sup);
    if (ret)
        return ret;

    ret = fetch_atomicvalid_DTxOP(SIZEOF_AMO_EX_DT, SIZEOF_AMO_EX_FOPS,
                                  DT_AMO_EXTENDED, FETCH_AMO_EXTENDED_OPS,
                                  general_atomic_sup);
    if (ret)
        return ret;

    /* Reduction OPS check */
    ret = atomicvalid_DTxOP(SIZEOF_RED_DT, SIZEOF_RED_OPS, DT_REDUCE_BITWISE,
                            REDUCE_BITWISE_OPS, reduction_sup);
    if (ret)
        return ret;

    ret = atomicvalid_DTxOP(SIZEOF_REDC_DT, SIZEOF_REDC_OPS, DT_REDUCE_COMPARE,
                            REDUCE_COMPARE_OPS, reduction_sup);
    if (ret)
        return ret;

    ret = atomicvalid_DTxOP(SIZEOF_REDA_DT, SIZEOF_REDA_OPS, DT_REDUCE_ARITH,
                            REDUCE_ARITH_OPS, reduction_sup);
    if (ret)
        return ret;

    /* Internal atomic requirement */
    ret = compare_atomicvalid_DTxOP(SIZEOF_INTERNAL_REQ_DT, SIZEOF_INTERNAL_REQ_OPS,
                                    DT_INTERNAL_REQ, INTERNAL_REQ_OPS,
                                    general_atomic_sup);
    if (ret)
        return ret;

    return 0;
}

static inline
int publish_av_info(struct fabric_info *info)
{
    int    ret = 0;
    char   epname[128];
    size_t epnamelen = sizeof(epname);

    ret = fi_getname((fid_t)shmem_transport_ofi_target_ep, epname, &epnamelen);
    if (ret != 0 || (epnamelen > sizeof(epname))) {
        RAISE_WARN_STR("fi_getname failed");
        return ret;
    }

    ret = shmem_runtime_put("fi_epname", epname, epnamelen);
    OFI_CHECK_RETURN_STR(ret, "shmem_runtime_put fi_epname failed");

    /* Note: we assume that the length of an address is the same for all
     * endpoints.  This is safe for most HPC systems, but could be incorrect in
     * a heterogeneous context. */
    shmem_transport_ofi_addrlen = epnamelen;

    return ret;
}

static inline
int populate_av(void)
{
    int    i, ret, err = 0;
    char   *alladdrs = NULL;


    alladdrs = malloc(shmem_internal_num_pes * shmem_transport_ofi_addrlen);
    if (alladdrs == NULL) {
        RAISE_WARN_STR("Out of memory allocating 'alladdrs'");
        return 1;
    }

    for (i = 0; i < shmem_internal_num_pes; i++) {
        char *addr_ptr = alladdrs + i * shmem_transport_ofi_addrlen;
        err = shmem_runtime_get(i, "fi_epname", addr_ptr, shmem_transport_ofi_addrlen);
        if (err != 0) {
            RAISE_ERROR_STR("Runtime get of 'fi_epname' failed");
        }
    }

    ret = fi_av_insert(shmem_transport_ofi_avfd,
                       alladdrs,
                       shmem_internal_num_pes,
                       addr_table,
                       0,
                       NULL);
    if (ret != shmem_internal_num_pes) {
        RAISE_WARN_STR("av insert failed");
        return ret;
    }

    free(alladdrs);
    return 0;
}



static int _compare(const void *v1, const void *v2)
{
    uint64_t *a1 = (uint64_t *)v1;
    uint64_t *a2 = (uint64_t *)v2;

    if (*a1 < *a2)
        return -1;
    if (*a1 > *a2)
        return 1;
    return 0;
}

static int polling_time(shmem_transport_ctx_t* ctx, void *flag){
    int ret = FI_SUCCESS;
    struct fi_cq_err_entry comp = {0};

    do {
        ret = fi_cq_read(ctx->rx_cq, &flag, 1);

        if (ret < 0 && ret != -FI_EAGAIN){
            return ret;
        }
        if (comp.op_context && comp.op_context == flag){
            return FI_SUCCESS;
        }

        ret = fi_cq_read(ctx->coll_tx_cq, &flag, 1);
 
        if (ret < 0 && ret != -FI_EAGAIN){
            return ret;
        }
        if (comp.op_context && comp.op_context == flag){
            return FI_SUCCESS;
        }
    } while (ret == -FI_EAGAIN);

    return ret;
}


#ifdef USE_HWLOC
static inline
struct fi_info *assign_nic_with_hwloc(struct fi_info *fabric, struct fi_info **provs, size_t num_nics) {
    int ret = 0;
    hwloc_bitmap_t bindset = hwloc_bitmap_alloc();

    ret = hwloc_get_proc_last_cpu_location(shmem_internal_topology, getpid(), bindset, HWLOC_CPUBIND_PROCESS);
    if (ret < 0) {
        RAISE_WARN_MSG("hwloc_get_proc_last_cpu_location failed (%s)\n", strerror(errno));
        return provs[shmem_internal_my_pe % num_nics];
    }

    // Identify which provider entries correspond to NICs with an affinity to the calling process
    struct fi_info *close_provs = NULL;
    struct fi_info *last_added = NULL;
    size_t num_close_nics = 0;
    for (size_t i = 0; i < num_nics; i++) {
        struct fi_info *cur_prov = provs[i];
        if (cur_prov->nic->bus_attr->bus_type != FI_BUS_PCI) continue;

        struct fi_pci_attr pci = cur_prov->nic->bus_attr->attr.pci;
        hwloc_obj_t io_device = hwloc_get_pcidev_by_busid(shmem_internal_topology, pci.domain_id, pci.bus_id, pci.device_id, pci.function_id);
        if (!io_device) {
            RAISE_WARN_MSG("hwloc_get_pcidev_by_busid failed\n");
            return provs[shmem_internal_my_pe % num_nics];
        };
        hwloc_obj_t first_non_io = hwloc_get_non_io_ancestor_obj(shmem_internal_topology, io_device);
        if (!first_non_io) {
            RAISE_WARN_MSG("hwloc_get_non_io_ancestor_obj failed\n");
            return provs[shmem_internal_my_pe % num_nics];
        }

        if (hwloc_bitmap_isincluded(bindset, first_non_io->cpuset) ||
            hwloc_bitmap_isincluded(first_non_io->cpuset, bindset)) {
            struct fi_info *dup = fi_dupinfo(cur_prov);
            if (!close_provs) close_provs = dup;
            if (last_added) last_added->next = dup;
            last_added = dup;
            num_close_nics++;
        }
    }
    DEBUG_MSG("Num. NICs w/ affinity to process: %zu\n", num_close_nics);

    if (!close_provs) {
        DEBUG_MSG("Could not detect any NICs with affinity to the process\n");

        /* If no 'close' NICs, select from list of all NICs using round-robin assignment */
        return provs[shmem_internal_my_pe % num_nics];
    }

    last_added->next = NULL;

    int idx = 0;
    struct fi_info **prov_list = (struct fi_info **) malloc(num_close_nics * sizeof(struct fi_info *));
    for (struct fi_info *cur_fabric = close_provs; cur_fabric; cur_fabric = cur_fabric->next) {
        prov_list[idx++] = cur_fabric;
    }

    hwloc_bitmap_free(bindset);

    struct fi_info *provider = prov_list[shmem_internal_my_pe % num_close_nics];
    free(prov_list);

    return provider;
}
#endif



int initialize_avset(int PE_start, int PE_stride, int PE_size){

    int err = 0;
    int i = 0;


    struct cxip_comm_key comm_key = {
        .keytype = COMM_KEY_NONE,
        .ucast.mcast_addr = 0,
        .ucast.hwroot_idx = 1
    };

    struct fi_av_set_attr avset_attr = {
        .count = PE_size,
        .start_addr = FI_ADDR_NOTAVAIL,
        .end_addr = FI_ADDR_NOTAVAIL,
        .stride = PE_stride,
        .comm_key_size = sizeof(comm_key),
        .comm_key = (void *)&comm_key,
        .flags = 0,
    };

    err = fi_av_set(shmem_transport_ofi_CXI_avfd, &avset_attr, &shmem_transport_ofi_CXI_avfd_set, NULL);
    OFI_CHECK_RETURN_STR(err, "AVSET creation failed");
    PRINT_DEBUG("shmem_transport_ofi_avset done %p\n", shmem_transport_ofi_CXI_avfd_set);


    int pe_count = 0;
    for (i = PE_start; pe_count < shmem_internal_num_pes && i < PE_size; i += PE_stride){
        PRINT_DEBUG("Iter %d Using addr 0x%lx, avset %p\n", i, shmem_transport_ofi_CXI_addr_table[i],
                shmem_transport_ofi_CXI_avfd_set);
        usleep(100000);
        err = fi_av_set_insert(shmem_transport_ofi_CXI_avfd_set, shmem_transport_ofi_CXI_addr_table[i]);
        OFI_CHECK_RETURN_STR(err, "av_set_insert_failed");
        pe_count += 1;
    }


    err = fi_av_set_addr(shmem_transport_ofi_CXI_avfd_set, &shmem_transport_ofi_CXI_world_addr);
    OFI_CHECK_RETURN_STR(err, "World_addr failed");
    if (shmem_transport_ofi_CXI_world_addr == 0){
        PRINT_ERROR("world addr is NULL\n");
        return -FI_EINVAL;
    }
    if (shmem_transport_ofi_CXI_world_addr == 0xffffffff){
        PRINT_ERROR("World addr is garbage value\n");
        return -FI_EINVAL;
    }

    PRINT_DEBUG ("world_addr 0x%lx\n", shmem_transport_ofi_CXI_world_addr);

    return err;
}

static inline enum fi_datatype find_type_name (char *dtype_string, int len, int *idx){
    int i = 0;
    enum fi_datatype ret = FI_VOID;
    for (i = 0; i < OSHMEM_STANDARD_len ; i++){
        if (strncmp(dtype_string, coll_type_arr[i].type, len) == 0 ){
            *idx = i;
            ret = coll_type_arr[i].match;
            return ret;
        }
    }
//    OFI_CHECK_ERROR_MSG(-FI_EINVAL, "Unsupported datatype: %s\n", dtype_string);
    return FI_VOID;
}

static inline int batch_polling_time (shmem_transport_ctx_t *ctx, void **contexts, 
        int nctx, const int max_inflight){

    int done = 0, i = 0;
    int16_t seen[max_inflight];
    memset(seen, 0, max_inflight *sizeof(int16_t));

    while (done < nctx) {
        void *got = cq_poll(ctx);
        if (got == NULL){
            continue;
        }

        for (i = 0; i< nctx; i++) {
            if (seen[i] == 0 && got == contexts[i]){
                seen[i] = 1;
                done++;
                break;
            }
        }
    }
    return 0;
}

void shmem_transport_coll_bcast(void *target, const void *source, size_t len,
                            int PE_root, int PE_start, int PE_stride, int PE_size,
                            long *pSync, int complete){

    int ret = FI_SUCCESS;
    shmem_transport_ctx_t *ctx = &shmem_transport_ctx_default;
    struct fid_ep *ep = ctx->CXI_ep;

    unsigned int nelems=0;
    int idx = 0;
    enum fi_datatype dtype;

    dtype = find_type_name(coll_type_string, strlen(coll_type_string), &idx);

    if (dtype == FI_VOID){
        PRINT_ERROR("WARNING: Unsupported datatype for broadcast.\n");
        shmem_global_exit(-FI_EINVAL);
    }

    nelems = len / coll_type_arr[idx].size;
    int sz = coll_type_arr[idx].size;
    uint64_t context = 0;
    const int max_inflight = 8;
    uint64_t contexts[max_inflight];
    void    *context_peers[max_inflight];

    memset(contexts, 0, max_inflight * sizeof(uint64_t));
    memset(context_peers, 0, sizeof(void *)*max_inflight);
    int offset = 0;
    int posted = 0;
    size_t cur_count = 0;
    int chunk_elems = 32/coll_type_arr[idx].size; 
    /* Can do 32 bytes for a given item. May increase to 256 later? */

    
#if 1
    avset_ary_t setary;
    d_entry_t joinlist;

    uint64_t mc; 
    ret = _simple_join(ctx, shmem_transport_ofi_CXI_addr_table, PE_size,
            &setary, &joinlist, PE_stride, PE_start);

    OFI_CHECK_ERROR_MSG(ret, "Failed to perform a join %d: %s\n", ret, fi_strerror(ret));

    mc = _simple_get_mc(&joinlist);
    OFI_CHECK_ERROR_MSG(!mc, "Failed to get the MC for bcast\n");

   
    while (offset < len) {
        posted = 0;
        while (posted < max_inflight && offset < len) { 
            cur_count = len - offset;
            if (cur_count > chunk_elems)
                cur_count = chunk_elems;

            context_peers[posted] = &contexts[posted];
            if (shmem_my_pe() == PE_root){
                memcpy(&target[offset], &source[offset], cur_count * sz);
            }
            
            ret = fi_broadcast(ep, &target[offset], cur_count, NULL,
                    mc, 
                    shmem_transport_ofi_CXI_addr_table[PE_root],
                    dtype, 0L,
                    context_peers[posted]);
            if (ret == -FI_EAGAIN){
                if (posted > 0){
                    batch_polling_time(ctx, context_peers, posted, max_inflight);
                    posted = 0;
                    continue;
                }
                do {
                } while(cq_poll(ctx) == NULL);
            }
            OFI_CHECK_ERROR_MSG(ret, "Bcast failed: %d %s\n", ret, fi_strerror(ret));
            offset += cur_count;
            posted++;
        }
        if (posted > 0){
            batch_polling_time(ctx, context_peers, posted, max_inflight);
        }
    }


#else
 
    ret = initialize_avset(PE_start, PE_stride, PE_size);
    OFI_CHECK_ERROR_MSG(ret, "failed to initialize avset: %d %s", ret, fi_strerror(ret));


    PRINT_DEBUG("Starting collective join\n");
    ret = fi_join_collective(ep, FI_ADDR_NOTAVAIL,
                             shmem_transport_ofi_CXI_avfd_set,
                             0, &ofi_coll_mc, &context);

    if (ret != FI_SUCCESS){
        PRINT_ERROR("Collective join failed!! %d %s\n", ret, fi_strerror(ret));
    }

    PRINT_DEBUG("Heading to wait_for_join...\n");

 //   OFI_CHECK_RETURN_MSG(ret, "collective_join failed!! %d %s", ret, fi_strerror(ret));

    ret = wait_for_join(ctx, FI_JOIN_COMPLETE, &context);
    OFI_CHECK_RETURN_STR(ret, "join_wait time failed\n");
    if (ofi_coll_mc == NULL){
        PRINT_ERROR("coll_mc is NULL\n");
        shmem_global_exit(-FI_EINVAL);
    }
    PRINT_DEBUG("Coll_mc %p\n", ofi_coll_mc);



    shmem_transport_ofi_CXI_coll_addr = fi_mc_addr(ofi_coll_mc);

    while (offset < len) {
        posted = 0;
        while (posted < max_inflight && offset < len) { 
            cur_count = len - offset;
            if (cur_count > chunk_elems)
                cur_count = chunk_elems;

            context_peers[posted] = &contexts[posted];
            if (shmem_my_pe() == PE_root){
                memcpy(&target[offset], &source[offset], cur_count * sz);
            }
            
            ret = fi_broadcast(ep, &target[offset], cur_count, NULL,
                    shmem_transport_ofi_CXI_coll_addr, 
                    shmem_transport_ofi_CXI_addr_table[PE_root],
                    dtype, 0L,
                    context_peers[posted]);
            if (ret == -FI_EAGAIN){
                if (posted > 0){
                    batch_polling_time(ctx, context_peers, posted, max_inflight);
                    posted = 0;
                    continue;
                }
                do {
                } while(cq_poll(ctx) == NULL);
            }
            OFI_CHECK_ERROR_MSG(ret, "Bcast failed: %d %s\n", ret, fi_strerror(ret));
            offset += cur_count;
            posted++;
        }
        if (posted > 0){
            batch_polling_time(ctx, context_peers, posted, max_inflight);
        }
//            ret = polling_time(ctx, &context);
//            OFI_CHECK_RETURN_STR(ret, "Polling failed\n");
    }



#endif /*if 0 for bcast */

}


void shmem_transport_coll_sync(int PE_start, int PE_stride, int PE_size, long *pSync){
    
    int ret = FI_SUCCESS;
    shmem_transport_ctx_t *ctx = &shmem_transport_ctx_default;
#if 1
    struct fid_ep *ep = ctx->CXI_ep;

    avset_ary_t setary;
    d_entry_t joinlist;
    uint64_t context;
    uint64_t mc; 
    ret = _simple_join(ctx, shmem_transport_ofi_CXI_addr_table, PE_size,
            &setary, &joinlist, PE_stride, PE_start);

    OFI_CHECK_ERROR_MSG(ret, "Failed to perform a join %d: %s\n", ret, fi_strerror(ret));

    mc = _simple_get_mc(&joinlist);

    PRINT_DEBUG("mc: 0x%lx\n", mc);
    OFI_CHECK_ERROR_MSG(mc == 0, "Failed to get the MC for barrier\n");

    ret = fi_barrier(ep, mc, &context);
    OFI_CHECK_ERROR_MSG(ret, "Failed to barrier: %d %s\n", ret, fi_strerror(ret));
    cq_wait(ctx, &context);

    avset_ary_destroy(&setary);


#else
    uint64_t done_flag = 0;

    struct fid_ep *ep = ctx->CXI_ep;
    PRINT_DEBUG("Starting join with addr_table %p, PE_start %d, PE_stride %d, PE_size %d\n",
            shmem_transport_ofi_CXI_addr_table, PE_start, PE_stride, PE_size);


    ret = initialize_avset(PE_start, PE_stride, PE_size);
    OFI_CHECK_ERROR_MSG(ret, "failed to initialize avset: %d %s", ret, fi_strerror(ret));


    PRINT_DEBUG("Starting collective join\n");
    ret = fi_join_collective(ep, FI_ADDR_NOTAVAIL,
                             shmem_transport_ofi_CXI_avfd_set,
                             0, &ofi_coll_mc, &done_flag);

    if (ret != FI_SUCCESS){
        PRINT_ERROR("collective join failed %d %s\n", ret, fi_strerror(ret));
    }

    //OFI_CHECK_RETURN_MSG(ret, "collective_join failed!! %d %s\n", ret, fi_strerror(ret));

    ret = wait_for_join(ctx, FI_JOIN_COMPLETE, &done_flag);
    OFI_CHECK_RETURN_STR(ret, "join_wait time failed\n");

    if (ofi_coll_mc == NULL){
        PRINT_ERROR("coll_mc is NULL\n");
        shmem_global_exit(-FI_EINVAL);
    }
//    PRINT_DEBUG("Coll_mc %p\n", ofi_coll_mc);

    polling_time(ctx, &done_flag);

    shmem_transport_ofi_CXI_coll_addr = fi_mc_addr(ofi_coll_mc);


    ret = fi_barrier(ep, shmem_transport_ofi_CXI_coll_addr, &done_flag);
    OFI_CHECK_ERROR_MSG(ret, "Barrier failed %d %s\n", ret, fi_strerror(ret));

    ret = polling_time(ctx, &done_flag);
    OFI_CHECK_RETURN_STR(ret, "Polling failed\n");
#endif

}

static int compare_nic_names(const void *f1, const void *f2)
{
    const struct fi_info **fabric1 = (const struct fi_info **) f1;
    const struct fi_info **fabric2 = (const struct fi_info **) f2;
    return strcmp((*fabric1)->nic->device_attr->name, (*fabric2)->nic->device_attr->name);
}

static inline
bool nic_already_used(struct fid_nic *nic, struct fi_info *fabrics, int num_nics)
{
    struct fi_info *cur_fabric = fabrics;
    for (int i = 0; i < num_nics; i++) {
        if (nic->bus_attr->bus_type == FI_BUS_PCI &&
            cur_fabric->nic->bus_attr->bus_type == FI_BUS_PCI) {
            struct fi_pci_attr nic_pci = nic->bus_attr->attr.pci;
            struct fi_pci_attr cur_fabric_pci = cur_fabric->nic->bus_attr->attr.pci;
            if (nic_pci.domain_id == cur_fabric_pci.domain_id && nic_pci.bus_id == cur_fabric_pci.bus_id &&
                nic_pci.device_id == cur_fabric_pci.device_id && nic_pci.function_id == cur_fabric_pci.function_id) {
                return true;
            }
        } else {
            if (strcmp(nic->device_attr->name, cur_fabric->nic->device_attr->name) == 0) {
                return true;
            }
        }

        cur_fabric = cur_fabric->next;
    }

    return false;
}


static inline
int query_for_fabric_collectives(struct fabric_info *info)
{
    int                 ret = 0;
    struct fi_info      hints = {0};
    struct fi_tx_attr   tx_attr = {0};
    struct fi_domain_attr domain_attr = {0};
    struct fi_fabric_attr fabric_attr = {0};
    struct fi_ep_attr   ep_attr = {0};

    //shmem_transport_ofi_max_buffered_send = sizeof(long double);

    fabric_attr.prov_name = strdup("cxi");

    PRINT_DEBUG("hints.caps is an OR of FI_MSG and FI_COLLECTIVE\n");
    hints.caps   = FI_MSG | FI_COLLECTIVE; //| FI_RMA |     /* request rma capability
                            //       implies FI_READ/WRITE FI_REMOTE_READ/WRITE */
                 //  FI_ATOMIC;  /* request atomics capability */
   // hints.caps  |= FI_COLLECTIVE; /* Requesting collective support for MR's and EP's */
#if ENABLE_TARGET_CNTR
//    hints.caps |= FI_RMA_EVENT; /* want to use remote counters */
#endif /* ENABLE_TARGET_CNTR */
#ifdef USE_FI_FENCE
    hints.caps |= FI_FENCE;     /* request fence capability; FI_FENCE adds
                                   ordering semantics to fi_atomicmsg
                                   for put with signal implementation */
#endif
#ifdef USE_FI_HMEM
    hints.caps |= FI_HMEM;
#endif
   // hints.addr_format         = FI_FORMAT_UNSPEC;
#ifdef ENABLE_FI_MANUAL_PROGRESS
    domain_attr.data_progress = FI_PROGRESS_MANUAL;
#else
    domain_attr.data_progress = FI_PROGRESS_AUTO;
#endif
    domain_attr.resource_mgmt = FI_RM_ENABLED;
#ifdef ENABLE_MR_SCALABLE
                                /* Scalable, offset-based addressing, formerly FI_MR_SCALABLE */
    domain_attr.mr_mode       = 0;
#  if !defined(ENABLE_HARD_POLLING) && defined(ENABLE_MR_RMA_EVENT)
   // domain_attr.mr_mode       = FI_MR_RMA_EVENT; /* can support RMA_EVENT on MR */
#  endif
#else
                                /* Portable, absolute addressing, formerly FI_MR_BASIC */
    domain_attr.mr_mode       = FI_MR_ENDPOINT | FI_COLLECTIVE ; //FI_MR_VIRT_ADDR | FI_MR_ALLOCATED | FI_MR_PROV_KEY;
    PRINT_DEBUG("MR Domains ALSO include FI_MR_ENDPOINT | FI_COLLECTIVE\n");
#endif
#ifdef ENABLE_MR_ENDPOINT
    domain_attr.mr_mode |= FI_MR_ENDPOINT | FI_COLLECTIVE;
#endif
#ifdef USE_FI_HMEM
    domain_attr.mr_mode |= FI_MR_HMEM;
#endif
#if !defined(ENABLE_MR_SCALABLE) || !defined(ENABLE_REMOTE_VIRTUAL_ADDRESSING)
//    domain_attr.mr_key_size   = 1; /* Heap and data use different MR keys, need
//                                      at least 1 byte */
#endif
#ifdef ENABLE_THREADS
    if (shmem_internal_thread_level == SHMEM_THREAD_MULTIPLE) {
#ifdef USE_THREAD_COMPLETION
        domain_attr.threading = FI_THREAD_COMPLETION;
#else
        domain_attr.threading = FI_THREAD_SAFE;
#endif /* USE_THREAD_COMPLETION */
    } else
        domain_attr.threading = FI_THREAD_DOMAIN;
#else
    domain_attr.threading     = FI_THREAD_DOMAIN;
#endif

    hints.domain_attr         = &domain_attr;
  //  ep_attr.type              = FI_EP_RDM; /* reliable connectionless */
  //  ep_attr.tx_ctx_cnt        = 0;
    hints.fabric_attr         = &fabric_attr;
 //   tx_attr.op_flags          = FI_DELIVERY_COMPLETE;
    tx_attr.inject_size       = shmem_transport_ofi_max_buffered_send; /* require provider to support this as a min */
    hints.tx_attr             = &tx_attr; /* TODO: fill tx_attr */
    hints.rx_attr             = NULL;
    hints.ep_attr             = &ep_attr;

    /* find fabric provider to use that is able to support RMA and ATOMIC */

    PRINT_DEBUG("Getting info for collective OFI things\n");
ret = fi_getinfo( FI_VERSION(OFI_MAJOR_VERSION, OFI_MINOR_VERSION),
                      NULL, NULL, 0, &hints, &(info->fabrics));

    OFI_CHECK_RETURN_MSG(ret, "OFI transport did not find any valid fabric services "
                              "(provider=%s)\n",
                              info->prov_name != NULL ? info->prov_name : "<auto>");

    /* If the user supplied a fabric or domain name, use it to select the
     * fabrics that may be chosen. Otherwise, consider all available
     * fabrics */
    int num_nics = 0;
    struct fi_info *fallback = NULL;
    struct fi_info *fabrics_list_head = NULL;
    struct fi_info *fabrics_list_tail = NULL;
    struct fi_info *multirail_fabric_list_head = NULL;
    struct fi_info *multirail_fabric_list_tail = NULL;

    if (info->fabric_name != NULL || info->domain_name != NULL) {
        struct fi_info *cur_fabric;

        for (cur_fabric = info->fabrics; cur_fabric; cur_fabric = cur_fabric->next) {
            if (info->fabric_name == NULL ||
                fnmatch(info->fabric_name, cur_fabric->fabric_attr->name, 0) == 0) {
                if (info->domain_name == NULL ||
                    fnmatch(info->domain_name, cur_fabric->domain_attr->name, 0) == 0) {
                    if (!fabrics_list_head) fabrics_list_head = cur_fabric;
                    if (fabrics_list_tail) fabrics_list_tail->next = cur_fabric;
                    fabrics_list_tail = cur_fabric;
                }
            }
        }
        if (fabrics_list_tail) fabrics_list_tail->next = NULL;
    }
    else {
        fabrics_list_head = info->fabrics;
    }

    info->p_info = NULL;

    if (shmem_internal_params.OFI_DISABLE_MULTIRAIL) {
        info->p_info = fabrics_list_head;
    }
    else {
        /* Generate a linked list of all fabrics with a non-null nic value */
        for (struct fi_info *cur_fabric = fabrics_list_head; cur_fabric; cur_fabric = cur_fabric->next) {
            if (!fallback) fallback = cur_fabric;
            if (cur_fabric->nic && !nic_already_used(cur_fabric->nic, multirail_fabric_list_head, num_nics)) {
                num_nics += 1;
                if (!multirail_fabric_list_head) multirail_fabric_list_head = cur_fabric;
                if (multirail_fabric_list_tail) multirail_fabric_list_tail->next = cur_fabric;
                multirail_fabric_list_tail = cur_fabric;
            }
        }
        if (multirail_fabric_list_tail) multirail_fabric_list_tail->next = NULL;

        if (num_nics == 0) {
            info->p_info = fallback;
        }
        else {
            int idx = 0;
            struct fi_info **prov_list = (struct fi_info **) malloc(num_nics * sizeof(struct fi_info *));
            for (struct fi_info *cur_fabric = multirail_fabric_list_head; cur_fabric; cur_fabric = cur_fabric->next) {
                prov_list[idx++] = cur_fabric;
            }
            qsort(prov_list, num_nics, sizeof(struct fi_info *), compare_nic_names);
#ifdef USE_HWLOC
            info->p_info = assign_nic_with_hwloc(info->p_info, prov_list, num_nics);
#else
            /* Round-robin assignment of NICs to PEs
             * FIXME: A more suitable indexing value would be
             * shmem_team_my_pe(SHMEM_TEAM_NODE) % num_nics, but it is too early in initialization to
             * do that here. We would also want to replace the similar occurrences in the
             * assign_nic_with_hwloc function. */
            info->p_info = prov_list[shmem_internal_my_pe % num_nics];
#endif
            free(prov_list);
        }
    }
    if (NULL == info->p_info) {
        RAISE_WARN_MSG("OFI transport, no valid fabric (prov=%s, fabric=%s, domain=%s)\n",
                       info->prov_name != NULL ? info->prov_name : "<auto>",
                       info->fabric_name != NULL ? info->fabric_name : "<auto>",
                       info->domain_name != NULL ? info->domain_name : "<auto>");
        return ret;
    }

    if (info->p_info->ep_attr->max_msg_size > 0) {
        shmem_transport_ofi_max_msg_size = info->p_info->ep_attr->max_msg_size;
    } else {
        RAISE_WARN_STR("OFI provider did not set max_msg_size");
        return 1;
    }

  //   Check if the domain supports STXs 
    if (info->p_info->domain_attr->max_ep_stx_ctx == 0) {
        shmem_transport_ofi_stx_max = 0;
    }

#if defined(ENABLE_MR_SCALABLE) && defined(ENABLE_REMOTE_VIRTUAL_ADDRESSING)
    /* Only use a single MR, no keys required */
    info->p_info->domain_attr->mr_key_size = 0;
#else
    /* Heap and data use different MR keys, need at least 1 byte of key space
     * if using provider selected keys */
    if (info->p_info->domain_attr->mr_mode & FI_MR_PROV_KEY)
        info->p_info->domain_attr->mr_key_size = 1;
    else
        info->p_info->domain_attr->mr_key_size = 0;
#endif

#ifndef DISABLE_OFI_INJECT
    shmem_internal_assertp(info->p_info->tx_attr->inject_size >= shmem_transport_ofi_max_buffered_send);
    shmem_transport_ofi_max_buffered_send = info->p_info->tx_attr->inject_size;
#else
    shmem_transport_ofi_max_buffered_send = 0;
#endif

#ifdef ENABLE_MR_RMA_EVENT
    shmem_transport_ofi_mr_rma_event = (info->p_info->domain_attr->mr_mode & FI_MR_RMA_EVENT) != 0;
#endif

    DEBUG_MSG("OFI provider: %s, fabric: %s, domain: %s, mr_mode: 0x%x\n"
              RAISE_PE_PREFIX "max_inject: %zu, max_msg: %zu, stx: %s, stx_max: %ld, num_nics: %d\n",
              info->p_info->fabric_attr->prov_name,
              info->p_info->fabric_attr->name, info->p_info->domain_attr->name,
              info->p_info->domain_attr->mr_mode,
              shmem_internal_my_pe,
              shmem_transport_ofi_max_buffered_send,
              shmem_transport_ofi_max_msg_size,
              info->p_info->domain_attr->max_ep_stx_ctx == 0 ? "no" : "yes",
              shmem_transport_ofi_stx_max,
              num_nics);

    return ret;
}


static void get_local_nic(shmem_transport_ctx_t *ctx, int hsn, nic_addr_t *nic){
    char fname[nicname_len];
    char text[nicname_len];
    char *ptr = NULL;
    FILE *fid = NULL;
    int i = 0, n = 0;

    /* 6 bytes --> length of 6 below as a magic number */
    strcpy(text, "FF:FF:FF:FF:FF:FF\n");
    snprintf(fname, sizeof(fname), "/sys/class/net/hsn%d/address", hsn);

    if ((fid = fopen(fname, "r"))) {
        n = fread(text, 1, sizeof(text), fid);
        fclose(fid);
        text[n] = 0; /* NULL termination is important */
      }else{
        PRINT_ERROR("Failed to get nic address fname %s\n", fname);
        memset(NULL, 0, 10);
    }

    PRINT_DEBUG("HSN addr: %s\n", text);

    nic->value = 0L;
    ptr = text;
    for (i = 0; i< 6; i++){
        nic->value <<= 8;
        nic->value |= strtol(ptr, &ptr, 16);
        ptr++;
    }

    nic->hsn = hsn;
    nic->rank = shmem_internal_my_pe;
    PRINT_DEBUG("NIC hsn=%d rank=%3d nic=%05x\n", nic->hsn, shmem_internal_my_pe, nic->nic);

}

static inline
int allocate_fabric_resources(struct fabric_info *info)
{
    int ret = 0;
    struct fi_av_attr   av_attr = {0}; 

    if (info == &shmem_transport_ofi_info){
        /* fabric domain: define domain of resources physical and logical */
        ret = fi_fabric(info->p_info->fabric_attr, &shmem_transport_ofi_fabfd, NULL);
        OFI_CHECK_RETURN_STR(ret, "fabric initialization failed");

        DEBUG_MSG("OFI version: built %"PRIu32".%"PRIu32", cur. %"PRIu32".%"PRIu32"; "
                "provider version: %"PRIu32".%"PRIu32"\n",
                FI_MAJOR_VERSION, FI_MINOR_VERSION,
                FI_MAJOR(fi_version()), FI_MINOR(fi_version()),
                FI_MAJOR(info->p_info->fabric_attr->prov_version),
                FI_MINOR(info->p_info->fabric_attr->prov_version));

        if (FI_MAJOR_VERSION != FI_MAJOR(fi_version()) ||
                FI_MINOR_VERSION != FI_MINOR(fi_version())) {
            RAISE_WARN_MSG("OFI version mismatch: built %"PRIu32".%"PRIu32", cur. %"PRIu32".%"PRIu32"\n",
                    FI_MAJOR_VERSION, FI_MINOR_VERSION,
                    FI_MAJOR(fi_version()), FI_MINOR(fi_version()));
        }

        /* access domain: define communication resource limits/boundary within
         * fabric domain */
        ret = fi_domain(shmem_transport_ofi_fabfd, info->p_info,
                &shmem_transport_ofi_domainfd,NULL);
        OFI_CHECK_RETURN_STR(ret, "domain initialization failed");

        /* AV table set-up for PE mapping */

#ifdef USE_AV_MAP
        av_attr.type = FI_AV_MAP;
        addr_table   = (fi_addr_t*) malloc(info->npes * sizeof(fi_addr_t));
#else
        /* open Address Vector and bind the AV to the domain */
        av_attr.type = FI_AV_TABLE;
        addr_table   = NULL;
#endif

        av_attr.count=1024;
        av_attr.rx_ctx_bits = 0;

        ret = fi_av_open(shmem_transport_ofi_domainfd,
                &av_attr,
                &shmem_transport_ofi_avfd,
                NULL);
        OFI_CHECK_RETURN_STR(ret, "AV creation failed");
    }else{

        PRINT_DEBUG("Creating the fabric\n");
        ret = fi_fabric(info->p_info->fabric_attr, &shmem_transport_ofi_CXI_fabfd, NULL);
        OFI_CHECK_RETURN_STR(ret, "coll_fabric initialization failed");

        DEBUG_MSG("OFI version: built %"PRIu32".%"PRIu32", cur. %"PRIu32".%"PRIu32"; "
                "provider version: %"PRIu32".%"PRIu32"\n",
                FI_MAJOR_VERSION, FI_MINOR_VERSION,
                FI_MAJOR(fi_version()), FI_MINOR(fi_version()),
                FI_MAJOR(info->p_info->fabric_attr->prov_version),
                FI_MINOR(info->p_info->fabric_attr->prov_version));

        if (FI_MAJOR_VERSION != FI_MAJOR(fi_version()) ||
                FI_MINOR_VERSION != FI_MINOR(fi_version())) {
            RAISE_WARN_MSG("OFI version mismatch: built %"PRIu32".%"PRIu32", cur. %"PRIu32".%"PRIu32"\n",
                    FI_MAJOR_VERSION, FI_MINOR_VERSION,
                    FI_MAJOR(fi_version()), FI_MINOR(fi_version()));
        }

        PRINT_DEBUG("Creating FI_domain\n");
        ret = fi_domain(shmem_transport_ofi_fabfd, info->p_info,
                &shmem_transport_ofi_CXI_domain_fd, NULL);
        OFI_CHECK_RETURN_STR(ret, "domain initialization failed");

        struct fi_av_attr av_attr = {};
        av_attr.type = FI_AV_TABLE;
        av_attr.rx_ctx_bits = 0;


        ret = fi_av_open(shmem_transport_ofi_CXI_domain_fd, &av_attr,
                &shmem_transport_ofi_CXI_avfd, NULL);
        OFI_CHECK_RETURN_MSG(ret, "CXI av init failed %d %s\n", ret, fi_strerror(errno));

    }

    return ret;
}

#define FAIL(cond, msg, label) \
    if (cond) { \
        PRINT_ERROR( "FAIL socket %s=%d\n", msg, cond); \
        goto label; \
    }


static ssize_t _fullread(int fd, char *ptr, ssize_t size)
{
    ssize_t rem = size;
    ssize_t len;

    while (rem > 0) {
        len = read(fd, ptr, rem);
        if (len < 0)
            return len;
        ptr += len;
        rem -= len;
    }
    return size;
}

static ssize_t _fullwrite(int fd, char *ptr, ssize_t size)
{
    ssize_t rem = size;
    ssize_t len;

    while (rem > 0) {
        len = write(fd, ptr, rem);
        if (len < 0)
            return len;
        ptr += len;
        rem -= len;
    }
    return size;
}

int socket_accept(shmem_transport_ctx_t *ctx, int portno, size_t size, void *data, void *rslt)
{
    int listenfd = 0;
    int *connfd, conncnt, connidx;
    struct sockaddr_in serv_addr = { 0 };
    char *rsltp;
    size_t siz;
    ssize_t len;
    int error, ret;

    error = -1;

    listenfd = socket(AF_INET, SOCK_STREAM, 0);
    FAIL(listenfd < 0, "socket", lablisten);
    ret = setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR,
            &(int){1}, sizeof(int));
    FAIL(ret < 0, "reuseaddr", lablisten);
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons(portno);
    ret = bind(listenfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr));
    FAIL(ret < 0, "bind", lablisten);

    conncnt = shmem_internal_num_pes - 1;
    ret = listen(listenfd, conncnt);
    FAIL(ret < 0, "listen", lablisten);

    connfd = calloc(conncnt, sizeof(*connfd));
    FAIL(!connfd, "connfd", lablisten);

    for (connidx = 0; connidx < conncnt; connidx++)
        connfd[connidx] = -1;

    rsltp = rslt;
    memcpy(rsltp, data, size);
    rsltp += size;

    for (connidx = 0; connidx < conncnt; connidx++) {
        int fd;

        fd = accept(listenfd, (struct sockaddr *)NULL, NULL);
        FAIL(fd < 0, "accept", labclose);

        connfd[connidx] = fd;

        siz = size;
        len = _fullread(fd, rsltp, siz);
        FAIL(len < siz, "read", labclose);
        rsltp += siz;
    }

    for (connidx = 0; connidx < conncnt; connidx++)
    {
        int fd;

        fd = connfd[connidx];
        siz = shmem_internal_num_pes * size;
        len = _fullwrite(fd, rsltp, siz);
        FAIL(len < siz, "write issue", labclose);
    }

    error = 0;
labclose:
    for (connidx = 0; connidx < conncnt ; connidx++)
        close(connfd[connidx]);
    free(connfd);
lablisten:
    close(listenfd);
    return error;
}



int socket_connect(shmem_transport_ctx_t *ctx, int portno, size_t size, void *data, void *rslt)
{
    int connfd = 0;
    struct sockaddr_in serv_addr = { 0 };
    struct hostent *he;
    struct in_addr **addr_list;
    size_t siz;
    ssize_t len;
    int error, ret;

    error = -1;

	connfd = socket(AF_INET, SOCK_STREAM, 0);
	FAIL(connfd < 0, "socket", labclose);

    ret = setsockopt(connfd, SOL_SOCKET, SO_REUSEADDR,
            &(int){1}, sizeof(int));
    if (ret < 0 ){
        PRINT_ERROR("ERROR in sockets: %d %s\n", ret, fi_strerror(errno));
        goto labclose;
    }

    he = gethostbyname(ctx->node_0);
    if (he == NULL){
        PRINT_ERROR("gethostbyname: %s\n", ctx->node_0);
        goto labclose;
    }
    //FAIL(!he, "gethostbyname", labclose);

    addr_list = (struct in_addr **)he->h_addr_list;
    if (!addr_list){
        PRINT_ERROR("Gethostbyname empty\n");
        goto labclose;
    }
    //FAIL(!addr_list, "gethostbyname empty", labclose);

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(portno);
    serv_addr.sin_addr = *addr_list[0];
    do {
        usleep(1000);
        ret = connect(connfd, (struct sockaddr *)&serv_addr,
                sizeof(serv_addr));
    } while (ret < 0);
    siz = size;
    len = _fullwrite(connfd, data, siz);
    if(len < siz){
        PRINT_ERROR("write\n");
        goto labclose;
    }

    siz = shmem_internal_num_pes * size;
    len = _fullread(connfd, rslt, siz);
    if (len < siz){
        PRINT_ERROR("fulllread failed\n");
        goto labclose;
    }
    //FAIL(len < siz, "read", labclose);

    // report success
    error = 0;

labclose:
    close(connfd);
    return error;
}
    
    
static int socket_allgather (shmem_transport_ctx_t *ctx, size_t size, void *data, void *res){

    int portno = 5000;


    return (!strcmp(ctx->node_0, ctx->nodename)) ? socket_accept(ctx, portno, size, data, res)
        : socket_connect(ctx, portno, size, data, res); ;
}





int shmem_collective_nic_initialization(void){

    shmem_transport_ctx_t *ctx = &shmem_transport_ctx_default;
    int err = FI_SUCCESS, i = 0, local_size = 0;
    internal_addr_t *alladdrs = NULL;
    char *alladdrs2 = NULL;
    local_size = ctx->nics_per_rank * NICSIZE;
    if (ctx->NIC_array){
        return FI_SUCCESS;
    }

    ctx->num_nics = shmem_internal_num_pes * ctx->nics_per_rank;
    ctx->eq = shmem_transport_ofi_CXI_eq;


    ctx->NIC_array = malloc(shmem_internal_num_pes * local_size);
    if (ctx->NIC_array == NULL){
        err = -FI_ENOMEM;
        goto fail;
    }
    nic_addr_t *local_nics = calloc(1, local_size);
    if (local_nics == NULL){
        err = -FI_ENOMEM;
        goto fail;
    }

    err = -FI_ENOMEM;
    alladdrs = malloc(shmem_internal_num_pes * sizeof(internal_addr_t));
    if (alladdrs == NULL){
        err = -FI_ENOMEM;
        goto fail;
    }

    alladdrs2 = malloc(shmem_internal_num_pes * shmem_transport_ofi_addrlen);
    if (alladdrs2 == NULL){
        err = -FI_ENOMEM;
        goto fail;
    }



    shmem_transport_ofi_CXI_addr_table = malloc(shmem_internal_num_pes * sizeof(fi_addr_t));
    if (shmem_transport_ofi_CXI_addr_table == NULL){
        PRINT_ERROR("NO MEMORY\n");
        return -FI_ENOMEM;
    }

    PRINT_DEBUG("Fetching local_NIC\n");

    err = -FI_EFAULT;
    PRINT_DEBUG(" local_size %d, NICSIZE %lu, ctx->nics_per_rank: %d\n", local_size, NICSIZE, ctx->nics_per_rank);
    for (i = 0 ; i < ctx->nics_per_rank; i++){
        get_local_nic(ctx, i, &local_nics[i]);
    }

    for (i = 0; i < ctx->num_nics; i++){
        PRINT_DEBUG("LOCAL rank=%2d hsn=%d nic=%05x\n",
                local_nics[0].rank,
                local_nics[0].hsn,
                local_nics[0].nic);
    }


    PRINT_DEBUG("Local NICs retrieved\n");


 //   err = socket_allgather(ctx, local_size, local_nics, ctx->NIC_array);
 //   OFI_CHECK_RETURN_MSG(err, "failed to perform a socket-based allgather on the NICS %d %s\n", err, fi_strerror(err));

    nic_addr_t *shmem_nics = shmem_malloc(shmem_internal_num_pes* local_size);
    nic_addr_t *shmem_nics_2 = shmem_malloc(local_size);
    memcpy(shmem_nics_2, local_nics, local_size);
    PRINT_DEBUG("Beginning shmem collect\n");
    shmem_fcollectmem(SHMEM_TEAM_WORLD, shmem_nics, shmem_nics_2, local_size); 
    PRINT_DEBUG("SHMEM Collect worked\n");
    memcpy(ctx->NIC_array, shmem_nics, local_size*shmem_internal_num_pes);
    PRINT_DEBUG("Memcpy worked\n");
    shmem_free(shmem_nics_2);
    shmem_free(shmem_nics);
    shmem_nics_2 = NULL;
    shmem_nics = NULL;


    for (i = 0; i < ctx->num_nics ; i++){
        PRINT_DEBUG("Pre-sort ctx i %d rank=%2d hsn=%d nic=%05x\n",
                i, ctx->NIC_array[i].rank,
                ctx->NIC_array[i].hsn,
                ctx->NIC_array[i].nic);
    }


    PRINT_DEBUG("Sorting NICs\n");
    qsort(ctx->NIC_array, ctx->num_nics, NICSIZE, _compare);

    for (i = 0; i < ctx->num_nics; i++){
        PRINT_DEBUG("post-sort ctx i %d rank=%2d hsn=%d nic=%05x\n",
                i, ctx->NIC_array[i].rank,
                ctx->NIC_array[i].hsn,
                ctx->NIC_array[i].nic);
        ctx->NIC_array[i].rank = i;
    }

    PRINT_DEBUG("Starting to add NIC addresses\n");
    for (i = 0; i < ctx->num_nics; i++){
        alladdrs[i].nic = ctx->NIC_array[i].nic;
        alladdrs[i].pid = 1;
    }

    for (i = 0; i < ctx->num_nics; i++){
        PRINT_DEBUG("alladdrs[i=%d] rank=%2d nic=%05x\n",
                i, alladdrs[i].pid,
                alladdrs[i].nic);
    }

    PRINT_DEBUG("Domain set up\n");

    err = fi_open_ops(&shmem_transport_ofi_CXI_domain_fd->fid, FI_CXI_DOM_OPS_1, 0,
            (void **)&cxi_dom_ops, NULL);
    OFI_CHECK_RETURN_STR(err, "CXI dom_op_1 failed\n");
    err = fi_open_ops(&shmem_transport_ofi_CXI_domain_fd->fid, FI_CXI_DOM_OPS_2, 0,
            (void **)&cxi_dom_ops, NULL);
    OFI_CHECK_RETURN_STR(err, "CXI dom_op_2 failed\n");
    err = fi_open_ops(&shmem_transport_ofi_CXI_domain_fd->fid, FI_CXI_DOM_OPS_3, 0,
            (void **)&cxi_dom_ops, NULL);
    OFI_CHECK_RETURN_STR(err, "CXI dom_op_3 failed\n");

    err = fi_set_ops(&shmem_transport_ofi_CXI_domain_fd->fid, FI_SET_OPS_HMEM_OVERRIDE, 0,
            &cxit_hmem_ops, NULL);

    OFI_CHECK_RETURN_STR(err, "Fi_set_ops failed\n");

    if (shmem_transport_ofi_CXI_avfd == NULL){
        PRINT_ERROR("avfd is null!!\n");
        goto fail;
    }

   ctx->rx_cq = shmem_transport_ofi_CXI_recv_cq;
   ctx->coll_tx_cq = shmem_transport_ofi_CXI_target_cq; 
     

    PRINT_DEBUG("shmem_transport_ofi_CXI_avfd: %p\n", shmem_transport_ofi_CXI_avfd);

    err = fi_av_insert(shmem_transport_ofi_CXI_avfd, alladdrs, shmem_internal_num_pes,
            shmem_transport_ofi_CXI_addr_table, 0, NULL);
    if (err != shmem_internal_num_pes){
        PRINT_ERROR("Failed to insert all addresses: %d\n", err);
        goto fail;
    }

    free(alladdrs);
    free(alladdrs2);


 //   fi_addr_t myaddr;
 //   size_t caddrlen;
 //   internal_addr_t internal_addr;
 //   myaddr = shmem_transport_ofi_CXI_addr_table[shmem_internal_my_pe];
 //   PRINT_DEBUG("my_addr 0x%lx\n", myaddr);
 //   err = fi_av_lookup(shmem_transport_ofi_CXI_avfd, myaddr, &internal_addr, &caddrlen);
 //   OFI_CHECK_RETURN_STR(err, "fi_av_lookup test failed\n");
 //
 //   PRINT_DEBUG("my_addr 0x%lx caddr %05x\n", myaddr, internal_addr.nic);

    return FI_SUCCESS;

fail:
    ctx->num_nics = 0;
    if (ctx->NIC_array) free(ctx->NIC_array);
    if (local_nics) free(local_nics);
    PRINT_ERROR("FAILED nic initialization: %d\n", err);
    return err;
}


static inline
int query_for_fabric(struct fabric_info *info)
{
    int                 ret = 0;
    struct fi_info      hints = {0};
    struct fi_tx_attr   tx_attr = {0};
    struct fi_domain_attr domain_attr = {0};
    struct fi_fabric_attr fabric_attr = {0};
    struct fi_ep_attr   ep_attr = {0};

    shmem_transport_ofi_max_buffered_send = sizeof(long double);

    fabric_attr.prov_name = info->prov_name;

    hints.caps   = FI_MSG| FI_RMA | FI_ATOMIC ; //| FI_COLLECTIVE; //| FI_RMA |     /* request rma capability
                           //        implies FI_READ/WRITE FI_REMOTE_READ/WRITE */
                //   FI_ATOMIC;  /* request atomics capability */
//    hints.caps  |= FI_COLLECTIVE; /* Requesting collective support for MR's and EP's */
#if ENABLE_TARGET_CNTR
//    hints.caps |= FI_RMA_EVENT; /* want to use remote counters */
#endif /* ENABLE_TARGET_CNTR */
#ifdef USE_FI_FENCE
    hints.caps |= FI_FENCE;     /* request fence capability; FI_FENCE adds
                                   ordering semantics to fi_atomicmsg
                                   for put with signal implementation */
#endif
#ifdef USE_FI_HMEM
    hints.caps |= FI_HMEM;
#endif
    hints.addr_format         = FI_FORMAT_UNSPEC;
#ifdef ENABLE_FI_MANUAL_PROGRESS
    domain_attr.data_progress = FI_PROGRESS_MANUAL;
#else
    domain_attr.data_progress = FI_PROGRESS_AUTO;
#endif
    domain_attr.resource_mgmt = FI_RM_ENABLED;
#ifdef ENABLE_MR_SCALABLE
                                /* Scalable, offset-based addressing, formerly FI_MR_SCALABLE */
    domain_attr.mr_mode       = 0;
#  if !defined(ENABLE_HARD_POLLING) && defined(ENABLE_MR_RMA_EVENT)
   // domain_attr.mr_mode       = FI_MR_RMA_EVENT; /* can support RMA_EVENT on MR */
#  endif
#else
                                /* Portable, absolute addressing, formerly FI_MR_BASIC */
    domain_attr.mr_mode       = FI_MR_VIRT_ADDR | FI_MR_ALLOCATED | FI_MR_PROV_KEY ;
#endif
#ifdef ENABLE_MR_ENDPOINT
    domain_attr.mr_mode |= FI_MR_ENDPOINT;
#endif
#ifdef USE_FI_HMEM
    domain_attr.mr_mode |= FI_MR_HMEM;
#endif
#if !defined(ENABLE_MR_SCALABLE) || !defined(ENABLE_REMOTE_VIRTUAL_ADDRESSING)
    domain_attr.mr_key_size   = 1; /* Heap and data use different MR keys, need
                                      at least 1 byte */
#endif
#ifdef ENABLE_THREADS
    if (shmem_internal_thread_level == SHMEM_THREAD_MULTIPLE) {
#ifdef USE_THREAD_COMPLETION
        domain_attr.threading = FI_THREAD_COMPLETION;
#else
        domain_attr.threading = FI_THREAD_SAFE;
#endif /* USE_THREAD_COMPLETION */
    } else
        domain_attr.threading = FI_THREAD_DOMAIN;
#else
    domain_attr.threading     = FI_THREAD_DOMAIN;
#endif

    hints.domain_attr         = &domain_attr;
    ep_attr.type              = FI_EP_RDM; /* reliable connectionless */
    ep_attr.tx_ctx_cnt        = 0;
    hints.fabric_attr         = &fabric_attr;
    tx_attr.op_flags          = FI_DELIVERY_COMPLETE;
    tx_attr.inject_size       = shmem_transport_ofi_max_buffered_send; /* require provider to support this as a min */
    hints.tx_attr             = &tx_attr; /* TODO: fill tx_attr */
    hints.rx_attr             = NULL;
    hints.ep_attr             = &ep_attr;

    /* find fabric provider to use that is able to support RMA and ATOMIC */
    ret = fi_getinfo( FI_VERSION(OFI_MAJOR_VERSION, OFI_MINOR_VERSION),
                      NULL, NULL, 0, &hints, &(info->fabrics));

    OFI_CHECK_RETURN_MSG(ret, "OFI transport did not find any valid fabric services "
                              "(provider=%s)\n",
                              info->prov_name != NULL ? info->prov_name : "<auto>");

    /* If the user supplied a fabric or domain name, use it to select the
     * fabrics that may be chosen. Otherwise, consider all available
     * fabrics */
    int num_nics = 0;
    struct fi_info *fallback = NULL;
    struct fi_info *fabrics_list_head = NULL;
    struct fi_info *fabrics_list_tail = NULL;
    struct fi_info *multirail_fabric_list_head = NULL;
    struct fi_info *multirail_fabric_list_tail = NULL;

    if (info->fabric_name != NULL || info->domain_name != NULL) {
        struct fi_info *cur_fabric;

        for (cur_fabric = info->fabrics; cur_fabric; cur_fabric = cur_fabric->next) {
            if (info->fabric_name == NULL ||
                fnmatch(info->fabric_name, cur_fabric->fabric_attr->name, 0) == 0) {
                if (info->domain_name == NULL ||
                    fnmatch(info->domain_name, cur_fabric->domain_attr->name, 0) == 0) {
                    if (!fabrics_list_head) fabrics_list_head = cur_fabric;
                    if (fabrics_list_tail) fabrics_list_tail->next = cur_fabric;
                    fabrics_list_tail = cur_fabric;
                }
            }
        }
        if (fabrics_list_tail) fabrics_list_tail->next = NULL;
    }
    else {
        fabrics_list_head = info->fabrics;
    }

    info->p_info = NULL;

    if (shmem_internal_params.OFI_DISABLE_MULTIRAIL) {
        info->p_info = fabrics_list_head;
    }
    else {
        /* Generate a linked list of all fabrics with a non-null nic value */
        for (struct fi_info *cur_fabric = fabrics_list_head; cur_fabric; cur_fabric = cur_fabric->next) {
            if (!fallback) fallback = cur_fabric;
            if (cur_fabric->nic && !nic_already_used(cur_fabric->nic, multirail_fabric_list_head, num_nics)) {
                num_nics += 1;
                if (!multirail_fabric_list_head) multirail_fabric_list_head = cur_fabric;
                if (multirail_fabric_list_tail) multirail_fabric_list_tail->next = cur_fabric;
                multirail_fabric_list_tail = cur_fabric;
            }
        }
        if (multirail_fabric_list_tail) multirail_fabric_list_tail->next = NULL;

        if (num_nics == 0) {
            info->p_info = fallback;
        }
        else {
            int idx = 0;
            struct fi_info **prov_list = (struct fi_info **) malloc(num_nics * sizeof(struct fi_info *));
            for (struct fi_info *cur_fabric = multirail_fabric_list_head; cur_fabric; cur_fabric = cur_fabric->next) {
                prov_list[idx++] = cur_fabric;
            }
            qsort(prov_list, num_nics, sizeof(struct fi_info *), compare_nic_names);
#ifdef USE_HWLOC
            info->p_info = assign_nic_with_hwloc(info->p_info, prov_list, num_nics);
#else
            /* Round-robin assignment of NICs to PEs
             * FIXME: A more suitable indexing value would be
             * shmem_team_my_pe(SHMEM_TEAM_NODE) % num_nics, but it is too early in initialization to
             * do that here. We would also want to replace the similar occurrences in the
             * assign_nic_with_hwloc function. */
            info->p_info = prov_list[shmem_internal_my_pe % num_nics];
#endif
            free(prov_list);
        }
    }
    if (NULL == info->p_info) {
        RAISE_WARN_MSG("OFI transport, no valid fabric (prov=%s, fabric=%s, domain=%s)\n",
                       info->prov_name != NULL ? info->prov_name : "<auto>",
                       info->fabric_name != NULL ? info->fabric_name : "<auto>",
                       info->domain_name != NULL ? info->domain_name : "<auto>");
        return ret;
    }

    if (info->p_info->ep_attr->max_msg_size > 0) {
        shmem_transport_ofi_max_msg_size = info->p_info->ep_attr->max_msg_size;
    } else {
        RAISE_WARN_STR("OFI provider did not set max_msg_size");
        return 1;
    }

    /* Check if the domain supports STXs */
    if (info->p_info->domain_attr->max_ep_stx_ctx == 0) {
        shmem_transport_ofi_stx_max = 0;
    }

#if defined(ENABLE_MR_SCALABLE) && defined(ENABLE_REMOTE_VIRTUAL_ADDRESSING)
    /* Only use a single MR, no keys required */
    info->p_info->domain_attr->mr_key_size = 0;
#else
    /* Heap and data use different MR keys, need at least 1 byte of key space
     * if using provider selected keys */
    if (info->p_info->domain_attr->mr_mode & FI_MR_PROV_KEY)
        info->p_info->domain_attr->mr_key_size = 1;
    else
        info->p_info->domain_attr->mr_key_size = 0;
#endif

#ifndef DISABLE_OFI_INJECT
    shmem_internal_assertp(info->p_info->tx_attr->inject_size >= shmem_transport_ofi_max_buffered_send);
    shmem_transport_ofi_max_buffered_send = info->p_info->tx_attr->inject_size;
#else
    shmem_transport_ofi_max_buffered_send = 0;
#endif

#ifdef ENABLE_MR_RMA_EVENT
    shmem_transport_ofi_mr_rma_event = (info->p_info->domain_attr->mr_mode & FI_MR_RMA_EVENT) != 0;
#endif

    DEBUG_MSG("OFI provider: %s, fabric: %s, domain: %s, mr_mode: 0x%x\n"
              RAISE_PE_PREFIX "max_inject: %zu, max_msg: %zu, stx: %s, stx_max: %ld, num_nics: %d\n",
              info->p_info->fabric_attr->prov_name,
              info->p_info->fabric_attr->name, info->p_info->domain_attr->name,
              info->p_info->domain_attr->mr_mode,
              shmem_internal_my_pe,
              shmem_transport_ofi_max_buffered_send,
              shmem_transport_ofi_max_msg_size,
              info->p_info->domain_attr->max_ep_stx_ctx == 0 ? "no" : "yes",
              shmem_transport_ofi_stx_max,
              num_nics);

    return ret;
}


static int shmem_transport_ofi_target_ep_init(void)
{
    int ret = 0;

    struct fabric_info* info = &shmem_transport_ofi_info;
    info->p_info->ep_attr->tx_ctx_cnt = 0;
    info->p_info->caps = FI_RMA | FI_ATOMIC | FI_REMOTE_READ | FI_REMOTE_WRITE;
    if (shmem_transport_ofi_single_ep) {
        info->p_info->caps |= FI_WRITE | FI_READ | FI_RECV;
    }
#if ENABLE_TARGET_CNTR
    info->p_info->caps |= FI_RMA_EVENT;
#endif
    info->p_info->tx_attr->op_flags = 0;
    info->p_info->mode = 0;
    info->p_info->tx_attr->mode = 0;
    info->p_info->rx_attr->mode = 0;
    info->p_info->tx_attr->caps = FI_RMA | FI_ATOMIC;
    info->p_info->rx_attr->caps = info->p_info->caps;

    ret = fi_endpoint(shmem_transport_ofi_domainfd,
                      info->p_info, &shmem_transport_ofi_target_ep, NULL);
    OFI_CHECK_RETURN_MSG(ret, "target endpoint creation failed (%s)\n", fi_strerror(errno));

    /* Attach the address vector */
    ret = fi_ep_bind(shmem_transport_ofi_target_ep, &shmem_transport_ofi_avfd->fid, 0);
    OFI_CHECK_RETURN_STR(ret, "fi_ep_bind AV to target endpoint failed");

    struct fi_cq_attr cq_attr = {
        .format = FI_CQ_FORMAT_TAGGED,
        .size=16384
    };

    ret = fi_cq_open(shmem_transport_ofi_domainfd, &cq_attr,
                     &shmem_transport_ofi_target_cq, NULL);
    OFI_CHECK_RETURN_MSG(ret, "target_cq_open failed (%s)\n", fi_strerror(errno));


    ret = fi_ep_bind(shmem_transport_ofi_target_ep,
            &shmem_transport_ofi_target_cq->fid, 
            FI_TRANSMIT | FI_SELECTIVE_COMPLETION | FI_RECV);
    OFI_CHECK_RETURN_STR(ret, "fi_ep_bind TX_CQ to target endpoint failed");

    PRINT_DEBUG("ENabling one-sided EP\n");
    ret = fi_enable(shmem_transport_ofi_target_ep);
    OFI_CHECK_RETURN_STR(ret, "fi_enable on target endpoint failed");

    ret = allocate_recv_cntr_mr();
    if (ret) return ret;


    /* Addition: Collective EP attributes */

    info = &shmem_transport_ofi_CXI_info;
    info->p_info->ep_attr->tx_ctx_cnt = 0;
    info->p_info->caps = FI_MSG | FI_RECV | FI_COLLECTIVE;

    ret = fi_endpoint(shmem_transport_ofi_CXI_domain_fd,
                      info->p_info, &shmem_transport_ofi_CXI_target_ep, NULL);
    OFI_CHECK_RETURN_MSG(ret, "target endpoint creation failed (%s)\n", fi_strerror(errno));


    ret = fi_cq_open(shmem_transport_ofi_CXI_domain_fd, &cq_attr,
            &shmem_transport_ofi_CXI_target_cq, NULL);
    OFI_CHECK_RETURN_MSG(ret, "collective target_cq failed: %d %s\n", ret, fi_strerror(errno));

    struct fi_cq_attr rx_cq_attr = {
        .format = FI_CQ_FORMAT_TAGGED
    };

    ret = fi_cq_open(shmem_transport_ofi_CXI_domain_fd, &rx_cq_attr,
            &shmem_transport_ofi_CXI_recv_cq, NULL);
    OFI_CHECK_RETURN_MSG(ret, "collective recv_cq failed: %d %s\n", ret, fi_strerror(errno));


    ret = fi_ep_bind(shmem_transport_ofi_CXI_target_ep,
            &shmem_transport_ofi_CXI_target_cq->fid, 
            FI_TRANSMIT);
    OFI_CHECK_RETURN_MSG(ret, "fi_ep_bind CXI_TX_CQ to target endpoint failed %d %s", ret, fi_strerror(errno));

    ret = fi_ep_bind(shmem_transport_ofi_CXI_target_ep,
            &shmem_transport_ofi_CXI_recv_cq->fid, 
            FI_RECV);
    OFI_CHECK_RETURN_STR(ret, "fi_ep_bind CXI_TX_CQ to target endpoint failed");
    
    
    struct fi_eq_attr eq_attr = {
        .size = 512,
        .flags = FI_WRITE,
        .wait_obj = FI_WAIT_NONE
    };


    PRINT_DEBUG("Setting up counters\n");
    ret = fi_cntr_open(shmem_transport_ofi_CXI_domain_fd, NULL, &coll_send_cntr, NULL);
    OFI_CHECK_RETURN_STR(ret, "coll_send_cntr opening failed\n");
    ret = fi_cntr_open(shmem_transport_ofi_CXI_domain_fd, NULL, &coll_recv_cntr, NULL);
    OFI_CHECK_RETURN_STR(ret, "coll_recv_cntr opening failed\n");
    ret = fi_cntr_open(shmem_transport_ofi_CXI_domain_fd, NULL, &coll_write_cntr, NULL);
    OFI_CHECK_RETURN_STR(ret, "coll_write_cntr opening failed\n");
    ret = fi_cntr_open(shmem_transport_ofi_CXI_domain_fd, NULL, &coll_read_cntr, NULL);
    OFI_CHECK_RETURN_STR(ret, "coll_read_cntr opening failed\n");

    ret = fi_cntr_open(shmem_transport_ofi_CXI_domain_fd, NULL, &coll_rem_cntr, NULL);
    OFI_CHECK_RETURN_STR(ret, "coll_rem_cntr opening failed\n");



    ret = fi_ep_bind(shmem_transport_ofi_CXI_target_ep, &coll_send_cntr->fid, FI_SEND);
    OFI_CHECK_RETURN_STR(ret, "coll_send_cntr binding failed\n");
    ret = fi_ep_bind(shmem_transport_ofi_CXI_target_ep, &coll_recv_cntr->fid, FI_RECV);
    OFI_CHECK_RETURN_STR(ret, "coll_recv_cntr binding failed\n");
    ret = fi_ep_bind(shmem_transport_ofi_CXI_target_ep, &coll_write_cntr->fid, FI_WRITE);
    OFI_CHECK_RETURN_STR(ret, "coll_write_cntr binding failed\n");
    ret = fi_ep_bind(shmem_transport_ofi_CXI_target_ep, &coll_read_cntr->fid, FI_READ);
    OFI_CHECK_RETURN_STR(ret, "coll_read_cntr binding failed\n");


    PRINT_DEBUG("Finished opening and binding counters\n");


    
    ret = fi_eq_open(shmem_transport_ofi_CXI_fabfd, &eq_attr, &(shmem_transport_ofi_CXI_eq), NULL);
    OFI_CHECK_RETURN_STR(ret, "EQ creation failed\n");

    ret = fi_domain_bind(shmem_transport_ofi_CXI_domain_fd, &(shmem_transport_ofi_CXI_eq->fid), 0);
    OFI_CHECK_RETURN_STR(ret, "Domain binding failed\n");

    ret = fi_ep_bind(shmem_transport_ofi_CXI_target_ep, &(shmem_transport_ofi_CXI_eq->fid), 0);


    ret = fi_ep_bind(shmem_transport_ofi_CXI_target_ep, &shmem_transport_ofi_CXI_avfd->fid, 0);
    OFI_CHECK_RETURN_STR(ret, "fi_ep_bind AV to target endpoint failed");

    PRINT_DEBUG("ENABLING COLLECTIVE EP\n");
    ret = fi_enable(shmem_transport_ofi_CXI_target_ep);
    OFI_CHECK_RETURN_STR(ret, "fi_enable on coll_target endpoint failed");

 
    return 0;
}

static int shmem_transport_ofi_ctx_init(shmem_transport_ctx_t *ctx, int id)
{
    int ret = 0;

    struct fi_cntr_attr cntr_put_attr = {0};
    struct fi_cntr_attr cntr_get_attr = {0};
    cntr_put_attr.events   = FI_CNTR_EVENTS_COMP;
    cntr_get_attr.events   = FI_CNTR_EVENTS_COMP;

    /* Set FI_WAIT based on the put and get polling limits defined above */
    if (shmem_transport_ofi_put_poll_limit < 0) {
        cntr_put_attr.wait_obj = FI_WAIT_NONE;
    } else {
        cntr_put_attr.wait_obj = FI_WAIT_UNSPEC;
    }
    if (shmem_transport_ofi_get_poll_limit < 0) {
        cntr_get_attr.wait_obj = FI_WAIT_NONE;
    } else {
        cntr_get_attr.wait_obj = FI_WAIT_UNSPEC;
    }

    /* Allow provider to choose CQ size, since we are using FI_RM_ENABLED.
     * Context format is used to return bounce buffer pointers in the event
     * so that they can be inserted back into to the free list. */
    struct fi_cq_attr cq_attr = {0};
    cq_attr.format = FI_CQ_FORMAT_CONTEXT;

    struct fabric_info* info = &shmem_transport_ofi_info;

    info->p_info->ep_attr->tx_ctx_cnt = shmem_transport_ofi_stx_max > 0 ? FI_SHARED_CONTEXT : 0;
    info->p_info->caps = FI_RMA | FI_WRITE | FI_READ | FI_ATOMIC | FI_RECV | FI_MSG;
    info->p_info->tx_attr->op_flags = FI_DELIVERY_COMPLETE;
    info->p_info->mode = 0;
    info->p_info->tx_attr->mode = 0;
    info->p_info->rx_attr->mode = 0;
    info->p_info->tx_attr->caps = info->p_info->caps;
    info->p_info->rx_attr->caps = FI_RECV; /* to drive progress on the CQ */;



    info = &shmem_transport_ofi_CXI_info;

    info->p_info->ep_attr->tx_ctx_cnt = shmem_transport_ofi_stx_max > 0 ? FI_SHARED_CONTEXT : 0;
    PRINT_DEBUG("p_info-> caps for shmem_transprot_OFI_CXI_info also has FI_MSG | FI_COLLECTIVE\n");
    info->p_info->caps =  FI_MSG |  FI_COLLECTIVE;
    info->p_info->tx_attr->op_flags = FI_DELIVERY_COMPLETE;
    info->p_info->mode = 0;
    info->p_info->tx_attr->mode = 0;
    info->p_info->rx_attr->mode = 0;
    info->p_info->tx_attr->caps = info->p_info->caps;
    info->p_info->rx_attr->caps = FI_RECV; /* to drive progress on the CQ */;

    info = &shmem_transport_ofi_info;

    ctx->id = id;
#ifdef USE_CTX_LOCK
    SHMEM_MUTEX_INIT(ctx->lock);
#endif

    ret = fi_cntr_open(shmem_transport_ofi_domainfd, &cntr_put_attr,
                       &ctx->put_cntr, NULL);
    OFI_CHECK_RETURN_MSG(ret, "put_cntr creation failed (%s)\n", fi_strerror(errno));

    ret = fi_cntr_open(shmem_transport_ofi_domainfd, &cntr_get_attr,
                       &ctx->get_cntr, NULL);
    OFI_CHECK_RETURN_MSG(ret, "get_cntr creation failed (%s)\n", fi_strerror(errno));

    if (shmem_transport_ofi_single_ep && id == SHMEM_TRANSPORT_CTX_DEFAULT_ID) {
        PRINT_DEBUG("id %d, default ctx_idi %d\n", id, SHMEM_TRANSPORT_CTX_DEFAULT_ID);
        ctx->tx_cq = shmem_transport_ofi_target_cq;
        ctx->rx_cq = shmem_transport_ofi_CXI_recv_cq;
        ctx->ep = shmem_transport_ofi_target_ep;
        ctx->CXI_ep = shmem_transport_ofi_CXI_target_ep;
        ctx->coll_tx_cq = shmem_transport_ofi_CXI_target_cq;
    } else {
        ret = fi_cq_open(shmem_transport_ofi_domainfd, &cq_attr, &ctx->tx_cq, NULL);
        if (ret && errno == FI_EMFILE) {
            DEBUG_STR("Context creation failed because of open files limit, consider increasing with 'ulimit' command");
        }
        OFI_CHECK_RETURN_MSG(ret, "cq_open failed (%s)\n", fi_strerror(errno));

        ret = fi_endpoint(shmem_transport_ofi_domainfd,
                          info->p_info, &ctx->ep, NULL);
        OFI_CHECK_RETURN_MSG(ret, "ep creation failed (%s)\n", fi_strerror(errno));

     /*   info = &shmem_transport_ofi_CXI_info;

        ret = fi_cq_open(shmem_transport_ofi_CXI_domain_fd, &cq_attr, &ctx->coll_tx_cq, NULL);
        if (ret && errno == FI_EMFILE) {
            DEBUG_STR("Context creation failed because of open files limit, consider increasing with 'ulimit' command");
        }
        OFI_CHECK_RETURN_MSG(ret, "coll_tx_cq_open failed (%s)\n", fi_strerror(errno));


        ret = fi_cq_open(shmem_transport_ofi_CXI_domain_fd, &cq_attr, &ctx->rx_cq, NULL);
        if (ret && errno == FI_EMFILE) {
            DEBUG_STR("Context creation failed because of open files limit, consider increasing with 'ulimit' command");
        }
        OFI_CHECK_RETURN_MSG(ret, "coll_rx_cq_open failed (%s)\n", fi_strerror(errno));

 
        ret = fi_endpoint(shmem_transport_ofi_CXI_domain_fd,
                info->p_info, &ctx->CXI_ep, NULL);
        OFI_CHECK_RETURN_MSG(ret, "ep creation failed (%s)\n", fi_strerror(errno)); */

        info = &shmem_transport_ofi_info;
    }

    /* TODO: Fill in TX attr */

    /* Allocate STX from the pool */
    if (shmem_internal_thread_level > SHMEM_THREAD_FUNNELED &&
        shmem_transport_ofi_is_private(ctx->options)) {
            ctx->tid = shmem_transport_ofi_gettid();
    }
    shmem_transport_ofi_stx_allocate(ctx);

    ret = bind_enable_ep_resources(ctx);
    OFI_CHECK_RETURN_MSG(ret, "context bind/enable endpoint failed (%s)\n", fi_strerror(errno));

    if (ctx->options & SHMEMX_CTX_BOUNCE_BUFFER &&
        shmem_transport_ofi_bounce_buffer_size > 0 &&
        shmem_transport_ofi_max_bounce_buffers > 0)
    {
        ctx->bounce_buffers =
            shmem_free_list_init(sizeof(shmem_transport_ofi_bounce_buffer_t) +
                                 shmem_transport_ofi_bounce_buffer_size,
                                 init_bounce_buffer);
    }
    else {
        ctx->options &= ~SHMEMX_CTX_BOUNCE_BUFFER;
        ctx->bounce_buffers = NULL;
    }

    /* Setting up framework and environment */

    ctx->nodename = getenv(NODENAME);
    ctx->jobid = getenv(JOBID);
    ctx->jobstep = getenv(JOBSTEP);
    ctx->fab_mgr_url = getenv(MGR_URL);
    ctx->mcast_token = getenv(MCAST_TOKEN);
    ctx->addrs_per_job = getenv(ADDRS_PER_JOB) == NULL ? 1 : atoi(getenv(ADDRS_PER_JOB));
    char *s, *d;
    s = getenv(NODELIST);
    d = (char *)ctx->node_0;

    int count = 0;
    while(s && *s && *s != ',') {
        if (*s == '['){
            s++;
            while( *s != '-' && *s != ']' && *s != ','){
                count++;
                *d++ = *s++;
            }
            break;
        }
        *d++ = *s++;
        count++;
    }
    *d = 0;
    d -=count ;
    for (int i = 0; i < count; i++){
        ctx->node_0[i] = d[i];
    }

    ctx->nics_per_rank = getenv(NICS_PER_RANK) == NULL ? 1 : atoi(getenv(NICS_PER_RANK));
    if (ctx->nics_per_rank < 1) 
        ctx->nics_per_rank = 1;

    return 0;
}


int shmem_transport_init(void)
{
    int ret = 0;

    SHMEM_MUTEX_INIT(shmem_transport_ofi_lock);

    PRINT_DEBUG("PID for today: %d\n", getpid());

    shmem_transport_ofi_info.npes = shmem_runtime_get_size();
    shmem_transport_ofi_CXI_info.npes = shmem_transport_ofi_info.npes;

    shmem_transport_ofi_info.prov_name = NULL;
    shmem_transport_ofi_info.fabric_name = NULL;
    shmem_transport_ofi_info.domain_name = NULL;

    shmem_transport_ofi_CXI_info.prov_name = NULL;
    shmem_transport_ofi_CXI_info.fabric_name = NULL;
    shmem_transport_ofi_CXI_info.domain_name = NULL;

    if (shmem_internal_params.OFI_PROVIDER_provided){
        shmem_transport_ofi_info.prov_name = shmem_internal_params.OFI_PROVIDER;
        shmem_transport_ofi_CXI_info.prov_name = shmem_internal_params.OFI_PROVIDER;
    }

    if (shmem_internal_params.OFI_FABRIC_provided){
        shmem_transport_ofi_info.fabric_name = shmem_internal_params.OFI_FABRIC;
        shmem_transport_ofi_CXI_info.fabric_name = shmem_internal_params.OFI_FABRIC;
    }

    if (shmem_internal_params.OFI_DOMAIN_provided){
        shmem_transport_ofi_info.domain_name = shmem_internal_params.OFI_DOMAIN;
        shmem_transport_ofi_CXI_info.domain_name = shmem_internal_params.OFI_DOMAIN;
    }

    /* Unless SHMEM_OFI_DISABLE_SINGLE_EP env var is set, each PE opens a single libfabric endpoint
     * for both transmission (on the default context) and as the target of communication */
    if (shmem_internal_params.OFI_DISABLE_SINGLE_EP_provided)
        shmem_transport_ofi_single_ep = 0;
    else
        shmem_transport_ofi_single_ep = 1;

    /* Check STX resource settings */
    if ((shmem_internal_thread_level == SHMEM_THREAD_SINGLE ||
         shmem_internal_thread_level == SHMEM_THREAD_FUNNELED ) &&
         shmem_internal_params.OFI_STX_MAX > 1) {
        if (shmem_internal_params.OFI_STX_MAX_provided) {
            /* We need only 1 STX per PE with SHMEM_THREAD_SINGLE or SHMEM_THREAD_FUNNELED */
            RAISE_WARN_MSG("Ignoring STX max setting '%ld'; using 1 STX in single-threaded mode\n",
                           shmem_internal_params.OFI_STX_MAX);
        }
        shmem_transport_ofi_stx_max = 1;
    } else {
        if (shmem_internal_params.OFI_STX_MAX < 0) {
            RAISE_ERROR_MSG("Invalid OFI_STX_MAX value '%ld'\n",
                            shmem_internal_params.OFI_STX_MAX);
        }
        shmem_transport_ofi_stx_max = shmem_internal_params.OFI_STX_MAX;
    }
    shmem_transport_ofi_stx_threshold = shmem_internal_params.OFI_STX_THRESHOLD;

    PRINT_DEBUG("Querying fot fabric and attributes used for pt2pt\n");
    ret = query_for_fabric(&shmem_transport_ofi_info);
    if (ret != 0) return ret;

    PRINT_DEBUG("Querying for collectives\n");
    ret = query_for_fabric_collectives(&shmem_transport_ofi_CXI_info);
    if (ret != 0) return ret;

    ret = allocate_fabric_resources(&shmem_transport_ofi_info);
    if (ret != 0) return ret;

    PRINT_DEBUG("Allocating resources for collectives now\n");
    ret = allocate_fabric_resources(&shmem_transport_ofi_CXI_info);
    if (ret != 0) return ret;


    /* STX sharing settings */
    char *type = shmem_internal_params.OFI_STX_ALLOCATOR;
    if (0 == strcmp(type, "round-robin")) {
        shmem_transport_ofi_stx_allocator = ROUNDROBIN;
    } else if (0 == strcmp(type, "random")) {
        shmem_transport_ofi_stx_allocator = RANDOM;
        shmem_transport_ofi_stx_rand_init();
    } else {
        RAISE_WARN_MSG("Ignoring bad STX share algorithm '%s', using 'round-robin'\n", type);
        shmem_transport_ofi_stx_allocator = ROUNDROBIN;
    }


    /* The current bounce buffering implementation is only compatible with
     * providers that don't require FI_CONTEXT or FI_CONTEXT2 */
    if (shmem_transport_ofi_info.p_info->mode & FI_CONTEXT || shmem_transport_ofi_info.p_info->mode & FI_CONTEXT2) {
        if (shmem_internal_my_pe == 0 && shmem_internal_params.BOUNCE_SIZE > 0) {
            DEBUG_STR("OFI provider requires FI_CONTEXT and or FI_CONTEXT2; disabling bounce buffering");
        }
        shmem_transport_ofi_bounce_buffer_size = 0;
        shmem_transport_ofi_max_bounce_buffers = 0;
    } else {
        shmem_transport_ofi_bounce_buffer_size = shmem_internal_params.BOUNCE_SIZE;
        shmem_transport_ofi_max_bounce_buffers = shmem_internal_params.MAX_BOUNCE_BUFFERS;
    }

    shmem_transport_ofi_put_poll_limit = shmem_internal_params.OFI_TX_POLL_LIMIT;
    shmem_transport_ofi_get_poll_limit = shmem_internal_params.OFI_RX_POLL_LIMIT;

#ifdef USE_CTX_LOCK
    /* In multithreaded mode, force completion polling so that threads yield
     * the lock during put/get completion operations.  User can still override
     * (get blocking behavior) by setting the env vars. */
    if (shmem_internal_thread_level == SHMEM_THREAD_MULTIPLE) {
        if (!shmem_internal_params.OFI_TX_POLL_LIMIT_provided)
            shmem_transport_ofi_put_poll_limit = -1;
        if (!shmem_internal_params.OFI_RX_POLL_LIMIT_provided)
            shmem_transport_ofi_get_poll_limit = -1;
    }
#endif

    shmem_transport_ctx_default.options = SHMEMX_CTX_BOUNCE_BUFFER;

    ret = shmem_transport_ofi_target_ep_init();
    if (ret != 0) return ret;

    ret = publish_mr_info();
    if (ret != 0) return ret;

    ret = publish_av_info(&shmem_transport_ofi_info);
    if (ret != 0) return ret;

    return 0;
}

int shmem_transport_startup(void)
{
    int ret;
    int i;

    if (shmem_internal_params.OFI_STX_AUTO && shmem_transport_ofi_stx_max == 0) {
        RAISE_WARN_STR("STXs disabled, ignoring request for automatic STX management");
    }
    else if (shmem_internal_params.OFI_STX_AUTO) {

        long ofi_tx_ctx_cnt = shmem_transport_ofi_info.fabrics->domain_attr->tx_ctx_cnt;
        int num_on_node = shmem_runtime_get_node_size();

        if (shmem_internal_params.OFI_STX_MAX_provided) {
            RAISE_WARN_MSG("Auto-setting STX_MAX; ignoring provided STX_MAX value '%ld'\n",
                           shmem_internal_params.OFI_STX_MAX);
        }

        if (ofi_tx_ctx_cnt <= 0)
            RAISE_ERROR_MSG("Invalid number of TX contexts (%ld)\n", ofi_tx_ctx_cnt);

        /* Paritition TX resources evenly across node-local PEs */
        /* Note: we assume that the domain reports the same tx_ctx_cnt for
         * every PE on the node.  We also assume that the resource reported
         * should be divided equally among all PEs.  These assumptions may not
         * be valid in all cases, for example when the provider has already
         * partitioned resources or when a node has multiple NICs. */
        shmem_transport_ofi_stx_max = ofi_tx_ctx_cnt / num_on_node;
        int remainder = ofi_tx_ctx_cnt % num_on_node;
        int node_pe = shmem_internal_my_pe % shmem_internal_num_pes;
        if (remainder > 0 && ((node_pe % num_on_node) < remainder)) {
            shmem_transport_ofi_stx_max++;
        }

        if (shmem_transport_ofi_stx_max <= 0)
            RAISE_ERROR_MSG("Not enough TX contexts (%d)\n", num_on_node);

        /* When running more PEs than available STXs, must assign each PE at least 1 */
        if (shmem_transport_ofi_stx_max <= 0) {
            shmem_transport_ofi_stx_max = 1;
            RAISE_WARN_MSG("Need at least 1 STX per PE, but detected %ld available STXs for %d PEs\n",
                           ofi_tx_ctx_cnt, num_on_node);
        }

        DEBUG_MSG("Auto-set STX max to %ld\n", shmem_transport_ofi_stx_max);
    }

    /* Allocate STX array with max length */
    if (shmem_transport_ofi_stx_max > 0) {
        shmem_transport_ofi_stx_pool = malloc(shmem_transport_ofi_stx_max *
                                              sizeof(shmem_transport_ofi_stx_t));
        if (shmem_transport_ofi_stx_pool == NULL) {
            RAISE_ERROR_STR("Out of memory when allocating OFI STX pool");
        }
    }

    for (i = 0; i < shmem_transport_ofi_stx_max; i++) {
        ret = fi_stx_context(shmem_transport_ofi_domainfd, NULL,
                             &shmem_transport_ofi_stx_pool[i].stx, NULL);
        OFI_CHECK_RETURN_MSG(ret, "STX context creation failed (%s)\n", fi_strerror(ret));
        shmem_transport_ofi_stx_pool[i].ref_cnt = 0;
        shmem_transport_ofi_stx_pool[i].is_private = 0;
    }

    shmem_transport_ctx_default.team = &shmem_internal_team_world;

    ret = shmem_transport_ofi_ctx_init(&shmem_transport_ctx_default, SHMEM_TRANSPORT_CTX_DEFAULT_ID);
    if (ret != 0) return ret;

    ret = atomic_limitations_check();
    if (ret != 0) return ret;

    ret = populate_mr_tables();
    if (ret != 0) return ret;

    ret = populate_av();
    if (ret != 0) return ret;

    return 0;
}

int shmem_transport_ctx_create(struct shmem_internal_team_t *team, long options, shmem_transport_ctx_t **ctx)
{
    int ret;
    size_t id;

    if (team == NULL)
        RAISE_ERROR_STR("Context creation occured on a NULL team");

    SHMEM_MUTEX_LOCK(shmem_transport_ofi_lock);

    /* Look for an open slot in the contexts array */
    for (id = 0; id < team->contexts_len; id++)
        if (team->contexts[id] == NULL) break;

    /* If none found, grow the array */
    if (id >= team->contexts_len) {
        id = team->contexts_len;

        size_t i = team->contexts_len;
        team->contexts_len += shmem_transport_ofi_grow_size;
        team->contexts = realloc(team->contexts, team->contexts_len * sizeof(shmem_transport_ctx_t*));

        if (team->contexts == NULL) {
            RAISE_ERROR_STR("Out of memory when allocating OFI ctx array");
        }

        for ( ; i < team->contexts_len; i++)
            team->contexts[i] = NULL;
    }

    shmem_transport_ctx_t *ctxp = malloc(sizeof(shmem_transport_ctx_t));

    if (ctxp == NULL) {
        RAISE_ERROR_STR("Out of memory when allocating OFI ctx object");
    }

    memset(ctxp, 0, sizeof(shmem_transport_ctx_t));

#ifndef USE_CTX_LOCK
    shmem_internal_cntr_write(&ctxp->pending_put_cntr, 0);
    shmem_internal_cntr_write(&ctxp->pending_get_cntr, 0);
#endif

    ctxp->stx_idx = -1;
    ctxp->options = options;

    ctxp->team = team;

    ret = shmem_transport_ofi_ctx_init(ctxp, id);

    if (ret) {
        shmem_transport_ctx_destroy(ctxp);
    } else {
        team->contexts[id] = ctxp;
        *ctx = ctxp;
    }

    SHMEM_MUTEX_UNLOCK(shmem_transport_ofi_lock);
    return ret;

}

void shmem_transport_ctx_destroy(shmem_transport_ctx_t *ctx)
{
    int ret;
    bool close_default_ctx = false;

    if (ctx == NULL)
        return;

    if(shmem_internal_params.DEBUG) {
        SHMEM_TRANSPORT_OFI_CTX_LOCK(ctx);
        if (ctx->bounce_buffers) SHMEM_TRANSPORT_OFI_CTX_BB_LOCK(ctx);
        DEBUG_MSG("id = %d, options = %#0lx, stx_idx = %d\n"
                  RAISE_PE_PREFIX "pending_put_cntr = %9"PRIu64", completed_put_cntr = %9"PRIu64"\n"
                  RAISE_PE_PREFIX "pending_get_cntr = %9"PRIu64", completed_get_cntr = %9"PRIu64"\n"
                  RAISE_PE_PREFIX "pending_bb_cntr  = %9"PRIu64", completed_bb_cntr  = %9"PRIu64"\n",
                  ctx->id, (unsigned long) ctx->options, ctx->stx_idx,
                  shmem_internal_my_pe,
                  SHMEM_TRANSPORT_OFI_CNTR_READ(&ctx->pending_put_cntr),
                  ctx->put_cntr ? fi_cntr_read(ctx->put_cntr) : 0,
                  shmem_internal_my_pe,
                  SHMEM_TRANSPORT_OFI_CNTR_READ(&ctx->pending_get_cntr),
                  ctx->get_cntr ? fi_cntr_read(ctx->get_cntr) : 0,
                  shmem_internal_my_pe,
                  ctx->pending_bb_cntr, ctx->completed_bb_cntr
                 );
        if (ctx->bounce_buffers) SHMEM_TRANSPORT_OFI_CTX_BB_UNLOCK(ctx);
        SHMEM_TRANSPORT_OFI_CTX_UNLOCK(ctx);
    }

    /* When in single-endpoint mode, defer closing the default context because it also
     * serves as the target endpoint, which is cleaned up later in transport_fini(). */
    if (!shmem_transport_ofi_single_ep || ctx->id != SHMEM_TRANSPORT_CTX_DEFAULT_ID)
        close_default_ctx = true;

    if (ctx->ep && close_default_ctx) {
        ret = fi_close(&ctx->ep->fid);
        OFI_CHECK_ERROR_MSG(ret, "Context endpoint close failed (%s)\n", fi_strerror(errno));
    }

    if (ctx->bounce_buffers) {
        shmem_free_list_destroy(ctx->bounce_buffers);
    }

    if (ctx->stx_idx >= 0) {
        SHMEM_MUTEX_LOCK(shmem_transport_ofi_lock);
        if (shmem_transport_ofi_is_private(ctx->options)) {
            shmem_transport_ofi_stx_kvs_t *e;
            HASH_FIND(hh, shmem_transport_ofi_stx_kvs, &ctx->tid,
                      sizeof(struct shmem_internal_tid), e);
            if (e) {
                shmem_transport_ofi_stx_t *stx = &shmem_transport_ofi_stx_pool[ctx->stx_idx];
                stx->ref_cnt--;
                if (stx->ref_cnt == 0) {
                    HASH_DEL(shmem_transport_ofi_stx_kvs, e);
                    free(e);
                    shmem_transport_ofi_stx_pool[ctx->stx_idx].is_private = 0;
                }
            }
            else {
                RAISE_WARN_STR("Unable to locate private STX");
            }
        } else {
            shmem_transport_ofi_stx_pool[ctx->stx_idx].ref_cnt--;
            if (shmem_transport_ofi_stx_pool[ctx->stx_idx].is_private) {
                SHMEM_MUTEX_UNLOCK(shmem_transport_ofi_lock);
                RAISE_ERROR_STR("Destroyed a ctx with an inconsistent is_private field");
            }
        }
        SHMEM_MUTEX_UNLOCK(shmem_transport_ofi_lock);
    }

    if (ctx->put_cntr && close_default_ctx) {
        ret = fi_close(&ctx->put_cntr->fid);
        OFI_CHECK_ERROR_MSG(ret, "Context put CNTR close failed (%s)\n", fi_strerror(errno));
    }

    if (ctx->get_cntr && close_default_ctx) {
        ret = fi_close(&ctx->get_cntr->fid);
        OFI_CHECK_ERROR_MSG(ret, "Context get CNTR close failed (%s)\n", fi_strerror(errno));
    }

    if (ctx->tx_cq && close_default_ctx) {
        ret = fi_close(&ctx->tx_cq->fid);
        OFI_CHECK_ERROR_MSG(ret, "Context CQ close failed (%s)\n", fi_strerror(errno));
    }

#ifdef USE_CTX_LOCK
    SHMEM_MUTEX_DESTROY(ctx->lock);
#endif

    if (ctx->id >= 0) {
        SHMEM_MUTEX_LOCK(shmem_transport_ofi_lock);
        ctx->team->contexts[ctx->id] = NULL;
        SHMEM_MUTEX_UNLOCK(shmem_transport_ofi_lock);
        free(ctx);
    }
    else if (ctx->id != SHMEM_TRANSPORT_CTX_DEFAULT_ID) {
        RAISE_ERROR_MSG("Attempted to destroy an invalid context (%d)\n", ctx->id);
    }
}

int shmem_transport_fini(void)
{
    int ret;
    shmem_transport_ofi_stx_kvs_t* e;
    int stx_len = 0;

    /* The default context is not inserted into the list of contexts on
     * SHMEM_TEAM_WORLD, so it must be destroyed here */
    shmem_transport_quiet(&shmem_transport_ctx_default);
    shmem_transport_ctx_destroy(&shmem_transport_ctx_default);

    for (e = shmem_transport_ofi_stx_kvs; e != NULL; ) {
        shmem_transport_ofi_stx_kvs_t *last = e;
        stx_len++;
        e = e->hh.next;
        free(last);
    }

    if (stx_len > 0) {
        RAISE_WARN_MSG("Key/value store contained %d unfreed private contexts\n", stx_len);
    }

    for (long i = 0; i < shmem_transport_ofi_stx_max; ++i) {
        if (shmem_transport_ofi_stx_pool[i].ref_cnt != 0)
            RAISE_WARN_MSG("Closing a %s STX (%zu) with nonzero ref. count (%ld)\n",
                           shmem_transport_ofi_stx_pool[i].is_private ? "private" : "shared",
                           i, shmem_transport_ofi_stx_pool[i].ref_cnt);
        ret = fi_close(&shmem_transport_ofi_stx_pool[i].stx->fid);
        OFI_CHECK_ERROR_MSG(ret, "STX context close failed (%s)\n", fi_strerror(errno));
    }
    if (shmem_transport_ofi_stx_pool) free(shmem_transport_ofi_stx_pool);

#if defined(ENABLE_MR_SCALABLE)
#if defined(ENABLE_REMOTE_VIRTUAL_ADDRESSING)
    ret = fi_close(&shmem_transport_ofi_target_mrfd->fid);
    OFI_CHECK_ERROR_MSG(ret, "Target MR close failed (%s)\n", fi_strerror(errno));
#else
    ret = fi_close(&shmem_transport_ofi_target_heap_mrfd->fid);
    OFI_CHECK_ERROR_MSG(ret, "Target heap MR close failed (%s)\n", fi_strerror(errno));

    ret = fi_close(&shmem_transport_ofi_target_data_mrfd->fid);
    OFI_CHECK_ERROR_MSG(ret, "Target data MR close failed (%s)\n", fi_strerror(errno));  
#endif
#else
    free(shmem_transport_ofi_target_heap_keys);
    free(shmem_transport_ofi_target_data_keys);

#if !defined(ENABLE_REMOTE_VIRTUAL_ADDRESSING)
    free(shmem_transport_ofi_target_heap_addrs);
    free(shmem_transport_ofi_target_data_addrs);
#endif

    ret = fi_close(&shmem_transport_ofi_target_heap_mrfd->fid);
    OFI_CHECK_ERROR_MSG(ret, "Target heap MR close failed (%s)\n", fi_strerror(errno));

    ret = fi_close(&shmem_transport_ofi_target_data_mrfd->fid);
    OFI_CHECK_ERROR_MSG(ret, "Target data MR close failed (%s)\n", fi_strerror(errno));
#endif

#ifdef USE_FI_HMEM
    if (shmem_external_heap_pre_initialized) {
        free(shmem_transport_ofi_external_heap_keys);
        free(shmem_transport_ofi_external_heap_addrs);
        ret = fi_close(&shmem_transport_ofi_external_heap_mrfd->fid);
        OFI_CHECK_ERROR_MSG(ret, "External heap MR close failed (%s)\n", fi_strerror(errno));
    }
#endif

    ret = fi_close(&shmem_transport_ofi_target_ep->fid);
    OFI_CHECK_ERROR_MSG(ret, "Target endpoint close failed (%s)\n", fi_strerror(errno));

    /* If single-endpoint mode, need to close the default context's put and get counters */
    if (shmem_transport_ofi_single_ep) {
        ret = fi_close(&shmem_transport_ctx_default.put_cntr->fid);
        OFI_CHECK_ERROR_MSG(ret, "Default EP put CNTR close failed (%s)\n", fi_strerror(errno));

        ret = fi_close(&shmem_transport_ctx_default.get_cntr->fid);
        OFI_CHECK_ERROR_MSG(ret, "Default EP get CNTR close failed (%s)\n", fi_strerror(errno));
    }

    ret = fi_close(&shmem_transport_ofi_target_cq->fid);
    OFI_CHECK_ERROR_MSG(ret, "Target CQ close failed (%s)\n", fi_strerror(errno));

#if ENABLE_TARGET_CNTR
    ret = fi_close(&shmem_transport_ofi_target_cntrfd->fid);
    OFI_CHECK_ERROR_MSG(ret, "Target CT close failed (%s)\n", fi_strerror(errno));
#endif

    ret = fi_close(&shmem_transport_ofi_avfd->fid);
    OFI_CHECK_ERROR_MSG(ret, "AV close failed (%s)\n", fi_strerror(errno));

    ret = fi_close(&shmem_transport_ofi_domainfd->fid);
    OFI_CHECK_ERROR_MSG(ret, "Domain close failed (%s)\n", fi_strerror(errno));

    ret = fi_close(&shmem_transport_ofi_fabfd->fid);
    OFI_CHECK_ERROR_MSG(ret, "Fabric close failed (%s)\n", fi_strerror(errno));

#ifdef USE_AV_MAP
    free(addr_table);
#endif

    fi_freeinfo(shmem_transport_ofi_info.fabrics);

    SHMEM_MUTEX_DESTROY(shmem_transport_ofi_lock);

    return 0;
}
