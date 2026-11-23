#include "dpu_topology_internal.h"

#include <dpu_management.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static struct dpu_rank_t *
rank_from_set(struct dpu_set_t set, size_t index)
{
    struct dpu_t *dpu;

    if (set.kind == DPU_SET_RANKS)
        return index < set.list.nr_ranks ? set.list.ranks[index] : NULL;

    dpu = dpu_from_set(set);
    return index == 0 && dpu != NULL ? dpu_get_rank(dpu) : NULL;
}

static int
same_dimm(const dpu_rank_topology_t *lhs, const dpu_rank_topology_t *rhs)
{
    return lhs->dimm_serial[0] != '\0' && rhs->dimm_serial[0] != '\0'
           && strcmp(lhs->dimm_serial, rhs->dimm_serial) == 0;
}

static int
same_channel(const dpu_rank_topology_t *lhs,
             const dpu_rank_topology_t *rhs)
{
    return lhs->channel_id >= 0 && lhs->channel_id == rhs->channel_id
           && lhs->numa_node == rhs->numa_node;
}

/* Maps every SDK rank to its sysfs device through the hw backend. */
static int
resolve_exact(struct dpu_rank_t *const *sdk_ranks, size_t nr_ranks,
              const dpu_rank_topology_t *physical_ranks,
              size_t nr_physical_ranks, dpu_rank_topology_t *resolved)
{
    size_t index;

    for (index = 0; index < nr_ranks; ++index) {
        uint32_t sysfs_index;
        size_t candidate;

        if (dpu_topology_rank_sysfs_index(sdk_ranks[index], &sysfs_index)
            != 0)
            return 0;
        for (candidate = 0; candidate < nr_physical_ranks; ++candidate)
            if (physical_ranks[candidate].sysfs_index == sysfs_index)
                break;
        if (candidate == nr_physical_ranks)
            return 0;
        resolved[index] = physical_ranks[candidate];
    }
    return 1;
}

static void
summarize(dpu_set_topology_t *topology)
{
    size_t index;

    topology->physical_mapping_complete = 1;
    for (index = 0; index < topology->nr_ranks; ++index) {
        const dpu_rank_topology_t *rank = &topology->ranks[index];
        size_t previous;

        if (rank->rank_device[0] == '\0' || rank->dax_device[0] == '\0'
            || rank->dimm_serial[0] == '\0')
            topology->physical_mapping_complete = 0;

        if (rank->numa_node >= 0) {
            for (previous = 0; previous < index; ++previous)
                if (topology->ranks[previous].numa_node == rank->numa_node)
                    break;
            topology->nr_numa_nodes += previous == index;
        }

        if (rank->socket_id >= 0) {
            for (previous = 0; previous < index; ++previous)
                if (topology->ranks[previous].socket_id == rank->socket_id)
                    break;
            topology->nr_sockets += previous == index;
        }

        if (rank->dimm_serial[0] != '\0') {
            for (previous = 0; previous < index; ++previous)
                if (same_dimm(&topology->ranks[previous], rank))
                    break;
            topology->nr_dimms += previous == index;
        }

        if (rank->channel_id >= 0) {
            for (previous = 0; previous < index; ++previous)
                if (same_channel(&topology->ranks[previous], rank))
                    break;
            topology->nr_channels += previous == index;
        }
    }

    topology->spans_numa_nodes = topology->nr_numa_nodes > 1;
    topology->spans_sockets = topology->nr_sockets > 1;

    for (index = 0; index < topology->nr_ranks; ++index) {
        size_t other;
        for (other = index + 1; other < topology->nr_ranks; ++other) {
            if (same_channel(&topology->ranks[index],
                             &topology->ranks[other])
                && !same_dimm(&topology->ranks[index],
                              &topology->ranks[other])) {
                topology->has_channel_contention = 1;
                return;
            }
        }
    }
}

int
dpu_topology_inspect(struct dpu_set_t set, dpu_set_topology_t *topology)
{
    dpu_rank_topology_t *physical_ranks = NULL;
    int *numa_nodes = NULL;
    struct dpu_rank_t **sdk_ranks = NULL;
    size_t nr_physical_ranks = 0;
    uint32_t nr_ranks;
    size_t index;
    int status;

    if (topology == NULL)
        return -EINVAL;
    memset(topology, 0, sizeof(*topology));

    if (dpu_get_nr_ranks(set, &nr_ranks) != DPU_OK
        || dpu_get_nr_dpus(set, &topology->nr_dpus) != DPU_OK)
        return -EIO;

    status = dpu_topology_discover_system(&physical_ranks,
                                          &nr_physical_ranks);
    if (status != 0)
        return status;

    topology->ranks = calloc(nr_ranks, sizeof(*topology->ranks));
    numa_nodes = calloc(nr_ranks, sizeof(*numa_nodes));
    sdk_ranks = calloc(nr_ranks, sizeof(*sdk_ranks));
    if ((nr_ranks != 0 && topology->ranks == NULL) || numa_nodes == NULL
        || sdk_ranks == NULL) {
        status = -ENOMEM;
        goto cleanup;
    }
    topology->nr_ranks = nr_ranks;

    for (index = 0; index < nr_ranks; ++index) {
        struct dpu_rank_t *rank = rank_from_set(set, index);

        if (rank == NULL) {
            status = -EINVAL;
            goto cleanup;
        }
        sdk_ranks[index] = rank;
        numa_nodes[index] = dpu_get_rank_numa_node(rank);
        dpu_topology_initialize_rank(&topology->ranks[index],
                                     numa_nodes[index]);
    }

    if (!resolve_exact(sdk_ranks, nr_ranks, physical_ranks,
                       nr_physical_ranks, topology->ranks)) {
        for (index = 0; index < nr_ranks; ++index)
            dpu_topology_initialize_rank(&topology->ranks[index],
                                         numa_nodes[index]);
        status = dpu_topology_resolve_process_ranks(
            physical_ranks, nr_physical_ranks, numa_nodes, nr_ranks,
            topology->ranks);
        if (status < 0)
            goto cleanup;
    }

    summarize(topology);
    status = 0;

cleanup:
    free(sdk_ranks);
    free(numa_nodes);
    free(physical_ranks);
    if (status != 0)
        dpu_topology_free(topology);
    return status;
}

void
dpu_topology_free(dpu_set_topology_t *topology)
{
    if (topology == NULL)
        return;
    free(topology->ranks);
    memset(topology, 0, sizeof(*topology));
}

void
dpu_topology_free_ranks(dpu_rank_topology_t *ranks)
{
    free(ranks);
}
