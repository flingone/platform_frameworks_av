#ifndef _QUEUE_H_
#define _QUEUE_H_

#include <pthread.h>
#include <unistd.h>
#include <stdbool.h>

extern "C" {
#include "libavutil/avstring.h"
#include "libavformat/avformat.h"


typedef struct node {
    AVPacket *pkt;
    struct node *next;
} node;

typedef struct Queue {
    struct node *head;
    struct node *tail;

    int size;
    bool is_exit;
    pthread_mutex_t mutex;
} Queue;

extern void queue_init(Queue *queue);
extern void queue_enqueue(Queue *queue, AVPacket *pkt);
extern AVPacket *queue_dequeue(Queue *queue);
extern int queue_size(Queue *queue);
extern void queue_flush(Queue *queue);
extern void queue_destroy(Queue *queue);
extern void notify_queue_exit(Queue *queue);
extern AVPacket *queue_head(Queue *queue);
extern AVPacket *queue_dequeue_head(Queue *queue);

}
#endif //_QUEUE_H_

