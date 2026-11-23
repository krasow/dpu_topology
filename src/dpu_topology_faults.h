#ifndef DPU_TOPOLOGY_FAULTS_H
#define DPU_TOPOLOGY_FAULTS_H

#include "dpu_topology_internal.h"

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
