#include "internal.h"

#include <dpu_management.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

void
dpu_fault_report_init(dpu_fault_report_t *report)
{
    if (report != NULL)
        memset(report, 0, sizeof(*report));
}

void
dpu_fault_report_free(dpu_fault_report_t *report)
{
    if (report == NULL)
        return;
    free(report->faults);
    memset(report, 0, sizeof(*report));
}

const char *
dpu_fault_kind_name(dpu_fault_kind_t kind)
{
    switch (kind) {
    case DPU_FAULT_CI_MASKED:
        return "chip masked by driver";
    case DPU_FAULT_VPD_DISABLED:
        return "disabled in DIMM VPD";
    case DPU_FAULT_VPD_REPAIR:
        return "SRAM repair in DIMM VPD";
    case DPU_FAULT_SDK_DISABLED:
        return "disabled by SDK";
    case DPU_FAULT_MEMORY:
        return "MRAM readback mismatch";
    case DPU_FAULT_EXEC_FAULT:
        return "execution fault";
    case DPU_FAULT_TIMEOUT:
        return "timeout";
    case DPU_FAULT_WRONG_RESULT:
        return "wrong result";
    case DPU_FAULT_REPORTED:
        return "reported by application";
    }
    return "unknown";
}

int
dpu_fault_report_append(dpu_fault_report_t *report, const dpu_fault_t *fault)
{
    if (report->nr_faults == report->capacity) {
        size_t capacity = report->capacity == 0 ? 16 : report->capacity * 2;
        dpu_fault_t *grown =
            realloc(report->faults, capacity * sizeof(*grown));

        if (grown == NULL)
            return -ENOMEM;
        report->faults = grown;
        report->capacity = capacity;
    }
    report->faults[report->nr_faults++] = *fault;
    return 0;
}

void
dpu_fault_locate_rank(struct dpu_rank_t *rank, dpu_rank_topology_t *location)
{
    uint32_t sysfs_index;

    if (dpu_topology_rank_sysfs_index(rank, &sysfs_index) == 0
        && dpu_topology_read_physical_rank(sysfs_index, location) == 0)
        return;
    dpu_topology_initialize_rank(location, dpu_get_rank_numa_node(rank));
}

void
dpu_fault_init_for_dpu(dpu_fault_t *fault, struct dpu_t *dpu,
                       const dpu_rank_topology_t *location,
                       dpu_fault_kind_t kind, uint32_t set_index,
                       const char *detail)
{
    memset(fault, 0, sizeof(*fault));
    fault->kind = kind;
    fault->set_index = set_index;
    fault->sdk_rank_id = dpu_topology_sdk_rank_number(dpu_get_rank(dpu));
    fault->slice_id = dpu_get_slice_id(dpu);
    fault->member_id = dpu_get_member_id(dpu);
    fault->rank = *location;
    if (detail != NULL)
        snprintf(fault->detail, sizeof(fault->detail), "%s", detail);
}

int
dpu_faults_add(dpu_fault_report_t *report, struct dpu_set_t dpu_set,
               uint32_t set_index, const char *detail)
{
    dpu_rank_topology_t location;
    dpu_fault_t fault;
    struct dpu_t *dpu;

    if (report == NULL)
        return -EINVAL;
    dpu = dpu_from_set(dpu_set);
    if (dpu == NULL)
        return -EINVAL;
    dpu_fault_locate_rank(dpu_get_rank(dpu), &location);
    dpu_fault_init_for_dpu(&fault, dpu, &location, DPU_FAULT_REPORTED,
                           set_index, detail);
    return dpu_fault_report_append(report, &fault);
}

static uint64_t
health_bit(const dpu_rank_health_t *health, int ci, int member)
{
    return UINT64_C(1) << (ci * health->nr_dpus_per_ci + member);
}

