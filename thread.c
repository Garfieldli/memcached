/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 * Thread management for memcached.
 */
#include "memcached.h"
#include <assert.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <pthread.h>

#ifdef __sun
#include <atomic.h>
#endif

#define ITEMS_PER_ALLOC 64

/* An item in the connection queue. */
typedef struct conn_queue_item CQ_ITEM;
struct conn_queue_item {
    int               sfd;
    enum conn_states  init_state;
    int               event_flags;
    int               read_buffer_size;
    enum network_transport     transport;
    CQ_ITEM          *next;
};

/* A connection queue. */
typedef struct conn_queue CQ;
struct conn_queue {
    CQ_ITEM *head;
    CQ_ITEM *tail;
    pthread_mutex_t lock;
};

/* Lock for cache operations (item_*, assoc_*) */
pthread_mutex_t cache_lock;

/* Connection lock around accepting new connections */
pthread_mutex_t conn_lock = PTHREAD_MUTEX_INITIALIZER;

#if !defined(HAVE_GCC_ATOMICS) && !defined(__sun)
pthread_mutex_t atomics_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif

/* Lock for global stats */
static pthread_mutex_t stats_lock;

/* Free list of CQ_ITEM structs */
static CQ_ITEM *cqi_freelist;
static pthread_mutex_t cqi_freelist_lock;

static pthread_mutex_t *item_locks;
/* size of the item lock hash table */
static uint32_t item_lock_count;
static unsigned int item_lock_hashpower;
#define hashsize(n) ((unsigned long int)1<<(n))
#define hashmask(n) (hashsize(n)-1)
/* this lock is temporarily engaged during a hash table expansion */
static pthread_mutex_t item_global_lock;
/* thread-specific variable for deeply finding the item lock type */
static pthread_key_t item_lock_type_key;

static LIBEVENT_DISPATCHER_THREAD dispatcher_thread;

/*
 * Each libevent instance has a wakeup pipe, which other threads
 * can use to signal that they've put a new connection on its queue.
 */
static LIBEVENT_THREAD *threads;

static LIBEVENT_LOG_THREAD *log_threads;


/*
 * Number of worker threads that have finished setting themselves up.
 */
static int init_count = 0;
static pthread_mutex_t init_lock;
static pthread_cond_t init_cond;


static void thread_libevent_process(int fd, short which, void *arg);

unsigned short refcount_incr(unsigned short *refcount) {
#ifdef HAVE_GCC_ATOMICS
    return __sync_add_and_fetch(refcount, 1);
#elif defined(__sun)
    return atomic_inc_ushort_nv(refcount);
#else
    unsigned short res;
    mutex_lock(&atomics_mutex);
    (*refcount)++;
    res = *refcount;
    mutex_unlock(&atomics_mutex);
    return res;
#endif
}

unsigned short refcount_decr(unsigned short *refcount) {
#ifdef HAVE_GCC_ATOMICS
    return __sync_sub_and_fetch(refcount, 1);
#elif defined(__sun)
    return atomic_dec_ushort_nv(refcount);
#else
    unsigned short res;
    mutex_lock(&atomics_mutex);
    (*refcount)--;
    res = *refcount;
    mutex_unlock(&atomics_mutex);
    return res;
#endif
}

/* Convenience functions for calling *only* when in ITEM_LOCK_GLOBAL mode */
void item_lock_global(void) {
    mutex_lock(&item_global_lock);
}

void item_unlock_global(void) {
    mutex_unlock(&item_global_lock);
}

void item_lock(uint32_t hv) {
    uint8_t *lock_type = pthread_getspecific(item_lock_type_key);
    if (likely(*lock_type == ITEM_LOCK_GRANULAR)) {
        mutex_lock(&item_locks[hv & hashmask(item_lock_hashpower)]);
    } else {
        mutex_lock(&item_global_lock);
    }
}

/* Special case. When ITEM_LOCK_GLOBAL mode is enabled, this should become a
 * no-op, as it's only called from within the item lock if necessary.
 * However, we can't mix a no-op and threads which are still synchronizing to
 * GLOBAL. So instead we just always try to lock. When in GLOBAL mode this
 * turns into an effective no-op. Threads re-synchronize after the power level
 * switch so it should stay safe.
 */
void *item_trylock(uint32_t hv) {
    pthread_mutex_t *lock = &item_locks[hv & hashmask(item_lock_hashpower)];
    if (pthread_mutex_trylock(lock) == 0) {
        return lock;
    }
    return NULL;
}

void item_trylock_unlock(void *lock) {
    mutex_unlock((pthread_mutex_t *) lock);
}

void item_unlock(uint32_t hv) {
    uint8_t *lock_type = pthread_getspecific(item_lock_type_key);
    if (likely(*lock_type == ITEM_LOCK_GRANULAR)) {
        mutex_unlock(&item_locks[hv & hashmask(item_lock_hashpower)]);
    } else {
        mutex_unlock(&item_global_lock);
    }
}

static void wait_for_thread_registration(int nthreads) {
    while (init_count < nthreads) {
        pthread_cond_wait(&init_cond, &init_lock);
    }
}

