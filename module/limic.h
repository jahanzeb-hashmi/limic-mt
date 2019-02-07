/* Copyright (c) 2001-2016, The Ohio State University. All rights
 * reserved.
 *
 * This file is part of the MVAPICH2 software package developed by the
 * team members of The Ohio State University's Network-Based Computing
 * Laboratory (NBCL), headed by Professor Dhabaleswar K. (DK) Panda.
 *
 * For detailed copyright and licensing information, please refer to the
 * copyright file COPYRIGHT in the top level MVAPICH2 directory.
 *
 */

/* 
 * limic.h
 *  
 * LiMIC2:  Linux Kernel Module for High-Performance MPI Intra-Node 
 *          Communication
 * 
 * Author:  Hyun-Wook Jin <jinh@konkuk.ac.kr>
 *          System Software Laboratory
 *          Department of Computer Science and Engineering
 *          Konkuk University
 *
 * History: Jul 15 2007 Launch
 *
 *          Feb 27 2009 Modified by Karthik Gopalakrishnan (gopalakk@cse.ohio-state.edu)
 *                                  Jonathan Perkins       (perkinjo@cse.ohio-state.edu)
 *            - Automatically create /dev/limic
 *            - Add versioning to the Kernel Module
 *
 *          Oct 10 2009 Modified by Hyun-Wook Jin
 *            - Fragmented memory mapping & data copy
 *
 */

#ifndef _LIMIC_INCLUDED_
#define _LIMIC_INCLUDED_

#include <linux/init.h>
#include <linux/module.h> 
#include <linux/cdev.h>
#include <linux/types.h>
#include <linux/kdev_t.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/version.h>
#include <linux/highmem.h>
#include <linux/slab.h>
#include <linux/kernel.h>
#include <asm/page.h>
#include <asm/uaccess.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <asm/pgtable.h>

#include <linux/list.h>
#include <asm/kmap_types.h>
#include <linux/spinlock.h>
#include <linux/sched.h>
#include <linux/highmem.h>
#include <linux/vmalloc.h>
#include <linux/kthread.h>
#include <linux/delay.h>

#ifdef HAVE_LIMIC_LOCK
#include <linux/mutex.h>
#endif

#define LIMIC_MODULE_MAJOR 0
#define LIMIC_MODULE_MINOR 7

/*
 * Account for changes in device_create and device_destroy
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,13)
#   define CREATE_LIMIC_DEVICE
#   if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,15)
#       define device_create(cls, parent, devt, device, ...) \
            class_device_create(cls, devt, device, __VA_ARGS__)
#   elif LINUX_VERSION_CODE < KERNEL_VERSION(2,6,18)
#       define device_create(cls, parent, devt, device, ...) \
            class_device_create(cls, parent, devt, device, __VA_ARGS__)
#   elif LINUX_VERSION_CODE < KERNEL_VERSION(2,6,26)
#       define device_create(cls, parent, devt, device, ...) \
            device_create(cls, parent, devt, __VA_ARGS__)
#   elif LINUX_VERSION_CODE < KERNEL_VERSION(2,6,27)
#       define device_create            device_create_drvdata
#   endif
#   if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,18)
#       define device_destroy   class_device_destroy
#   endif
#endif

/* /dev file name */
#define DEV_NAME "limic"
#define DEV_CLASS "limic"

#define LIMIC_TX      0x1c01
#define LIMIC_RX      0x1c02
#define LIMIC_VERSION 0x1c03
#define LIMIC_TXW     0x1c04

#define LIMIC_TX_DONE    1
#define LIMIC_RX_DONE    2
#define LIMIC_VERSION_OK 3
#define LIMIC_TXW_DONE   4

#define NR_PAGES_4_FRAG (16 * 1024)

#ifdef HAVE_LIMIC_LOCK
struct mutex limic_lock;
#endif

typedef struct limic_user {
    int nr_pages;   /* pages actually referenced */
    size_t offset;     /* offset to start of valid data */
    size_t length;     /* number of valid bytes of data */

    unsigned long va;
    void *mm;        /* struct mm_struct * */
    void *tsk;       /* struct task_struct * */
} limic_user;

typedef struct limic_request {
    void *buf;       /* user buffer */
    size_t len;         /* buffer length */
    limic_user *lu;  /* shandle or rhandle */

    int needed_workers; /* how many workers a request is reqeusting */
    int pool_threshold; /* msg size after which pool is involved */

} limic_request;

typedef enum {
    CPY_TX,
    CPY_RX
} limic_copy_flag;


/********************* Multi-threaded Copy Impl. (begin) ************************/


/* Development Notes
 * Author: Jahanzeb Maqbool Hashmi
 * Date: 09-16-2016
 * Summary: A thread-pool based implementation for multithreaded
 * kernel-mapped copy implementation for optimized MPI communications 
 */


/*  struct workitem
 *      |__ list_head work_element
 *      |__ page **src_maplist;
 *      |__ page **dst_maplist;
 *      |__ size_t page_offset;
 *      |__ size_t num_pages;
 *      |__ offload 
 *      |__ flag
 *      |__ xxxxxxxx
 *
 *  struct worker 
 *      |__ task_struct *thread
 *      |__ list_head worker_element
 *      |__ logical_tid
 *      |__ wait_queue local (on which this thread sleeps/wakes-up)
 *      |__ xxxxxxxxx
 * 
 *  struct worker_pool 
 *      |__ list_head list_of_workers
 *      |__ spinlock_t threadpool_lock
 *      |__ xxxxxxxxx
 *
 *  struct workqueue_pool
 *      |__ list_head list_of_workqueues
 *      |__ spinlock_t workqueue_pool_lock
 *      |__ xxxxxxxxx
 *
 *  struct workqueue {
 *      |__ list_head workqueue_element (this workqueue is an element of the
 *                                      workqueue_pool)
 *      |__ list_head list_of_workitems (this is head of the list of posted 
 *                                      workitems to this workqueue)
 *      |__ int poll_counter (producer incr / consumer dec / producer polls)
 *      |__ spinlock_t workqueue_lock (lock to add/remove workitem
 *      |__ xxxxxxxxx 
 *
 *
 *  ** a list of pointers to specifiy which workqueue is associated to which
 *  set of workers after the request is served with M workers;  0 < M < K
 *
 *  struct tid_workqueue_map 
 *      |__ spinlock_t map_lock
 *      |__ struct workqueue* tid_workqueue_map [POOL_SIZE]
 *
 *      for each w in M;; tid_worklerlist_map [worker->tid] = &workqueue;
 *
 * NOTE: need to check if there is a better way to do this?
 *  
 *  struct oversub_list (a list of pointers to workqueues which couldn't get
 *                      threadpool resources due to oversubscription)
        |__ list_head *workqueue_ptr
 *      |__ spinlock
 *
 */

