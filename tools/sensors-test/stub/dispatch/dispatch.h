#pragma once
typedef struct dispatch_queue_s *dispatch_queue_t;
dispatch_queue_t dispatch_queue_create(const char *label, void *attr);