static void register_thread_initialized(void) {
    pthread_mutex_lock(&init_lock);
    init_count++;
    pthread_cond_signal(&init_cond);
    pthread_mutex_unlock(&init_lock);
}

void switch_item_lock_type(enum item_lock_types type) {
    char buf[1];
    int i;

    switch (type) {
        case ITEM_LOCK_GRANULAR:
            buf[0] = 'l';
            break;
        case ITEM_LOCK_GLOBAL:
            buf[0] = 'g';
            break;
        default:
            fprintf(stderr, "Unknown lock type: %d\n", type);
            assert(1 == 0);
            break;
    }

    pthread_mutex_lock(&init_lock);
    init_count = 0;
    for (i = 0; i < settings.num_threads; i++) {
        if (write(threads[i].notify_send_fd, buf, 1) != 1) {
            perror("Failed writing to notify pipe");
            /* TODO: This is a fatal problem. Can it ever happen temporarily? */
        }
    }
    wait_for_thread_registration(settings.num_threads);
    pthread_mutex_unlock(&init_lock);
}

/*
 * Initializes a connection queue.
 */
static void cq_init(CQ *cq) {
    pthread_mutex_init(&cq->lock, NULL);
    cq->head = NULL;
    cq->tail = NULL;
}

/*
 * Looks for an item on a connection queue, but doesn't block if there isn't
 * one.
 * Returns the item, or NULL if no item is available
 */
static CQ_ITEM *cq_pop(CQ *cq) {
    CQ_ITEM *item;

    pthread_mutex_lock(&cq->lock);
    item = cq->head;
    if (NULL != item) {
        cq->head = item->next;
        if (NULL == cq->head)
            cq->tail = NULL;
    }
    pthread_mutex_unlock(&cq->lock);

    return item;
}

/*
 * Adds an item to a connection queue.
 */
static void cq_push(CQ *cq, CQ_ITEM *item) {
    item->next = NULL;

    pthread_mutex_lock(&cq->lock);
    if (NULL == cq->tail)
        cq->head = item;
    else
        cq->tail->next = item;
    cq->tail = item;
    pthread_mutex_unlock(&cq->lock);
}

/*
 * Returns a fresh connection queue item.
 */
static CQ_ITEM *cqi_new(void) {
    CQ_ITEM *item = NULL;
    pthread_mutex_lock(&cqi_freelist_lock);
    if (cqi_freelist) {
        item = cqi_freelist;
        cqi_freelist = item->next;
    }
    pthread_mutex_unlock(&cqi_freelist_lock);

    if (NULL == item) {
        int i;

        /* Allocate a bunch of items at once to reduce fragmentation */
        item = malloc(sizeof(CQ_ITEM) * ITEMS_PER_ALLOC);
        if (NULL == item) {
            STATS_LOCK();
            stats.malloc_fails++;
            STATS_UNLOCK();
            return NULL;
        }

        /*
         * Link together all the new items except the first one
         * (which we'll return to the caller) for placement on
         * the freelist.
         */
        for (i = 2; i < ITEMS_PER_ALLOC; i++)
            item[i - 1].next = &item[i];

        pthread_mutex_lock(&cqi_freelist_lock);
        item[ITEMS_PER_ALLOC - 1].next = cqi_freelist;
        cqi_freelist = &item[1];
        pthread_mutex_unlock(&cqi_freelist_lock);
    }

    return item;
}


/*
 * Frees a connection queue item (adds it to the freelist.)
 */
static void cqi_free(CQ_ITEM *item) {
    pthread_mutex_lock(&cqi_freelist_lock);
    item->next = cqi_freelist;
    cqi_freelist = item;
    pthread_mutex_unlock(&cqi_freelist_lock);
}


/*
 * Creates a worker thread.
 */
static void create_worker(void *(*func)(void *), void *arg) {
    pthread_t       thread;
    pthread_attr_t  attr;
    int             ret;

    pthread_attr_init(&attr);

    if ((ret = pthread_create(&thread, &attr, func, arg)) != 0) {
        fprintf(stderr, "Can't create thread: %s\n",
                strerror(ret));
        exit(1);
    }
}

/*
 * Sets whether or not we accept new connections.
 */
void accept_new_conns(const bool do_accept) {
    pthread_mutex_lock(&conn_lock);
    do_accept_new_conns(do_accept);
    pthread_mutex_unlock(&conn_lock);
}
/****************************** LIBEVENT THREADS *****************************/

/*
 * Set up a thread's information.
 */
