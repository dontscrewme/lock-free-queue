#include "LFQueue.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdint.h>

static int default_error_callback(const char *format, ...)
{
    int ret;
    va_list argptr;
    va_start(argptr, format);
    ret = vfprintf(stderr, format, argptr);
    va_end(argptr);
    return ret;
}

int (*LFQueue_error_callback)(const char *, ...) = default_error_callback;

void LFQueue_set_error_callback(int (*errback)(const char *, ...))
{
    if (!errback)
    {
        return;
    }

    LFQueue_error_callback = errback;
}

static atomic_uint g_numOfHPRecord = ATOMIC_VAR_INIT(0); /*total hazard pointers across all threads*/
static atomic_uint g_retireThreshold = ATOMIC_VAR_INIT(0);

static void update_retireThreshold(void)
{
    unsigned current_H = atomic_load_explicit(&g_numOfHPRecord, memory_order_relaxed);
    unsigned expected_R = atomic_load_explicit(&g_retireThreshold, memory_order_relaxed);
    unsigned desired_R = current_H << 2; /*H * (1+c), where c = 3*/

    int retry_count = 0;
    const int max_retires = 10;
    while (!atomic_compare_exchange_weak_explicit(&g_retireThreshold, &expected_R, desired_R,
                                                  memory_order_acq_rel, memory_order_acquire))
    {
        if (++retry_count >= max_retires)
        {
            break;
        }

        current_H = atomic_load_explicit(&g_numOfHPRecord, memory_order_relaxed);
        desired_R = current_H << 2;
    }
}

static inline void rlist_push(node_t **head, node_t *node)
{
    // node->next = *head;
    // atomic_store_explicit(&node->next, *head, memory_order_seq_cst);

    node->retired_next = *head;
    *head = node;
}
static inline node_t *rlist_pop(node_t **head)
{
    if (!head || !(*head))
    {

        return NULL;
    }

    node_t *node_to_return = *head;
    // *head = (*head)->next;
    // node_t* next_node = atomic_load_explicit(&node_to_return->next, memory_order_seq_cst); *head = next_node;
    *head = (*head)->retired_next;

    return node_to_return;
}

unsigned rlist_delete(node_t *head)
{
    if (!head)
    {
        return 0;
    }

    unsigned node_count = 0;

    node_t *curr_node = head;
    node_t *next_node = NULL;
    while (curr_node)
    {
        node_count++;
        // next_node = curr_node->next;
        // next_node = atomic_load_explicit(&curr_node->next, memory_order_seq_cst);
        next_node = curr_node->retired_next;
        free(curr_node);
        curr_node = next_node;
    }

    return node_count;
}

static _Atomic(hp_record_t*) g_HPRecordHead = NULL;
static _Thread_local hp_record_t *g_threadHPRecord = NULL;

static hp_record_t *HPRecord_allocate(void)
{
    hp_record_t *me = malloc(sizeof(hp_record_t));
    if (!me)
    {
        return NULL;
    }

    atomic_init(&me->active, true);
    me->rlist = NULL;
    me->rcount = 0;
    atomic_store_explicit(&me->HP[0], NULL, memory_order_relaxed);
    atomic_store_explicit(&me->HP[1], NULL, memory_order_relaxed);
    me->next = NULL;

    return me;
}

static unsigned HPRecord_free(hp_record_t *myhprec)
{
    if (!myhprec)
    {
        return 0;
    }

    unsigned node_count = rlist_delete(myhprec->rlist);
    free(myhprec);
    return node_count;
}

static unsigned HPRecord_freeAll(void)
{
    hp_record_t *curr_hprec = g_HPRecordHead;
    hp_record_t *next_hprec = NULL;

    unsigned total_node_count = 0;

    while (curr_hprec)
    {
        next_hprec = curr_hprec->next;
        total_node_count = total_node_count + HPRecord_free(curr_hprec);
        curr_hprec = next_hprec;
    }

    g_HPRecordHead = NULL;

    return total_node_count;
}