int
dpu_faults_scan_system(dpu_fault_report_t *report)
{
    dpu_rank_topology_t *ranks = NULL;
    size_t nr_ranks = 0;
    size_t index;
    int status;

    if (report == NULL)
        return -EINVAL;
    status = dpu_topology_discover_system(&ranks, &nr_ranks);
    if (status != 0)
        return status;

    for (index = 0; index < nr_ranks && status == 0; ++index) {
        dpu_rank_health_t health;
        int ci;

        if (dpu_topology_read_rank_health(ranks[index].sysfs_index, &health)
            != 0)
            continue;
        report->nr_dpus_checked +=
            (uint32_t)(health.nr_cis * health.nr_dpus_per_ci);

        for (ci = 0; ci < health.nr_cis && status == 0; ++ci) {
            int member;
            for (member = 0; member < health.nr_dpus_per_ci && status == 0;
                 ++member) {
                uint64_t bit = health_bit(&health, ci, member);
                dpu_fault_t fault;

                memset(&fault, 0, sizeof(fault));
                fault.set_index = DPU_FAULT_NO_INDEX;
                fault.sdk_rank_id = -1;
                fault.slice_id = ci;
                fault.member_id = member;
                fault.rank = ranks[index];

                if (!(health.ci_mask & (1u << ci))) {
                    fault.kind = DPU_FAULT_CI_MASKED;
                    status = dpu_fault_report_append(report, &fault);
                    continue;
                }
                if (health.vpd_disabled & bit) {
                    fault.kind = DPU_FAULT_VPD_DISABLED;
                    status = dpu_fault_report_append(report, &fault);
                }
                if (status == 0 && (health.vpd_repaired & bit)) {
                    fault.kind = DPU_FAULT_VPD_REPAIR;
                    status = dpu_fault_report_append(report, &fault);
                }
                if (status != 0)
                    break;
            }
        }
    }
    dpu_topology_free_ranks(ranks);
    return status;
}

static const char *
disabled_reason(const dpu_rank_health_t *health, int have_health, int ci,
                int member)
{
    uint64_t bit;

    if (!have_health)
        return NULL;
    bit = health_bit(health, ci, member);
    if (!(health->ci_mask & (1u << ci)))
        return "chip masked by driver";
    if (health->vpd_disabled & bit)
        return "marked disabled in DIMM VPD";
    if (health->vpd_repaired & bit)
        return "SRAM repair listed in DIMM VPD";
    return NULL;
}

int
dpu_faults_scan_set(struct dpu_set_t set, dpu_fault_report_t *report)
{
    struct dpu_set_t rank_set;
    uint32_t nr_positions = 0;
    int status = 0;

    if (report == NULL || set.kind != DPU_SET_RANKS)
        return -EINVAL;

    DPU_RANK_FOREACH(set, rank_set)
    {
        struct dpu_rank_t *rank = dpu_rank_from_set(rank_set);
        dpu_description_t description;
        dpu_rank_topology_t location;
        dpu_rank_health_t health = {0};
        int have_health = 0;
        int nr_cis;
        int nr_members;
        int ci;

        if (rank == NULL)
            return -EINVAL;
        description = dpu_get_description(rank);
        nr_cis = description->hw.topology.nr_of_control_interfaces;
        nr_members = description->hw.topology.nr_of_dpus_per_control_interface;
        dpu_fault_locate_rank(rank, &location);
        if (location.sysfs_index != UINT32_MAX)
            have_health = dpu_topology_read_rank_health(location.sysfs_index,
                                                        &health)
                              == 0
                          && health.nr_dpus_per_ci == nr_members;
        nr_positions += (uint32_t)(nr_cis * nr_members);

        for (ci = 0; ci < nr_cis; ++ci) {
            int member;
            for (member = 0; member < nr_members; ++member) {
                struct dpu_t *dpu = dpu_get(rank, (dpu_slice_id_t)ci,
                                            (dpu_member_id_t)member);
                dpu_fault_t fault;
                const char *reason;

                if (dpu != NULL && dpu_is_enabled(dpu))
                    continue;
                reason = disabled_reason(&health, have_health, ci, member);
                if (dpu != NULL) {
                    dpu_fault_init_for_dpu(&fault, dpu, &location,
                                           DPU_FAULT_SDK_DISABLED,
                                           DPU_FAULT_NO_INDEX, reason);
                } else {
                    memset(&fault, 0, sizeof(fault));
                    fault.kind = DPU_FAULT_SDK_DISABLED;
                    fault.set_index = DPU_FAULT_NO_INDEX;
                    fault.sdk_rank_id = dpu_topology_sdk_rank_number(rank);
                    fault.slice_id = ci;
                    fault.member_id = member;
                    fault.rank = location;
                    if (reason != NULL)
                        snprintf(fault.detail, sizeof(fault.detail), "%s",
                                 reason);
                }
                status = dpu_fault_report_append(report, &fault);
                if (status != 0)
                    return status;
            }
        }
    }
    /* Other scans of the same set may already have counted these DPUs. */
    if (report->nr_dpus_checked < nr_positions)
        report->nr_dpus_checked = nr_positions;
    return status;
}

/* ---- Grouping and printing ------------------------------------------- */

