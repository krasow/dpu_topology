#include "dpu_topology.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Exit status when the requested scan found at least one faulty DPU. */
#define EXIT_FAULTS_FOUND 2

static void
usage(const char *program)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s --system [--group-by-dimm]\n"
            "  %s <nr_dpus> [allocation_profile] [--group-by-dimm]\n"
            "  %s --faults\n"
            "  %s --check [nr_dpus|all] [allocation_profile]\n"
            "      [--mram-bytes N] [--timeout-ms N]\n"
            "\n"
            "--faults lists DPUs the driver or DIMM VPD disables (no allocation).\n"
            "--check allocates DPUs (default: all), lists disabled ones, and runs\n"
            "an MRAM + compute health test on the rest. Both exit with status %d\n"
            "when faulty DPUs are found.\n",
            program, program, program, program, EXIT_FAULTS_FOUND);
}

static int
parse_u32(const char *text, uint32_t *value)
{
    char *end;
    unsigned long parsed;

    errno = 0;
    parsed = strtoul(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' || parsed > UINT32_MAX)
        return -1;
    *value = (uint32_t)parsed;
    return 0;
}

static const char *
default_profile(const char *profile)
{
    if (profile == NULL)
        profile = getenv("UPMEM_PROFILE");
    return profile == NULL || *profile == '\0' ? "backend=hw" : profile;
}

static int
run_static_scan(void)
{
    dpu_fault_report_t report;
    int status;

    dpu_fault_report_init(&report);
    status = dpu_faults_scan_system(&report);
    if (status != 0) {
        fprintf(stderr, "Cannot scan DPU sysfs: %s\n", strerror(-status));
        dpu_fault_report_free(&report);
        return EXIT_FAILURE;
    }
    dpu_fault_report_fprint(stdout, &report);
    status = report.nr_faults != 0 ? EXIT_FAULTS_FOUND : EXIT_SUCCESS;
    dpu_fault_report_free(&report);
    return status;
}

static int
run_health_check(uint32_t nr_dpus, const char *profile,
                 const dpu_health_options_t *options)
{
    dpu_fault_report_t report;
    struct dpu_set_t set;
    dpu_error_t error;
    int status;

    error = dpu_alloc(nr_dpus, default_profile(profile), &set);
    if (error != DPU_OK) {
        fprintf(stderr, "dpu_alloc failed: %s\n", dpu_error_to_string(error));
        return EXIT_FAILURE;
    }
    dpu_get_nr_dpus(set, &nr_dpus);
    fprintf(stderr, "Checking %u DPUs (%u bytes of MRAM each)...\n", nr_dpus,
            options->mram_bytes != 0 ? options->mram_bytes : 4u << 20);

    dpu_fault_report_init(&report);
    status = dpu_faults_scan_set(set, &report);
    if (status == 0)
        status = dpu_faults_health_check(set, options, &report);

    if (status != 0 && status != -ETIMEDOUT) {
        fprintf(stderr, "Health check failed: %s\n", strerror(-status));
        dpu_fault_report_fprint(stdout, &report);
        dpu_fault_report_free(&report);
        dpu_free(set);
        return EXIT_FAILURE;
    }
    dpu_fault_report_fprint(stdout, &report);
    fflush(stdout);

    if (status == -ETIMEDOUT) {
        /* dpu_free() would wait forever on the hung DPUs. */
        fprintf(stderr, "Some DPUs hung; exiting without freeing them.\n");
        _exit(EXIT_FAULTS_FOUND);
    }
    status = report.nr_faults != 0 ? EXIT_FAULTS_FOUND : EXIT_SUCCESS;
    dpu_fault_report_free(&report);
    dpu_free(set);
    return status;
}

int
main(int argc, char **argv)
{
    dpu_set_topology_t topology;
    dpu_health_options_t health_options = {0, 0};
    struct dpu_set_t set;
    const char *profile = NULL;
    const char *dpu_count = NULL;
    uint32_t nr_dpus;
    dpu_error_t error;
    int group_by_dimm = 0;
    int system = 0;
    int faults = 0;
    int check = 0;
    int index;
    int status;

    for (index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--system") == 0)
            system = 1;
        else if (strcmp(argv[index], "--group-by-dimm") == 0)
            group_by_dimm = 1;
        else if (strcmp(argv[index], "--faults") == 0)
            faults = 1;
        else if (strcmp(argv[index], "--check") == 0)
            check = 1;
        else if (strcmp(argv[index], "--mram-bytes") == 0
                 && index + 1 < argc) {
            if (parse_u32(argv[++index], &health_options.mram_bytes) != 0) {
                usage(argv[0]);
                return EXIT_FAILURE;
            }
        } else if (strcmp(argv[index], "--timeout-ms") == 0
                   && index + 1 < argc) {
            if (parse_u32(argv[++index], &health_options.timeout_ms) != 0) {
                usage(argv[0]);
                return EXIT_FAILURE;
            }
        } else if (argv[index][0] == '-') {
            usage(argv[0]);
            return EXIT_FAILURE;
        } else if (dpu_count == NULL)
            dpu_count = argv[index];
        else if (profile == NULL)
            profile = argv[index];
        else {
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (system + faults + check > 1) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (faults) {
        if (dpu_count != NULL) {
            usage(argv[0]);
            return EXIT_FAILURE;
        }
        return run_static_scan();
    }

    if (check) {
        nr_dpus = DPU_ALLOCATE_ALL;
        if (dpu_count != NULL && strcmp(dpu_count, "all") != 0
            && (parse_u32(dpu_count, &nr_dpus) != 0 || nr_dpus == 0)) {
            usage(argv[0]);
            return EXIT_FAILURE;
        }
        return run_health_check(nr_dpus, profile, &health_options);
    }

    if (system) {
        if (dpu_count != NULL) {
            usage(argv[0]);
            return EXIT_FAILURE;
        }
        status = group_by_dimm
                     ? dpu_topology_fprint_system_grouped(stdout)
                     : dpu_topology_fprint_system(stdout);
        if (status != 0) {
            fprintf(stderr, "Cannot inspect DPU sysfs topology: %s\n",
                    strerror(-status));
            return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
    }

    if (dpu_count == NULL || parse_u32(dpu_count, &nr_dpus) != 0
        || nr_dpus == 0) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    error = dpu_alloc(nr_dpus, default_profile(profile), &set);
    if (error != DPU_OK) {
        fprintf(stderr, "dpu_alloc failed: %s\n",
                dpu_error_to_string(error));
        return EXIT_FAILURE;
    }

    status = dpu_topology_inspect(set, &topology);
    if (status != 0) {
        fprintf(stderr, "Cannot inspect allocated DPUs: %s\n",
                strerror(-status));
        dpu_free(set);
        return EXIT_FAILURE;
    }

    if (group_by_dimm)
        dpu_topology_fprint_grouped(stdout, &topology);
    else
        dpu_topology_fprint(stdout, &topology);
    dpu_topology_free(&topology);
    dpu_free(set);
    return EXIT_SUCCESS;
}
