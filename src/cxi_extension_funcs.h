#ifndef _CXI_EXTENSION_FUNCS_H_
#define _CXI_EXTENSION_FUNCS_H_

#include "shmem_ofi_ctx.h"

extern internal_addr_t myaddr; 

enum shmem_ofi_list_end {
    SHMEM_LIST_TAIL,
    SHMEM_LIST_HEAD
};

typedef struct d_entry {
    struct d_entry  *next;
    struct d_entry  *prev;
}d_entry_t;


#define DLIST_INIT(addr) { addr, addr }
#define DEFINE_LIST(name) struct d_entry name = DLIST_INIT(&name)

static inline void d_init(struct d_entry *head)
{
        head->next = head;
            head->prev = head;
}

static inline int d_empty(struct d_entry *head)
{
        return head->next == head;
}


static inline void
d_insert_after(struct d_entry *item, struct d_entry *head)
{
        item->next = head->next;
            item->prev = head;
                head->next->prev = item;
                    head->next = item;
}

static inline void
d_insert_before(struct d_entry *item, struct d_entry *head)
{
        d_insert_after(item, head->prev);
}

#define d_insert_head d_insert_after
#define d_insert_tail d_insert_before


static inline void d_remove(struct d_entry *item)
{
    item->prev->next = item->next;
    item->next->prev = item->prev;
}

static inline void d_remove_init(struct d_entry *item)
{
    d_remove(item);
    d_init(item);
}

#define d_first_entry_or_null(head, type, member) ({    \
        struct d_entry *pos = (head)->next;             \
        pos != (head) ? container_of((pos), type, member) : NULL;   \
        })

#define d_pop_front(head, type, container, member)          \
    do {                                \
        container = container_of((head)->next, type, member);   \
        d_remove((head)->next);             \
    } while (0)

#define d_foreach(head, item)                       \
    for ((item) = (head)->next; (item) != (head); (item) = (item)->next)

#define d_foreach_reverse(head, item)                   \
    for ((item) = (head)->prev; (item) != (head); (item) = (item)->prev)

#define d_foreach_container(head, type, container, member)          \
    for ((container) = container_of((head)->next, type, member);        \
            &((container)->member) != (head);                  \
            (container) = container_of((container)->member.next,       \
                type, member))

#define d_foreach_container_reverse(head, type, container, member)      \
    for ((container) = container_of((head)->prev, type, member);        \
            &((container)->member) != (head);                  \
            (container) = container_of((container)->member.prev,       \
                type, member))

#define d_foreach_container_reverse_safe(head, type, container, member, tmp)\
    for ((container) = container_of((head)->prev, type, member),        \
            (tmp) = (container)->member.prev;                  \
            &((container)->member) != (head);                  \
            (container) = container_of((tmp), type, member),           \
            (tmp) = (container)->member.prev)

typedef int d_func_t(struct d_entry *item, const void *arg);

    static inline int
d_match_func_same_entry(struct d_entry *item,
        const void *arg)
{
    return item == arg;
}

    static inline struct d_entry *
d_find_first_match(struct d_entry *head, d_func_t *match,
        const void *arg)
{
    struct d_entry *item;

    d_foreach(head, item) {
        if (match(item, arg))
            return item;
    }

    return NULL;
}

    static inline bool
d_entry_in_list(struct d_entry *head,
        struct d_entry *entry)
{
    if (d_find_first_match(head, &d_match_func_same_entry,
                (void *) entry))
        return true;

    return false;
}

    static inline struct d_entry *
d_remove_first_match(struct d_entry *head, d_func_t *match,
        const void *arg)
{
    struct d_entry *item;

    item = d_find_first_match(head, match, arg);
    if (item)
        d_remove(item);

    return item;
}

static inline void d_insert_order(struct d_entry *head, d_func_t *order,
        struct d_entry *entry)
{
    struct d_entry *item;

    item = d_find_first_match(head, order, entry);
    if (item)
        d_insert_before(entry, item);
    else
        d_insert_tail(entry, head);
}