/************** ALGORITHMS AND DESIGNS *******************************************/
/*
 * Init
 * 1. Read system wide policy-file to determine the optimal size of chunk
 * per workitem. This policy-file will be generate after an empirical
 * evaluation based on system characteristics. 
 * 2. Initializes the thread_pool and workers based on policy-file params.
 *
 * MPI Process
 * 1. A limic_rx_copy request is posted (request)
 * 2. requests K workers from pool and gets K = min (K, pool.available)
 * 3. requests a workqueue from workqueue_pool to post its own workitems
 *          wq = workqueue_pool.get_workqueue()
 * 4. prepare workitems (set pages, offset, addresses etc...)
 * 5. post each workitem to wq; increment counter;  wq->poll_counter++
 * 6. for each worker w in K;
 *      spinlock_t (&map->map_lock)
 *      map->tid_workqueue_map[w->tid] = &wq
 *      spin_unlock (&map->map_lock)
 * 7.   wake-up (&w->local_waitqueue);
 * 8. start poll; while (wq->poll_counter != 0);
 * 9. once request completed;
 * 9. clear up the map (shouldn't the worker be doing it? making himself
 * free?)
 * 10. return the wq back to workqueue pool
 * 11. add workers in K, back to worker_pool
 * 12. return competions (xxxxxxx)
 *
 * Worker
 * 1. after start, go into interrupted sleep
 *      while (!kthread_should_stop()) {
 *          ... schedule ();
 *      } 
 * 2. upon waking up; get the workqueue reference from map against tid.
 *    if workqueue again tid is NULL, do nothing; go back to sleep
 * 3. while (!workqueue_empy) process workitem;
 * 3. go back to sleep
 *
*/

/****************************************************************************/

#define limic_debug 0
#define WORKER_POOL_SIZE 64
//#define WORKERS_REQUESTED 4
//#define WORKITEMS_PER_REQUEST 1
//#define WORKQUEUE_POOL_INITIAL_SIZE 16

#define MAX_CPU_ID 272
#define KTHREAD_SPIN_INTERVAL 1<<24

//typedef void (*Callback_wrapper) (struct Status_map *map, int workerset_count, int lid); 
typedef void (*Callback_wrapper) (); 

typedef struct Status_map {
    int key; /* logical worker id */
    int val; /* val = status (0 = working, 1 = completed */
} Status_map;

typedef struct Workqueue { /* now private to each worker */
    spinlock_t workqueue_lock;
    struct list_head list_of_workitems;
    int pending_workitems; /* num worklets remains to be completed */
} Workqueue;

typedef struct Workitem {
    struct list_head work_element;
    struct limic_user *src_req;
    struct limic_user *dst_req;
    struct page **src_maplist; /* sender's page map */
    struct page **dst_maplist; /* receiver's page map */
    size_t src_pg_offset; /* my offset in the global pagelist */
    size_t dst_pg_offset;		
    limic_copy_flag flag;
} Workitem;
 
typedef struct Worker {
    int lid;
    int local_completion; /* marks the completion of work */
    int exiting; /* flag indication whether its time to exit */
    struct list_head worker_element;
    struct task_struct *kthread; /* kthread for execution */
    struct Wrapper_context *ctx;
    struct Workqueue *wq; /*private wq containing my worklets */ 
    wait_queue_head_t kthread_waitqueue;
    int sleeping;
    
    Callback_wrapper callback; /* callback functino to set completion */
    Status_map *curr_smap; /* status map of workers in current itemset i am part of*/
    int curr_workerset_size; /* size of current workerset I am part of */

} Worker;

typedef struct Worker_pool {
    struct list_head list_of_workers;
    spinlock_t worker_pool_lock;
    int available_workers;
} Worker_pool;


typedef struct Wrapper_context {
    struct Worker_pool worker_pool;
    int worker_pool_sz;
    int self_worker; /* should process be a worker himself? */ 
    /* other stuff goes here; cpumask, binding info etc (later) */
} Wrapper_context;

/*
typedef struct Workqueue_pool {
    struct list_head list_of_workqueues;
    spinlock_t workqueue_pool_lock;
    int available_wqueues;
} Workqueue_pool;
*/
/*
typedef struct Thread_workqueue_map {
    spinlock_t map_lock;
    struct Workqueue *associated_wq;
} Thread_workqueue_map;
*/


/* define global constants (i.e., context)
 * FIXME: is there a better way to do this?
 */

static Wrapper_context *__global_context;
#define CREATE_CONTEXT() __global_context = \
                kmalloc (sizeof (Wrapper_context), GFP_KERNEL);
#define GET_CONTEXT() __global_context;
#define DESTROY_CONTEXT() kfree (__global_context);

#define dprintk(args...) do { \
    if (unlikely(limic_debug)) \
        printk(KERN_DEBUG "LiMIC: " args); \
} while (0)



/* forwared declarations of limic functions */

int limic_get_info(void *buf, size_t len, limic_user *lu);
struct page **limic_get_pages(limic_user *lu, int rw);
void limic_release_pages(struct page **maplist, int pgcount);
int limic_map_and_rxcopy(limic_request *req, struct page **maplist);
int limic_map_and_txcopy(limic_request *req, struct page **maplist);
struct page **limic_get_pages_receiver (void * buf, size_t len, int *nr_pages, int rw);
int limic_map_and_copy(limic_request *req, struct page **maplist, limic_copy_flag flag);


