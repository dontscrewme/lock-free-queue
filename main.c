#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "LFQueue.h"

typedef struct
{
    pthread_mutex_t lock;
    pthread_cond_t start_cond;
    int start_flag;
    atomic_ulong total_items_produced;
    atomic_ulong total_items_consumed;
} sync_primitives_t;

typedef struct
{
    sync_primitives_t sync;
    struct LFQueue queue;
    unsigned long total_items;
} thread_args_t;

void *producer_thread(void *arg)
{
    thread_args_t *args = (thread_args_t *)arg;

    pthread_mutex_lock(&(args->sync.lock));
    while (!(args->sync.start_flag))
    {
        pthread_cond_wait(&(args->sync.start_cond), &(args->sync.lock));
    }
    pthread_mutex_unlock(&(args->sync.lock));

    int value = 1;
    while (1)
    {
        unsigned long old_count = atomic_fetch_add(&(args->sync.total_items_produced), 1);
        if (old_count >= args->total_items)
        {
            atomic_fetch_sub(&(args->sync.total_items_produced), 1);
            break;
        }

        lfq_err_t ret = enqueueLF(&(args->queue), value);
        if (ret != LFQ_OK)
        {
            atomic_fetch_sub(&(args->sync.total_items_produced), 1);
        }
    }

    LFQueue_cleanup_thread();

    return NULL;
}

void *consumer_thread(void *arg)
{
    thread_args_t *args = (thread_args_t *)arg;

    pthread_mutex_lock(&(args->sync.lock));
    while (!(args->sync.start_flag))
    {
        pthread_cond_wait(&(args->sync.start_cond), &(args->sync.lock));
    }
    pthread_mutex_unlock(&(args->sync.lock));

    int value = 0;
    while (1)
    {
        lfq_err_t ret = dequeueLF(&(args->queue), &value);

        if (ret == LFQ_OK)
        {
            atomic_fetch_add(&(args->sync.total_items_consumed), 1);
        }
        else
        {
            unsigned long consumed_count = atomic_load(&(args->sync.total_items_consumed));
            if (consumed_count >= args->total_items)
            {
                break;
            }
        }
    }

    LFQueue_cleanup_thread();

    return NULL;
}

int isolated_enqueue_test(unsigned num_producers, unsigned long total_items)
{
    printf("Isolated enqueue concurrency test with %d producer(s), %lu items to enqueue: ", num_producers, total_items);

    thread_args_t args = {
        .sync = {
            .lock = PTHREAD_MUTEX_INITIALIZER,
            .start_cond = PTHREAD_COND_INITIALIZER,
            .total_items_produced = ATOMIC_VAR_INIT(0),
            .start_flag = 0},
        .queue = {0},
        .total_items = total_items,
    };

    LFQueue_init(&args.queue, NULL);

    pthread_t producer_threads[num_producers];
    for (unsigned i = 0; i < num_producers; i++)
    {
        if (pthread_create(&producer_threads[i], NULL, producer_thread, &args) !=
            0)
        {
            fprintf(stderr, "Failed to create producer thread %d.\n", i);
            exit(EXIT_FAILURE);
        }
    }

    pthread_mutex_lock(&args.sync.lock);
    args.sync.start_flag = 1;
    pthread_cond_broadcast(&args.sync.start_cond);
    pthread_mutex_unlock(&args.sync.lock);

    for (unsigned i = 0; i < num_producers; i++)
    {
        pthread_join(producer_threads[i], NULL);
    }

    LFQueue_destroy(&args.queue);

    /* Determine test result */
    unsigned long expected_produced = total_items;
    unsigned long actual_produced = atomic_load(&(args.sync.total_items_produced));

    if (expected_produced != actual_produced)
    {
        printf("FAILED\n");
        printf("Mismatch: Expected Produced (%lu), Actual Produced (%lu)\n",
               expected_produced, actual_produced);
        exit(EXIT_FAILURE);
        return -1;
    }

    printf("SUCCESS\n");

    return 0;
}

