#include <memory.h>

volatile uint64_t * pml4;
volatile uint64_t * pdp;
volatile uint64_t * pte;

struct memory_block * head_memory_block;

static void * map_page(uint64_t physical_addr, uint64_t virtual_addr) {
    uint64_t pd_index = (virtual_addr / PAGE_SIZE_2MB) / NO_OF_PT_ENTRIES;
    uint64_t pt_index = (pd_index * NO_OF_PT_ENTRIES) + ((virtual_addr / PAGE_SIZE_2MB) % NO_OF_PT_ENTRIES);

    if((pdp[pd_index] & PG_PRESENT) == 0) {
        pdp[pd_index] = (uint64_t)&pte[pt_index] | PG_PRESENT | PG_RW;
    }

    pte[pt_index] = physical_addr | PG_PRESENT | PG_RW | PG_PSE;

    return (void *)virtual_addr;
}

#if 0
static inline void dump_blocks() {
    struct memory_block * block = head_memory_block;

    printk("\nDUMP MEM. BLOCKS\n");

    while(block) {
        printk("addr: 0x%08x%08x len: 0x%08x%08x available: %c\n",
                block->address >> 32, block->address & 0xffffffff, block->length >> 32,
                block->length & 0xffffffff, block->available ? 'y' : 'n');

        block = block->next;
    }
}
#endif

static struct memory_block * memory_block_new(uintptr_t address, uint64_t length, bool available) {
    struct memory_block * block = head_memory_block;

    while(block->magic == MEMORY_BLOCK_MAGIC) {
        block++;
    }

    block->magic = MEMORY_BLOCK_MAGIC;
    block->address = address;
    block->length = length;
    block->available = available;

    return block;
}

static bool memory_split_block(uintptr_t address, uint64_t length) {
    struct memory_block * block = head_memory_block,
        * alloc_block,
        * remainder;

    while(block) {
        if(block->address <= address && (block->address + block->length) > (address + length)) {
            remainder = memory_block_new(
                            (address + length),
                            (block->address + block->length) - (address + length),
                            true);

            alloc_block = memory_block_new(address, length, false);

            if(block->address < address) {
                // block | alloc | remainder
                block->length = address - block->address;
                alloc_block->prev = block;
                alloc_block->next = remainder;
                remainder->prev = alloc_block;

                if(!block->next) {
                    block->next = alloc_block;
                }
                else {
                    remainder->next = block->next;
                    remainder->next->prev = remainder;
                }
            }
            else if(block->address == address) {
                // alloc | remainder
                block->length = (block->address + block->length) - (address + length);
                block->address = (address + length);


                alloc_block->prev = block->prev;
                alloc_block->prev->next = alloc_block;
                alloc_block->next = block;
                block->prev = alloc_block;

                if(head_memory_block == block) {
                    head_memory_block = alloc_block;
                }
            }

            return true;
        }

        block = block->next;
    }

    return false;
}

void * alloc_page() {
    struct memory_block * block = head_memory_block;
    uintptr_t aligned = 0;
    uint64_t length = memory_page_size();

    while(block) {
        aligned = ALIGN_UP(block->address, length);

        if(block->available && (aligned - block->address) - block->length >= length) {
            memory_split_block(aligned, length);
            return map_page(aligned, aligned);
        }

        block = block->next;
    }

    return nullptr;
}

void init_memory(struct bootdata * bootdata) {
    pml4 = (uint64_t *)bootdata->pml4;
    pdp = (uint64_t *)bootdata->pdp;
    pte = (uint64_t *)bootdata->pte;

    struct mmap * mmap = (struct mmap *)bootdata->mmap,
        * largest_block = nullptr;
    for(uint32_t i = 0; i < bootdata->mmap_count; i++) {
        if(mmap[i].type == MMAP_MEMORY_AVAILABLE && (!largest_block || largest_block->len < mmap[i].len)) {
            largest_block = &mmap[i];
        }
    }

    // Manually allocate a page at the start of the available block of mem, and use it as backing for the memory_block structs
    uint64_t page_size = memory_page_size();
    uintptr_t addr = ALIGN_UP(largest_block->addr, page_size);
    void * allocated = map_page(addr, addr);
    head_memory_block = (struct memory_block *)allocated;

    // Fill out the first block, which details all free memory in the system
    memory_block_new(largest_block->addr, largest_block->len, true);

    // Mark the block we just allocated
    memory_split_block(addr, page_size);
}