#define _XOPEN_SOURCE 700

#include "dpu_topology_internal.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DPU_RANK_CLASS "/sys/class/dpu_rank"
#define PROCESS_FDS "/proc/self/fd"

typedef struct {
    int fd;
    uint32_t sysfs_index;
} open_rank_t;

static int
join_path(char *output, size_t capacity, const char *directory,
          const char *name)
{
    size_t directory_length = strlen(directory);
    size_t name_length = strlen(name);

    if (directory_length + name_length + 2 > capacity)
        return -ENAMETOOLONG;
    memcpy(output, directory, directory_length);
    output[directory_length] = '/';
    memcpy(output + directory_length + 1, name, name_length + 1);
    return 0;
}

static int
read_file(const char *path, char *output, size_t capacity)
{
    FILE *file;
    size_t length;

    file = fopen(path, "r");
    if (file == NULL)
        return -errno;
    if (fgets(output, (int)capacity, file) == NULL) {
        int status = ferror(file) ? -EIO : -ENODATA;
        fclose(file);
        return status;
    }
    fclose(file);

    length = strlen(output);
    while (length > 0 && isspace((unsigned char)output[length - 1]))
        output[--length] = '\0';
    return 0;
}

static int
read_attribute(const char *directory, const char *name, char *output,
               size_t capacity)
{
    char path[DPU_TOPOLOGY_PATH_LEN];
    int status = join_path(path, sizeof(path), directory, name);
    return status == 0 ? read_file(path, output, capacity) : status;
}

static int
read_int_attribute(const char *directory, const char *name, int *output)
{
    char text[64];
    char *end;
    long value;
    int status = read_attribute(directory, name, text, sizeof(text));

    if (status != 0)
        return status;
    value = strtol(text, &end, 0);
    if (end == text || *end != '\0' || value < INT_MIN || value > INT_MAX)
        return -EINVAL;
    *output = (int)value;
    return 0;
}

static int
parse_index(const char *name, const char *prefix, uint32_t *output)
{
    size_t prefix_length = strlen(prefix);
    char *end;
    unsigned long value;

    if (strncmp(name, prefix, prefix_length) != 0
        || name[prefix_length] == '\0')
        return -EINVAL;
    value = strtoul(name + prefix_length, &end, 10);
    if (*end != '\0' || value > UINT32_MAX)
        return -EINVAL;
    *output = (uint32_t)value;
    return 0;
}

static int
socket_for_numa_node(int numa_node)
{
    char path[DPU_TOPOLOGY_PATH_LEN];
    char cpulist[256];
    char *end;
    long cpu;
    int socket_id;

    if (numa_node < 0)
        return -1;
    snprintf(path, sizeof(path),
             "/sys/devices/system/node/node%d/cpulist", numa_node);
    if (read_file(path, cpulist, sizeof(cpulist)) != 0)
        return -1;
    cpu = strtol(cpulist, &end, 10);
    if (end == cpulist || cpu < 0 || cpu > INT_MAX)
        return -1;
    snprintf(path, sizeof(path),
             "/sys/devices/system/cpu/cpu%ld/topology", cpu);
    return read_int_attribute(path, "physical_package_id", &socket_id) == 0
               ? socket_id
               : -1;
}

void
dpu_topology_initialize_rank(dpu_rank_topology_t *rank, int numa_node)
{
    memset(rank, 0, sizeof(*rank));
    rank->sysfs_index = UINT32_MAX;
    rank->numa_node = numa_node;
    rank->socket_id = socket_for_numa_node(numa_node);
    rank->channel_id = -1;
    rank->dimm_rank_index = -1;
    rank->dimm_rank_count = -1;
}

static void
read_dax_info(const char *rank_path, dpu_rank_topology_t *rank)
{
    char path[DPU_TOPOLOGY_PATH_LEN];
    char region[DPU_TOPOLOGY_PATH_LEN];
    char dax_directory[DPU_TOPOLOGY_PATH_LEN];
    char dax_path[DPU_TOPOLOGY_PATH_LEN];
    struct dirent *entry;
    DIR *directory;

    if (join_path(path, sizeof(path), rank_path, "device") != 0
        || realpath(path, region) == NULL
        || join_path(dax_directory, sizeof(dax_directory), region, "dpu_dax")
               != 0)
        return;

    directory = opendir(dax_directory);
    if (directory == NULL)
        return;
    while ((entry = readdir(directory)) != NULL) {
        if (strncmp(entry->d_name, "dax", 3) != 0)
            continue;
        if (join_path(rank->dax_device, sizeof(rank->dax_device), "/dev",
                      entry->d_name)
                == 0
            && join_path(dax_path, sizeof(dax_path), dax_directory,
                         entry->d_name)
                   == 0)
            read_int_attribute(dax_path, "numa_node", &rank->numa_node);
        break;
    }
    closedir(directory);
}

static int
read_rank(const char *device_name, dpu_rank_topology_t *rank)
{
    char rank_path[DPU_TOPOLOGY_PATH_LEN];

    dpu_topology_initialize_rank(rank, -1);
    if (parse_index(device_name, "dpu_rank", &rank->sysfs_index) != 0
        || join_path(rank_path, sizeof(rank_path), DPU_RANK_CLASS,
                     device_name)
               != 0
        || join_path(rank->rank_device, sizeof(rank->rank_device), "/dev",
                     device_name)
               != 0)
        return -EINVAL;

    read_int_attribute(rank_path, "channel_id", &rank->channel_id);
    read_int_attribute(rank_path, "rank_index", &rank->dimm_rank_index);
    read_int_attribute(rank_path, "rank_count", &rank->dimm_rank_count);
    read_attribute(rank_path, "serial_number", rank->dimm_serial,
                   sizeof(rank->dimm_serial));
    read_attribute(rank_path, "part_number", rank->dimm_part_number,
                   sizeof(rank->dimm_part_number));
    read_dax_info(rank_path, rank);
    rank->socket_id = socket_for_numa_node(rank->numa_node);
    return 0;
}

