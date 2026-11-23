#define _XOPEN_SOURCE 700

#include "dpu_topology_internal.h"

#include <dpu_management.h>

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

int
dpu_topology_read_physical_rank(uint32_t sysfs_index,
                                dpu_rank_topology_t *rank)
{
    char name[32];

    snprintf(name, sizeof(name), "dpu_rank%u", sysfs_index);
    return read_rank(name, rank);
}

static uint64_t
read_u64_le(const unsigned char *bytes)
{
    uint64_t value = 0;
    int index;

    for (index = 7; index >= 0; --index)
        value = value << 8 | bytes[index];
    return value;
}

/*
 * Mirrors the SDK's hw backend: a DPU is unusable when its CI is masked by
 * the driver, when the DIMM VPD marks it disabled, or when the VPD lists an
 * SRAM repair for it (the SDK currently disables those DPUs too).
 */
int
dpu_topology_read_rank_health(uint32_t sysfs_index,
                              dpu_rank_health_t *health)
{
    /* struct dpu_vpd_header: 16-byte preamble then 4 x 24-byte rank data. */
    enum {
        VPD_MAX_BYTES = 2048,
        VPD_HEADER_BYTES = 16 + DPU_TOPOLOGY_VPD_MAX_RANKS * 24,
        VPD_REPAIR_BYTES = 16,
    };
    unsigned char vpd[VPD_MAX_BYTES];
    char rank_path[DPU_TOPOLOGY_PATH_LEN];
    char vpd_path[DPU_TOPOLOGY_PATH_LEN];
    char name[32];
    int value;
    size_t length;
    FILE *file;

    memset(health, 0, sizeof(*health));
    health->nr_cis = 8;
    health->nr_dpus_per_ci = 8;
    health->ci_mask = 0xff;
    health->rank_index = -1;

    snprintf(name, sizeof(name), "dpu_rank%u", sysfs_index);
    if (join_path(rank_path, sizeof(rank_path), DPU_RANK_CLASS, name) != 0)
        return -ENAMETOOLONG;
    if (access(rank_path, F_OK) != 0)
        return -errno;

    if (read_int_attribute(rank_path, "nb_ci", &value) == 0 && value > 0
        && value <= 8)
        health->nr_cis = value;
    if (read_int_attribute(rank_path, "nb_dpus_per_ci", &value) == 0
        && value > 0 && value <= 8)
        health->nr_dpus_per_ci = value;
    if (read_int_attribute(rank_path, "ci_mask", &value) == 0)
        health->ci_mask = (unsigned)value & 0xff;
    read_int_attribute(rank_path, "rank_index", &health->rank_index);

    if (join_path(vpd_path, sizeof(vpd_path), rank_path, "dimm_vpd") != 0)
        return 0;
    file = fopen(vpd_path, "rb");
    if (file == NULL)
        return 0;
    length = fread(vpd, 1, sizeof(vpd), file);
    fclose(file);

    if (length < VPD_HEADER_BYTES || memcmp(vpd, "UPMV", 4) != 0)
        return 0;
    health->vpd_valid = 1;

    if (health->rank_index >= 0
        && health->rank_index < DPU_TOPOLOGY_VPD_MAX_RANKS) {
        uint16_t repair_count = (uint16_t)(vpd[12] | vpd[13] << 8);
        size_t entry;

        health->vpd_disabled =
            read_u64_le(vpd + 16 + (size_t)health->rank_index * 24);

        /* 0xffff means "repairs never run"; no entries follow. */
        for (entry = 0; repair_count != 0xffff && entry < repair_count;
             ++entry) {
            const unsigned char *repair =
                vpd + VPD_HEADER_BYTES + entry * VPD_REPAIR_BYTES;
            unsigned bit;

            if ((size_t)(repair - vpd) + VPD_REPAIR_BYTES > length)
                break;
            if (repair[1] != health->rank_index)
                continue;
            bit = (unsigned)repair[2] * (unsigned)health->nr_dpus_per_ci
                  + repair[3];
            if (bit < 64)
                health->vpd_repaired |= UINT64_C(1) << bit;
        }
    }
    return 0;
}

/*
 * The hw backend builds SDK rank ids as (backend type << 12) | the driver's
 * rank_id sysfs attribute, which is also what "26" in a "26.5.3" DPU id is.
 */
#define SDK_RANK_TARGET_SHIFT 12
#define SDK_RANK_TARGET_HW 3

int
dpu_topology_sdk_rank_number(struct dpu_rank_t *rank)
{
    return (int)(dpu_get_rank_id(rank) & ((1u << SDK_RANK_TARGET_SHIFT) - 1));
}

static int
rank_id_matches(uint32_t sysfs_index, int driver_rank_id)
{
    char rank_path[DPU_TOPOLOGY_PATH_LEN];
    char name[32];
    int value;

    snprintf(name, sizeof(name), "dpu_rank%u", sysfs_index);
    return join_path(rank_path, sizeof(rank_path), DPU_RANK_CLASS, name) == 0
           && read_int_attribute(rank_path, "rank_id", &value) == 0
           && value == driver_rank_id;
}

int
dpu_topology_rank_sysfs_index(struct dpu_rank_t *rank, uint32_t *sysfs_index)
{
    struct dirent *entry;
    DIR *directory;
    int driver_rank_id;

    if (rank == NULL || sysfs_index == NULL)
        return -EINVAL;
    if ((dpu_get_rank_id(rank) >> SDK_RANK_TARGET_SHIFT) != SDK_RANK_TARGET_HW)
        return -ENOTSUP;
    driver_rank_id = dpu_topology_sdk_rank_number(rank);

    /* rank_id normally equals the device number; verify, else search. */
    if (rank_id_matches((uint32_t)driver_rank_id, driver_rank_id)) {
        *sysfs_index = (uint32_t)driver_rank_id;
        return 0;
    }
    directory = opendir(DPU_RANK_CLASS);
    if (directory == NULL)
        return -errno;
    while ((entry = readdir(directory)) != NULL) {
        uint32_t index;
        if (parse_index(entry->d_name, "dpu_rank", &index) == 0
            && rank_id_matches(index, driver_rank_id)) {
            closedir(directory);
            *sysfs_index = index;
            return 0;
        }
    }
    closedir(directory);
    return -ENOENT;
}
