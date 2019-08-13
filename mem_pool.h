#ifndef __MEM_POOL__
#define __MEM_POOL__

#include <stddef.h>

void * memory_alloc(size_t nsize, const char *file_name);
void * memory_realloc(void *ptr, size_t old_size, size_t new_size);
void memory_free(void *ptr, size_t osize);
void dump_memory();

#define MALLOC(size) memory_alloc(nsize, __FILE__);

#endif //__MEM_POOL__
