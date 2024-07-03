#ifndef __RSWAP_SCHEDULER_H
#define __RSWAP_SCHEDULER_H

#include "rswap_rdma.h"
#include "utils.h"
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/spinlock.h>
#include <linux/types.h>

// #define RSWAP_SCHEDULER_CORE 7
#define RSWAP_SCHEDULER_CORE 46
#define RSWAP_VQUEUE_MAX_SIZE 2048

struct rswap_request {
	pgoff_t offset;
	struct page *page;
};

static inline int rswap_request_copy(struct rswap_request *dst,
                                     struct rswap_request *src)
{
	if (!src || !dst) {
		return -EINVAL;
	}
	memcpy(dst, src, sizeof(struct rswap_request));

	return 0;
}

struct rswap_vqueue {
	atomic_t cnt;
	int max_cnt;
	unsigned head;
	unsigned tail;
	struct rswap_request *reqs;
	spinlock_t lock;
};

int rswap_vqueue_init(struct rswap_vqueue *queue);
int rswap_vqueue_destroy(struct rswap_vqueue *queue);
int rswap_vqueue_enqueue(struct rswap_vqueue *queue,
                         struct rswap_request *request);
int rswap_vqueue_dequeue(struct rswap_vqueue *queue,
                         struct rswap_request **request);
int rswap_vqueue_drain(int cpu, enum rdma_queue_type type);

#define RSWAP_PROC_NAME_LEN 32
#define RSWAP_ONLINE_CORES 96
struct rswap_proc {
	char name[RSWAP_PROC_NAME_LEN];
	int bw_weight;
	int num_threads;
	int cores[RSWAP_ONLINE_CORES]; // online cores
	int priorities[NUM_QP_TYPE]; // the larger the lower priority
	atomic_t sent_pkts[NUM_QP_TYPE];  // #(packets sent) for bandwidth control
	int ongoing_pkts;

	int total_pkts[NUM_QP_TYPE]; // total #(packets) for bw measurement

	spinlock_t lock;
	struct list_head list_node;
};
int rswap_proc_init(struct rswap_proc *proc, char *name);
int rswap_proc_destroy(struct rswap_proc *proc);
int rswap_proc_clr_weight(struct rswap_proc *proc);
int rswap_proc_set_weight(struct rswap_proc *proc, int bw_weight, int num_threads, int *cores);

int rswap_proc_get_total_pkts(struct rswap_proc *proc, char *buf);
int rswap_proc_clr_total_pkts(struct rswap_proc *proc);

void rswap_proc_send_pkts_inc(struct rswap_proc *proc,
                              enum rdma_queue_type type);
void rswap_proc_send_pkts_dec(struct rswap_proc *proc,
                              enum rdma_queue_type type);
bool is_proc_throttled(struct rswap_proc *proc, enum rdma_queue_type type);

// for swap RDMA bandwidth control
void rswap_activate_bw_control(int enable);
void rswap_get_all_procs_total_pkts(int *num_procs, char *buf);

struct rswap_vqtriple {
	int id;                  // identifier of the queue
	struct rswap_proc *proc; // which proc the vqtriple belongs to
	struct rswap_vqueue qs[NUM_QP_TYPE];
};

int rswap_vqtriple_init(struct rswap_vqtriple *vqtri, int id);
int rswap_vqtriple_destroy(struct rswap_vqtriple *vqtri);

struct rswap_vqlist {
	int cnt;
	struct rswap_vqtriple *vqtris; // array of queue triples
	spinlock_t lock;
};

int rswap_vqlist_init(void);
int rswap_vqlist_destroy(void);
struct rswap_vqueue *rswap_vqlist_get(int qid, enum rdma_queue_type type);
struct rswap_vqtriple *rswap_vqlist_get_triple(int qid);

struct rswap_scheduler {
	struct rswap_vqlist *vqlist;
	struct rdma_session_context *rdma_session;

	struct task_struct *scher_thd;

	int total_bw_weight;
	atomic_t total_sent_pkts[NUM_QP_TYPE];
	spinlock_t lock;
	struct list_head proc_list;
};

int rswap_scheduler_init(void);
int rswap_scheduler_stop(void);
int rswap_scheduler_thread(void *args); // struct rswap_scheduler *

extern struct rswap_vqlist *global_rswap_vqueue_list;
extern struct rswap_scheduler *gloabl_rswap_scheduler;

// utils
#define print_err(errno)                                                       \
	pr_err(KERN_ERR "%s, line %d : %d\n", __func__, __LINE__, errno)

#endif /* __RSWAP_SCHEDULER_H */