/* forward declarations of pool design functions */
static int limic_map_and_copy_opt ( limic_request *req,  struct page **maplist, 
                                     limic_copy_flag flag );
static int thread_work_fn (void *args);
static int initialize_context  (struct Wrapper_context *ctx);
//static int post_work_to_wq (struct Wrapper_context *ctx, 
//                            struct Workqueue *wq, struct Workitem *work); 
//static struct Workqueue * get_wq_from_workqueue_pool (struct Workqueue_pool *wq_pool);
static struct Worker * get_worker_from_worker_pool (struct Worker_pool *worker_pool);
static int process_request (struct Wrapper_context *ctx, struct limic_request *usr_req,
        struct page ** src_maplist, struct page ** dst_maplist, limic_copy_flag flag);
static int do_work_memcpy_pinned (struct Wrapper_context *ctx, struct Workitem *work); 
static int cleanup_pools_resources (struct Wrapper_context *ctx);
static int prepare_requests_for_workers (int num_workers, 
        struct limic_user *src_rqs, struct limic_user *dst_rqs, 
        struct limic_request *req, limic_copy_flag flag); 

void callbackfn_update_notify (struct Status_map *map, int workerset_count, int lid);

    
/* 
size_t list_size (struct list_head *list) {
    size_t count = 0;
    struct list_head *p;
    list_for_each (p, list) {
        count = count + 1;
    }
  return count;
}
*/
void callbackfn_update_notify (struct Status_map *map, int workerset_count, int workerid) {
    int i;
    /* find key agains my id, and update status */
    for (i = 0; i < workerset_count; i++) {
        if (map[i].key == workerid) {
            dprintk ("worker[%d] in callback, setting status \n", workerid);
            map[i].val = 1;
            //break; // in real scenario id will be unique, so we break
        }
    }
}


/* function to do copy (handles both aligned and unaligned */
static int do_work_memcpy_pinned (struct Wrapper_context *ctx, struct Workitem *work)
{
    int num_pages, sp, dp, ret;
    void *kaddr_src, *kaddr_dst;
    size_t src_pgnum, dst_pgnum, src_bytes, dst_bytes, src_length;
    size_t dst_length, src_offset, dst_offset, nbytes, len;

    src_pgnum = work->src_pg_offset;
    dst_pgnum = work->dst_pg_offset;
    src_length = work->src_req->length;
    dst_length = work->dst_req->length;
    src_offset = work->src_req->offset;
    dst_offset = work->dst_req->offset;
        
    num_pages = (work->src_req->nr_pages < work->dst_req->nr_pages) ? work->src_req->nr_pages : work->src_req->nr_pages;
    len = (src_length < dst_length) ? src_length : dst_length;
    
    sp = src_offset;
    dp = dst_offset;
    
    nbytes = 0; 
    ret = 0;

    /* map first pages */ 
    kaddr_src = kmap (work->src_maplist [src_pgnum]);
    kaddr_dst = kmap (work->dst_maplist [dst_pgnum]);
    
    while ( (len > 0) ) {
        src_bytes = PAGE_SIZE - sp;
        dst_bytes = PAGE_SIZE - dp;        
        
        if (src_bytes > len)
            src_bytes  = len;

        if (dst_bytes > len)
            dst_bytes  = len;

        nbytes = (src_bytes < dst_bytes) ? src_bytes : dst_bytes;
        
        if( work->flag == CPY_TX ) {
            memcpy ( kaddr_src+sp, kaddr_dst+dp, nbytes);
        }
        else if( work->flag == CPY_RX ){
            memcpy ( kaddr_dst+dp, kaddr_src+sp, nbytes); 
        }
        
        sp = (sp + nbytes) % PAGE_SIZE;
        dp = (dp + nbytes) % PAGE_SIZE;
        len -= nbytes;
        ret += nbytes;
        
        if ( sp == 0 && len > 0) {
            kunmap (work->src_maplist [src_pgnum]);
            src_pgnum++;
            kaddr_src = kmap (work->src_maplist [src_pgnum]);
        }
        if ( dp == 0 && len > 0) { 
            kunmap (work->dst_maplist [dst_pgnum]);
            dst_pgnum++;
            kaddr_dst = kmap (work->dst_maplist [dst_pgnum]);
        }
    }
    /* unmap the last remaining pages */
    kunmap (work->src_maplist [src_pgnum]);
    kunmap (work->dst_maplist [dst_pgnum]);
    
    /*returns total valid bytes copied successfully */
    return len; //returns total valid bytes copied successfully 
}

/* functions implementations */
/*static int do_work_memcpy_pinned (struct Wrapper_context *ctx, struct Workitem *work)
{
    
    int ret = 0;
    long pid = current->pid;
    void *kaddr_src, *kaddr_dst;
    size_t src_pgnum = work->page_offset;
    size_t dst_pgnum = work->page_offset;
    size_t src_bytes, dst_bytes, len;
    size_t src_length = work->src_req->length;
    size_t dst_length = work->dst_req->length;
    size_t src_offset = work->src_req->offset;
    size_t dst_offset = work->dst_req->offset;
    int num_pages = (work->src_req->nr_pages < work->dst_req->nr_pages) ? work->src_req->nr_pages : work->src_req->nr_pages;

    len = (src_length < dst_length) ? src_length : dst_length;
    int sp = src_offset, dp = dst_offset;
    size_t nbytes = 0; // min (src_bytes, dst_bytes)
    dprintk ("sp = %d , dp = %d \n", sp, dp);
    
    while (len > 0)  {
        src_bytes = PAGE_SIZE - sp;
        dst_bytes = PAGE_SIZE - dp;        

        if (src_bytes > len)
            src_bytes  = len;

        if (dst_bytes > len)
            dst_bytes  = len;

        nbytes = (src_bytes < dst_bytes) ? src_bytes : dst_bytes;

        kaddr_src = kmap (work->src_maplist [src_pgnum]);
        kaddr_dst = kmap (work->dst_maplist [dst_pgnum]);

        if( work->flag == CPY_TX ) {
            copy_from_user ( kaddr_src+sp, (void __user *)kaddr_dst+dp, nbytes);
        }
        else if( work->flag == CPY_RX ) {
            copy_to_user ( (void __user *)kaddr_dst+dp, kaddr_src+sp, nbytes); 
        }

        kunmap (work->src_maplist [src_pgnum]);
        kunmap (work->dst_maplist [dst_pgnum]);

        sp = 0;
        dp = 0;

        len -= nbytes;
        ret += nbytes;
        src_pgnum++;
        dst_pgnum++; 
    }
    
    dprintk ("[%d] len = %d \n", pid, len);
    return ret; //returns total valid bytes copied successfully 
}*/