static void HPRecord_push(hp_record_t *myhprec)
{
    hp_record_t *oldhead = atomic_load_explicit(&g_HPRecordHead, memory_order_relaxed);
    do
    {
        myhprec->next = oldhead;
    } while (!atomic_compare_exchange_weak_explicit(&g_HPRecordHead, &oldhead, myhprec,
                                                    memory_order_acq_rel, memory_order_acquire));
}

static hp_record_t* HPRecord_tryReuse(void) 
{
    hp_record_t *hprec = NULL;
    for (hprec = atomic_load_explicit(&g_HPRecordHead, memory_order_seq_cst); hprec != NULL; hprec = hprec->next)
    {
        if (atomic_load_explicit(&hprec->active, memory_order_seq_cst))
        {
            continue;
        }

        bool expected = false;
        if (!atomic_compare_exchange_strong_explicit(&hprec->active, &expected, true, memory_order_seq_cst, memory_order_seq_cst))
        {
            /*TAS failed, the lock was already held by another thread.*/
            continue;
        }

        return hprec;
    }

    return NULL;
}

static void HPRecord_deactivate(hp_record_t *myhprec)
{
    atomic_store_explicit(&myhprec->active, false, memory_order_release);

    for (unsigned i = 0; i < K; i++)
    {
        atomic_store_explicit(&myhprec->HP[i], NULL, memory_order_release);
    }
}

static inline hp_record_t *getThreadHPRecord(void)
{
    if (g_threadHPRecord)
    {
        return g_threadHPRecord;
    }

    g_threadHPRecord = HPRecord_tryReuse();
    if (!g_threadHPRecord)
    {
        g_threadHPRecord = HPRecord_allocate();
        if (!g_threadHPRecord)
        {
            LFQueue_error_callback("%s: HPRecord_allocate() failed\n", __func__);
            return NULL;
        }
        HPRecord_push(g_threadHPRecord);
    }

    atomic_fetch_add_explicit(&g_numOfHPRecord, K, memory_order_relaxed);
    update_retireThreshold();
    return g_threadHPRecord;
}

void LFQueue_cleanup_thread(void)
{
    hp_record_t *myhprec = getThreadHPRecord();
    if (!myhprec)
    {
        return;
    }

    HPRecord_deactivate(myhprec);
    g_threadHPRecord = NULL;
    atomic_fetch_sub_explicit(&g_numOfHPRecord, K, memory_order_relaxed);
    update_retireThreshold();
}

typedef struct plist_entry
{
    node_t *node;
    struct plist_entry *next;

} plist_entry_t;
struct plist
{
    plist_entry_t **table;
    unsigned count;
    unsigned size;
};

static inline uintptr_t plist_hash(uintptr_t key)
{
    key = ~key + (key << 15); /*Shift left and add complement*/
    key = key ^ (key >> 12);  /*XOR with shifted right*/
    key = key + (key << 2);   /*Shift left and add*/
    key = key ^ (key >> 4);   /*XOR with shifted right*/
    key = key * 2057;         /*Multiply by a prime number*/
    key = key ^ (key >> 16);  /*XOR with shifted right*/
    return key;
}
struct plist *plist_init(unsigned size)
{

    struct plist *me = malloc(sizeof(struct plist));
    if (!me)
    {
        return NULL;
    }

    me->table = calloc(size, sizeof(plist_entry_t *));
    if (!me->table)
    {
        free(me);
        return NULL;
    }

    me->count = 0;
    me->size = size;

    return me;
}

void plist_free(struct plist *me)
{
    if (!me)
    {
        return;
    }

    for (unsigned i = 0; i < me->size; i++)
    {
        plist_entry_t *curr = me->table[i];
        while (curr)
        {
            plist_entry_t *prev = curr;
            curr = curr->next;
            free(prev);
        }
    }

    free(me->table);
    free(me);
}

void plist_insert(struct plist *me, node_t *node)
{
    if (!me || !node)
    {
        return;
    }

    unsigned index = (unsigned)(plist_hash((uintptr_t)node) % me->size);
    plist_entry_t *entry = malloc(sizeof(plist_entry_t));
    if (!entry)
    {
        return;
    }

    entry->node = node;
    entry->next = me->table[index];
    me->table[index] = entry;
    me->count++;
}

