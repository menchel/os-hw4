#include <unistd.h>
#include <string.h>
#include <sys/mman.h>
#include <cstdint>
#define MAX_MEMORY_ALLOCATED_SIZE 100000000 // 10^8
#define MAX_ORDER 10
#define MAX_SIZE_BLOCK 128*1024
#define BLOCK_UNIT 128
#define ALLINMENT_FACTOR 32*128*1024
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

class BlockTable
{
   size_t bytes_used_not_by_mmap;
   size_t num_of_blocks_not_used_by_mmap;
   MetaData* allocated_by_mmap;
   MetaData* array[MAX_ORDER+1];
public:
    BlockTable():bytes_used_not_by_mmap(0),num_of_blocks_not_used_by_mmap(0),allocated_by_mmap(NULL)
    {
        for(int i=0;i<=MAX_ORDER;i++)
        {
            array[i]=NULL;
        }
    }
    MetaData* get_start_of_block(void* block)
    {
        return (MetaData*)((char*)block-sizeof(MetaData));
    }
    MetaData* calculate_xor_of_pointers_for_buddy_search(void* adress, size_t size)
    {
        uintptr_t first_reused=reinterpret_cast<uintptr_t>(adress);
        uintptr_t second_reused=reinterpret_cast<uintptr_t>(size);
        uintptr_t result=first_reused^second_reused;
        return (MetaData*)(result);
    }
    int get_order_of_block(MetaData* block)
    {
        size_t size_with_metadata=(sizeof(MetaData)+block->size)/BLOCK_UNIT;
        int order=0;
        while(size_with_metadata>1)
        {
            size_with_metadata=size_with_metadata/2;
            order++;
        }
        return order;
    }
    void insert_block_to_array(MetaData* block)
    {
        int order=get_order_of_block(block);
        MetaData* list=array[order];
        //list is empty
        if(list==NULL)
        {
            block->next=NULL;
            block->prev=NULL;
            array[order]=block;
            return;
        }
        //list has elements
        MetaData* insertion_block=NULL;
        while(list!=NULL)
        {
            if(block<list) // we want list to be sorted by memory adresses
            {
                insertion_block=list;
                break;
            }
            insertion_block=list;
            list=list->next;
        }
        //we reached the end
        if(list==NULL)
        {
            insertion_block->next=block;
            block->prev=insertion_block;
            block->next=NULL;
            return;
        }
        //we found where to insert

        //insert in head
        if(insertion_block->prev==NULL)
        {
            insertion_block->prev=block;
            block->next=insertion_block;
            block->prev=NULL;
            array[order]=block;
            return;
        }
        //we got here, so we insert in the middle
        block->next=insertion_block;
        block->prev=insertion_block->prev;
        (block->prev)->next=block;
        insertion_block->prev=block;
    }
    void delete_block_from_array(MetaData* block)
    {
        int order=get_order_of_block(block);

        //if we want to delete head
        if(block->prev==NULL)
        {
            array[order]=block->next;
            if(block->next!=NULL)
            {
                (block->next)->prev=NULL;
            }
            block->next=NULL;
            return;
        }

        //if we want to remove at last
        if(block->next==NULL)
        {
            (block->prev)->next=NULL;
            block->prev=NULL;
            return;
        }

        //regular delete
        (block->prev)->next=block->next;
        (block->next)->prev=block->prev;
        block->next=NULL;
        block->prev=NULL;
    }
    MetaData* find_best_block_for_allocation(size_t size)
    {
        int order=0;
        size_t num=128;
        while(size+sizeof(MetaData)>num)
        {
            num=num*2;
            order++;
        }
        for(int i=order;i<=MAX_ORDER;i++)
        {
            MetaData* list=array[i];
            while(list!=NULL)
            {
                if(list->size>=size && list->is_free)
                {
                    return list;
                }
                list=list->next;
            }
        }
        return NULL;
    }
    void split_block(MetaData* block_to_split)
    {
        size_t new_block_total_size=(block_to_split->size+sizeof(MetaData))/2;
        MetaData* second_half=(MetaData*)((char*)block_to_split+new_block_total_size); // new free block
        second_half->size=new_block_total_size-sizeof(MetaData);
        second_half->is_free=true;
        second_half->next=NULL;
        second_half->prev=NULL;
        block_to_split->size=new_block_total_size-sizeof(MetaData);

        insert_block_to_array(second_half); // insert the free block back
    }
    MetaData* allocate_block_without_mmap(size_t size)
    {
        MetaData* optimal_block=find_best_block_for_allocation(size);
        if(optimal_block==NULL)
        {
            //what to do? Is it possible?
            return NULL;
        }

        //we have our block. Can it be seperated?
        delete_block_from_array(optimal_block);
        size_t half_block_size=(optimal_block->size+sizeof(MetaData))/2;
        while(half_block_size>=size+sizeof(MetaData) && half_block_size>=BLOCK_UNIT) // can split
        {
            split_block(optimal_block);
            half_block_size=(optimal_block->size+sizeof(MetaData))/2;
        }
        //now we can use the block
        optimal_block->is_free=false;
        bytes_used_not_by_mmap+=optimal_block->size;
        num_of_blocks_not_used_by_mmap++;
        return optimal_block;
    }
    void free_used_block(MetaData* block_to_free)
    {
        block_to_free->is_free=true;
        bytes_used_not_by_mmap-=block_to_free->size;
        num_of_blocks_not_used_by_mmap--;
        if(block_to_free->size>=MAX_SIZE_BLOCK-sizeof(MetaData))
        {
            insert_block_to_array(block_to_free);
            block_to_free->is_free=true;
            return;
        }
        MetaData* buddy=calculate_xor_of_pointers_for_buddy_search(block_to_free,(block_to_free->size+sizeof(MetaData)));
        if(!buddy->is_free)
        {
            insert_block_to_array(block_to_free);
            block_to_free->is_free=true;
            return;
        }
        MetaData* first;
        MetaData* second;
        while(buddy->is_free)
        {
            //we can merge :)
            delete_block_from_array(buddy);

            if(block_to_free<buddy) // decide who is first
            {
                first=block_to_free;
                second=buddy;
            }
            else
            {
                first=buddy;
                second=block_to_free;
            }
            first->size=first->size+sizeof(MetaData)+second->size;
            if(first->size>=MAX_SIZE_BLOCK-sizeof(MetaData))
            {
                break;
            }
            block_to_free=first;
            buddy=calculate_xor_of_pointers_for_buddy_search(block_to_free,sizeof(MetaData)+block_to_free->size);
        }
        insert_block_to_array(first);
        first->is_free=true;
        return;
    }
    void insert_mmap_block(MetaData* block)
    {
        MetaData* prev=NULL;
        MetaData* current=allocated_by_mmap;
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
            allocated_by_mmap=block;
        }
    }
    void remove_mmap_block(MetaData* block)
    {
        //if we want to delete head
        if(block->prev==NULL)
        {
            allocated_by_mmap=block->next;
            block->next=NULL;
            return;
        }

        //if we want to remove at last
        if(block->next==NULL)
        {
            (block->prev)->next=NULL;
            block->prev=NULL;
            return;
        }

        //regular delete
        (block->prev)->next=block->next;
        (block->next)->prev=block->prev;
        block->next=NULL;
        block->prev=NULL;
    }
    MetaData* allocate_block_with_mmap(size_t size)
    {
        void* mmap_block=mmap(NULL,sizeof(MetaData)+size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        if(mmap_block==MAP_FAILED) // something failed
        {
            return NULL;
        }

        MetaData* block_allocated=(MetaData*)mmap_block;
        insert_mmap_block(block_allocated);
        block_allocated->size=size;
        block_allocated->is_free=false;
        return block_allocated;
    }
    void free_mmap_allocated_block(void* block_to_free)
    {
        MetaData* meta_data_for_block=(MetaData*)get_start_of_block(block_to_free);
        remove_mmap_block(meta_data_for_block);
        munmap(block_to_free,meta_data_for_block->size+sizeof(MetaData));
    }
    bool can_merge_to_create_block(MetaData* current, size_t size)
    {
        size_t current_size=current->size;
        while(current_size<MAX_SIZE_BLOCK-sizeof(MetaData))
        {
            MetaData* buddy=calculate_xor_of_pointers_for_buddy_search(current,sizeof(MetaData)+current_size); // buddy
            if(!buddy->is_free)
            {
                return false;
            }
            current_size=current_size+sizeof(MetaData)+buddy->size;
            if(current_size>=size) // we found a fitting size!
            {
                return true;
            }
            if(current>buddy)
            {
                current=buddy;
            }
        }
        return false;
    }
    MetaData* merge_to_create_block(MetaData* current, size_t size)
    {
        bytes_used_not_by_mmap-=current->size;
        num_of_blocks_not_used_by_mmap--;
        MetaData* buddy=calculate_xor_of_pointers_for_buddy_search(current,sizeof(MetaData)+current->size);
        MetaData* first=current;
        MetaData* second;
        while(buddy->is_free)
        {
            //we can merge :)
            delete_block_from_array(buddy);
            if(current<buddy) // decide who is first
            {
                first=current;
                second=buddy;
            }
            else
            {
                first=buddy;
                second=current;
            }
            first->size=first->size+sizeof(MetaData)+second->size;
            current=first;
            if(current->size>=size)
            {
                break;
            }
            buddy=calculate_xor_of_pointers_for_buddy_search(current,sizeof(MetaData)+current->size);
        }
        bytes_used_not_by_mmap+=current->size;
        num_of_blocks_not_used_by_mmap++;
        return first;
    }
    //get sum of all bytes (no metadata)
    size_t get_sum_of_all_bytes()
    {
        size_t sum=0;
        //used blocks
        sum+=bytes_used_not_by_mmap;
        //free blocks
        for(int i=0;i<=MAX_ORDER;i++)
        {
             MetaData* current=array[i];
             while(current!=NULL)
            {
                sum+=current->size;
                current=current->next;
            }
        }
        //mmap blocks
        MetaData* temp=allocated_by_mmap;
        while(temp!=NULL)
        {
            sum+=temp->size;
            temp=temp->next;
        }
        return sum;
    }
    //get sum of all blocks
    size_t get_number_of_all_blocks()
    {
        size_t counter=0;
        //used blocks
        counter+=num_of_blocks_not_used_by_mmap;
        //free blocks
        for(int i=0;i<=MAX_ORDER;i++)
        {
            MetaData* current=array[i];
            while(current!=NULL)
            {
                counter++;
                current=current->next;
            }
        }
        //mmap blocks
        MetaData* temp=allocated_by_mmap;
        while(temp!=NULL)
        {
            counter++;
            temp=temp->next;
        }
        return counter;
    }
    //get sum of all free bytes (no metadata)
    size_t get_sum_of_all_free_bytes()
    {
        size_t sum=0;
        //free blocks
        for(int i=0;i<=MAX_ORDER;i++)
        {
            MetaData* current=array[i];
            while(current!=NULL)
            {
                sum+=current->size;
                current=current->next;
            }
        }
        return sum;
    }
    //get sum of all free blocks
    size_t get_number_of_all_free_blocks()
    {
        size_t counter=0;
        //free blocks
        for(int i=0;i<=MAX_ORDER;i++)
        {
            MetaData* current=array[i];
            while(current!=NULL)
            {
                counter++;
                current=current->next;
            }
        }
        return counter;
    }
    void first_assign()
    {
        //allinment
        void* current_p_break = sbrk(0);
        intptr_t p_break_adress = (intptr_t)current_p_break;
        intptr_t aligned_p_break_adress = (p_break_adress + ALLINMENT_FACTOR - 1) & ~(ALLINMENT_FACTOR - 1);
        sbrk(aligned_p_break_adress - p_break_adress);
        //creating
        size_t allocation_cost=MAX_SIZE_BLOCK;
        size_t size_of_data=allocation_cost-sizeof(MetaData);
        for(int i=0;i<32;i++)
        {
            void* p_break=sbrk(allocation_cost);
            // allocate the new block
            MetaData* new_block_allocated=(MetaData*)p_break;
            new_block_allocated->size=size_of_data;
            new_block_allocated->is_free=true;
            new_block_allocated->next=NULL;
            new_block_allocated->prev=NULL;
            insert_block_to_array(new_block_allocated);
        }
    }
};

