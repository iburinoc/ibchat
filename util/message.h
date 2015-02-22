#ifndef IBCHAT_UTIL_MESSAGE_H
#define IBCHAT_UTIL_MESSAGE_H

#include <stdint.h>

struct message {
	uint64_t length;
	uint64_t seq_num;
	uint8_t *message;
};

struct message_queue_element;
struct message_queue {
	uint64_t size;
	struct message_queue_element *first;
	struct message_queue_element *last;
};

extern const struct message_queue EMPTY_MESSAGE_QUEUE;

struct message *message_queue_top(struct message_queue *queue);
struct message *message_queue_pop(struct message_queue *queue);
int message_queue_push(struct message_queue *queue, struct message *message);

#endif
