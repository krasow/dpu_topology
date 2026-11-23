#ifndef DPU_TOPOLOGY_HEALTH_COMMON_H
#define DPU_TOPOLOGY_HEALTH_COMMON_H

/* Shared by the host checker and the DPU health kernel. */

#include <stdint.h>

#define HEALTH_NR_TASKLETS 16
#define HEALTH_BLOCK_BYTES 1024
#define HEALTH_BLOCK_WORDS (HEALTH_BLOCK_BYTES / 8)

static inline uint64_t
health_rotl(uint64_t value, unsigned shift)
{
    return (value << shift) | (value >> (64 - shift));
}

/* Input pattern written by the host before the kernel runs. */
static inline uint64_t
health_input(uint64_t seed, uint64_t position)
{
    uint64_t value = seed + (position + 1) * 0x9E3779B97F4A7C15ULL;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31);
}

static inline uint64_t
health_transform(uint64_t value, uint64_t seed, uint64_t position)
{
    uint32_t low = (uint32_t)value;
    uint32_t high = (uint32_t)(value >> 32);

    low = low * 0x01000193u + (uint32_t)position;
    high = (high ^ (uint32_t)seed) + (low >> 7);
    value = ((uint64_t)high << 32 | low) ^ health_rotl(seed, (unsigned)(position & 31) + 1);
    return value - position;
}

static inline uint64_t
health_checksum_init(unsigned tasklet)
{
    return 0xCBF29CE484222325ULL ^ tasklet;
}

static inline uint64_t
health_checksum_step(uint64_t checksum, uint64_t value)
{
    return health_rotl(checksum, 5) ^ value;
}

#endif
