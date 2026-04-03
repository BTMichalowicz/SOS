
#include "cxi_extension_funcs.h"
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

int shmem_collective_nic_initialization(void){

    shmem_transport_ctx_t *ctx = &shmem_transport_ctx_default;
    int err = FI_SUCCESS, i = 0, local_size = 0;
   // fi_addr_t *fi_addrs = NULL;
    internal_addr_t *alladdrs = NULL;
    local_size = ctx->nics_per_rank * NICSIZE;
    if (ctx->NIC_array){
        return FI_SUCCESS;
    }

 //   err = query_for_fabric(&shmem_transport_ofi_CXI_info);
 //   OFI_CHECK_RETURN_STR(err, "CXI info query failed\n");
 //   err = allocate_fabric_resources(&shmem_transport_ofi_CXI_info);
 //   OFI_CHECK_RETURN_STR(err, "CXI info fabric allocation failed\n");

    PRINT_DEBUG("Initialization \n");
    ctx->NIC_array = calloc(shmem_internal_num_pes, local_size);
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
    alladdrs = calloc(shmem_internal_num_pes, sizeof(internal_addr_t));
    if (alladdrs == NULL){
        err = -FI_ENOMEM;
        goto fail;
    }

    shmem_transport_ofi_CXI_addr_table = calloc(shmem_internal_num_pes, sizeof(fi_addr_t));
    if (!shmem_transport_ofi_CXI_addr_table){
        PRINT_ERROR("NO MEMORY\n");
        return -FI_ENOMEM;
    }

    PRINT_DEBUG("Fetching local_NIC\n");

    err = -FI_EFAULT;
    PRINT_DEBUG("ctx->nics_per_rank: %d\n", ctx->nics_per_rank);
    for (i = 0 ; i < ctx->nics_per_rank; i++){
        get_local_nic(ctx, i, &local_nics[i]);
    }

//    for (i = 0; i < ctx->num_nics; i++){
        PRINT_DEBUG("LOCAL rank=%2d hsn=%d nic=%05x\n",
                local_nics[0].rank,
                local_nics[0].hsn,
                local_nics[0].nic);
//    }


    PRINT_DEBUG("Local NICs retrieved\n");

    nic_addr_t *shmem_nics = shmem_malloc(shmem_internal_num_pes* local_size);
    nic_addr_t *shmem_nics_2 = shmem_malloc(local_size);
    memcpy(shmem_nics_2, local_nics, local_size);
    PRINT_DEBUG("Beginning shmem collect\n");
    shmem_fcollectmem(SHMEM_TEAM_WORLD, shmem_nics, shmem_nics_2, local_size); 
    PRINT_DEBUG("SHMEM Collect worked\n");
    memcpy(ctx->NIC_array, shmem_nics, local_size*shmem_internal_num_pes);
    PRINT_DEBUG("Memcpy worked\n");
    //shmem_free(shmem_nics_2);
    //shmem_free(shmem_nics);
    shmem_nics_2 = NULL;
    shmem_nics = NULL;


    for (i = 0; i < ctx->num_nics; i++){
        PRINT_DEBUG("i %d rank=%2d hsn=%d nic=%05x\n",
                i, ctx->NIC_array[i].rank,
                ctx->NIC_array[i].hsn,
                ctx->NIC_array[i].nic);
    }


    PRINT_DEBUG("Sorting NICs\n");
    ctx->num_nics = shmem_internal_num_pes * ctx->nics_per_rank;
    qsort(ctx->NIC_array, ctx->num_nics, NICSIZE, _compare);

    for (i = 0; i < ctx->num_nics; i++){
        PRINT_DEBUG("i %d rank=%2d hsn=%d nic=%05x\n",
                i, ctx->NIC_array[i].rank,
                ctx->NIC_array[i].hsn,
                ctx->NIC_array[i].nic);
    }

    PRINT_DEBUG("Starting to add NIC addresses\n");
    for (i = 0; i < shmem_internal_num_pes; i++){
        alladdrs[i].nic = ctx->NIC_array[i].nic;
    }

    for (i = 0; i < ctx->num_nics; i++){
        PRINT_DEBUG("i %d rank=%2d hsn=%d nic=%05x\n",
                i, ctx->NIC_array[i].rank,
                ctx->NIC_array[i].hsn,
                ctx->NIC_array[i].nic);
    }

    PRINT_DEBUG("OFI CXI init\n");

    err = fi_fabric(shmem_transport_ofi_info.p_info->fabric_attr, &shmem_transport_ofi_CXI_fabfd, NULL);
    OFI_CHECK_RETURN_STR(err, "CXI fab failed\n");
    if (shmem_transport_ofi_CXI_fabfd == NULL){
        PRINT_ERROR("fabric fd is null\n");
        goto fail;
    }

    PRINT_DEBUG("fabric set up\n");
    
    err = fi_domain(shmem_transport_ofi_CXI_fabfd, shmem_transport_ofi_info.p_info,
            &shmem_transport_ofi_CXI_domainfd, NULL);
    OFI_CHECK_RETURN_STR(err, "CXI domain init failed\n");

    if (shmem_transport_ofi_CXI_domainfd == NULL){
        PRINT_ERROR("domainfd is NULL\n");
        goto fail;
    }
    PRINT_DEBUG("Domain set up\n");



    struct fi_av_attr av_attr = {};
    av_attr.type = FI_AV_TABLE;

    err = fi_av_open(shmem_transport_ofi_CXI_domainfd, &av_attr,
            &shmem_transport_ofi_CXI_avfd, NULL);
    OFI_CHECK_RETURN_STR(err, "CXI domain init failed\n");

    if (shmem_transport_ofi_CXI_avfd == NULL){
        PRINT_ERROR("avfd is null!!\n");
        goto fail;
    }



    err = fi_endpoint(shmem_transport_ofi_CXI_domainfd, shmem_transport_ofi_CXI_info.p_info,
            &shmem_transport_ofi_CXI_target_ep, NULL);
    

    OFI_CHECK_RETURN_STR(err, "CXI ep failed\n");
    
    err = fi_ep_bind(shmem_transport_ofi_CXI_target_ep, &shmem_transport_ofi_CXI_avfd->fid, 0);
    OFI_CHECK_RETURN_STR(err, "CXI_fi_ep_bind AV to target endpoint failed");

    struct fi_cq_attr cq_attr = {0};

    err = fi_cq_open(shmem_transport_ofi_CXI_domainfd, &cq_attr,
                     &shmem_transport_ofi_CXI_target_cq, NULL);
    OFI_CHECK_RETURN_MSG(err, "cq_open failed (%s)\n", fi_strerror(errno));

    err = fi_ep_bind(shmem_transport_ofi_CXI_target_ep,
                     &shmem_transport_ofi_CXI_target_cq->fid, FI_SELECTIVE_COMPLETION | FI_TRANSMIT | FI_RECV);
    OFI_CHECK_RETURN_STR(err, "fi_ep_bind CQ to target endpoint failed");

    err = fi_enable(shmem_transport_ofi_CXI_target_ep);
    OFI_CHECK_RETURN_STR(err, "fi_enable on target endpoint failed");

    ctx->ep = shmem_transport_ofi_CXI_target_ep;
    

    PRINT_DEBUG("shmem_transport_ofi_CXI_avfd: %p\n", shmem_transport_ofi_CXI_avfd);

    err = fi_av_insert(shmem_transport_ofi_CXI_avfd, alladdrs, shmem_internal_num_pes,
            shmem_transport_ofi_CXI_addr_table, 0, NULL);
    if (err != shmem_internal_num_pes){
        PRINT_ERROR("Failed to insert all addresses: %d\n", err);
        goto fail;
    }

    PRINT_DEBUG("AV insertion is done! Time to set everything else up; addr_table: %p\n", 
            shmem_transport_ofi_CXI_addr_table);

    fi_addr_t myaddr;
    size_t caddrlen;
    myaddr = shmem_transport_ofi_CXI_addr_table[shmem_internal_my_pe];
    err = fi_av_lookup(shmem_transport_ofi_CXI_avfd, myaddr, &shmem_transport_ofi_CXI_my_addr, &caddrlen);
    OFI_CHECK_RETURN_STR(err, "fi_av_lookup test failed\n");

    PRINT_DEBUG("my_addr 0x%lx\n", shmem_transport_ofi_CXI_my_addr);


    PRINT_DEBUG("Finished here. Performing a joining of collectives and avset configuration\n");

    return err;


fail:
    ctx->num_nics = 0;
    if (ctx->NIC_array) free(ctx->NIC_array);
    if (local_nics) free(local_nics);
    PRINT_ERROR("FAILED nic initialization: %d\n", err);
    return err;
}