static int prepare_requests_for_workers (int num_workers, 
        struct limic_user *src_rqs, struct limic_user *dst_rqs, 
        struct limic_request *req, limic_copy_flag flag) 
{
    limic_user *src_lu = req->lu;
    
    int i, addr_offset;
    unsigned long saddr = src_lu->va;
    unsigned long daddr = (unsigned long) req->buf;
    size_t chunk_bytes, total_bytes = req->len;
    
    addr_offset = 0;
    
    for (i  = 0; i < num_workers; i++) {
        chunk_bytes = (i!=num_workers-1) ? total_bytes / num_workers : 
                    (total_bytes / num_workers) + (total_bytes % num_workers);
        
        src_rqs[i].va = saddr + addr_offset; 
        src_rqs[i].offset = src_rqs[i].va & (PAGE_SIZE - 1);
        src_rqs[i].length = chunk_bytes;
        src_rqs[i].mm = src_lu->mm;
        src_rqs[i].tsk = src_lu->tsk;     
        src_rqs[i].nr_pages = (src_rqs[i].va + src_rqs[i].length + PAGE_SIZE -1) /
                    PAGE_SIZE - src_rqs[i].va/PAGE_SIZE;    
       
        if( !src_rqs[i].nr_pages ){
            printk("LiMIC: (limic_get_info) number of src pages is 0\n");
            return 1;
        }       

        dst_rqs[i].va = daddr + addr_offset;
        dst_rqs[i].offset = dst_rqs[i].va & (PAGE_SIZE - 1);
        dst_rqs[i].length = chunk_bytes;
        dst_rqs[i].mm = (void *)current->mm;
        dst_rqs[i].tsk = (void *) current;    
        dst_rqs[i].nr_pages = (dst_rqs[i].va + dst_rqs[i].length + PAGE_SIZE -1) /
                    PAGE_SIZE - dst_rqs[i].va/PAGE_SIZE;    

        if( !dst_rqs[i].nr_pages ){
            printk("LiMIC: (limic_get_info) number of dst pages is 0\n");
            //return -EINVAL;
            return 1; 
        } 
       
        /* need to add nr_pages*/
        addr_offset += (chunk_bytes); 
    }

  return 0; 
}

static int process_request (struct Wrapper_context *ctx, struct limic_request *usr_req,
        struct page ** src_maplist, struct page ** dst_maplist, limic_copy_flag flag) 
{
    int i, sum, requested_workers;
    struct Worker *worker;
    struct Workitem *work;
    struct limic_user *src_rqs, *dst_rqs;
    size_t src_pg_offset, dst_pg_offset;
    size_t sbytes_sofar, dbytes_sofar;
    struct Status_map *smap; 
    
    
    requested_workers = usr_req->needed_workers - 1;
    struct Worker *myworkers[requested_workers];

    //int requested_workers = WORKERS_REQUESTED - 1; // from template
    int obtained_workers = 0; // from template
    
    if (requested_workers < 1) {
        dprintk ("process_request: requested_workers 0 \n"); 
        return 1;  
    }
    
    for (i = 0; i < requested_workers; i++) {
        spin_lock (&ctx->worker_pool.worker_pool_lock);
        worker = get_worker_from_worker_pool(&ctx->worker_pool);
        spin_unlock (&ctx->worker_pool.worker_pool_lock);
        if (worker != NULL) {
            myworkers[obtained_workers] = worker;
            obtained_workers++;
        }
        else {
            break;
        }
    }
    /* this is workers obtained from pool + myself */
    obtained_workers += 1;

    dprintk ("process_request[%d]: requested %d workers. initiated %d workers \n",
                      current->pid, requested_workers, obtained_workers);

    src_rqs = kmalloc (sizeof (limic_user) * obtained_workers, GFP_KERNEL);    
    dst_rqs = kmalloc (sizeof (limic_user) * obtained_workers, GFP_KERNEL);    
    if (prepare_requests_for_workers (obtained_workers, src_rqs, dst_rqs,
                usr_req, flag)) {
        kfree (src_rqs);
        kfree (dst_rqs);
        return 1;
    }
    
    /* completion statuses list of currently obtained workers (workerset) */
    smap = kmalloc (sizeof (struct Status_map) * obtained_workers, GFP_KERNEL);
    
    src_pg_offset = dst_pg_offset = 0;
    sbytes_sofar = dbytes_sofar = 0;

