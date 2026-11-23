#include "dpu_topology.h"

#include <errno.h>
#include <limits.h>

typedef enum {
    QUERY_SOCKETS,
    QUERY_NUMA_NODES,
    QUERY_DIMMS,
    QUERY_CHANNELS,
    QUERY_SPANS_SOCKETS,
    QUERY_CHANNEL_CONTENTION,
} allocation_query_t;

static const char *
plural(size_t count)
{
    return count == 1 ? "" : "s";
}

static int
allocation_query(const struct dpu_set_t *set, allocation_query_t query)
{
    dpu_set_topology_t topology;
    size_t count = 0;
    int value = 0;
    int status;

    if (set == NULL)
        return -EINVAL;
    status = dpu_topology_inspect(*set, &topology);
    if (status != 0)
        return status;

    switch (query) {
    case QUERY_SOCKETS:
        count = topology.nr_sockets;
        break;
    case QUERY_NUMA_NODES:
        count = topology.nr_numa_nodes;
        break;
    case QUERY_DIMMS:
        count = topology.nr_dimms;
        break;
    case QUERY_CHANNELS:
        count = topology.nr_channels;
        break;
    case QUERY_SPANS_SOCKETS:
        value = topology.spans_sockets;
        break;
    case QUERY_CHANNEL_CONTENTION:
        value = topology.has_channel_contention;
        break;
    }

    dpu_topology_free(&topology);
    if (count > INT_MAX)
        return -EOVERFLOW;
    return count != 0 ? (int)count : value;
}

int
allocation_socket_count(const struct dpu_set_t *set)
{
    return allocation_query(set, QUERY_SOCKETS);
}

int
allocation_numa_node_count(const struct dpu_set_t *set)
{
    return allocation_query(set, QUERY_NUMA_NODES);
}

int
allocation_dimm_count(const struct dpu_set_t *set)
{
    return allocation_query(set, QUERY_DIMMS);
}

int
allocation_channel_count(const struct dpu_set_t *set)
{
    return allocation_query(set, QUERY_CHANNELS);
}

int
allocation_spans_sockets(const struct dpu_set_t *set)
{
    return allocation_query(set, QUERY_SPANS_SOCKETS);
}

int
allocation_has_channel_contention(const struct dpu_set_t *set)
{
    return allocation_query(set, QUERY_CHANNEL_CONTENTION);
}

int
allocation_fprint_overview(FILE *stream, const struct dpu_set_t *set)
{
    dpu_set_topology_t topology;
    int status;

    if (stream == NULL || set == NULL)
        return -EINVAL;
    status = dpu_topology_inspect(*set, &topology);
    if (status != 0)
        return status;

    fprintf(stream,
            "DPU topology: %u DPUs across %zu DIMM%s, %zu channel%s, "
            "%zu NUMA node%s, %zu socket%s\n",
            topology.nr_dpus, topology.nr_dimms, plural(topology.nr_dimms),
            topology.nr_channels, plural(topology.nr_channels),
            topology.nr_numa_nodes, plural(topology.nr_numa_nodes),
            topology.nr_sockets, plural(topology.nr_sockets));
    fprintf(stream, "  cross-socket: %s; channel contention: %s\n",
            topology.spans_sockets ? "yes" : "no",
            topology.has_channel_contention ? "yes" : "no");
    if (!topology.physical_mapping_complete)
        fprintf(stream, "  physical mapping: incomplete\n");

    dpu_topology_free(&topology);
    return 0;
}