typedef struct join_item {
    d_entry_t   entry;
    struct fid_av_set *avset;
    struct fid_mc     *mc;
    int join_index;
    int prov_errno;
    int retval;
} join_item_t;

typedef struct avset_ary {
    struct fid_av_set **avset;
    int avset_cnt;
    int avset_siz;
} avset_ary_t;

static void avset_ary_init(struct avset_ary *setary)
{
    setary->avset = NULL;
    setary->avset_cnt = 0;
    setary->avset_siz = 0;
}

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



/*End that direct steal */


#define NODENAME "SLURMD_NODENAME"
#define JOBID "FI_CXI_JOB_ID"
#define JOBSTEP "FI_CXI_COLL_JOB_STEP_ID"
#define MGR_URL "FI_CXI_COLL_FABRIC_MGR_URL"
#define MCAST_TOKEN "FI_CXI_COLL_MCAST_TOKEN"
#define ADDRS_PER_JOB "FI_CXI_HWCOLL_ADDRS_PER_JOB"
#define MIN_NODES "FI_CXI_HWCOLL_MIN_NODES"
#define NODELIST "SLURM_NODELIST"
#define NICS_PER_RANK "PMI_NUM_HSNS"

/* TODO: Currently applies ONLY to libfabrics implementations
 * TODO: ALSO needs Slingshot and OFI setups here 
 */

int shmem_collective_nic_initialization(void);


/* begin ben shenanigans - Start Jan 13 2026 */

#ifndef container_of
#define container_of(ptr, type, field) \
        ((type *) ((char *) ptr - offsetof(type, field)))
#endif

#define BEN_DEBUG 0

#if BEN_DEBUG /* BEN_DEBUG == 1 */
#ifndef PRINT_DEBUG
#define PRINT_DEBUG(fmt, args...)                               \
    do {                                                        \
        fflush(stdout);                                         \
        fflush(stderr);                                         \
        fprintf(stderr, "[rank_%d][%s][%s:%d] "fmt,             \
                        shmem_internal_my_pe,                   \
                        __FILE__, __func__, __LINE__,           \
                        ##args);                                \
    } while(0);
#endif /* PRINT_DEBUG */
#else /* BEN_DEBUG != 1 */

#define PRINT_DEBUG(...)

#endif /* BEN_DEBUG */

#ifndef PRINT_ERROR
#define PRINT_ERROR(fmt, args...)                               \
    do {                                                        \
        fflush(stdout);                                         \
        fflush(stderr);                                         \
        fprintf(stderr, "[rank_%d][%s][%s:%d][ERROR] "fmt,             \
                        shmem_internal_my_pe,                   \
                        __FILE__, __func__, __LINE__,           \
                        ##args);                                \
    } while(0);
#endif /* PRINT_ERROR */



/* Libfabric shenanignas */


extern struct fid_fabric*              shmem_transport_ofi_CXI_fabfd;
extern struct fid_domain*              shmem_transport_ofi_CXI_domain_fd;
extern struct fid_av*                  shmem_transport_ofi_CXI_avfd;
extern struct fid_av_set*              shmem_transport_ofi_CXI_avfd_set;
extern fi_addr_t                       shmem_transport_ofi_CXI_world_addr;
extern fi_addr_t                       shmem_transport_ofi_CXI_coll_addr;
extern fi_addr_t                       shmem_transport_ofi_CXI_my_addr;
extern fi_addr_t                       *shmem_transport_ofi_CXI_addr_table;
extern struct fid_ep                   *shmem_transport_ofi_CXI_target_ep;
extern struct fid_ep                   *shmem_transport_ofi_CXI_recv_ep;
extern struct fid_cq                   *shmem_transport_ofi_CXI_target_cq;
extern struct fid_cq                   *shmem_transport_ofi_CXI_recv_cq;
extern  fi_addr_t                      *CXI_addr_table; 


int wait_for_join(shmem_transport_ctx_t *ctx, uint32_t signal, void *context); 


#endif /* _CXI_EXTENSION_FUNCS_H_ */
