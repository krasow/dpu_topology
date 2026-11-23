#define _POSIX_C_SOURCE 200809L

#include "internal.h"
#include "health.h"

#include <dpu_management.h>
#include <dpu_runner.h>

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef DPU_TOPOLOGY_HEALTH_KERNEL
#error "DPU_TOPOLOGY_HEALTH_KERNEL must name the compiled health kernel"
#endif

DPU_INCBIN(dpu_topology_health_kernel, DPU_TOPOLOGY_HEALTH_KERNEL)

#define DEFAULT_MRAM_BYTES (4u << 20)
#define DEFAULT_TIMEOUT_MS 10000u
#define MAX_MRAM_BYTES (60u << 20)
#define STRIDE_BYTES (HEALTH_BLOCK_BYTES * HEALTH_NR_TASKLETS)
/* Host memory for one chunk of every DPU, per direction. */
#define CHUNK_BUDGET_BYTES (256u << 20)
#define MAX_THREADS 64

typedef struct {
    struct dpu_set_t dpu;
    uint32_t set_index;
    uint64_t seed;
    size_t memory_wrong;
    size_t memory_first;
    size_t output_wrong;
    size_t output_first;
    uint64_t expected_checksums[HEALTH_NR_TASKLETS];
    uint64_t checksums[HEALTH_NR_TASKLETS];
    int crashed;
} dpu_state_t;

typedef struct {
    dpu_state_t *dpus;
    uint32_t nr_dpus;
    size_t nr_words;    /* per DPU */
    size_t chunk_words; /* per DPU per transfer */
    uint64_t *input;    /* nr_dpus * chunk_words */
    uint64_t *output;   /* nr_dpus * chunk_words */
    /* Current chunk, read by the worker callbacks. */
    size_t offset;
    size_t length;
} health_state_t;

typedef void (*dpu_work_fn)(health_state_t *state, uint32_t dpu);

typedef struct {
    health_state_t *state;
    dpu_work_fn work;
    uint32_t begin;
    uint32_t end;
} worker_t;

static void *
run_worker(void *argument)
{
    worker_t *worker = argument;
    uint32_t dpu;

    for (dpu = worker->begin; dpu < worker->end; ++dpu)
        worker->work(worker->state, dpu);
    return NULL;
}

/* Runs work(state, dpu) for every DPU, split across CPU cores. */
static void
for_each_dpu(health_state_t *state, dpu_work_fn work)
{
    pthread_t threads[MAX_THREADS];
    worker_t workers[MAX_THREADS];
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    uint32_t nr_threads = cpus < 1 ? 1 : (uint32_t)cpus;
    uint32_t started = 0;
    uint32_t index;

    if (nr_threads > MAX_THREADS)
        nr_threads = MAX_THREADS;
    if (nr_threads > state->nr_dpus)
        nr_threads = state->nr_dpus;

    for (index = 0; index < nr_threads; ++index) {
        workers[index].state = state;
        workers[index].work = work;
        workers[index].begin =
            (uint32_t)((uint64_t)state->nr_dpus * index / nr_threads);
        workers[index].end =
            (uint32_t)((uint64_t)state->nr_dpus * (index + 1) / nr_threads);
        if (index != 0
            && pthread_create(&threads[index], NULL, run_worker,
                              &workers[index])
                   == 0)
            ++started;
        else if (index != 0)
            run_worker(&workers[index]); /* No thread: do it inline. */
    }
    run_worker(&workers[0]);
    for (index = 1; index <= started; ++index)
        pthread_join(threads[index], NULL);
}

static uint64_t *
chunk_of(const health_state_t *state, uint64_t *buffer, uint32_t dpu)
{
    return buffer + (size_t)dpu * state->chunk_words;
}

static void
generate_input(health_state_t *state, uint32_t dpu)
{
    uint64_t *input = chunk_of(state, state->input, dpu);
    uint64_t seed = state->dpus[dpu].seed;
    size_t word;

    for (word = 0; word < state->length; ++word)
        input[word] = health_input(seed, state->offset + word);
}