static int
rank_known(const dpu_fault_t *fault)
{
    return fault->rank.sysfs_index != UINT32_MAX;
}

static int
compare_int(long long lhs, long long rhs)
{
    return (lhs > rhs) - (lhs < rhs);
}

/* Orders by rank (physical first), then chip, then DPU. */
static int
compare_rank(const dpu_fault_t *lhs, const dpu_fault_t *rhs)
{
    int result = compare_int(!rank_known(lhs), !rank_known(rhs));

    if (result != 0)
        return result;
    if (rank_known(lhs))
        return compare_int(lhs->rank.sysfs_index, rhs->rank.sysfs_index);
    return compare_int(lhs->sdk_rank_id, rhs->sdk_rank_id);
}

static int
compare_dpu(const dpu_fault_t *lhs, const dpu_fault_t *rhs)
{
    int result = compare_rank(lhs, rhs);

    if (result == 0)
        result = compare_int(lhs->slice_id, rhs->slice_id);
    if (result == 0)
        result = compare_int(lhs->member_id, rhs->member_id);
    return result;
}

static int
compare_faults(const void *left, const void *right)
{
    const dpu_fault_t *lhs = left;
    const dpu_fault_t *rhs = right;
    int result = compare_dpu(lhs, rhs);

    return result != 0 ? result : compare_int(lhs->kind, rhs->kind);
}

static dpu_fault_t *
sorted_copy(const dpu_fault_report_t *report)
{
    dpu_fault_t *faults;

    if (report->nr_faults == 0)
        return NULL;
    faults = malloc(report->nr_faults * sizeof(*faults));
    if (faults == NULL)
        return NULL;
    memcpy(faults, report->faults, report->nr_faults * sizeof(*faults));
    qsort(faults, report->nr_faults, sizeof(*faults), compare_faults);
    return faults;
}

static size_t
count_distinct(const dpu_fault_t *faults, size_t count,
               int (*compare)(const dpu_fault_t *, const dpu_fault_t *))
{
    size_t distinct = 0;
    size_t index;

    for (index = 0; index < count; ++index)
        distinct += index == 0 || compare(&faults[index - 1], &faults[index]);
    return distinct;
}

size_t
dpu_fault_report_nr_dpus(const dpu_fault_report_t *report)
{
    dpu_fault_t *faults;
    size_t count;

    if (report == NULL || report->nr_faults == 0)
        return 0;
    faults = sorted_copy(report);
    if (faults == NULL)
        return report->nr_faults;
    count = count_distinct(faults, report->nr_faults, compare_dpu);
    free(faults);
    return count;
}

static size_t
count_dimms(const dpu_fault_t *faults, size_t count)
{
    size_t dimms = 0;
    size_t index;

    for (index = 0; index < count; ++index) {
        size_t previous;

        if (faults[index].rank.dimm_serial[0] == '\0')
            continue;
        for (previous = 0; previous < index; ++previous)
            if (strcmp(faults[previous].rank.dimm_serial,
                       faults[index].rank.dimm_serial)
                == 0)
                break;
        dimms += previous == index;
    }
    return dimms;
}

/* "26.5.3" when allocated, "dpu_rank13:5.3" for a sysfs-only scan. */
static void
format_dpu_id(char *output, size_t capacity, const dpu_fault_t *fault)
{
    if (fault->sdk_rank_id >= 0)
        snprintf(output, capacity, "%d.%d.%d", fault->sdk_rank_id,
                 fault->slice_id, fault->member_id);
    else
        snprintf(output, capacity, "dpu_rank%u:%d.%d",
                 fault->rank.sysfs_index, fault->slice_id, fault->member_id);
}

static const char *
plural(size_t count)
{
    return count == 1 ? "" : "s";
}

static void
print_rank_header(FILE *stream, const dpu_fault_t *first, size_t nr_dpus)
{
    const dpu_rank_topology_t *rank = &first->rank;

    if (rank_known(first)) {
        fprintf(stream, "  %s", rank->rank_device);
        if (first->sdk_rank_id >= 0
            && (uint32_t)first->sdk_rank_id != rank->sysfs_index)
            fprintf(stream, " (SDK rank %d)", first->sdk_rank_id);
        fprintf(stream, ": DIMM %s",
                rank->dimm_serial[0] != '\0' ? rank->dimm_serial : "unknown");
        if (rank->dimm_rank_index >= 0 && rank->dimm_rank_count > 0)
            fprintf(stream, " rank %d/%d", rank->dimm_rank_index + 1,
                    rank->dimm_rank_count);
        fprintf(stream, ", socket %d, NUMA %d, channel %d", rank->socket_id,
                rank->numa_node, rank->channel_id);
    } else {
        fprintf(stream, "  SDK rank %d (physical device unknown): socket %d, "
                        "NUMA %d",
                first->sdk_rank_id, rank->socket_id, rank->numa_node);
    }
    fprintf(stream, " -- %zu faulty DPU%s\n", nr_dpus, plural(nr_dpus));
}