int plist_lookup(struct plist *me, node_t *node)
{
    if (!me || !node)
    {
        return 0;
    }

    unsigned index = (unsigned)(plist_hash((uintptr_t)node) % me->size);
    plist_entry_t *curr = me->table[index];
    while (curr)
    {
        if (curr->node == node)
        {
            return 1;
        }
        curr = curr->next;
    }

    return 0;
}

void Scan(hp_record_t *myhprec)
{
    unsigned current_H = atomic_load_explicit(&g_numOfHPRecord, memory_order_relaxed);
    unsigned size = current_H + (current_H >> 1); /*approximately 1.5H*/
    if (size == 0)
    {
        LFQueue_error_callback("plist size shouldn't be zero. Set it to default value\n");
        const unsigned default_size = 100;
        size = default_size;
    }

    struct plist *plist = plist_init(size);
    if (!plist)
    {
        LFQueue_error_callback("plist_init() failed");
        return;
    }

    hp_record_t *hprec = atomic_load_explicit(&g_HPRecordHead, memory_order_acquire);
    while (hprec != NULL)
    {
        for (unsigned i = 0; i < K; i++)
        {
            node_t *hptr = atomic_load_explicit(&hprec->HP[i], memory_order_acquire);
            if (hptr != NULL)
            {
                plist_insert(plist, hptr);
            }
        }
        hprec = hprec->next;
    }

    node_t *tmplist = myhprec->rlist;
    myhprec->rlist = NULL;
    myhprec->rcount = 0;
    node_t *node = rlist_pop(&tmplist);
    while (node != NULL)
    {
        if (plist_lookup(plist, node))
        {
            rlist_push(&myhprec->rlist, node);
            myhprec->rcount++;
        }
        else
        {
            /*PrepareForReuse(node);*/
            free(node);
        }
        node = rlist_pop(&tmplist);
    }

    plist_free(plist);
}

void HelpScan(hp_record_t *myhprec)
{
    hp_record_t *hprec = NULL;
    for (hprec = atomic_load_explicit(&g_HPRecordHead, memory_order_acquire); hprec != NULL; hprec = hprec->next)
    {

        if (atomic_load_explicit(&hprec->active, memory_order_acquire))
        {
            continue;
        }

        bool expected = false;
        if (!atomic_compare_exchange_strong_explicit(&hprec->active, &expected, true, memory_order_acq_rel, memory_order_relaxed))
        {
            /*TAS failed, the lock was already held by another thread.*/
            continue;
        }

        while (hprec->rcount > 0)
        {
            node_t *node = rlist_pop(&hprec->rlist);
            hprec->rcount--;
            rlist_push(&myhprec->rlist, node);
            myhprec->rcount++;
            if (myhprec->rcount >= atomic_load_explicit(&g_retireThreshold, memory_order_relaxed))
            {
                Scan(myhprec);
            }
        }

        atomic_store_explicit(&hprec->active, false, memory_order_release);
    }
}

void retireNode(hp_record_t *myhprec, node_t *node)
{
    rlist_push(&myhprec->rlist, node);
    myhprec->rcount++;
    if (myhprec->rcount >= atomic_load_explicit(&g_retireThreshold, memory_order_relaxed))
    {
        Scan(myhprec);
        HelpScan(myhprec);
    }
}

int queue_attr_init(queue_attr_t* attr) {
    if (!attr) {
        LFQueue_error_callback("%s: invalid input\n", __func__);
        return -1;
    }
    attr->enqueueCallback = NULL;
    attr->onEmptyCallback = NULL;
    return 0;
}

int LFQueue_init(struct LFQueue* me, queue_attr_t* attr)
{
    if (!me)
    {
        LFQueue_error_callback("%s: invalid input\n", __func__);
        return -1;
    }

    node_t *dummy = malloc(sizeof(struct node));
    if (!dummy)
    {
        LFQueue_error_callback("%s: malloc() for dummy node failed\n", __func__);
        return -1;
    }

    atomic_init(&dummy->next, NULL);
    atomic_init(&me->head, dummy);
    atomic_init(&me->tail, dummy);

    if (!attr) {
        queue_attr_init(&me->attr);
    }
    else {
        me->attr = *attr;
    }

    return 0;
}