static void setup_thread(LIBEVENT_THREAD *me) {
    me->base = event_init();
    if (! me->base) {
        fprintf(stderr, "Can't allocate event base\n");
        exit(1);
    }

    /* Listen for notifications from other threads */
    event_set(&me->notify_event, me->notify_receive_fd,
              EV_READ | EV_PERSIST, thread_libevent_process, me);
    event_base_set(me->base, &me->notify_event);

    if (event_add(&me->notify_event, 0) == -1) {
        fprintf(stderr, "Can't monitor libevent notify pipe\n");
        exit(1);
    }

    me->new_conn_queue = malloc(sizeof(struct conn_queue));
    if (me->new_conn_queue == NULL) {
        perror("Failed to allocate memory for connection queue");
        exit(EXIT_FAILURE);
    }
    cq_init(me->new_conn_queue);

    if (pthread_mutex_init(&me->stats.mutex, NULL) != 0) {
        perror("Failed to initialize mutex");
        exit(EXIT_FAILURE);
    }

    me->suffix_cache = cache_create("suffix", SUFFIX_SIZE, sizeof(char*),
                                    NULL, NULL);
    if (me->suffix_cache == NULL) {
        fprintf(stderr, "Failed to create suffix cache\n");
        exit(EXIT_FAILURE);
    }
}

/*
 * Worker thread: main event loop
 */
static void *worker_libevent(void *arg) {
    LIBEVENT_THREAD *me = arg;

    /* Any per-thread setup can happen here; thread_init() will block until
     * all threads have finished initializing.
     */

    /* set an indexable thread-specific memory item for the lock type.
     * this could be unnecessary if we pass the conn *c struct through
     * all item_lock calls...
     */
    me->item_lock_type = ITEM_LOCK_GRANULAR;
    pthread_setspecific(item_lock_type_key, &me->item_lock_type);

    register_thread_initialized();

    event_base_loop(me->base, 0);
    return NULL;
}


/*
 * Processes an incoming "handle a new connection" item. This is called when
 * input arrives on the libevent wakeup pipe.
 */
static void thread_libevent_process(int fd, short which, void *arg) {
    LIBEVENT_THREAD *me = arg;
    CQ_ITEM *item;
    char buf[1];

    if (read(fd, buf, 1) != 1)
        if (settings.verbose > 0)
            fprintf(stderr, "Can't read from libevent pipe\n");

    switch (buf[0]) {
    case 'c':
    item = cq_pop(me->new_conn_queue);

    if (NULL != item) {
        conn *c = conn_new(item->sfd, item->init_state, item->event_flags,
                           item->read_buffer_size, item->transport, me->base);
        if (c == NULL) {
            if (IS_UDP(item->transport)) {
                fprintf(stderr, "Can't listen for events on UDP socket\n");
                exit(1);
            } else {
                if (settings.verbose > 0) {
                    fprintf(stderr, "Can't listen for events on fd %d\n",
                        item->sfd);
                }
                close(item->sfd);
            }
        } else {
            c->thread = me;
        }
        cqi_free(item);
    }
        break;
    /* we were told to flip the lock type and report in */
    case 'l':
    me->item_lock_type = ITEM_LOCK_GRANULAR;
    register_thread_initialized();
        break;
    case 'g':
    me->item_lock_type = ITEM_LOCK_GLOBAL;
    register_thread_initialized();
        break;
    }
}

/* Which thread we assigned a connection to most recently. */
static int last_thread = -1;

/*
 * Dispatches a new connection to another thread. This is only ever called
 * from the main thread, either during initialization (for UDP) or because
 * of an incoming connection.
 */
void dispatch_conn_new(int sfd, enum conn_states init_state, int event_flags,
                       int read_buffer_size, enum network_transport transport) {
    CQ_ITEM *item = cqi_new();
    char buf[1];
    if (item == NULL) {
        close(sfd);
        /* given that malloc failed this may also fail, but let's try */
        fprintf(stderr, "Failed to allocate memory for connection object\n");
        return ;
    }

    int tid = (last_thread + 1) % settings.num_threads;

    LIBEVENT_THREAD *thread = threads + tid;

    last_thread = tid;

    item->sfd = sfd;
    item->init_state = init_state;
    item->event_flags = event_flags;
    item->read_buffer_size = read_buffer_size;
    item->transport = transport;

    cq_push(thread->new_conn_queue, item);

    MEMCACHED_CONN_DISPATCH(sfd, thread->thread_id);
    buf[0] = 'c';
    if (write(thread->notify_send_fd, buf, 1) != 1) {
        perror("Writing to thread notify pipe");
    }
}

/*
 * Returns true if this is the thread that listens for new TCP connections.
 */
int is_listen_thread() {
    return pthread_self() == dispatcher_thread.thread_id;
}

/********************************* ITEM ACCESS *******************************/

/*
 * Allocates a new item.
 */
item *item_alloc(char *key, size_t nkey, int flags, rel_time_t exptime, int nbytes) {
    item *it;
    /* do_item_alloc handles its own locks */
    it = do_item_alloc(key, nkey, flags, exptime, nbytes, 0);
    return it;
}

/*
 * Returns an item if it hasn't been marked as expired,
 * lazy-expiring as needed.
 */
item *item_get(const char *key, const size_t nkey) {
    item *it;
    uint32_t hv;
    hv = hash(key, nkey);
    item_lock(hv);
    it = do_item_get(key, nkey, hv);
    item_unlock(hv);
    return it;
}

item *item_touch(const char *key, size_t nkey, uint32_t exptime) {
    item *it;
    uint32_t hv;
    hv = hash(key, nkey);
    item_lock(hv);
    it = do_item_touch(key, nkey, exptime, hv);
    item_unlock(hv);
    return it;
}

