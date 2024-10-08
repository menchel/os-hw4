#include <unistd.h>

#define MAX_MEMORY_ALLOCATED_SIZE 100000000 // 10^8

void* smalloc(size_t size)
{
    //size conditions
    if(size ==0)
    {
        return NULL;
    }
    if(size > MAX_MEMORY_ALLOCATED_SIZE)
    {
        return NULL;
    }
    void* p_break=sbrk(size);
    if(p_break == (void*)-1) // sbrk failed
    {
        return NULL;
    }
    return p_break;
}