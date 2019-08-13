#define _GNU_SOURCE
#include <sys/mman.h>

#include <stdio.h>
#include <stddef.h>
#include <unistd.h>
#include <string.h>
#include "mem_pool.h"

#define SMALLSIZE 8	// should be pow of 2, and at lease sizeof(size_t).
#define SMALLLEVEL 32	// [1,256]
#define CHUNKSIZE (32 * 1024)
#define HUGESIZE (CHUNKSIZE - 16)	// 16 means chunk cookie (sizeof(chunk)) + block cookie (size_t)
#define BIGSEARCHDEPTH 128

struct chunk
{
    struct chunk * next;
    int chunk_used;
};

struct smallblock
{
    struct smallblock * next;
};

struct bigblock
{
    size_t sz;	// used block only has sz field
    struct bigblock * next;	// free block has next field
};

struct hugeblock
{
    size_t sz;
    struct hugeblock * prev;
    struct hugeblock * next;
};

struct mgr
{
    struct smallblock * small_list[SMALLLEVEL+1];
    struct chunk * chunk_head;
    struct chunk * chunk_tail;
    struct bigblock * big_head;
    struct bigblock * big_tail;
    struct hugeblock huge_list;
};

static struct mgr g_mgr;

static inline void * alloc_page(size_t sz)
{
    return mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
}

static inline void * new_chunk(int sz)
{
    // alloc new chunk
    struct chunk * nc = alloc_page(CHUNKSIZE);
    if (nc == NULL)
        return NULL;
    nc->next = NULL;
    nc->chunk_used = sizeof(struct chunk) + sz;
    if (g_mgr.chunk_head == NULL) {
        g_mgr.chunk_head = nc;
    }
    if (g_mgr.chunk_tail) {
        g_mgr.chunk_tail->next = nc;
    }
    g_mgr.chunk_tail= nc;
    return nc+1;
}

static void split_small_memory(char *ptr, int nsize)
{
    struct smallblock * sb = (struct smallblock *)ptr;
    int idx;
    while (nsize > 0) {
        idx = ((int)(nsize) - 1) / SMALLSIZE;
        sb->next = g_mgr.small_list[idx];
        g_mgr.small_list[idx] = sb;
        sb = (struct smallblock *)((char *)sb + (idx + 1) * SMALLSIZE);
        nsize = nsize - (idx + 1) * SMALLSIZE;
    }
}

static void * alloc_small_memory(int n)
{
    // find from free list
    struct smallblock * node = g_mgr.small_list[n];
    if (node) {
        g_mgr.small_list[n] = node->next;
        return node;
    }
    int sz = (n+1) * SMALLSIZE;

    // alloc from chunk list
    struct chunk *chunk = g_mgr.chunk_head; 
    while (chunk != NULL) {
        if (chunk->chunk_used + sz <= CHUNKSIZE) {
            void * ret = (char *)chunk + chunk->chunk_used;
            chunk->chunk_used += sz;
            return ret;
        }
        chunk = chunk->next;
    }

    // lookup larger small list
    for (int i = n + 1; i <= SMALLLEVEL; i++) {
        void * ret = g_mgr.small_list[i];
        if (ret) {
            g_mgr.small_list[i] = g_mgr.small_list[i]->next;
            split_small_memory(ret + sz, (i + 1)*SMALLSIZE - sz);
            return ret;			
        }
    }

    // alloc new chunk
    return new_chunk(sz);
}

static inline void free_small_memory(struct smallblock *ptr, int n)
{
    ptr->next = g_mgr.small_list[n];
    g_mgr.small_list[n] = ptr;
}

static void * alloc_huge_memory(size_t sz)
{
    struct hugeblock * h = alloc_page(sizeof(struct hugeblock) + sz);
    if (h == NULL)
        return NULL;
    h->prev = &g_mgr.huge_list;
    if (g_mgr.huge_list) {
        h->next = g_mgr.huge_list.next;
    }
    h->sz = sz;
    g_mgr.huge_list.next->prev = h;
    g_mgr.huge_list.next = h;
    return h+1;
}

static void free_huge_memory(struct hugeblock * ptr)
{
    --ptr;
    ptr->prev->next = ptr->next;
    ptr->next->prev = ptr->prev;
    munmap(ptr, ptr->sz + sizeof(struct hugeblock));
}

//find the most suitable big memory
static struct bigblock * lookup_big_memory(int sz)
{
    if (g_mgr.big_head == NULL)
        return NULL;
    struct bigblock *b = g_mgr.big_head;

    // only one node in big list
    if (b == g_mgr.big_tail) {
        if (b->sz >= sz) {
            int left = b->sz - sz;
            if (left == 0) {
                g_mgr.big_head = g_mgr.big_tail = NULL;
                return b;
            }
            b->sz = sz;
            int idx = (left - 1) / SMALLSIZE;
            void * ptr = (char *)b + sz;
            if (idx < SMALLLEVEL) {
                free_small_memory(ptr, idx);
                g_mgr.big_head = g_mgr.big_tail = NULL;
            } else {
                struct bigblock *left_block = (struct bigblock *)ptr; 
                left_block->sz = left - sizeof(size_t);
                g_mgr.big_head = g_mgr.big_tail = left_block;
            }
            return b;
        }
        return NULL;		
    }

