#include "cxi_extension_funcs.h"
#include "shmem_internal.h"
#include "shmem.h"
#include "shmem_comm.h"
#include <unistd.h>
#include <stdint.h>
#include <inttypes.h>
#include <netdb.h>
#include <rdma/fabric.h>

/*struct fabric_info {
    struct fi_info *fabrics;
    struct fi_info *p_info;
    char *prov_name;
    char *fabric_name;
    char *domain_name;
    int npes;
};
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
struct fid_cq                   *shmem_transport_ofi_CXI_recv_cq;
struct fid_mc                   *ofi_coll_mc = NULL;
*/

int wait_for_join(shmem_transport_ctx_t *ctx, uint32_t signal, void *context){
    int err;
    uint32_t event;
    struct fi_cq_err_entry comp = {};
    struct fi_eq_entry entry;

    PRINT_DEBUG("ctx %p, signal %u, context %p, cq %p, eq %p\n", ctx, signal, context, ctx->tx_cq, ctx->eq);

    do {
        err = fi_eq_read(ctx->eq, &event, &entry, sizeof(entry), 0);
        PRINT_DEBUG("eq_read Err %d, event %u hoping for -FI_EAGAIN %d or FI_SUCCESS %d, FI_JOIN_COMPLETE %u entry.context %p\n", err, event, -FI_EAGAIN, FI_SUCCESS, FI_JOIN_COMPLETE, entry.context);
        if (err >= 0){
            if (event == signal){
                if (context == NULL || (entry.context == context)){
                    return FI_SUCCESS;
                } else if (context != NULL){
                    return -FI_EOTHER;
                }
            }
        } else if (err != -FI_EAGAIN) {
            return err;
        }

        err = fi_cq_read(ctx->tx_cq, &comp, 1);
        PRINT_DEBUG("cq_read Err %d, hoping for -FI_EAGAIN %d or FI_SUCCESS %d\n", err, -FI_EAGAIN, FI_SUCCESS);
        if (err < 0 && err != -FI_EAGAIN){
            return err;
        }
    } while (err == -FI_EAGAIN);

    return err;
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

        ret = fi_cq_read(ctx->tx_cq, &flag, 1);
 
        if (ret < 0 && ret != -FI_EAGAIN){
            return ret;
        }
        if (comp.op_context && comp.op_context == flag){
            return FI_SUCCESS;
        }
    } while (ret == -FI_EAGAIN);

    return ret;
}
