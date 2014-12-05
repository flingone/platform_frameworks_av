#include "Queue.h"

void queue_init(Queue *queue)
{
    queue->head = NULL;
    queue->tail = NULL;
    queue->size = 0;
    queue->is_exit = false;

    pthread_mutex_init(&queue->mutex, NULL);
}

static struct node * make_node(AVPacket *pkt)
{
    struct node *n;

    n = (struct node *)malloc(sizeof(struct node));
    n->pkt = pkt;
    n->next = NULL;

    return n;
}

void queue_enqueue(Queue *queue, AVPacket *pkt)
{
    struct node *n;

    n = make_node(pkt);

    pthread_mutex_lock(&queue->mutex);

    if (! queue->head) {
        queue->head = n;
        queue->tail = n;
    } else {
        queue->tail->next = n;
        queue->tail = n;
    }

    queue->size ++;
    pthread_mutex_unlock(&queue->mutex);
}

AVPacket *queue_dequeue(Queue *queue)
{
    AVPacket *pkt;
    struct node *n;

    while((n = queue->head) == NULL &&
            (! queue->is_exit)) {
        usleep(1000);
    }

    pthread_mutex_lock(&queue->mutex);

    pkt = n->pkt;
    queue->head = n->next;
    free(n);
    queue->size --;

    pthread_mutex_unlock(&queue->mutex);

    return pkt;
}

int queue_size(Queue *queue)
{
    int size;

    pthread_mutex_lock(&queue->mutex);
    size = queue->size;
    pthread_mutex_unlock(&queue->mutex);

    return size;
}

void queue_flush(Queue *queue)
{
    AVPacket *pkt;
    struct node *s;

    while(queue->head) {
        s = queue->head->next;
        pkt = queue->head->pkt;
        av_free_packet(pkt);
        free(pkt);
        free(queue->head);
        queue->head = s;
    }

    queue->size = 0;
}

void queue_destroy(Queue *queue)
{
    queue_flush(queue);
    pthread_mutex_destroy(&queue->mutex);
}

void notify_queue_exit(Queue *queue)
{
    queue->is_exit = true;
}

AVPacket *queue_head(Queue *queue)
{
    /* do not need mutex, */
    if (! queue->head) {
        return NULL;
    }
    return queue->head->pkt;
}

AVPacket *queue_dequeue_head(Queue *queue)
{
    AVPacket *pkt;
    struct node *n;

    n = queue->head;
    queue->head = n->next;
    pkt = n->pkt;
    free(n);

    return pkt;
}