/*
 * Links an item into the LRU and hashtable.
 */
int item_link(item *item) {
    int ret;
    uint32_t hv;

    hv = hash(ITEM_key(item), item->nkey);
    item_lock(hv);
    ret = do_item_link(item, hv);
    item_unlock(hv);
    return ret;
}

/*
 * Decrements the reference count on an item and adds it to the freelist if
 * needed.
 */
void item_remove(item *item) {
    uint32_t hv;
    hv = hash(ITEM_key(item), item->nkey);

    item_lock(hv);
    do_item_remove(item);
    item_unlock(hv);
}

/*
 * Replaces one item with another in the hashtable.
 * Unprotected by a mutex lock since the core server does not require
 * it to be thread-safe.
 */
int item_replace(item *old_it, item *new_it, const uint32_t hv) {
    return do_item_replace(old_it, new_it, hv);
}

/*
 * Unlinks an item from the LRU and hashtable.
 */
void item_unlink(item *item) {
    uint32_t hv;
    hv = hash(ITEM_key(item), item->nkey);
    item_lock(hv);
    do_item_unlink(item, hv);
    item_unlock(hv);
}


/*
 * Moves an item to the back of the LRU queue.
 */
void item_update(item *item) {
    uint32_t hv;
    hv = hash(ITEM_key(item), item->nkey);

    item_lock(hv);
    do_item_update(item);
    item_unlock(hv);
}

/*
 * Does arithmetic on a numeric item value.
 */
enum delta_result_type add_delta(conn *c, const char *key,
                                 const size_t nkey, int incr,
                                 const int64_t delta, char *buf,
                                 uint64_t *cas) {
    enum delta_result_type ret;
    uint32_t hv;

    hv = hash(key, nkey);
    item_lock(hv);
    ret = do_add_delta(c, key, nkey, incr, delta, buf, cas, hv);
    item_unlock(hv);
    return ret;
}

/*
 * Stores an item in the cache (high level, obeys set/add/replace semantics)
 */
enum store_item_type store_item(item *item, int comm, conn* c) {
    enum store_item_type ret;
    uint32_t hv;

    hv = hash(ITEM_key(item), item->nkey);
    item_lock(hv);
    ret = do_store_item(item, comm, c, hv);
    item_unlock(hv);
    return ret;
}

/*
 * Flushes expired items after a flush_all call
 */
void item_flush_expired() {
    mutex_lock(&cache_lock);
    do_item_flush_expired();
    mutex_unlock(&cache_lock);
}

/*
 * Dumps part of the cache
 */
char *item_cachedump(unsigned int slabs_clsid, unsigned int limit, unsigned int *bytes) {
    char *ret;

    mutex_lock(&cache_lock);
    ret = do_item_cachedump(slabs_clsid, limit, bytes);
    mutex_unlock(&cache_lock);
    return ret;
}

/*
 * Dumps statistics about slab classes
 */
void  item_stats(ADD_STAT add_stats, void *c) {
    mutex_lock(&cache_lock);
    do_item_stats(add_stats, c);
    mutex_unlock(&cache_lock);
}

void  item_stats_totals(ADD_STAT add_stats, void *c) {
    mutex_lock(&cache_lock);
    do_item_stats_totals(add_stats, c);
    mutex_unlock(&cache_lock);
}

/*
 * Dumps a list of objects of each size in 32-byte increments
 */
void  item_stats_sizes(ADD_STAT add_stats, void *c) {
    mutex_lock(&cache_lock);
    do_item_stats_sizes(add_stats, c);
    mutex_unlock(&cache_lock);
}

/******************************* GLOBAL STATS ******************************/

void STATS_LOCK() {
    pthread_mutex_lock(&stats_lock);
}

void STATS_UNLOCK() {
    pthread_mutex_unlock(&stats_lock);
}

void threadlocal_stats_reset(void) {
    int ii, sid;
    for (ii = 0; ii < settings.num_threads; ++ii) {
        pthread_mutex_lock(&threads[ii].stats.mutex);

        threads[ii].stats.get_cmds = 0;
        threads[ii].stats.get_misses = 0;
        threads[ii].stats.touch_cmds = 0;
        threads[ii].stats.touch_misses = 0;
        threads[ii].stats.delete_misses = 0;
        threads[ii].stats.incr_misses = 0;
        threads[ii].stats.decr_misses = 0;
        threads[ii].stats.cas_misses = 0;
        threads[ii].stats.bytes_read = 0;
        threads[ii].stats.bytes_written = 0;
        threads[ii].stats.flush_cmds = 0;
        threads[ii].stats.conn_yields = 0;
        threads[ii].stats.auth_cmds = 0;
        threads[ii].stats.auth_errors = 0;

        for(sid = 0; sid < MAX_NUMBER_OF_SLAB_CLASSES; sid++) {
            threads[ii].stats.slab_stats[sid].set_cmds = 0;
            threads[ii].stats.slab_stats[sid].get_hits = 0;
            threads[ii].stats.slab_stats[sid].touch_hits = 0;
            threads[ii].stats.slab_stats[sid].delete_hits = 0;
            threads[ii].stats.slab_stats[sid].incr_hits = 0;
            threads[ii].stats.slab_stats[sid].decr_hits = 0;
            threads[ii].stats.slab_stats[sid].cas_hits = 0;
            threads[ii].stats.slab_stats[sid].cas_badval = 0;
        }

        pthread_mutex_unlock(&threads[ii].stats.mutex);
    }
}