static void
compare_readback(health_state_t *state, uint32_t dpu)
{
    const uint64_t *input = chunk_of(state, state->input, dpu);
    const uint64_t *output = chunk_of(state, state->output, dpu);
    dpu_state_t *info = &state->dpus[dpu];
    size_t word;

    for (word = 0; word < state->length; ++word) {
        if (input[word] == output[word])
            continue;
        if (info->memory_wrong++ == 0)
            info->memory_first = state->offset + word;
    }
}

/* Replays the kernel for this chunk; tasklets own interleaved blocks. */
static void
verify_output(health_state_t *state, uint32_t dpu)
{
    const uint64_t *output = chunk_of(state, state->output, dpu);
    dpu_state_t *info = &state->dpus[dpu];
    size_t word;

    for (word = 0; word < state->length; ++word) {
        size_t position = state->offset + word;
        unsigned tasklet =
            (unsigned)(position / HEALTH_BLOCK_WORDS % HEALTH_NR_TASKLETS);
        uint64_t expected = health_transform(
            health_input(info->seed, position), info->seed, position);

        info->expected_checksums[tasklet] = health_checksum_step(
            info->expected_checksums[tasklet], expected);
        if (output[word] == expected)
            continue;
        if (info->output_wrong++ == 0)
            info->output_first = position;
    }
}

static int
transfer_chunk(health_state_t *state, dpu_xfer_t direction,
               struct dpu_set_t set, uint64_t *buffer)
{
    struct dpu_set_t dpu;
    uint32_t index;

    DPU_FOREACH(set, dpu, index)
    {
        if (dpu_prepare_xfer(dpu, chunk_of(state, buffer, index)) != DPU_OK)
            return -EIO;
    }
    return dpu_push_xfer(set, direction, DPU_MRAM_HEAP_POINTER_NAME,
                         state->offset * sizeof(uint64_t),
                         state->length * sizeof(uint64_t), DPU_XFER_DEFAULT)
                   == DPU_OK
               ? 0
               : -EIO;
}

/* Calls step() once per chunk after moving the chunk window. */
static int
for_each_chunk(health_state_t *state,
               int (*step)(health_state_t *, struct dpu_set_t),
               struct dpu_set_t set)
{
    int status = 0;

    for (state->offset = 0; state->offset < state->nr_words && status == 0;
         state->offset += state->chunk_words) {
        state->length = state->nr_words - state->offset;
        if (state->length > state->chunk_words)
            state->length = state->chunk_words;
        status = step(state, set);
    }
    return status;
}

static int
memory_step(health_state_t *state, struct dpu_set_t set)
{
    int status;

    for_each_dpu(state, generate_input);
    if ((status = transfer_chunk(state, DPU_XFER_TO_DPU, set, state->input))
            != 0
        || (status = transfer_chunk(state, DPU_XFER_FROM_DPU, set,
                                    state->output))
               != 0)
        return status;
    for_each_dpu(state, compare_readback);
    return 0;
}

static int
verify_step(health_state_t *state, struct dpu_set_t set)
{
    int status = transfer_chunk(state, DPU_XFER_FROM_DPU, set, state->output);

    if (status == 0)
        for_each_dpu(state, verify_output);
    return status;
}

static uint64_t
elapsed_ms(const struct timespec *start)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)(now.tv_sec - start->tv_sec) * 1000u
           + (uint64_t)((now.tv_nsec - start->tv_nsec) / 1000000);
}

static int
add_fault(dpu_fault_report_t *report, const dpu_state_t *info,
          dpu_fault_kind_t kind, const char *detail)
{
    struct dpu_t *dpu = dpu_from_set(info->dpu);
    dpu_rank_topology_t location;
    dpu_fault_t fault;

    dpu_fault_locate_rank(dpu_get_rank(dpu), &location);
    dpu_fault_init_for_dpu(&fault, dpu, &location, kind, info->set_index,
                           detail);
    return dpu_fault_report_append(report, &fault);
}

