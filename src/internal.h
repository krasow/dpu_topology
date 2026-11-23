#ifndef DPU_TOPOLOGY_SRC_INTERNAL_H
#define DPU_TOPOLOGY_SRC_INTERNAL_H

#include "dpu_faults.h"

#define DPU_TOPOLOGY_PATH_LEN 512
#define DPU_TOPOLOGY_VPD_MAX_RANKS 4

/* Per-rank DPU availability as recorded by the driver and DIMM VPD. */
typedef struct {
    int nr_cis;
    int nr_dpus_per_ci;
    unsigned ci_mask;
    int rank_index;
    int vpd_valid;
    /* Bit (ci * nr_dpus_per_ci + member) set when the DPU is affected. */
    uint64_t vpd_disabled;
    uint64_t vpd_repaired;
} dpu_rank_health_t;

void dpu_topology_initialize_rank(dpu_rank_topology_t *rank, int numa_node);

/* Reads /sys/class/dpu_rank/dpu_rank<sysfs_index>. */
int dpu_topology_read_physical_rank(uint32_t sysfs_index,
                                    dpu_rank_topology_t *rank);
int dpu_topology_read_rank_health(uint32_t sysfs_index,
                                  dpu_rank_health_t *health);

/* Rank number shown in SDK DPU ids such as "26.5.3" (here 26). */
int dpu_topology_sdk_rank_number(struct dpu_rank_t *rank);

/*
 * Exact SDK-rank to /dev/dpu_rankN mapping via the driver's rank_id.
 * Returns -ENOTSUP for non-hardware backends (e.g. the simulator).
 */
int dpu_topology_rank_sysfs_index(struct dpu_rank_t *rank,
                                  uint32_t *sysfs_index);

/*
 * Returns 1 when every rank is resolved, 0 when the process owns additional
 * rank sets and the association is ambiguous, or a negative errno value.
 */
int dpu_topology_resolve_process_ranks(
    const dpu_rank_topology_t *physical_ranks, size_t nr_physical_ranks,
    const int *numa_nodes, size_t nr_ranks, dpu_rank_topology_t *resolved);

/* Fault report helpers shared by faults.c and health.c. */
int dpu_fault_report_append(dpu_fault_report_t *report,
                            const dpu_fault_t *fault);

/* Physical location of an SDK rank, or NUMA-only when unresolvable. */
void dpu_fault_locate_rank(struct dpu_rank_t *rank,
                           dpu_rank_topology_t *location);

void dpu_fault_init_for_dpu(dpu_fault_t *fault, struct dpu_t *dpu,
                            const dpu_rank_topology_t *location,
                            dpu_fault_kind_t kind, uint32_t set_index,
                            const char *detail);

#endif