void threadlocal_stats_aggregate(struct thread_stats *stats) {
    int ii, sid;

    /* The struct has a mutex, but we can safely set the whole thing
     * to zero since it is unused when aggregating. */
    memset(stats, 0, sizeof(*stats));

    for (ii = 0; ii < settings.num_threads; ++ii) {
        pthread_mutex_lock(&threads[ii].stats.mutex);

        stats->get_cmds += threads[ii].stats.get_cmds;
        stats->get_misses += threads[ii].stats.get_misses;
        stats->touch_cmds += threads[ii].stats.touch_cmds;
        stats->touch_misses += threads[ii].stats.touch_misses;
        stats->delete_misses += threads[ii].stats.delete_misses;
        stats->decr_misses += threads[ii].stats.decr_misses;
        stats->incr_misses += threads[ii].stats.incr_misses;
        stats->cas_misses += threads[ii].stats.cas_misses;
        stats->bytes_read += threads[ii].stats.bytes_read;
        stats->bytes_written += threads[ii].stats.bytes_written;
        stats->flush_cmds += threads[ii].stats.flush_cmds;
        stats->conn_yields += threads[ii].stats.conn_yields;
        stats->auth_cmds += threads[ii].stats.auth_cmds;
        stats->auth_errors += threads[ii].stats.auth_errors;

        for (sid = 0; sid < MAX_NUMBER_OF_SLAB_CLASSES; sid++) {
            stats->slab_stats[sid].set_cmds +=
                threads[ii].stats.slab_stats[sid].set_cmds;
            stats->slab_stats[sid].get_hits +=
                threads[ii].stats.slab_stats[sid].get_hits;
            stats->slab_stats[sid].touch_hits +=
                threads[ii].stats.slab_stats[sid].touch_hits;
            stats->slab_stats[sid].delete_hits +=
                threads[ii].stats.slab_stats[sid].delete_hits;
            stats->slab_stats[sid].decr_hits +=
                threads[ii].stats.slab_stats[sid].decr_hits;
            stats->slab_stats[sid].incr_hits +=
                threads[ii].stats.slab_stats[sid].incr_hits;
            stats->slab_stats[sid].cas_hits +=
                threads[ii].stats.slab_stats[sid].cas_hits;
            stats->slab_stats[sid].cas_badval +=
                threads[ii].stats.slab_stats[sid].cas_badval;
        }

        pthread_mutex_unlock(&threads[ii].stats.mutex);
    }
}

void slab_stats_aggregate(struct thread_stats *stats, struct slab_stats *out) {
    int sid;

    out->set_cmds = 0;
    out->get_hits = 0;
    out->touch_hits = 0;
    out->delete_hits = 0;
    out->incr_hits = 0;
    out->decr_hits = 0;
    out->cas_hits = 0;
    out->cas_badval = 0;

    for (sid = 0; sid < MAX_NUMBER_OF_SLAB_CLASSES; sid++) {
        out->set_cmds += stats->slab_stats[sid].set_cmds;
        out->get_hits += stats->slab_stats[sid].get_hits;
        out->touch_hits += stats->slab_stats[sid].touch_hits;
        out->delete_hits += stats->slab_stats[sid].delete_hits;
        out->decr_hits += stats->slab_stats[sid].decr_hits;
        out->incr_hits += stats->slab_stats[sid].incr_hits;
        out->cas_hits += stats->slab_stats[sid].cas_hits;
        out->cas_badval += stats->slab_stats[sid].cas_badval;
    }
}

/*
 * Initializes the thread subsystem, creating various worker threads.
 *
 * nthreads  Number of worker event handler threads to spawn
 * main_base Event base for main thread
 */
