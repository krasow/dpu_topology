#ifndef DPU_TOPOLOGY_H
#define DPU_TOPOLOGY_H

#include <dpu.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DPU_TOPOLOGY_DEVICE_LEN 128
#define DPU_TOPOLOGY_ID_LEN 64

typedef struct {
    uint32_t sysfs_index;
    int numa_node;
    int socket_id;
    int channel_id;
    int dimm_rank_index;
    int dimm_rank_count;
    char rank_device[DPU_TOPOLOGY_DEVICE_LEN];
    char dax_device[DPU_TOPOLOGY_DEVICE_LEN];
    char dimm_serial[DPU_TOPOLOGY_ID_LEN];
    char dimm_part_number[DPU_TOPOLOGY_ID_LEN];
} dpu_rank_topology_t;

typedef struct {
    dpu_rank_topology_t *ranks;
    size_t nr_ranks;
    uint32_t nr_dpus;
    size_t nr_numa_nodes;
    size_t nr_sockets;
    size_t nr_dimms;
    size_t nr_channels;
    int physical_mapping_complete;
    int spans_numa_nodes;
    int spans_sockets;
    int has_channel_contention;
} dpu_set_topology_t;

/*
 * Inspects the physical ranks backing an allocated DPU set.
 * Returns 0 on success and a negative errno value on failure.
 */
int dpu_topology_inspect(struct dpu_set_t set, dpu_set_topology_t *topology);

/* Releases memory owned by a topology returned by dpu_topology_inspect. */
void dpu_topology_free(dpu_set_topology_t *topology);

/*
 * Discovers all DPU rank devices visible in sysfs. The caller owns *ranks and
 * must release it with dpu_topology_free_ranks.
 */
int dpu_topology_discover_system(dpu_rank_topology_t **ranks,
                                 size_t *nr_ranks);
void dpu_topology_free_ranks(dpu_rank_topology_t *ranks);

/* Human-readable reports for benchmark logs and standalone diagnostics. */
void dpu_topology_fprint(FILE *stream, const dpu_set_topology_t *topology);
void dpu_topology_fprint_grouped(FILE *stream,
                                 const dpu_set_topology_t *topology);
int dpu_topology_fprint_system(FILE *stream);
int dpu_topology_fprint_system_grouped(FILE *stream);

#ifdef __cplusplus
}
#endif

#endif
