#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void runtime_init();
void runtime_exit();

/* low-level apis */
void *runtime_fetch(const void *object, int object_size);
bool runtime_read(void *dst, const void *object, int object_size);

#ifdef __cplusplus
}
#endif