int isolated_dequeue_test(unsigned num_consumers, unsigned long total_items)
{
    printf("Isolated dequeue concurrency test with %d consumer(s), %lu items to dequeue: ", num_consumers, total_items);

    thread_args_t args = {
        .sync = {.lock = PTHREAD_MUTEX_INITIALIZER,
                 .start_cond = PTHREAD_COND_INITIALIZER,
                 .start_flag = 0,
                 .total_items_produced = ATOMIC_VAR_INIT(0),
                 .total_items_consumed = ATOMIC_VAR_INIT(0)},
        .queue = {0},
        .total_items = total_items,
    };

    LFQueue_init(&args.queue, NULL);

    /* Pre-enqueue known number of items */
    pthread_t producer_threads = {0};
    if (pthread_create(&producer_threads, NULL, producer_thread, &args) != 0)
    {
        fprintf(stderr, "Failed to create producer thread.\n");
        exit(EXIT_FAILURE);
    }

    pthread_mutex_lock(&args.sync.lock);
    args.sync.start_flag = 1;
    pthread_cond_broadcast(&args.sync.start_cond);
    pthread_mutex_unlock(&args.sync.lock);

    pthread_join(producer_threads, NULL);

    args.sync.start_flag = 0;

    pthread_t consumer_threads[num_consumers];
    for (unsigned i = 0; i < num_consumers; i++)
    {
        if (pthread_create(&consumer_threads[i], NULL, consumer_thread, &args) !=
            0)
        {
            fprintf(stderr, "Failed to create consumer thread %d.\n", i);
            exit(EXIT_FAILURE);
        }
    }

    pthread_mutex_lock(&args.sync.lock);
    args.sync.start_flag = 1;
    pthread_cond_broadcast(&args.sync.start_cond);
    pthread_mutex_unlock(&args.sync.lock);

    /* Wait for all consumer threads to finish */
    for (unsigned i = 0; i < num_consumers; i++)
    {
        pthread_join(consumer_threads[i], NULL);
    }

    LFQueue_destroy(&args.queue);

    /* Validate the total number of consumed items */
    unsigned long expected_produced = total_items;
    unsigned long expected_consumed = total_items;
    unsigned long actual_produced = atomic_load(&(args.sync.total_items_produced));
    unsigned long actual_consumed = atomic_load(&(args.sync.total_items_consumed));

    /* Determine test result */
    if (expected_produced != actual_produced)
    {
        printf("FAILED\n");
        printf("Mismatch: Expected Produced (%lu), Actual Produced (%lu)\n",
               expected_produced, actual_produced);
        exit(EXIT_FAILURE);
        return -1;
    }

    if (actual_consumed != expected_consumed)
    {
        printf("FAILED\n");
        printf("Mismatch: Expected Consumed (%lu), Actual Consumed (%lu)\n",
               expected_consumed, actual_consumed);
        exit(EXIT_FAILURE);
        return -1;
    }

    printf("SUCCESS\n");

    return 0;
}

int integrated_test(unsigned num_producers, unsigned num_consumers, unsigned long total_items)
{
    printf("Integrated concurrency test with %d producer(s)/%d consumer(s), %lu items to enqueue/dequeue: ",
           num_producers, num_consumers, total_items);

    thread_args_t args = {
        .sync = {.lock = PTHREAD_MUTEX_INITIALIZER,
                 .start_cond = PTHREAD_COND_INITIALIZER,
                 .start_flag = 0,
                 .total_items_produced = ATOMIC_VAR_INIT(0),
                 .total_items_consumed = ATOMIC_VAR_INIT(0)},
        .queue = {0},
        .total_items = total_items,
    };

    LFQueue_init(&args.queue, NULL);

    pthread_t producer_threads[num_producers];
    for (unsigned i = 0; i < num_producers; i++)
    {
        if (pthread_create(&producer_threads[i], NULL, producer_thread, &args) !=
            0)
        {
            fprintf(stderr, "Failed to create producer thread %d.\n", i);
            exit(EXIT_FAILURE);
        }
    }

    pthread_t consumer_threads[num_consumers];
    for (unsigned i = 0; i < num_consumers; i++)
    {
        if (pthread_create(&consumer_threads[i], NULL, consumer_thread, &args) !=
            0)
        {
            fprintf(stderr, "Failed to create consumer thread %d.\n", i);
            exit(EXIT_FAILURE);
        }
    }

    pthread_mutex_lock(&args.sync.lock);
    args.sync.start_flag = 1;
    pthread_cond_broadcast(&args.sync.start_cond);
    pthread_mutex_unlock(&args.sync.lock);

    for (unsigned i = 0; i < num_producers; i++)
    {
        pthread_join(producer_threads[i], NULL);
    }

    for (unsigned i = 0; i < num_consumers; i++)
    {
        pthread_join(consumer_threads[i], NULL);
    }

    LFQueue_destroy(&args.queue);

    /* Determine test result */
    unsigned long expected_produced = total_items;
    unsigned long expected_consumed = total_items;
    unsigned long actual_produced = atomic_load(&(args.sync.total_items_produced));
    unsigned long actual_consumed = atomic_load(&(args.sync.total_items_consumed));

    if (expected_produced != actual_produced)
    {
        printf("FAILED\n");
        printf("Mismatch: Expected Produced (%lu), Actual Produced (%lu)\n",
               expected_produced, actual_produced);

        exit(EXIT_FAILURE);
        return -1;
    }

    if (actual_consumed != expected_consumed)
    {
        printf("FAILED\n");
        printf("Mismatch: Expected Consumed (%lu), Actual Consumed (%lu)\n",
               expected_consumed, actual_consumed);
        exit(EXIT_FAILURE);
        return -1;
    }

    printf("SUCCESS\n");

    return 0;
}