void thread_init(int nthreads, struct event_base *main_base) {
    int         i;
    int         power;

    pthread_mutex_init(&cache_lock, NULL);
    pthread_mutex_init(&stats_lock, NULL);

    pthread_mutex_init(&init_lock, NULL);
    pthread_cond_init(&init_cond, NULL);

    pthread_mutex_init(&cqi_freelist_lock, NULL);
    cqi_freelist = NULL;

    /* Want a wide lock table, but don't waste memory */
    if (nthreads < 3) {
        power = 10;
    } else if (nthreads < 4) {
        power = 11;
    } else if (nthreads < 5) {
        power = 12;
    } else {
        /* 8192 buckets, and central locks don't scale much past 5 threads */
        power = 13;
    }

    item_lock_count = hashsize(power);
    item_lock_hashpower = power;

    item_locks = calloc(item_lock_count, sizeof(pthread_mutex_t));
    if (! item_locks) {
        perror("Can't allocate item locks");
        exit(1);
    }
    for (i = 0; i < item_lock_count; i++) {
        pthread_mutex_init(&item_locks[i], NULL);
    }
    pthread_key_create(&item_lock_type_key, NULL);
    pthread_mutex_init(&item_global_lock, NULL);

    threads = calloc(nthreads, sizeof(LIBEVENT_THREAD));
    if (! threads) {
        perror("Can't allocate thread descriptors");
        exit(1);
    }

    dispatcher_thread.base = main_base;
    dispatcher_thread.thread_id = pthread_self();

    for (i = 0; i < nthreads; i++) {
        int fds[2];
        if (pipe(fds)) {
            perror("Can't create notify pipe");
            exit(1);
        }

        threads[i].notify_receive_fd = fds[0];
        threads[i].notify_send_fd = fds[1];

        setup_thread(&threads[i]);
        /* Reserve three fds for the libevent base, and two for the pipe */
        stats.reserved_fds += 5;
    }

    /* Create threads after we've done all the libevent setup. */
    for (i = 0; i < nthreads; i++) {
        create_worker(worker_libevent, &threads[i]);
    }

    /* Wait for all the threads to set themselves up before returning. */
    pthread_mutex_lock(&init_lock);
    wait_for_thread_registration(nthreads);
    pthread_mutex_unlock(&init_lock);
}


// log thread

#define LOG_ITEMS_PER_ALLOC 32

// log thread message Q
static LQ_ITEM *lqi_freelist;
static pthread_mutex_t lqi_freelist_lock;

static void lq_init(LQ *lq) {
    pthread_mutex_init(&lq->lock, NULL);
    lq->head = NULL;
    lq->tail = NULL;
}

static LQ_ITEM *lq_pop(LQ *lq) {
    LQ_ITEM *item;

    pthread_mutex_lock(&lq->lock);
    item = lq->head;
    if (NULL != item) {
        lq->head = item->next;
        if (NULL == lq->head)
            lq->tail = NULL;
    }
    pthread_mutex_unlock(&lq->lock);

    return item;
}

void lq_push(LQ *lq, LQ_ITEM *item) {
    item->next = NULL;

    pthread_mutex_lock(&lq->lock);
    if (NULL == lq->tail)
        lq->head = item;
    else
        lq->tail->next = item;
    lq->tail = item;
    pthread_mutex_unlock(&lq->lock);
}

LQ_ITEM *lqi_new(void) {
    LQ_ITEM *item = NULL;
    pthread_mutex_lock(&lqi_freelist_lock);
    if (lqi_freelist) {
        item = lqi_freelist;
        lqi_freelist = item->next;
    }
    pthread_mutex_unlock(&lqi_freelist_lock);

    if (NULL == item) {
        int i;

        item = malloc(sizeof(LQ_ITEM) * LOG_ITEMS_PER_ALLOC);
        if (NULL == item) {
            STATS_LOCK();
            stats.malloc_fails++;
            STATS_UNLOCK();
            return NULL;
        }

        for (i = 2; i < LOG_ITEMS_PER_ALLOC; i++)
            item[i - 1].next = &item[i];

        pthread_mutex_lock(&lqi_freelist_lock);
        item[LOG_ITEMS_PER_ALLOC - 1].next = lqi_freelist;
        lqi_freelist = &item[1];
        pthread_mutex_unlock(&lqi_freelist_lock);
    }

    return item;
}

static void lqi_free(LQ_ITEM *item) {
    pthread_mutex_lock(&lqi_freelist_lock);
	if (item->item != NULL) {
		free(item);
	}
    item->next = lqi_freelist;
    lqi_freelist = item;
    pthread_mutex_unlock(&lqi_freelist_lock);
}

void log_thread_init(struct event_base *main_base) {
	int     i;
	char	*path;
	int		back_up_init_count = init_count; //复用原有全局变量
	int 	nthreads = stats.slabs_num;	
	init_count = 0;

    log_threads = calloc(nthreads, sizeof(LIBEVENT_LOG_THREAD));
    if (! log_threads) {
        perror("Can't allocate thread descriptors");
        exit(1);
    }

    for (i = 0; i < nthreads; i++) {
        int fds[2];
        if (pipe(fds)) {
            perror("Can't create notify pipe");
            exit(1);
        }

        log_threads[i].notify_receive_fd = fds[0];
        log_threads[i].notify_send_fd = fds[1];
		path = calloc(512, sizeof(char));
		sprintf(path, "%s/log_%d", settings.persisted_data_path, i);
		log_threads[i].log_filepath = path;
		log_threads[i].log_fd = fopen(path, "ab+");
		log_threads[i].slab_no = i;
		pthread_mutex_init(&log_threads[i].log_file_lock, NULL);

        setup_log_thread(&log_threads[i]);
        /* Reserve three fds for the libevent base, and two for the pipe */
        stats.reserved_fds += 5;
    }

    /* Create threads after we've done all the libevent setup. */
    for (i = 0; i < nthreads; i++) {
        create_worker(worker_libevent, &log_threads[i]);
    }

    /* Wait for all the threads to set themselves up before returning. */
    pthread_mutex_lock(&init_lock);
    wait_for_thread_registration(nthreads);
    pthread_mutex_unlock(&init_lock);
	
	init_count = back_up_init_count;
}