/* Launches asynchronously so a hung DPU is reported instead of blocking. */
static int
run_kernel(struct dpu_set_t set, health_state_t *state, uint32_t timeout_ms,
           dpu_fault_report_t *report)
{
    const struct timespec pause = {0, 1000000};
    struct timespec start;
    int all_done = 0;
    uint32_t index;
    int status = 0;

    if (dpu_launch(set, DPU_ASYNCHRONOUS) != DPU_OK)
        return -EIO;

    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        struct dpu_set_t rank_set;

        all_done = 1;
        DPU_RANK_FOREACH(set, rank_set)
        {
            bool done = false;
            bool fault = false;

            if (dpu_status_rank(dpu_rank_from_set(rank_set), &done, &fault)
                != DPU_OK)
                return -EIO;
            all_done = all_done && done;
        }
        if (all_done || elapsed_ms(&start) > timeout_ms)
            break;
        nanosleep(&pause, NULL);
    }
    if (all_done)
        (void)dpu_sync(set); /* Faults are collected per DPU below. */

    for (index = 0; index < state->nr_dpus && status == 0; ++index) {
        dpu_state_t *info = &state->dpus[index];
        bool done = false;
        bool fault = false;

        if (dpu_status_dpu(dpu_from_set(info->dpu), &done, &fault) != DPU_OK)
            continue;
        if (fault) {
            info->crashed = 1;
            status = add_fault(report, info, DPU_FAULT_EXEC_FAULT, NULL);
        } else if (!done) {
            char detail[DPU_FAULT_DETAIL_LEN];

            info->crashed = 1;
            snprintf(detail, sizeof(detail), "still running after %u ms",
                     timeout_ms);
            status = add_fault(report, info, DPU_FAULT_TIMEOUT, detail);
        }
    }
    if (status != 0)
        return status;
    return all_done ? 0 : -ETIMEDOUT;
}

static int
read_checksums(struct dpu_set_t set, health_state_t *state)
{
    struct dpu_set_t dpu;
    uint32_t index;

    DPU_FOREACH(set, dpu, index)
    {
        if (dpu_prepare_xfer(dpu, state->dpus[index].checksums) != DPU_OK)
            return -EIO;
    }
    return dpu_push_xfer(set, DPU_XFER_FROM_DPU, "health_checksums", 0,
                         sizeof(state->dpus[0].checksums), DPU_XFER_DEFAULT)
                   == DPU_OK
               ? 0
               : -EIO;
}

/* verified is 0 when the kernel never completed; only MRAM faults count. */
static int
report_results(const health_state_t *state, int verified,
               dpu_fault_report_t *report)
{
    uint32_t index;

    for (index = 0; index < state->nr_dpus; ++index) {
        const dpu_state_t *info = &state->dpus[index];
        char detail[DPU_FAULT_DETAIL_LEN];
        unsigned bad_tasklets = 0;
        unsigned tasklet;
        int status;

        if (info->memory_wrong != 0) {
            snprintf(detail, sizeof(detail),
                     "%zu/%zu words differ, first at heap+0x%zx",
                     info->memory_wrong, state->nr_words,
                     info->memory_first * sizeof(uint64_t));
            if ((status = add_fault(report, info, DPU_FAULT_MEMORY, detail))
                != 0)
                return status;
        }
        if (!verified || info->crashed)
            continue; /* No trustworthy output; any crash is reported. */

        for (tasklet = 0; tasklet < HEALTH_NR_TASKLETS; ++tasklet)
            if (info->checksums[tasklet]
                != info->expected_checksums[tasklet])
                bad_tasklets |= 1u << tasklet;
        if (info->output_wrong == 0 && bad_tasklets == 0)
            continue;

        if (info->output_wrong != 0)
            snprintf(detail, sizeof(detail),
                     "%zu/%zu words wrong, first at heap+0x%zx; "
                     "tasklet mask 0x%04x",
                     info->output_wrong, state->nr_words,
                     info->output_first * sizeof(uint64_t), bad_tasklets);
        else
            snprintf(detail, sizeof(detail),
                     "checksum mismatch, tasklet mask 0x%04x", bad_tasklets);
        if ((status = add_fault(report, info, DPU_FAULT_WRONG_RESULT, detail))
            != 0)
            return status;
    }
    return 0;
}