int LFQueue_destroy(struct LFQueue *me)
{
    if (!me)
    {
        LFQueue_error_callback("%s: invalid input\n", __func__);
        return -1;
    }

    node_t *curr = atomic_load_explicit(&me->head, memory_order_relaxed);
    node_t *next = NULL;
    while (curr)
    {
        next = atomic_load_explicit(&curr->next, memory_order_relaxed);
        free(curr);
        curr = next;
    }

    me->head = me->tail = NULL;

    HPRecord_freeAll();

    return 0;
}

lfq_err_t enqueueLF(struct LFQueue *me, int data)
{
    if (!me)
    {
        LFQueue_error_callback("%s: invalid input\n", __func__);
        return LFQ_EINVAL;
    }

    if (me->attr.enqueueCallback && (me->attr.enqueueCallback(me, data) != 0)) {
        return LFQ_EUSRDEF;
    }

    hp_record_t *myhprec = getThreadHPRecord();
    if (!myhprec)
    {
        LFQueue_error_callback("%s: getThreadHPRecord() failed\n", __func__);
        return LFQ_ENOMEM;
    }

    node_t *newNode = malloc(sizeof(struct node));
    if (!newNode)
    {
        LFQueue_error_callback("%s: malloc() failed\n", __func__);
        return LFQ_ENOMEM;
    }

    newNode->data = data;
    atomic_store_explicit(&newNode->next, NULL, memory_order_relaxed);

    node_t *t = NULL;
    node_t *next = NULL;
    while (1)
    {
        t = atomic_load_explicit(&me->tail, memory_order_acquire);
        atomic_store_explicit(&myhprec->HP[0], t, memory_order_release);
        if (atomic_load_explicit(&me->tail, memory_order_acquire) != t)
        {
            continue;
        }
        
        next = atomic_load_explicit(&t->next, memory_order_acquire);
        if (next != NULL)
        {
            atomic_compare_exchange_strong_explicit(&me->tail, &t, next, memory_order_acq_rel, memory_order_relaxed);
            continue;
        }

        node_t *expected = NULL;
        if (atomic_compare_exchange_strong_explicit(&t->next, &expected, newNode, memory_order_acq_rel, memory_order_relaxed))
        {
            break;
        }
    }

    atomic_compare_exchange_strong_explicit(&me->tail, &t, newNode, memory_order_acq_rel, memory_order_relaxed);

    return LFQ_OK;
}

lfq_err_t dequeueLF(struct LFQueue *me, int *output)
{
    if (!me || !output)
    {
        LFQueue_error_callback("%s: invalid input\n", __func__);
        return LFQ_EINVAL;
    }

    hp_record_t *myhprec = getThreadHPRecord();
    if (!myhprec)
    {
        LFQueue_error_callback("%s: getThreadHPRecord() failed\n", __func__);
        return LFQ_ENOMEM;
    }

    node_t *h = NULL;
    node_t *t = NULL;
    node_t *next = NULL;
    while (1)
    {
        h = atomic_load_explicit(&me->head, memory_order_acquire);
        atomic_store_explicit(&myhprec->HP[0], h, memory_order_release);
        if (atomic_load_explicit(&me->head, memory_order_acquire) != h)
        {
            continue;
        }

        t = atomic_load_explicit(&me->tail, memory_order_acquire);
        next = atomic_load_explicit(&h->next, memory_order_acquire);
        atomic_store_explicit(&myhprec->HP[1], next, memory_order_release);
        if (atomic_load_explicit(&me->head, memory_order_acquire) != h)
        {
            continue;
        }

        if (next == NULL)
        { /*is empty*/
            if (me->attr.onEmptyCallback) {
                me->attr.onEmptyCallback(me);
            }
            return LFQ_EEMPTY;
        }

        if (h == t)
        {
            atomic_compare_exchange_strong_explicit(&me->tail, &t, next, memory_order_acq_rel, memory_order_relaxed);
            continue;
        }

        if (atomic_compare_exchange_strong_explicit(&me->head, &h, next, memory_order_acq_rel, memory_order_relaxed))
        {
            break;
        }
    }

    *output = h->data;
    retireNode(myhprec, h);

    return LFQ_OK;
}
