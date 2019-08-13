#include <stdio.h>
#include "mem_pool.h"

struct user {
    int age;
    char name[200];
    char *ptr;
};

void test()
{
    struct user * ptr = (struct user *)memory_alloc(sizeof(struct user)*1000, __FILE__);
    if (ptr) {
        printf("malloc\n");
    }
    ptr->age = 123;
    dump_memory();
    memory_free(ptr, sizeof(struct user)*1000);
    dump_memory();
}

int main()
{
    test();
}
