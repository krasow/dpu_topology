#include "dpu_topology.h"

#include <errno.h>
#include <string.h>

static const char *
known(const char *value)
{
    return value[0] == '\0' ? "unknown" : value;
}

static int
same_dimm(const dpu_rank_topology_t *lhs, const dpu_rank_topology_t *rhs)
{
    return lhs->dimm_serial[0] != '\0' && rhs->dimm_serial[0] != '\0'
           && strcmp(lhs->dimm_serial, rhs->dimm_serial) == 0;
}

static void
print_rank_table(FILE *stream, const dpu_rank_topology_t *ranks, size_t count)
{
    size_t index;

    fprintf(stream,
            "  idx  rank device       dax device       socket  numa  channel  "
            "DIMM serial  DIMM rank  part\n");
    for (index = 0; index < count; ++index) {
        const dpu_rank_topology_t *rank = &ranks[index];
        char dimm_rank[32] = "?";

        if (rank->dimm_rank_index >= 0 && rank->dimm_rank_count > 0)
            snprintf(dimm_rank, sizeof(dimm_rank), "%d/%d",
                     rank->dimm_rank_index + 1, rank->dimm_rank_count);
        fprintf(stream, "  %-4zu %-17s %-16s %-7d %-5d %-8d %-12s %-10s %s\n",
                index, known(rank->rank_device), known(rank->dax_device),
                rank->socket_id, rank->numa_node, rank->channel_id,
                known(rank->dimm_serial), dimm_rank,
                known(rank->dimm_part_number));
    }
}

static void
print_dimm_groups(FILE *stream, const dpu_rank_topology_t *ranks, size_t count)
{
    size_t index;
    size_t group = 0;

    for (index = 0; index < count; ++index) {
        const dpu_rank_topology_t *dimm = &ranks[index];
        size_t previous;
        size_t member;

        for (previous = 0; previous < index; ++previous)
            if (same_dimm(&ranks[previous], dimm))
                break;
        if (previous != index)
            continue;

        fprintf(stream,
                "  DIMM %zu: serial=%s part=%s socket=%d numa=%d channel=%d\n",
                group++, known(dimm->dimm_serial),
                known(dimm->dimm_part_number), dimm->socket_id,
                dimm->numa_node, dimm->channel_id);
        for (member = index; member < count; ++member) {
            if (member != index && !same_dimm(&ranks[member], dimm))
                continue;
            fprintf(stream, "    %s -> %s\n",
                    known(ranks[member].rank_device),
                    known(ranks[member].dax_device));
        }
    }
}

static void
print_allocation_summary(FILE *stream, const dpu_set_topology_t *topology)
{
    fprintf(stream,
            "DPU allocation: %u DPUs, %zu ranks, %zu physical DIMMs, "
            "%zu channels, %zu NUMA nodes, %zu CPU sockets\n",
            topology->nr_dpus, topology->nr_ranks, topology->nr_dimms,
            topology->nr_channels, topology->nr_numa_nodes,
            topology->nr_sockets);
    fprintf(stream, "  spans sockets: %s; channel contention: %s\n",
            topology->spans_sockets ? "yes" : "no",
            topology->has_channel_contention ? "yes" : "no");
    if (!topology->physical_mapping_complete)
        fprintf(stream, "  physical mapping: incomplete\n");
}

void
dpu_topology_fprint(FILE *stream, const dpu_set_topology_t *topology)
{
    if (stream == NULL || topology == NULL)
        return;
    print_allocation_summary(stream, topology);
    print_rank_table(stream, topology->ranks, topology->nr_ranks);
}

void
dpu_topology_fprint_grouped(FILE *stream,
                            const dpu_set_topology_t *topology)
{
    if (stream == NULL || topology == NULL)
        return;
    print_allocation_summary(stream, topology);
    print_dimm_groups(stream, topology->ranks, topology->nr_ranks);
}

int
dpu_topology_fprint_system(FILE *stream)
{
    dpu_rank_topology_t *ranks = NULL;
    size_t nr_ranks = 0;
    int status;

    if (stream == NULL)
        return -EINVAL;

    status = dpu_topology_discover_system(&ranks, &nr_ranks);
    if (status != 0)
        return status;
    fprintf(stream, "DAX-to-UPMEM-DIMM mapping (%zu rank devices):\n",
            nr_ranks);
    print_rank_table(stream, ranks, nr_ranks);
    free(ranks);
    return 0;
}

int
dpu_topology_fprint_system_grouped(FILE *stream)
{
    dpu_rank_topology_t *ranks = NULL;
    size_t nr_ranks = 0;
    int status;

    if (stream == NULL)
        return -EINVAL;

    status = dpu_topology_discover_system(&ranks, &nr_ranks);
    if (status != 0)
        return status;
    fprintf(stream, "DAX-to-UPMEM-DIMM mapping grouped by physical DIMM:\n");
    print_dimm_groups(stream, ranks, nr_ranks);
    dpu_topology_free_ranks(ranks);
    return 0;
}
