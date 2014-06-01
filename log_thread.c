/*
 * Thread writes op log
 */
#include "memcached.h"
#include <stdlib.h>
#include <stdio.h>

/*
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
    item->next = lqi_freelist;
    lqi_freelist = item;
    pthread_mutex_unlock(&lqi_freelist_lock);
}

//:~ log thread message Q

void setup_log_thread(LIBEVENT_LOG_THREAD *me) {
    me->base = event_init();
    if (! me->base) {
        fprintf(stderr, "Can't allocate event base\n");
        exit(1);
    }

    event_set(&me->notify_event, me->notify_receive_fd,
              EV_READ | EV_PERSIST, log_event_process, me);
    event_base_set(me->base, &me->notify_event);

    if (event_add(&me->notify_event, 0) == -1) {
        fprintf(stderr, "Can't monitor libevent notify pipe\n");
        exit(1);
    }

    me->new_conn_queue = malloc(sizeof(struct log_queue));
    if (me->new_conn_queue == NULL) {
        perror("Failed to allocate memory for connection queue");
        exit(EXIT_FAILURE);
    }
    lq_init(me->new_conn_queue);

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


void log_event_process(int fd, short which, void*arg) {
	LIBEVENT_LOG_THREAD *me = arg;
	LQ_ITEM *lq_item;
	item *item;
	unsigned int log_size = 0;

	lq_item = lq_pop(me->new_log_queue);

	if (NULL == lq_item) {
		return 0;
	}

	item = lq_item->item;

	fwrite(item, ITEM_ntotal(item), 1, me->log_fd);
		
}
*/