//:~ log thread message Q

void setup_log_thread(LIBEVENT_LOG_THREAD *me) {
    me->base = event_init();
    if (! me->base) {
        fprintf(stderr, "Can't allocate event base\n");
        exit(1);
    }

    /* Listen for notifications from other threads */
    event_set(&me->notify_event, me->notify_receive_fd,
              EV_READ | EV_PERSIST, log_event_process, me);
    event_base_set(me->base, &me->notify_event);

    if (event_add(&me->notify_event, 0) == -1) {
        fprintf(stderr, "Can't monitor libevent notify pipe\n");
        exit(1);
    }

    me->new_log_queue = malloc(sizeof(struct log_queue));
    if (me->new_log_queue == NULL) {
        perror("Failed to allocate memory for connection queue");
        exit(EXIT_FAILURE);
    }
    lq_init(me->new_log_queue);

    if (pthread_mutex_init(&me->stats.mutex, NULL) != 0) {
        perror("Failed to initialize mutex");
        exit(EXIT_FAILURE);
    }

    me->suffix_cache = cache_create("suffix", SUFFIX_SIZE, sizeof(char*),
                                    NULL, NULL);
    if (me->suffix_cache == NULL) {
        fprintf(stderr, "Failed to create suffix cache\n");
        exit(EXIT_FAILURE);
    }
}

static int begin_recover = 0;
void log_event_process(int fd, short which, void*arg) {
	LIBEVENT_LOG_THREAD *me = arg;
	LQ_ITEM *lq_item;
	item *item_p;
	char buf[1];
	char snapshot_before_path[512];
	//unsigned int log_size = 0;

	if (read(fd, buf, 1) != 1) {
		fprintf(stderr, "Can't read from libevent pipe\n");
	}

	switch(buf[0]) {
		case 'l': // log
			lq_item = lq_pop(me->new_log_queue);

			if (NULL == me->log_fd) {
				fprintf(stderr, "thread log fd is null %d\n", me->slab_no);
				return ;
			}

			if (NULL == lq_item) {
				return ;
			}
//			fprintf(stderr, "thread %d\n", me->slab_no);

			item_p = lq_item->item;
//			pthread_mutex_lock(&me->log_file_lock);
			fwrite(item_p, ITEM_ntotal(item_p), 1, me->log_fd);
//			pthread_mutex_unlock(&me->log_file_lock);
			fflush(me->log_fd);
			lqi_free(lq_item);
			break;
		case 's': //snapshot
			sprintf(snapshot_before_path, "%s.snapshot_before", me->log_filepath);	
//			fprintf(stderr, "snapshot thread %d:%s\n", me->slab_no, snapshot_before_path);
//			pthread_mutex_lock(&me->log_file_lock);
			fclose(me->log_fd);
			rename(me->log_filepath, snapshot_before_path);
			me->log_fd = fopen(me->log_filepath, "ab+");
//			pthread_mutex_unlock(&me->log_file_lock);
			break;
		case 'd': //snapshot done, delete snapshot_before
//			fprintf(stderr, "delete thread %d\n", me->slab_no);
			sprintf(snapshot_before_path, "%s.snapshot_before", me->log_filepath);
//			pthread_mutex_lock(&me->log_file_lock);
			unlink(snapshot_before_path);
//			pthread_mutex_unlock(&me->log_file_lock);
			break;
	}
			
}

void snapshot_thread_init(void) {
	pthread_t       thread;
    pthread_attr_t  attr;
    int             ret;

    pthread_attr_init(&attr);

    if ((ret = pthread_create(&thread, &attr, snapshot_libevent, NULL)) != 0) {
        fprintf(stderr, "Can't create snapshot thread: %s\n",
                strerror(ret));
        exit(1);
    }
}

static struct timeval snapshot_tv; 
static struct event snapshot_ev_timer;  

void *snapshot_libevent(void *arg) {
	//	struct event_base *base = event_base_new();

	evutil_timerclear(&snapshot_tv); 
	snapshot_tv.tv_sec = settings.snapshot_period;  

	evtimer_set(&snapshot_ev_timer, snapshot_process, arg);
	//evtimer_assign(&snapshot_ev_timer, base, shnapshot_process, &nslabs);

	event_add(&snapshot_ev_timer, &snapshot_tv);
	event_dispatch();
	return NULL;
}

void snapshot_process(int fd, short n, void *arg) {
	char buf[1];
	int i = 0;


	if (begin_recover == 0 && stats.changes_after_last_snapshot >= settings.change_num_need_snapshop) {
		STATS_LOCK();
		stats.changes_after_last_snapshot = 0;
		STATS_UNLOCK();
		
		// 开始snapshot，告诉所有log进程，备份原有log
		// 并将新的log进入新的文件中
		buf[0] = 's';
		
		for (i=0; i<stats.slabs_num; i++) {
			write(log_threads[i].notify_send_fd, buf, 1);
		}

		snapshot_all_slab();

		buf[0] = 'd';
		for (i=0; i<stats.slabs_num; i++) {
			write(log_threads[i].notify_send_fd, buf, 1);
		}

		
	}

	event_add(&snapshot_ev_timer, &snapshot_tv);
}


