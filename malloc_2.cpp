#include <unistd.h>
#include <string.h>
#define MAX_MEMORY_ALLOCATED_SIZE 100000000 // 10^8

/*
---------------------------------------
            HELPER STUFF
---------------------------------------
*/
//struct for metadata
typedef struct MallocMetadata 
{ 
    size_t size;
    bool is_free;
    MallocMetadata* next;
    MallocMetadata* prev;
}MetaData;

//we need a list for all the blocks

class SortedBlocks
{
    MetaData* list;

public:
    SortedBlocks(): list(NULL)
    {

    }
    MetaData* get_start_of_block(void* block)
    {
        return (MetaData*)((char*)block-sizeof(MetaData));
    }
    // basically free
    void release_used_block(void* block_place)
    {
        MetaData* current_block=get_start_of_block(block_place);
        current_block->is_free=true;
    }
    // adding a block in sorted manner
    void add_block_to_list(MetaData* block)
    {
        //we need to add it sorted.
        //However, since we only insert onces with larger value from sbrk
        //It will be sorted by defenition
        MetaData* prev=NULL;
        MetaData* current=list;
        while(current!=NULL)
        {
            prev=current;
            current=current->next;
        }

        if(prev!=NULL) // list is in size 1 or more
        {
            prev->next=block;
            block->prev=prev;
        }
        else // empty list
        {
            list=block;
        }
    }
    // malloc
    void* create_memory_for_block(size_t size)
    {
        MetaData* current=list;
        while(current!=NULL)
        {
            if(current->size>=size && current->is_free) // we found a free block!
            {
                current->is_free=false;
                return current;
            }
            current=current->next;
        }
        // we got here, so we need a new block
        // by proposed solution, will be alocated in the heap (sbrk)
        size_t total_allocation_cost=size+sizeof(MetaData);
        void* p_break=sbrk(total_allocation_cost);
        if(p_break == (void*)-1) // sbrk failed
        {
            return NULL;
        }
        // allocate the new block
        MetaData* new_block_allocated=(MetaData*)p_break;
        new_block_allocated->size=size;
        new_block_allocated->is_free=false;
        new_block_allocated->next=NULL;
        new_block_allocated->prev=NULL;
        add_block_to_list(new_block_allocated);
        return new_block_allocated;
    }
    //get sum of all bytes (no metadata)
    size_t get_sum_of_all_bytes()
    {
        size_t sum=0;
        MetaData* current=list;
        while(current!=NULL)
        {
            sum+=current->size;
            current=current->next;
        }
        return sum;
    }
    //get sum of all blocks
    size_t get_number_of_all_blocks()
    {
        size_t counter=0;
        MetaData* current=list;
        while(current!=NULL)
        {
            counter++;
            current=current->next;
        }
        return counter;
    }
    //get sum of all free bytes (no metadata)
    size_t get_sum_of_all_free_bytes()
    {
        size_t sum=0;
        MetaData* current=list;
        while(current!=NULL)
        {
            if(current->is_free)
            {
                sum+=current->size;
            }
            current=current->next;
        }
        return sum;
    }
    //get sum of all free blocks
    size_t get_number_of_all_free_blocks()
    {
        size_t counter=0;
        MetaData* current=list;
        while(current!=NULL)
        {
            if(current->is_free)
            {
                counter++;
            }
            current=current->next;
        }
        return counter;
    }
};

/*
---------------------------------------
            IMPLEMENTATION
---------------------------------------
*/

//global list
SortedBlocks list=SortedBlocks();

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
    void* p_break = list.create_memory_for_block(size);
    if(p_break==NULL) // something failed
    {
        return NULL;
    }
    return (char*)p_break+sizeof(MetaData);
}
void* scalloc(size_t num, size_t size)
{
    //so we need the same behaviour as smalloc, but set all to 0
    void* place=smalloc(size*num); // will also check for size*num constrains
    if(place==NULL) // problem detected
    {
        return NULL;
    }
    memset(place,0,size*num); // we were told to use it in the pdf
    return place;
}
void sfree(void* p)
{
    if(p!=NULL)
    {
        list.release_used_block(p);
    }
}
void* srealloc(void* oldp, size_t size)
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

    //no previous block
    if(oldp==NULL)
    {
        return smalloc(size);
    }

    //check if size can fit in this block
    MetaData* block_data=list.get_start_of_block(oldp);
    size_t old_block_size=block_data->size;
    if(old_block_size>=size) // current vlock will do
    {
        return oldp;
    }

    //we need a new block

    void* new_block=smalloc(size);
    if(new_block==NULL)
    {
        return NULL;
    }
    memmove(new_block,oldp,old_block_size); // copy the content
    sfree(oldp);
    return new_block;
}
size_t _num_free_blocks()
{
    return list.get_number_of_all_free_blocks();
}
size_t _num_free_bytes()
{
    return list.get_sum_of_all_free_bytes();
}
size_t _num_allocated_blocks()
{
    return list.get_number_of_all_blocks();
}
size_t _num_allocated_bytes()
{
    return list.get_sum_of_all_bytes();
}
size_t _size_meta_data()
{
    return sizeof(MetaData);
}
size_t _num_meta_data_bytes()
{
    return (_size_meta_data()*_num_allocated_blocks());
}