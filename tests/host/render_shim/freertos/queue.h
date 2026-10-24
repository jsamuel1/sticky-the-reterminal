#pragma once
typedef void *QueueHandle_t;
static inline QueueHandle_t xQueueCreate(int, int) { return nullptr; }
static inline int xQueueReceive(QueueHandle_t, void *, unsigned long) { return 0; }
static inline int xQueueSend(QueueHandle_t, const void *, unsigned long) { return 0; }