/*
---------------------------------------
            IMPLEMENTATION
---------------------------------------
*/

//global list
BlockTable table=BlockTable();
bool allocate=false;

void* smalloc(size_t size)
{
    if(!allocate)
    {
        allocate=true;
        table.first_assign();
    }
    //size conditions
    if(size ==0)
    {
        return NULL;
    }
    if(size > MAX_MEMORY_ALLOCATED_SIZE)
    {
        return NULL;
    }
    void* p_break;
    if(size>=MAX_SIZE_BLOCK)
    {
        p_break=table.allocate_block_with_mmap(size);
    }
    else
    {
        p_break=table.allocate_block_without_mmap(size);
    }
    //if allocation failed
    if(p_break==NULL)
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
        MetaData* metadata=table.get_start_of_block(p);
        if(metadata->size>=MAX_SIZE_BLOCK) //mmap
        {
            table.free_mmap_allocated_block(p);
        }
        else //regular
        {
            table.free_used_block(metadata);
        }
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
    MetaData* details=(MetaData*)table.get_start_of_block(oldp);
    if(details->size>MAX_SIZE_BLOCK) // mmaped
    {
        if(details->size==size) // according to pdf
        {
            return oldp;
        }
        void* new_mmap_block=smalloc(size);
        if(new_mmap_block==NULL)
        {
            return NULL;
        }
        if(size>details->size)
        {
            memmove(new_mmap_block,oldp,details->size);
        }
        else
        {
            memmove(new_mmap_block,oldp,size);
        }
        sfree(oldp);
        return new_mmap_block;
    }
    else //regular
    {
        // try to use this block first
        if(details->size>=size)
        {
            return oldp;
        }
        if(table.can_merge_to_create_block(details,size)) // check if we can merge
        {
            //make it return the new Metadata
            MetaData* new_allocated=table.merge_to_create_block(details,size);
            void* adrees_of_data=(char*)new_allocated+sizeof(MetaData);
            memmove(adrees_of_data,oldp,details->size);
            new_allocated->is_free=false;
            new_allocated->prev=NULL;
            new_allocated->next=NULL;
            return (char*)new_allocated+sizeof(MetaData);
        }
    }

    //we need a new block

    MetaData* metadata=(MetaData*)table.get_start_of_block(oldp);
    void* new_block=smalloc(size);
    if(new_block==NULL)
    {
        return NULL;
    }
    memmove(new_block,oldp,metadata->size); // copy the content
    sfree(oldp);
    return new_block;
}
size_t _num_free_blocks()
{
    return table.get_number_of_all_free_blocks();
}
size_t _num_free_bytes()
{
    return table.get_sum_of_all_free_bytes();
}
size_t _num_allocated_blocks()
{
    return table.get_number_of_all_blocks();
}
size_t _num_allocated_bytes()
{
    return table.get_sum_of_all_bytes();
}
size_t _size_meta_data()
{
    return sizeof(MetaData);
}
size_t _num_meta_data_bytes()
{
    return (_size_meta_data()*_num_allocated_blocks());
}