    for (i = 0; i < obtained_workers; i++) {
        /* prepare work for current worker */ 
        work = kmalloc (sizeof (Workitem), GFP_KERNEL);
        INIT_LIST_HEAD (&work->work_element); 
        work->src_pg_offset = src_pg_offset; 
        work->dst_pg_offset = dst_pg_offset; 
        work->src_req = &src_rqs[i];
        work->dst_req = &dst_rqs[i]; 
        work->flag = flag;
        work->src_maplist = src_maplist;
        work->dst_maplist = dst_maplist;
        //pg_offset = pg_offset + (work->src_req->length / PAGE_SIZE);

        if (i == 0) {
            sbytes_sofar = work->src_req->offset;
            dbytes_sofar = work->dst_req->offset;	
        }
        /* prepare offsets for next worker */
        sbytes_sofar = sbytes_sofar + work->src_req->length;
        dbytes_sofar = dbytes_sofar + work->dst_req->length;
        src_pg_offset = sbytes_sofar / PAGE_SIZE;
        dst_pg_offset = dbytes_sofar / PAGE_SIZE;
        
        /* do work on last chunk of data yourself */
        if (i == (obtained_workers-1) ){
            do_work_memcpy_pinned (ctx, work);
        }
        else {

            /* get current worker */
            worker = myworkers[i];
            
            /* add current worker to my workerset */ 
            smap[i].key = worker->lid;
            smap[i].val = 0; /* MPI process will poll for 1 */
           
            worker->curr_workerset_size = obtained_workers; 
            worker->curr_smap = smap;
            worker->callback = callbackfn_update_notify; /*register callback */
            /* submit work to worker's workqueue */
            list_add(&work->work_element, &worker->wq->list_of_workitems);
            
            /* wake_up worker */        
            if (myworkers[i]->sleeping) {
                wake_up (&myworkers[i]->kthread_waitqueue);
                dprintk ("process_request[%d] wokeup %d\n", current->pid, myworkers[i]->lid);
            }
            else {
                dprintk ("process_request[%d] workers already woken %d\n", current->pid, myworkers[i]->lid);
            }

        }

    }
       
    dprintk ("process_request[%d]: finished my work\n", current->pid);



    /* Polling:
     * 1 - busy-wait: poll (basic) 
     * 2 - sleep based polling. Sleep on a waitqueue which workers have reference
     *     in workers. Last worker will wake me up !!
     * 3 - overlapped: Pick one workitem -> (do_work | =poll) -> check
     * again for work (this will overlap polling for others with useful
     * work)
     * */
    

    /* 1 - Busy wait */
    /* check worker->wq->pending_workitems == 0 && worker->local_completion == 1 */
    /*do {
        poll = 1;
        for (i = 0; i < obtained_workers; i++) {
            poll &= myworkers[i]->local_completion;
        }
    } while (poll != 1);
    */
    /*int poll;
    while (1) {
        poll = myworkers[0]->local_completion;
        for (i = 1; i < obtained_workers; i++) {
            poll &= myworkers[i]->local_completion;
        }
        printk ("[%d]: %d %d = %d\n", current->pid, myworkers[0]->local_completion, myworkers[1]->local_completion, poll);
        
        if (poll == 1) 
            break;
    }*/
    
    sum = 0;
    while (sum < obtained_workers-1) {
        sum = 0;
        for (i = 0; i < obtained_workers-1; i++) {
            //sum += myworkers[i]->local_completion;
            sum += smap[i].val;
        }
    }

    dprintk ("process_request[%d]: after Poll sum=%d\n", current->pid, sum);
    
    

    /* free up resources first */
    /*struct Workitem *tmp_work; 
    for (i = 0; i < obtained_workers; i++) {

        // size of worker list
        struct list_head *p;
        int count = 0;
        if (list_empty(&myworkers[i]->wq->list_of_workitems)) {
            dprintk ("worker[%d]: wq->workitmes list is empty\n", myworkers[i]->lid);
        }
        list_for_each(p, &myworkers[i]->wq->list_of_workitems) {
            count ++;
        }
        dprintk ("worker[%d]: wq->workitems count = %d\n", myworkers[i]->lid, count);


        dprintk ("going to free worker %d\n", i);
        list_for_each_entry_safe (work, tmp_work, &myworkers[i]->wq->list_of_workitems, 
            work_element) {
            //list_del (&work->work_element);
            list_del_init (&work->work_element);
            kfree (work);   
            dprintk ("process_request[%d]: freed workitem from wq%d\n", current->pid, i); 
        }
    }
    */ 
   

    /* 2 - poll based on sleep */
    /* Process (owner to workers) will wait on a waitqueue and the workers
     * (whoever finishes the job last) will wake it up */
 
    /*spin_lock (&wq->pending_completion_lock);
    if (wq->pending_completion != 0) {

        wq->already_done = 0;
        //unlock before going to sleep 
        spin_unlock (&wq->pending_completion_lock);
        
        // register to waitqueue  
        DECLARE_WAITQUEUE (wait, current); 
        add_wait_queue (&wq->owner_waitqueue, &wait);
        
        // schedule: goto sleep until last worker wake you up ! 
        set_current_state (TASK_INTERRUPTIBLE);
        schedule ();
        
        dprintk ("process_request: polling completed. pending_jobs = %d \n", 
                    wq->pending_completion);
        
        remove_wait_queue(&wq->owner_waitqueue, &wait); 
        // before exiting, set state to running 
        __set_current_state(TASK_RUNNING);

    } else {
        spin_unlock (&wq->pending_completion_lock);
    } */


    /* 3 - self_worker based polling : if we want this process to participate in 
     * work as well, then we have him perform the same thing that kthread will do 
     * once it is doing the work. 1) get work 2) perform work 3 repeat until no work.
     *
     *   spin_lock (&wq->workqueue_lock);
     *   while (!list_empty (&wq->list_of_workitems)) {
     *       work = list_entry (wq->list_of_workitems, struct Workitem, work_element);
     *       list_del (&work->work_element);   
     *
     *       spin_unlock (&wq->workqueue_lock);
     *       
     *       // go ahead and do the work //
     *       // ..... do_work () .....//
     *   
     *   }
     */   

    
    //FIXME: after callback, each worker will put back himself into pool
    //as soon as it finishes
    /* request is completed - reset wq and add back to workqueue_pool */

