#ifndef SHMEM_OFI_CTX_H
#define SHMEM_OFI_CTX_H

#include <stdio.h>
#include <stdlib.h>
#include <sys/uio.h>
#include <stdbool.h>
#include <rdma/fabric.h>
#include <rdma/fi_errno.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_tagged.h>
#include <rdma/fi_rma.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_atomic.h>
#include <string.h>
#include <unistd.h>
#include <stddef.h>
#include <inttypes.h>
#include "shmem.h"
#include "shmem_internal.h"
#include "shmem_free_list.h"
#include <sys/types.h>
#include <rdma/fi_cxi_ext.h>
#include <rdma/fi_collective.h>


/* Some things taken directly from cxip.h to replicate what exactly is going
 * on/what should be going on */

#define MAX_BITS 9
#define NIC_BITS 20
#define PAD 3


#define NICSIZE (sizeof(union nic_addr))

#define nodename_len 128
#define nicname_len 256

enum shmem_internal_tid_t { tid_is_pid_t, tid_is_uint64_t };
struct shmem_internal_tid
{
    enum shmem_internal_tid_t tid_t;
    union
    {
        pid_t pid_val;
        uint64_t uint64_val;
    } val;
};



typedef struct internal_addr {
    uint32_t pid:MAX_BITS;
    uint32_t nic:NIC_BITS;
    uint32_t pad:PAD;
    uint16_t vni;
} internal_addr_t;

typedef union nic_addr {
    uint64_t value;
    struct {
        uint64_t nic:20;
        uint64_t net:28;
        uint64_t hsn:2;
        uint64_t rank:14;
    } __attribute__((__packed__));
} nic_addr_t;

struct shmem_transport_ctx_t {
    int                             id;
#ifdef USE_CTX_LOCK
    shmem_internal_mutex_t          lock;
#endif
    long                            options;
    struct fid_ep*                  ep;
    struct fid_ep*                  CXI_ep;
    struct fid_cntr*                put_cntr;
    struct fid_cntr*                get_cntr;
    struct fid_cq*                  tx_cq;
    struct fid_cq*                  coll_tx_cq;
    struct fid_cq*                  rx_cq;
#ifdef USE_CTX_LOCK
    /* Pending cntr accesses are protected by ctx lock */
    uint64_t                        pending_put_cntr;
    uint64_t                        pending_get_cntr;
#else
    shmem_internal_cntr_t           pending_put_cntr;
    shmem_internal_cntr_t           pending_get_cntr;
#endif
    /* These counters are protected by the BB lock */
    uint64_t                        pending_bb_cntr;
    uint64_t                        completed_bb_cntr;
    shmem_free_list_t              *bounce_buffers;
    int                             stx_idx;
    struct shmem_internal_tid       tid;
    struct shmem_internal_team_t   *team;
    /* Start Ben items */
    struct fid_eq*                  eq;
    int nics_per_rank; /* PMI_NUM_HSNS */
    const char *nodename; /* SLURMD_NODENAME */
    const char *unique_secret; /* PMI_SHARED_SECRET */
    const char *jobid; /* FI_CXI_COLL_JOB_ID or SLURM_JOBID */
    const char *jobstep; /* FI_CXI_COLL_JOB_STEP_ID */
    const char *mcast_token; /* FI_CXI_COLL_MCAST_TOKEN */
    const char *fab_mgr_url; /* FI_CXI_COLL_FABRIC_MGR_URL */
    char node_0[nodename_len]; /* First name in SLURMD_NODELIST */
    int addrs_per_job;
    nic_addr_t *NIC_array;
    int num_nics;
    /* End Ben items */
};


typedef struct shmem_transport_ctx_t shmem_transport_ctx_t;
extern shmem_transport_ctx_t shmem_transport_ctx_default;



#endif /* ifndef SHMEM_OFI_CTX_H */
