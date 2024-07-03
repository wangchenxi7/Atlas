#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct atlas_unique_ptr {
    uintptr_t handle;
} atlas_unique_ptr;

atlas_unique_ptr atlas_make_unique_ptr(void *object, unsigned object_size);
void *atlas_up_deref_get(atlas_unique_ptr *up);
void atlas_up_deref_put(atlas_unique_ptr *up, void *ptr);
void atlas_up_reset(atlas_unique_ptr *up, void *ptr, unsigned object_size);
void atlas_up_release(atlas_unique_ptr *up);
void atlas_up_mark_evacuation(atlas_unique_ptr *up);
void atlas_up_clear_evacuation(atlas_unique_ptr *up);

void atlas_clean_card();

#ifdef __cplusplus
}
#endif