static int
compare_physical_ranks(const void *left, const void *right)
{
    const dpu_rank_topology_t *lhs = left;
    const dpu_rank_topology_t *rhs = right;
    return (lhs->sysfs_index > rhs->sysfs_index)
               - (lhs->sysfs_index < rhs->sysfs_index);
}

int
dpu_topology_discover_system(dpu_rank_topology_t **ranks, size_t *nr_ranks)
{
    dpu_rank_topology_t *result = NULL;
    size_t count = 0;
    size_t capacity = 0;
    struct dirent *entry;
    DIR *directory;

    if (ranks == NULL || nr_ranks == NULL)
        return -EINVAL;
    *ranks = NULL;
    *nr_ranks = 0;

    directory = opendir(DPU_RANK_CLASS);
    if (directory == NULL)
        return -errno;
    while ((entry = readdir(directory)) != NULL) {
        dpu_rank_topology_t rank;

        if (read_rank(entry->d_name, &rank) != 0)
            continue;
        if (count == capacity) {
            size_t new_capacity = capacity == 0 ? 16 : capacity * 2;
            void *grown = realloc(result, new_capacity * sizeof(*result));
            if (grown == NULL) {
                free(result);
                closedir(directory);
                return -ENOMEM;
            }
            result = grown;
            capacity = new_capacity;
        }
        result[count++] = rank;
    }
    closedir(directory);

    qsort(result, count, sizeof(*result), compare_physical_ranks);
    *ranks = result;
    *nr_ranks = count;
    return 0;
}

static int
compare_open_ranks(const void *left, const void *right)
{
    const open_rank_t *lhs = left;
    const open_rank_t *rhs = right;
    return (lhs->fd > rhs->fd) - (lhs->fd < rhs->fd);
}

static size_t
find_open_ranks(open_rank_t *ranks, size_t capacity)
{
    struct dirent *entry;
    DIR *directory = opendir(PROCESS_FDS);
    size_t count = 0;

    if (directory == NULL)
        return 0;
    while ((entry = readdir(directory)) != NULL) {
        char fd_path[64];
        char target[DPU_TOPOLOGY_PATH_LEN];
        const char *device_name;
        char *end;
        long fd = strtol(entry->d_name, &end, 10);
        ssize_t length;
        uint32_t sysfs_index;

        if (end == entry->d_name || *end != '\0' || fd < 0 || fd > INT_MAX)
            continue;
        snprintf(fd_path, sizeof(fd_path), "%s/%ld", PROCESS_FDS, fd);
        length = readlink(fd_path, target, sizeof(target) - 1);
        if (length < 0)
            continue;
        target[length] = '\0';
        device_name = strrchr(target, '/');
        device_name = device_name == NULL ? target : device_name + 1;
        if (parse_index(device_name, "dpu_rank", &sysfs_index) != 0)
            continue;

        if (count < capacity) {
            ranks[count].fd = (int)fd;
            ranks[count].sysfs_index = sysfs_index;
        }
        ++count;
    }
    closedir(directory);

    if (count <= capacity)
        qsort(ranks, count, sizeof(*ranks), compare_open_ranks);
    return count;
}

static const dpu_rank_topology_t *
find_physical_rank(const dpu_rank_topology_t *ranks, size_t count,
                   uint32_t sysfs_index)
{
    size_t index;
    for (index = 0; index < count; ++index)
        if (ranks[index].sysfs_index == sysfs_index)
            return &ranks[index];
    return NULL;
}

int
dpu_topology_resolve_process_ranks(
    const dpu_rank_topology_t *physical_ranks, size_t nr_physical_ranks,
    const int *numa_nodes, size_t nr_ranks, dpu_rank_topology_t *resolved)
{
    dpu_rank_topology_t *matches = NULL;
    open_rank_t *open_ranks = NULL;
    unsigned char *used = NULL;
    size_t nr_open_ranks;
    size_t index;
    int status = 0;

    open_ranks = calloc(nr_physical_ranks, sizeof(*open_ranks));
    matches = calloc(nr_ranks, sizeof(*matches));
    used = calloc(nr_ranks, sizeof(*used));
    if ((nr_physical_ranks != 0 && open_ranks == NULL)
        || (nr_ranks != 0 && (matches == NULL || used == NULL))) {
        status = -ENOMEM;
        goto cleanup;
    }

    nr_open_ranks = find_open_ranks(open_ranks, nr_physical_ranks);
    if (nr_open_ranks != nr_ranks)
        goto cleanup;

    for (index = 0; index < nr_ranks; ++index) {
        size_t candidate;

        for (candidate = 0; candidate < nr_open_ranks; ++candidate) {
            const dpu_rank_topology_t *rank;

            if (used[candidate])
                continue;
            rank = find_physical_rank(
                physical_ranks, nr_physical_ranks,
                open_ranks[candidate].sysfs_index);
            if (rank != NULL && rank->numa_node == numa_nodes[index]) {
                matches[index] = *rank;
                used[candidate] = 1;
                break;
            }
        }
        if (candidate == nr_open_ranks)
            goto cleanup;
    }

    memcpy(resolved, matches, nr_ranks * sizeof(*resolved));
    status = 1;

cleanup:
    free(used);
    free(matches);
    free(open_ranks);
    return status;
}
