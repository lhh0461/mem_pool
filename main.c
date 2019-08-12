#include <stdio.h>
#include "mem_pool.h"

int main()
{
    char * ptr = (char *)memory_alloc(100, __FILE__);
    if (ptr) {
        printf("malloc\n");
    }
}