    /* polling done! now add the workers back to pool */ 
    /*spin_lock (&ctx->worker_pool.worker_pool_lock);
    for (i = 0; i < obtained_workers; i++) {
        list_add (&myworkers[i]->worker_element, &ctx->worker_pool.list_of_workers);
        ctx->worker_pool.available_workers ++;
    } 
    spin_unlock (&ctx->worker_pool.worker_pool_lock);
    */

    
    /* free up current workerset map */ 
    kfree (smap);
    /* clean the resources */
    kfree (src_rqs);
    kfree (dst_rqs);

    dprintk ("process_request[%d]: returning \n", current->pid); 
    return 0;
}



//FIXME: should we take total requested worker as argument and fetch all ?
static struct Worker * get_worker_from_worker_pool (struct Worker_pool *worker_pool) {
    struct Worker *worker = NULL;
    
    if (worker_pool != NULL) {
        //if (!list_empty (&worker_pool->list_of_workers)) {
        if (worker_pool->available_workers > 0) {
            worker = list_entry (worker_pool->list_of_workers.next, 
                                        struct Worker,  worker_element);
            list_del (&worker->worker_element);
            worker_pool->available_workers --;
        }
    }

    return worker;
}

/*
static struct Workqueue * get_wq_from_workqueue_pool (struct Workqueue_pool *wq_pool) {
    struct Workqueue *wq;
    
    spin_lock (&wq_pool->workqueue_pool_lock); 
    if (wq_pool != NULL) {
        if (wq_pool->available_wqueues > 0) {
            wq = list_entry (wq_pool->list_of_workqueues.next, 
                                    struct Workqueue,  workqueue_element);
            list_del (&wq->workqueue_element);
            wq_pool->available_wqueues --;
        } else {
            // create additional wq and add it to wq 
            wq = vmalloc (sizeof (Workqueue));       
            INIT_LIST_HEAD (&wq->workqueue_element);
            INIT_LIST_HEAD (&wq->list_of_workitems);
            init_waitqueue_head(&wq->owner_waitqueue);
            wq->pending_completion = 0;
            wq->already_done = 0;
            spin_lock_init (&wq->workqueue_lock);
            spin_lock_init (&wq->pending_completion_lock);
            list_add (&wq->workqueue_element, &wq_pool->list_of_workqueues); 
            wq_pool->available_wqueues ++; 
        }
    }
    spin_unlock (&wq_pool->workqueue_pool_lock);

    return wq;
}
static int post_work_to_wq (struct Wrapper_context *ctx,
                            struct Workqueue *wq, struct Workitem *work) {

    spin_lock (&wq->workqueue_lock);
    list_add_tail (&work->work_element, &wq->list_of_workitems);
    wq->pending_completion ++;
    spin_unlock (&wq->workqueue_lock);
    
    return 0;
}
*/

static int cleanup_pools_resources (struct Wrapper_context *ctx)
{

    struct Worker *worker, *tmp_worker;
    struct Workitem *work, *tmp_work;
    
    /* cleanup worker pool and it's resources */
    list_for_each_entry_safe (worker, tmp_worker, &ctx->worker_pool.list_of_workers,
                                            worker_element) {

        list_del (&worker->worker_element);

        /* first, set exiting flag to let thread_fn know that it has to skip the normal
         * codepath and directly go to exit */
        worker->exiting = 1;

        /* second, stop kthread */
        kthread_stop (worker->kthread);

        /* third, remove any pending taks from worker's wq, if any 
         * (in case of abnormal termination) */
        list_for_each_entry_safe (work, tmp_work, &worker->wq->list_of_workitems, 
                work_element) {
            list_del (&work->work_element);
            kfree (work);   
        }
        list_del (&worker->wq->list_of_workitems);

        /* fourth, free up the local wq of this worker */
        kfree (worker->wq);

        /* fifth, free the worker */
        kfree (worker);
    }    
    list_del (&ctx->worker_pool.list_of_workers);

    printk ("cleanup: worker pool resources deallocated ...\n");
    
    return 0;
}

static int initialize_context  (struct Wrapper_context *ctx)
{
    int i, err, cpu;
    char *threadfn_name;

    err = 0;
    cpu = MAX_CPU_ID;
    ctx->self_worker = 0;
    ctx->worker_pool_sz = WORKER_POOL_SIZE;
    ctx->worker_pool.available_workers = 0;
    
    /* initialize lists */    
    INIT_LIST_HEAD (&ctx->worker_pool.list_of_workers);
    dprintk ("init_ctx: pool lists initialized ...\n");
    
    /* init spin_locks */
    spin_lock_init (&ctx->worker_pool.worker_pool_lock);
    dprintk ("init_ctx: pool lists spin_locks initialized ...\n");
     
    for (i = 0; i < ctx->worker_pool_sz; i++) {
        struct Worker *worker = kmalloc (sizeof (Worker), GFP_KERNEL);
        worker->wq = kmalloc (sizeof (Workqueue), GFP_KERNEL);       
        
    	spin_lock_init (&worker->wq->workqueue_lock);
        INIT_LIST_HEAD (&worker->wq->list_of_workitems);
        worker->wq->pending_workitems = 0;

        /* init worker's wait_queue */
        init_waitqueue_head(&worker->kthread_waitqueue);

        /* initialize the worker, add to pool and call run */
        worker->lid = i;
        worker->ctx = ctx;
        worker->exiting = 0;
        worker->sleeping = 1;
        ctx->worker_pool.available_workers ++;
        worker->callback = NULL;
        worker->curr_smap = NULL;
        worker->curr_workerset_size = 0;
        list_add_tail (&worker->worker_element, &ctx->worker_pool.list_of_workers);
        
        threadfn_name = (char*)kmalloc( (30 * sizeof(char) ) , GFP_KERNEL);
        snprintf(threadfn_name, 30, "%s-%d", "threadfn", i);

        //printk ("%s \n", threadfn_name);        

        //worker->kthread = kthread_run (thread_work_fn, worker, "threadfn_name");
        worker->kthread = kthread_create (thread_work_fn, worker, threadfn_name);
        kthread_bind (worker->kthread, cpu);
        wake_up_process(worker->kthread);
        cpu = cpu-1;
    
        kfree (threadfn_name);
    } 
    
    dprintk ("init_ctx: worker_pool(%d/%d) created and initialized ...\n", ctx->worker_pool.available_workers, ctx->worker_pool_sz);
   
   return err; 
}


