#ifndef _LOCKFREE_QUEUE_H_
#define _LOCKFREE_QUEUE_H_

#include <stdbool.h>
#include <stdatomic.h>
#include <stdalign.h>

#define CACHE_LINE_SIZE (64)

typedef enum {
    LFQ_OK,
    LFQ_ENOMEM,
    LFQ_EINVAL,
    LFQ_EUSRDEF,
    LFQ_EEMPTY,
}lfq_err_t;

typedef struct node node_t;
struct node {
    int data;
    _Atomic(node_t*) next;
    struct node* retired_next;
};

#define K (2) /*num of hazard pointers per-thread*/
typedef struct HPRecord hp_record_t; /*per-thread*/
struct HPRecord {
    atomic_bool active;
    node_t* rlist; /*retired list*/
    unsigned rcount; /*retired count*/
    _Atomic(node_t*) HP[K]; /*hazard pointers*/
    struct HPRecord* next;
}__attribute__ ((aligned (CACHE_LINE_SIZE)));

struct LFQueue;

typedef struct {
    int (*enqueueCallback)(struct LFQueue* me, int enqueue_data);
    int (*onEmptyCallback)(struct LFQueue* me);
}queue_attr_t;

struct LFQueue {
    alignas(CACHE_LINE_SIZE) _Atomic(node_t*) head;
    alignas(CACHE_LINE_SIZE) _Atomic(node_t*) tail;
    queue_attr_t attr;
};

int queue_attr_init(queue_attr_t* attr);

void LFQueue_set_error_callback(int (*errback)(const char *, ...));

int LFQueue_init(struct LFQueue* me, queue_attr_t* attr);
int LFQueue_destroy(struct LFQueue* me);
void LFQueue_cleanup_thread(void);

lfq_err_t enqueueLF(struct LFQueue* me, int data);
lfq_err_t dequeueLF(struct LFQueue* me, int* output);

#endif