void print_usage(const char* program_name) {
    printf("Usage: %s [items] [iterations] [test_number]\n", program_name);
    printf("Test numbers:\n");
    printf(" 1: Enqueue test with 1 producer\n");
    printf(" 2: Enqueue test with 10 producers\n");
    printf(" 3: Dequeue test with 1 consumer\n");
    printf(" 4: Dequeue test with 10 consumers\n");
    printf(" 5: Integrated test with 1 producer, 1 consumer\n");
    printf(" 6: Integrated test with 1 producer, 10 consumers\n");
    printf(" 7: Integrated test with 10 producers, 1 consumer\n");
    printf(" 8: Integrated test with 10 producers, 10 consumers\n");
    printf(" 0: Run all tests (default)\n");
}


int main(int argc, char **argv)
{
    unsigned long total_items = 10000;
    unsigned max = 0;
    int test_number = -1;

    if (argc >= 2)
    {
        total_items = strtoul(argv[1], NULL, 10);
    }
    if (argc >= 3)
    {
        max = atoi(argv[2]);
    }
    if (argc >= 4)
    {
        test_number = atoi(argv[3]);
    }

    if (test_number < 0 || test_number > 8)
    {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }   

    printf("RUNNING TEST: %lu items to enqueue/dequeue, iteration %u times:\n", total_items, max);

    switch (test_number)
    {
        case 0: // All tests
            for (unsigned i = 0; i < max; i++)
                isolated_enqueue_test(1, total_items);

            for (unsigned i = 0; i < max; i++)
                isolated_enqueue_test(10, total_items);

            for (unsigned i = 0; i < max; i++)
                isolated_dequeue_test(1, total_items);

            for (unsigned i = 0; i < max; i++)
                isolated_dequeue_test(10, total_items);

            for (unsigned i = 0; i < max; i++)
                integrated_test(1, 1, total_items);

            for (unsigned i = 0; i < max; i++)
                integrated_test(1, 10, total_items);

            for (unsigned i = 0; i < max; i++)
                integrated_test(10, 1, total_items);

            for (unsigned i = 0; i < max; i++)
                integrated_test(10, 10, total_items);
            break;

        case 1:
            for (unsigned i = 0; i < max; i++)
                isolated_enqueue_test(1, total_items);
            break;

        case 2:
            for (unsigned i = 0; i < max; i++)
                isolated_enqueue_test(10, total_items);
            break;

        case 3:
            for (unsigned i = 0; i < max; i++)
                isolated_dequeue_test(1, total_items);
            break;

        case 4:
            for (unsigned i = 0; i < max; i++)
                isolated_dequeue_test(10, total_items);
            break;

        case 5:
            for (unsigned i = 0; i < max; i++)
                integrated_test(1, 1, total_items);
            break;

        case 6:
            for (unsigned i = 0; i < max; i++)
                integrated_test(1, 10, total_items);
            break;

        case 7:
            for (unsigned i = 0; i < max; i++)
                integrated_test(10, 1, total_items);
            break;

        case 8:
            for (unsigned i = 0; i < max; i++)
                integrated_test(10, 10, total_items);
            break;
    }

    return EXIT_SUCCESS;
}