/* kthread work function, when created, the workers will fall into this */
static int thread_work_fn (void *args)
{
    struct Worker *worker = (Worker *)args;
    struct Wrapper_context *ctx = worker->ctx;
    int counter;

    printk (KERN_DEBUG "[%d] thread_fn: worker_id = %d\n", current->pid, worker->lid);
    
    counter = 0; 
    DECLARE_WAITQUEUE (wait, current); 
    add_wait_queue (&worker->kthread_waitqueue, &wait);
    
    set_current_state (TASK_INTERRUPTIBLE);
    while (!kthread_should_stop ()) {
        if (worker->exiting) goto bail_out;
        
        if ( (list_empty (&worker->wq->list_of_workitems)) &&
                (counter == KTHREAD_SPIN_INTERVAL) ) {
            worker->sleeping = 1;
            dprintk ("thread_fn[%d]: going to sleep \n", current->pid);
            schedule ();
            dprintk ("thread_fn[%d]: woke-up \n", current->pid);
            counter = 0;
            worker->sleeping = 0;
        }
        else {
            struct Workitem *work;
            
            if (!list_empty (&worker->wq->list_of_workitems)) {
                __set_current_state(TASK_RUNNING);
                
                counter = 0;
                while (!list_empty (&worker->wq->list_of_workitems)) {
                    
                    work = list_entry (worker->wq->list_of_workitems.next, 
                            struct Workitem, work_element);
                    
                    list_del (&work->work_element);  

                    do_work_memcpy_pinned (ctx, work);
                    // have to free it here because list_del has removed the entry from
                    // link-list
                    kfree (work);
                }
                
                /*callback function to update the status map to notify MPI proc*/    
                worker->callback(worker->curr_smap, worker->curr_workerset_size, worker->lid); 
                
                
                /* go ahead and add yourself to worker pool and make your
                 * available */
                spin_lock (&worker->ctx->worker_pool.worker_pool_lock);
                list_add (&worker->worker_element, 
                        &worker->ctx->worker_pool.list_of_workers);
                worker->ctx->worker_pool.available_workers ++;
                spin_unlock (&ctx->worker_pool.worker_pool_lock);
                
                //worker->local_completion = 1;
                set_current_state(TASK_INTERRUPTIBLE); // remove = 99% utilization
            }
            else {
                counter ++;
                worker->sleeping = 0;
            }
        } 



/*
        if (list_empty (&worker->wq->list_of_workitems)) {
            if (worker->exiting) 
                goto bail_out;
            
            if (counter == KTHREAD_SPIN_INTERVAL) { 
                dprintk ("thread_fn[%d]: going to sleep \n", current->pid);
                worker->sleeping = 1; 
                schedule ();
                dprintk ("thread_fn[%d]: woke-up \n", current->pid);
                counter = 0; 
                //remove_wait_queue(&worker->kthread_waitqueue, &wait); 
            }
            else {
                counter ++;
                worker->sleeping = 0;
                //set_current_state(TASK_INTERRUPTIBLE);
                //continue;
            }
        }
        else {
            __set_current_state(TASK_RUNNING);
        } 
	    
        struct Workitem *work;
        int item = 0;
        while (!list_empty (&worker->wq->list_of_workitems)) {
            
            work = list_entry (worker->wq->list_of_workitems.next, struct Workitem, 
                    work_element);
            
            list_del (&work->work_element);  

            do_work_memcpy_pinned (ctx, work);
            // have to free it here because list_del has removed the entry from
            // link-list
            kfree (work);
        }	

        worker->local_completion = 1;
        set_current_state(TASK_INTERRUPTIBLE); // remove = 99% utilization
*/

    }
    
bail_out:    
    remove_wait_queue(&worker->kthread_waitqueue, &wait); 
    
    /* before exiting, set state to running */
    __set_current_state(TASK_RUNNING);                                          
                                                                                    
    dprintk("Stopping kthread ... (%d)\n", worker->lid);

    return 0;
} 


/******************* Multi-threaded Copy Impl. (End)  ************************/


static int limic_map_and_copy_opt ( limic_request *req,
                                struct page **src_maplist,
                                limic_copy_flag flag )
{
    int err = 0;
    Wrapper_context *ctx;
    //limic_user *src_lu = req->lu;
    limic_user *dst_lu; 
    struct page ** dst_maplist; 
    unsigned long va;
    size_t pgcount;

    int req_size = req->len;

    //int requested_workers = WORKERS_REQUESTED;
    int requested_workers = req->needed_workers;
    int pool_threshold = req->pool_threshold;   
 
    //printk ("msg length = %d\n", req->len); 
    /* get global context and process request */
    /* Note: At this point, it should get the policy_template as well this 
     * should get policy_template as well and pass it to process_request
     */
    ctx = GET_CONTEXT();
    if (!ctx) {
        err = 1;
        goto bail_out;
    }
    
    /* check the current status of worker-pool, if none available, then proceed
     * to do copy yourself */ 
    if (ctx->worker_pool.available_workers < 1) {
        dprintk ("process[%d] no worker found, doing myself \n", current->pid);
        err = limic_map_and_copy (req, src_maplist, flag);
    }
    
    else {

        if (requested_workers < 0) {
            dprintk ("process_request: requested_workers < 0 \n"); 
            err = 1;    
            goto bail_out;  
        }
        //else if ( (requested_workers == 1) ) {
        //else if ( (req_size < 131072) || (requested_workers == 1) ) {
        //else if ( (req_size < 2097152) || (requested_workers == 1) ) {
        //else if ( (req_size < 1048576) || (requested_workers == 1) ) {
        else if ( (req_size < pool_threshold) || (requested_workers == 1) ) {
            /* process himself should do the normal copy */
            err = limic_map_and_copy (req, src_maplist, flag);
        }
        else {

            /* now we need a receiver's limic_user similar to sender's */ 
            dst_lu = kmalloc (sizeof (limic_user), GFP_KERNEL);
            if (!dst_lu) {
                err = 1;
                goto bail_out;
            }

            /* fill-up the dst limic_user */
            va = (unsigned long) req->buf;
            pgcount = (va + req->len + PAGE_SIZE -1)/PAGE_SIZE - va/PAGE_SIZE;
            if ( !pgcount ) {
                dprintk ("number of pages is 0\n");
                err = 1;
                goto bail_out;
            }

            dst_lu->va = va;
            dst_lu->mm = (void *) current->mm;
            dst_lu->tsk = (void *) current;
            dst_lu->nr_pages = pgcount;
            dst_lu->offset = va & (PAGE_SIZE-1);
            dst_lu->length = req->len;
            
            /* get the page map of receiver */
            dst_maplist = limic_get_pages (dst_lu, 1); /* WRITE=1 */ 
            if (!dst_maplist) {
                err = 1;
                goto bail_out;
            }
            /* start processing */
            if (process_request (ctx, req, src_maplist, dst_maplist, flag)) {
                err = 1;
                goto clear_up;
            }

        clear_up:
            /* clear up */
            limic_release_pages (dst_maplist, dst_lu->nr_pages);
            kfree (dst_lu);
        } 
    }

bail_out:
    return err;
}



