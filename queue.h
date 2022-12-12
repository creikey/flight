#pragma once

#include <stdbool.h>

#ifndef QUEUE_ASSERT
void __flight_assert(bool cond, const char *file, int line, const char *cond_string);
#define QUEUE_ASSERT(condition) __flight_assert(condition, __FILE__, __LINE__, #condition)
#endif

typedef struct QueueElementHeader
{
  bool exists;
  struct QueueElementHeader *next;
  char data[];
} QueueElementHeader;

typedef struct Queue
{
  char *data;
  size_t data_length; // must be a multiple of sizeof(QueueElementHeader) + element_size
  size_t element_size;
  QueueElementHeader *next;
} Queue;

#define QUEUE_SIZE_FOR_ELEMENTS(element_size, max_elements) ((sizeof(QueueElementHeader) + element_size) * max_elements)

// oldest to newest
#define QUEUE_ITER(q_ptr, type, cur) for (QueueElementHeader *cur_header = (q_ptr)->next; cur_header != NULL; cur_header = cur_header->next) for(type *cur = (type*)cur_header->data; cur != NULL; cur = NULL)
void queue_init(Queue *q, size_t element_size, char *data, size_t data_length);
void queue_clear(Queue *q);
void *queue_push_element(Queue *q);
size_t queue_num_elements(Queue *q);
void *queue_pop_element(Queue *q);
void *queue_most_recent_element(Queue *q);

#ifdef QUEUE_IMPL

void queue_init(Queue *q, size_t element_size, char *data, size_t data_length)
{
  q->data = data;
  q->data_length = data_length;
  q->element_size = element_size;
  QUEUE_ASSERT(data_length % (sizeof(QueueElementHeader) + element_size) == 0);
}

void queue_clear(Queue *q)
{
  QUEUE_ASSERT(q->data != NULL);
  for (size_t i = 0; i < q->data_length; i++)
  {
    q->data[i] = 0;
  }
  q->next = NULL;
}

#define QUEUE_ELEM_ITER(cur) for (QueueElementHeader *cur = (QueueElementHeader *)q->data; (char *)cur < q->data + q->data_length; cur = (QueueElementHeader*)((char*)cur + (sizeof(QueueElementHeader) + q->element_size)))

// you push an element, get the return value, cast it to your type, and fill it with data. It's that easy!
// if it's null the queue is out of space
void *queue_push_element(Queue *q)
{
  QUEUE_ASSERT(q->data != NULL);
  QueueElementHeader *to_return = NULL;
  QUEUE_ELEM_ITER(cur)
  {
    if (!cur->exists)
    {
      to_return = cur;
      break;
    }
  }

  // no free packet found in the buffer
  if (to_return == NULL)
  {
    return NULL;
  }
  else
  {
    to_return->exists = true;
    to_return->next = NULL; // very important.
    for (size_t i = 0; i < q->element_size; i++)
      to_return->data[i] = 0;

    // add to the end of the linked list chain
    if (q->next != NULL)
    {
      QueueElementHeader *cur = q->next;
      while (cur->next != NULL)
        cur = cur->next;
      cur->next = to_return;
    }
    else
    {
      q->next = to_return;
    }

    return (void *)to_return->data;
  }
}

size_t queue_num_elements(Queue *q)
{
  QUEUE_ASSERT(q->data != NULL);
  size_t to_return = 0;
  QUEUE_ELEM_ITER(cur)
  if (cur->exists)
    to_return++;
  return to_return;
}

// returns null if the queue is empty
void *queue_pop_element(Queue *q)
{
  QUEUE_ASSERT(q->data != NULL);
  QueueElementHeader *to_return = q->next;
  if (q->next != NULL)
    q->next = q->next->next;
  if (to_return != NULL)
    to_return->exists = false; // jank!
  return to_return == NULL ? NULL : (void *)to_return->data;
}

void *queue_most_recent_element(Queue *q)
{
  if (q->next == NULL)
    return NULL;
  else
  {
    QueueElementHeader *cur = q->next;
    while (cur->next != NULL)
      cur = cur->next;
    return (void *)cur->data;
  }
}
#undef QUEUE_ELEM_ITER
#endif
