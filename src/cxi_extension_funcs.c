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


void shmem_transport_CXI_coll_sync(int PE_start, int PE_stride, int PE_size, long *pSync){

    int ret = FI_SUCCESS;
    shmem_transport_ctx_t *ctx = &shmem_transport_ctx_default;

    uint64_t done_flag = 0;

    struct fid_ep *ep = ctx->ep;
    PRINT_DEBUG("Starting join with addr_table %p, PE_start %d, PE_stride %d, PE_size %d\n",
            shmem_transport_ofi_CXI_addr_table, PE_start, PE_stride, PE_size);


    ret = initialize_avset(PE_start, PE_stride, PE_size);
    OFI_CHECK_RETURN_STR(ret, "failed to initialize avset");


    PRINT_DEBUG("Starting collective join\n");
    ret = fi_join_collective(ep, shmem_transport_ofi_CXI_world_addr,
                             shmem_transport_ofi_CXI_avfd_set,
                             0, &ofi_coll_mc, &done_flag);

    OFI_CHECK_RETURN_STR(ret, "collective_join failed!!");

    if (ofi_coll_mc == NULL){
        PRINT_ERROR("coll_mc is NULL\n");
        shmem_global_exit(-FI_EINVAL);
    }
    PRINT_DEBUG("Coll_mc %p\n", ofi_coll_mc);

    ret = wait_for_join(ctx, FI_JOIN_COMPLETE, &done_flag);
    OFI_CHECK_RETURN_STR(ret, "join_wait time failed\n");

    shmem_transport_ofi_CXI_coll_addr = fi_mc_addr(ofi_coll_mc);


    ret = fi_barrier(ep, shmem_transport_ofi_CXI_coll_addr, &done_flag);
    OFI_CHECK_RETURN_STR(ret, "Barrier failed\n");

    ret = polling_time(ctx, &done_flag);
    OFI_CHECK_RETURN_STR(ret, "Polling failed\n");

}
