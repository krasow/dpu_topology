#ifndef DPU_TOPOLOGY_INTERNAL_H
#define DPU_TOPOLOGY_INTERNAL_H

#include "dpu_topology.h"

#define DPU_TOPOLOGY_PATH_LEN 512

void dpu_topology_initialize_rank(dpu_rank_topology_t *rank, int numa_node);

/*
 * Returns 1 when every rank is resolved, 0 when the process owns additional
 * rank sets and the association is ambiguous, or a negative errno value.
 */
int dpu_topology_resolve_process_ranks(
    const dpu_rank_topology_t *physical_ranks, size_t nr_physical_ranks,
    const int *numa_nodes, size_t nr_ranks, dpu_rank_topology_t *resolved);

#endif