    struct bigblock *term = b;
    int n = 0;
    do {
        // remove from head
        g_mgr.big_head = b->next;
        if (b->sz >= sz) {
            // find one suitable
            if (b->sz == sz)
                return b;
            int left = b->sz - sz;
            b->sz = sz;
            // split
            int idx = (left - 1) / SMALLSIZE;
            void * ptr = (char *)b + sz;
            if (idx < SMALLLEVEL) {
                free_small_memory(ptr, idx);
            } else {
                struct bigblock * left_part = ptr;
                left_part->sz = left - sizeof(size_t);
                if (left > sz) {
                    // move lastpart (larger part) to head
                    left_part->next = g_mgr.big_head;
                    g_mgr.big_head = left_part;
                } else {
                    // move lastpart (small part) to tail
                    left_part->next = NULL;
                    g_mgr.big_tail->next = left_part;
                    g_mgr.big_tail = left_part;
                }
            }
            return b;
        }
        // add b to tail
        b->next = NULL;
        g_mgr.big_tail->next = b;
        g_mgr.big_tail = b;
        // read head
        b = g_mgr.big_head;
        ++n;
        // don't search too depth
    } while (b != term && n < BIGSEARCHDEPTH);

    return NULL;
}

static void * alloc_big_memory(int sz)
{
    sz = (sz + sizeof(size_t) + 7) & ~7;	// align to 8
    // lookup the last big chunk
    struct chunk *chunk = g_mgr.chunk_tail;
    if (chunk->chunk_used + sz <= CHUNKSIZE) {
        void * b = (char *)chunk + chunk->chunk_used;
        chunk->chunk_used += sz;
        struct bigblock *bb = b;
        bb->sz = sz;
        return (char *)b + sizeof(size_t);
    }
    // lookup big list
    struct bigblock * b = lookup_big_memory(sz);
    if (b == NULL) {
        b = new_chunk((int)sz);
        if (b == NULL)
            return NULL;
        b->sz = sz;
    }
    return (char *)b + sizeof(size_t);
}

static inline void free_big_memory(void *ptr)
{
    struct bigblock *b = (struct bigblock *)((char *)ptr - sizeof(size_t));
    if (g_mgr.big_head == NULL) {
        b->next = NULL;
        g_mgr.big_head = g_mgr.big_tail = b;
    } else {
        b->next = g_mgr.big_head;
        g_mgr.big_head = b;
    }
}

void * memory_alloc(size_t nsize, const char *file_name)
{
    if (nsize <= SMALLLEVEL * SMALLSIZE) {
        if (nsize == 0)
            return NULL;
        int idx = ((int)(nsize) - 1) / SMALLSIZE;
        return alloc_small_memory(idx);
    } else {
        if (nsize > HUGESIZE) {
            return alloc_huge_memory(nsize);
        } else {
            return alloc_big_memory((int)nsize);
        }
    }
}

void memory_free(void *ptr, size_t osize)
{
    if (osize <= SMALLLEVEL * SMALLSIZE) {
        // osize never be 0
        free_small_memory(ptr, (osize - 1) / SMALLSIZE);
    } else if (osize < HUGESIZE) {
        free_big_memory(ptr);
    } else {
        free_huge_memory(ptr);
    }
}

static void * realloc_huge_memory(void *ptr, size_t nsize)
{
    struct hugeblock *h = (struct hugeblock *)ptr - 1;
    struct hugeblock *nh = mremap(h, h->sz + sizeof(struct hugeblock), 
            nsize + sizeof(struct hugeblock), MREMAP_MAYMOVE);
    if (nh == NULL)
        return NULL;
    nh->sz = nsize;
    if (h == nh) {
        return ptr;
    }
    nh->prev->next = nh;
    nh->next->prev = nh;
    return nh + 1;
}

void * memory_realloc(void *ptr, size_t osize, size_t nsize)
{
    if (osize > HUGESIZE && nsize > HUGESIZE) {
        return realloc_huge_memory(ptr, nsize);
    } else if (nsize <= osize) {
        return ptr;
    } else {
        void * tmp = memory_alloc(nsize, "");
        if (tmp == NULL)
            return NULL;
        memcpy(tmp, ptr, osize);
        memory_free(ptr, osize);
        return tmp;
    }
}

void dump_memory()
{
    for (int i = 0; i < SMALLLEVEL; i++) {
        struct smallblock *p = g_mgr.small_list[i];
        int count = 0;
        while (p) {
            count++;
            p = p->next;
        }
        if (count > 0) {
            printf("i=%d,count=%d\n", i, count);
        }
    }
    struct chunk *p = g_mgr.chunk_head;
    while (p) {
        printf("chunk_used=%d\n", p->chunk_used);
        p = p->next;
    }
}
