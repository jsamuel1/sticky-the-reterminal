#pragma once
static inline void vTaskDelay(unsigned long) {}
static inline int xTaskCreate(void (*)(void *), const char *, int, void *, int, void *) { return 1; }