void notify_log(item *vitem) {
	int   item_ntotal = ITEM_ntotal(vitem);
	item *copy_item = NULL;

	if (begin_recover) {
		return;
	}
	copy_item = calloc(item_ntotal, sizeof(int));
	memcpy(copy_item, vitem, item_ntotal);

	/*
	 * 放置new_item到线程的消息队列
	 */
	unsigned int id = slabs_clsid(item_ntotal);
	LQ_ITEM *log_item = lqi_new();
	log_item->item = copy_item;
	lq_push(log_threads[id].new_log_queue, log_item);
	char buf[1];
	buf[0] = 'l';
	if (write(log_threads[id].notify_send_fd, buf, 1) != 1) {
        perror("Writing to thread notify pipe");
    }
	stats.changes_after_last_snapshot++;
}


static int recover_finished = 0;
void recover_thread_init(void) {
	pthread_t       thread;
    pthread_attr_t  attr;
    int             ret;

    pthread_attr_init(&attr);

    if ((ret = pthread_create(&thread, &attr, recover, NULL)) != 0) {
        fprintf(stderr, "Can't create snapshot thread: %s\n",
                strerror(ret));
        exit(1);
    }

	pthread_mutex_lock(&init_lock);
	while (recover_finished == 0) {
		pthread_cond_wait(&init_cond, &init_lock);
	}
	pthread_mutex_unlock(&init_lock);
}

int static recover_lock_type = ITEM_LOCK_GLOBAL;
void *recover(void *arg) {
	char path_buffer[512];
	char path_buffer2[512];
	char *snapshot_path = path_buffer;

	int slab_num = 0;
	char *log_path = path_buffer;
	char *log_before_snapshot_path = path_buffer2;
	
	pthread_setspecific(item_lock_type_key, &recover_lock_type);

	begin_recover = 1;
	sprintf(snapshot_path, "%s/snapshot", settings.persisted_data_path);
	redo_file(snapshot_path);

	// 恢复bin log
	sprintf(log_path, "%s/log_%d", settings.persisted_data_path, slab_num);
	while (access(log_path, R_OK) == 0) {
		sprintf(log_before_snapshot_path, "%s/log_%d.snapshot_before", settings.persisted_data_path, slab_num);
		redo_file(log_before_snapshot_path);
		redo_file(log_path);
		slab_num++;
		sprintf(log_path, "%s/log_%d", settings.persisted_data_path, slab_num);
	}
	begin_recover = 0;

	pthread_mutex_lock(&init_lock);
    recover_finished = 1;
    pthread_cond_signal(&init_cond);
    pthread_mutex_unlock(&init_lock);
	return NULL;
}

int redo_file(char *fpath) {
	FILE *fp = NULL;
	char read_buffer[1024*1024*8]; // 8M
	int buffer_size = 1024*1024*8;
	int file_readed_length = 0;
	char *current_ptr = NULL;
	item *cr_item = NULL;
	item *cw_item = NULL;
	uint32_t hv = 0;
	int processed_size = 0;
	int need_to_read_size = buffer_size;
	char *read_begin_ptr = read_buffer;

	if (access(fpath, R_OK)!=0) {
		return 1;
	}

	fp = fopen(fpath, "r");
	if (fp == NULL) {
		return -1;
	}

	while(!feof(fp)) {
		file_readed_length = fread(read_begin_ptr, sizeof(char), need_to_read_size, fp);
		current_ptr = read_buffer;
		cr_item = (item *)current_ptr;
		processed_size = 0;

		while (processed_size + ITEM_ntotal(cr_item) <= buffer_size - need_to_read_size + file_readed_length) {
			if ((cr_item->it_flags & ITEM_LINKED) != 0) {
				// add
				cw_item = item_alloc(ITEM_key(cr_item), cr_item->nkey, 0, 0, cr_item->nbytes);
				memcpy(cw_item,  cr_item, ITEM_ntotal(cr_item));
				hv = hash(ITEM_key(cw_item), cw_item->nkey);
				item_lock(hv);
				do_item_link(cw_item, hv);
				item_unlock(hv);
			} else {
				// delete
				cw_item = item_get(ITEM_key(cr_item), cr_item->nkey);
				if (ITEM_ntotal(cw_item) == ITEM_ntotal(cr_item) && cw_item->nbytes == cr_item->nbytes) {
					if (memcmp(ITEM_data(cw_item), ITEM_data(cr_item), cr_item->nbytes)) {
						hv = hash(ITEM_key(cw_item), cw_item->nkey);
						item_lock(hv);
						do_item_unlink(cw_item, hv);
						item_unlock(hv);
					}
				}
			}
			
			current_ptr += ITEM_ntotal(cr_item);
			processed_size += ITEM_ntotal(cr_item);
		}

		// 读取更多文件
		need_to_read_size = processed_size;
		read_begin_ptr = read_buffer + (buffer_size - processed_size);
		memcpy(read_buffer, current_ptr, buffer_size - processed_size);
	}
	return 0;
}

