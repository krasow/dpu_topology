#include "dpu_topology.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
usage(const char *program)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s --system [--group-by-dimm]\n"
            "  %s <nr_dpus> [allocation_profile] [--group-by-dimm]\n",
            program, program);
}

int
main(int argc, char **argv)
{
    dpu_set_topology_t topology;
    struct dpu_set_t set;
    const char *profile = NULL;
    const char *dpu_count = NULL;
    char *end;
    unsigned long nr_dpus;
    dpu_error_t error;
    int group_by_dimm = 0;
    int system = 0;
    int index;
    int status;

    for (index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--system") == 0)
            system = 1;
        else if (strcmp(argv[index], "--group-by-dimm") == 0)
            group_by_dimm = 1;
        else if (dpu_count == NULL)
            dpu_count = argv[index];
        else if (profile == NULL)
            profile = argv[index];
        else {
            usage(argv[0]);
            return EXIT_FAILURE;
        }
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

    if (dpu_count == NULL) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    errno = 0;
    nr_dpus = strtoul(dpu_count, &end, 10);
    if (errno != 0 || end == dpu_count || *end != '\0' || nr_dpus == 0
        || nr_dpus > UINT32_MAX) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (profile == NULL)
        profile = getenv("UPMEM_PROFILE");
    if (profile == NULL || *profile == '\0')
        profile = "backend=hw";

    error = dpu_alloc((uint32_t)nr_dpus, profile, &set);
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