/* normal copy of the limic which will be invoked if only one worker
is requested */
int limic_map_and_copy( limic_request *req, 
                        struct page **maplist, 
                        limic_copy_flag flag )
{
    limic_user *lu = req->lu;
    int pg_num = 0, ret = 0;
    size_t  offset = lu->offset;
    size_t pcount, len = (lu->length>req->len)?req->len:lu->length;
    void *kaddr, *buf = req->buf;
	
    lu->length = len; 
    while( ( pg_num <= lu->nr_pages ) && ( len > 0 ) ){
        pcount = PAGE_SIZE - offset;
        if (pcount > len)
            pcount = len;
	
        kaddr = kmap(maplist[pg_num]);

        if( flag == CPY_TX ){
            if( copy_from_user(kaddr+offset, buf, pcount) ){
                printk("LiMIC: (limic_map_and_copy) copy_from_user() is failed\n");
                return -EFAULT;
            }
        }
        else if( flag == CPY_RX ){
            if( copy_to_user(buf, kaddr+offset, pcount) ){
                printk("LiMIC: (limic_map_and_copy) copy_to_user() is failed\n");
                return -EFAULT;
            }
        }
	/* flush_dcache_page(maplist[pg_num]); */
        kunmap(maplist[pg_num]);

        len -= pcount;
        buf += pcount;
        ret += pcount;
        pg_num++;
        offset = 0;
    }

    return 0;
}




int limic_map_and_txcopy(limic_request *req, struct page **maplist)
{
    return limic_map_and_copy(req, maplist, CPY_TX);
    //return limic_map_and_copy_opt(req, maplist, CPY_TX);
}


int limic_map_and_rxcopy(limic_request *req, struct page **maplist)
{
    //return limic_map_and_copy(req, maplist, CPY_RX);
    return limic_map_and_copy_opt(req, maplist, CPY_RX);
}


void limic_release_pages(struct page **maplist, int pgcount)
{
    int i;
    struct page *map;
	
    for (i = 0; i < pgcount; i++) {
        map = maplist[i];
        if (map) {
            /* FIXME: cache flush missing for rw==READ
             * FIXME: call the correct reference counting function
             */
            page_cache_release(map); 
         }
    }

    kfree(maplist);
}


struct page **limic_get_pages(limic_user *lu, int write)
{
    int err, pgcount;
    struct mm_struct *mm;
    struct page **maplist;

    mm = lu->mm;
    pgcount = lu->nr_pages;

    maplist = kmalloc(pgcount * sizeof(struct page **), GFP_KERNEL);
    if (unlikely(!maplist)) 
        return NULL;
	 
    /* Try to fault in all of the necessary pages */
    down_read(&mm->mmap_sem); 
    //err = get_user_pages(lu->tsk, mm, lu->va, pgcount,
    //                     (rw==READ), 0, maplist, NULL); 
    err = get_user_pages(lu->tsk, mm, lu->va, pgcount,
                         write, 0, maplist, NULL); 
    up_read(&mm->mmap_sem);

    if (err < 0) { 
        limic_release_pages(maplist, pgcount); 
        return NULL;
    }
    lu->nr_pages = err;
 
    while (pgcount--) {
        /* FIXME: flush superflous for rw==READ,
         * probably wrong function for rw==WRITE
         */
        /* flush_dcache_page(maplist[pgcount]); */
    }
	
    return maplist;
}


int limic_get_info(void *buf, size_t len, limic_user *lu)
{
    limic_user limic_u;
    unsigned long va;
    int pgcount;

    va = (unsigned long)buf;
    limic_u.va = va;
    limic_u.mm = (void *)current->mm;
    limic_u.tsk = (void *)current;

    pgcount = (va + len + PAGE_SIZE - 1)/PAGE_SIZE - va/PAGE_SIZE;  

    if( !pgcount ){
        printk("LiMIC: (limic_get_info) number of pages is 0\n");
        return -EINVAL; 
    }       
    limic_u.nr_pages = pgcount;
    limic_u.offset = va & (PAGE_SIZE-1);
    limic_u.length = len;

    if( copy_to_user(lu, &limic_u, sizeof(limic_user)) ){
        printk("LiMIC: (limic_get_info) copy_to_user fail\n");
        return -EINVAL; 
    }       

    return 0;
}

#endif


/************************************************************************** /
 *
 * dev notes:
 * ----------
 * 09-17-2016: completed until process_request. Need to do the work
 * distribution and workitem creation. Afterwards, need to integrate with
 * existing limic
 * 
 * 09-18-2016: completed functionality and integrated into limic
 * TODO: add cleanup function for module exit
 */