static int
setup(struct dpu_set_t set, uint32_t mram_bytes, health_state_t *state)
{
    struct dpu_set_t dpu;
    uint32_t nr_blocks;
    uint32_t index;
    size_t chunk_bytes;

    if (dpu_get_nr_dpus(set, &state->nr_dpus) != DPU_OK
        || state->nr_dpus == 0)
        return -EIO;

    nr_blocks = (mram_bytes + STRIDE_BYTES - 1) / STRIDE_BYTES
                * HEALTH_NR_TASKLETS;
    state->nr_words = (size_t)nr_blocks * HEALTH_BLOCK_WORDS;

    chunk_bytes = CHUNK_BUDGET_BYTES / state->nr_dpus / HEALTH_BLOCK_BYTES
                  * HEALTH_BLOCK_BYTES;
    if (chunk_bytes < HEALTH_BLOCK_BYTES)
        chunk_bytes = HEALTH_BLOCK_BYTES;
    state->chunk_words = chunk_bytes / sizeof(uint64_t);
    if (state->chunk_words > state->nr_words)
        state->chunk_words = state->nr_words;

    state->dpus = calloc(state->nr_dpus, sizeof(*state->dpus));
    state->input = malloc((size_t)state->nr_dpus * state->chunk_words
                          * sizeof(uint64_t));
    state->output = malloc((size_t)state->nr_dpus * state->chunk_words
                           * sizeof(uint64_t));
    if (state->dpus == NULL || state->input == NULL || state->output == NULL)
        return -ENOMEM;

    DPU_FOREACH(set, dpu, index)
    {
        dpu_state_t *info = &state->dpus[index];
        unsigned tasklet;

        info->dpu = dpu;
        info->set_index = index;
        info->seed = health_input(0x5EEDF00DULL, index);
        for (tasklet = 0; tasklet < HEALTH_NR_TASKLETS; ++tasklet)
            info->expected_checksums[tasklet] = health_checksum_init(tasklet);
    }

    if (dpu_load_from_incbin(set, &dpu_topology_health_kernel, NULL)
        != DPU_OK)
        return -EIO;
    DPU_FOREACH(set, dpu, index)
    {
        if (dpu_prepare_xfer(dpu, &state->dpus[index].seed) != DPU_OK)
            return -EIO;
    }
    if (dpu_push_xfer(set, DPU_XFER_TO_DPU, "health_seed", 0,
                      sizeof(uint64_t), DPU_XFER_DEFAULT)
            != DPU_OK
        || dpu_broadcast_to(set, "health_nr_blocks", 0, &nr_blocks,
                            sizeof(nr_blocks), DPU_XFER_DEFAULT)
               != DPU_OK)
        return -EIO;
    return 0;
}

int
dpu_faults_health_check(struct dpu_set_t set,
                        const dpu_health_options_t *options,
                        dpu_fault_report_t *report)
{
    health_state_t state;
    uint32_t mram_bytes = DEFAULT_MRAM_BYTES;
    uint32_t timeout_ms = DEFAULT_TIMEOUT_MS;
    int status;

    if (report == NULL || set.kind != DPU_SET_RANKS)
        return -EINVAL;
    if (options != NULL && options->mram_bytes != 0)
        mram_bytes = options->mram_bytes;
    if (options != NULL && options->timeout_ms != 0)
        timeout_ms = options->timeout_ms;
    if (mram_bytes > MAX_MRAM_BYTES)
        return -EINVAL;

    memset(&state, 0, sizeof(state));
    status = setup(set, mram_bytes, &state);
    if (status == 0)
        status = for_each_chunk(&state, memory_step, set);
    if (status == 0)
        status = run_kernel(set, &state, timeout_ms, report);
    if (status == 0)
        status = read_checksums(set, &state);
    if (status == 0)
        status = for_each_chunk(&state, verify_step, set);
    if (status == 0)
        status = report_results(&state, 1, report);
    else if (status == -ETIMEDOUT) /* Still report pre-launch MRAM faults. */
        (void)report_results(&state, 0, report);

    if (report->nr_dpus_checked < state.nr_dpus)
        report->nr_dpus_checked = state.nr_dpus;
    free(state.dpus);
    free(state.input);
    free(state.output);
    return status;
}
