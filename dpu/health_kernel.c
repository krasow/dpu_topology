/*
 * DPU health-check kernel. Every tasklet streams its share of the MRAM heap
 * through WRAM, transforms each 64-bit word, writes it back, and accumulates a
 * checksum. The host recomputes everything and flags DPUs that disagree.
 * Keep the arithmetic in sync with src/health.c.
 */
#include <defs.h>
#include <mram.h>
#include <stdint.h>

#include "health.h"

__host uint64_t health_seed;
__host uint32_t health_nr_blocks;
__host uint64_t health_checksums[HEALTH_NR_TASKLETS];

static __dma_aligned uint64_t cache[HEALTH_NR_TASKLETS][HEALTH_BLOCK_WORDS];

int
main(void)
{
    unsigned tasklet = me();
    uint64_t *block = cache[tasklet];
    uint64_t checksum = health_checksum_init(tasklet);
    uint32_t index;

    for (index = tasklet; index < health_nr_blocks;
         index += HEALTH_NR_TASKLETS) {
        __mram_ptr void *address =
            DPU_MRAM_HEAP_POINTER + (uint32_t)index * HEALTH_BLOCK_BYTES;
        uint32_t word;

        mram_read(address, block, HEALTH_BLOCK_BYTES);
        for (word = 0; word < HEALTH_BLOCK_WORDS; ++word) {
            uint64_t position = (uint64_t)index * HEALTH_BLOCK_WORDS + word;
            block[word] = health_transform(block[word], health_seed, position);
            checksum = health_checksum_step(checksum, block[word]);
        }
        mram_write(block, address, HEALTH_BLOCK_BYTES);
    }
    health_checksums[tasklet] = checksum;
    return 0;
}