static void
print_dpu_line(FILE *stream, const dpu_fault_t *faults, size_t count)
{
    char id[48];
    char label[80];
    size_t index;
    uint32_t set_index = DPU_FAULT_NO_INDEX;

    for (index = 0; index < count; ++index)
        if (faults[index].set_index != DPU_FAULT_NO_INDEX)
            set_index = faults[index].set_index;

    format_dpu_id(id, sizeof(id), &faults[0]);
    if (set_index != DPU_FAULT_NO_INDEX)
        snprintf(label, sizeof(label), "DPU %u (%s)", set_index, id);
    else
        snprintf(label, sizeof(label), "DPU %s", id);
    fprintf(stream, "    %-24s chip %d dpu %d: ", label, faults[0].slice_id,
            faults[0].member_id);

    for (index = 0; index < count; ++index) {
        if (index != 0)
            fputs("; ", stream);
        fputs(dpu_fault_kind_name(faults[index].kind), stream);
        if (faults[index].detail[0] != '\0')
            fprintf(stream, " (%s)", faults[index].detail);
    }
    fputc('\n', stream);
}

void
dpu_fault_report_fprint(FILE *stream, const dpu_fault_report_t *report)
{
    dpu_fault_t *faults;
    size_t nr_dpus;
    size_t nr_ranks;
    size_t index;

    if (stream == NULL || report == NULL)
        return;
    if (report->nr_faults == 0) {
        fprintf(stream, "DPU faults: none (%u DPUs checked)\n",
                report->nr_dpus_checked);
        return;
    }
    faults = sorted_copy(report);
    if (faults == NULL) {
        fprintf(stream, "DPU faults: %zu (out of memory while printing)\n",
                report->nr_faults);
        return;
    }

    nr_dpus = count_distinct(faults, report->nr_faults, compare_dpu);
    nr_ranks = count_distinct(faults, report->nr_faults, compare_rank);
    fprintf(stream, "DPU faults: %zu faulty DPU%s on %zu rank%s",
            nr_dpus, plural(nr_dpus), nr_ranks, plural(nr_ranks));
    if (count_dimms(faults, report->nr_faults) != 0)
        fprintf(stream, ", %zu DIMM%s",
                count_dimms(faults, report->nr_faults),
                plural(count_dimms(faults, report->nr_faults)));
    if (report->nr_dpus_checked != 0)
        fprintf(stream, " (%u DPUs checked)", report->nr_dpus_checked);
    fputc('\n', stream);

    for (index = 0; index < report->nr_faults;) {
        size_t rank_end = index;
        size_t dpus_in_rank;
        size_t dpu;

        while (rank_end < report->nr_faults
               && compare_rank(&faults[index], &faults[rank_end]) == 0)
            ++rank_end;
        dpus_in_rank =
            count_distinct(faults + index, rank_end - index, compare_dpu);
        print_rank_header(stream, &faults[index], dpus_in_rank);

        for (dpu = index; dpu < rank_end;) {
            size_t dpu_end = dpu;
            while (dpu_end < rank_end
                   && compare_dpu(&faults[dpu], &faults[dpu_end]) == 0)
                ++dpu_end;
            print_dpu_line(stream, faults + dpu, dpu_end - dpu);
            dpu = dpu_end;
        }
        index = rank_end;
    }

    fputs("  Faulty DPU list:", stream);
    for (index = 0; index < report->nr_faults; ++index) {
        char id[48];
        if (index != 0 && compare_dpu(&faults[index - 1], &faults[index]) == 0)
            continue;
        format_dpu_id(id, sizeof(id), &faults[index]);
        fprintf(stream, " %s", id);
    }
    fputc('\n', stream);
    if (report->nr_dpus_unverified != 0)
        fprintf(stream,
                "  %u DPU%s on ranks with a hung DPU were only checked for "
                "MRAM faults\n",
                report->nr_dpus_unverified,
                plural(report->nr_dpus_unverified));
    free(faults);
}
