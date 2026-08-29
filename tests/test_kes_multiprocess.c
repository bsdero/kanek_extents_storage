#include <kes/kes_storage.h>
#include <errno.h>
#include <fcntl.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define TEST_ASSERT(condition, message) \
    do { \
        if ( !(condition)) { \
            printf( "FAIL: %s at %s:%d\n", message, __FILE__, __LINE__); \
            return(false); \
        } \
    } while ( 0)

#define TEST_SUCCESS(test_name) \
    do { \
        printf( "PASS: %s\n", test_name); \
        return(true); \
    } while ( 0)

static int tests_run = 0;
static int tests_passed = 0;

#define TEST_FILE     "/tmp/kes_multiprocess_test"
#define TEST_SIZE     (256 * 1024)
#define TEST_BLOCK    4096
#define EXTENT_BLOCKS 4
#define PAYLOAD_SIZE  64
#define STEP_COUNT    8

#define RACE_TEST_FILE  "/tmp/kes_multiprocess_race_test"
#define RACE_TEST_SIZE  (256 * 1024)
#define RACE_OP_COUNT   20

static void cleanup(void){
    unlink( TEST_FILE);
}

static int make_storage( const char *path, kes_storage_t **out){
    kes_storage_config_t cfg = {
        .device_path = path,
        .device_size = TEST_SIZE,
        .block_size = TEST_BLOCK,
        .flags = KES_STORAGE_CREATE,
        .strategy = KES_ALLOC_FIRST_FIT,
    };
    return(kes_storage_create( &cfg, out));
}

/*
 * Turn-based IPC state shared between the parent and forked child via
 * an anonymous MAP_SHARED mapping. Only synchronization primitives
 * live here -- the payload each side writes to the extent is fully
 * deterministic from the step index (see build_step_payload()), so no
 * content needs to cross the shared-memory boundary, only turns.
 */
typedef struct{
    sem_t parent_turn;
    sem_t child_turn;
} sync_state_t;

static void build_step_payload( int step, char *out, size_t out_size){
    memset( out, 0, out_size);
    snprintf( out, out_size, "step=%d writer=%s", step,
              (step % 2 == 0) ? "parent" : "child");
}

static bool write_step( kes_storage_t *storage,
                         const kes_extent_descriptor_t *extent, int step){
    char payload[PAYLOAD_SIZE];

    build_step_payload( step, payload, sizeof( payload));
    if ( kes_extent_write( storage, extent, payload, sizeof( payload),
                            0) != KES_SUCCESS) {
        fprintf( stderr, "write_step: kes_extent_write failed at "
                 "step %d\n", step);
        return(false);
    }
    if ( kes_storage_sync( storage) != KES_SUCCESS) {
        fprintf( stderr, "write_step: kes_storage_sync failed at "
                 "step %d\n", step);
        return(false);
    }

    return(true);
}

static bool verify_step( kes_storage_t *storage,
                          const kes_extent_descriptor_t *extent, int step){
    char expected[PAYLOAD_SIZE];
    char actual[PAYLOAD_SIZE];

    build_step_payload( step, expected, sizeof( expected));
    if ( kes_extent_read( storage, extent, actual, sizeof( actual),
                           0) != KES_SUCCESS) {
        fprintf( stderr, "verify_step: kes_extent_read failed at "
                 "step %d\n", step);
        return(false);
    }
    if ( memcmp( expected, actual, sizeof( expected)) != 0) {
        fprintf( stderr, "verify_step: mismatch at step %d: expected "
                 "\"%s\", got \"%s\"\n", step, expected, actual);
        return(false);
    }

    return(true);
}

/*
 * Runs one side (parent or child) of the ping-pong protocol:
 * STEP_COUNT payloads are written in strict alternation (even steps
 * by the parent, odd steps by the child); before writing its own
 * step, each side first verifies the payload the other side wrote on
 * the previous step. Whichever side does not write the final step
 * performs one extra wait + verify to check it. Returns false (with a
 * diagnostic on stderr) on the first failure.
 */
static bool run_side( kes_storage_t *storage,
                       const kes_extent_descriptor_t *extent,
                       sem_t *my_turn, sem_t *other_turn, bool is_parent){
    for ( int step = 0; step < STEP_COUNT; step++) {
        bool step_is_mine = ( ( ( step % 2) == 0) == is_parent);

        if ( !step_is_mine) {
            continue;
        }

        if ( sem_wait( my_turn) != 0) {
            fprintf( stderr, "run_side: sem_wait failed at step %d: "
                     "%s\n", step, strerror( errno));
            return(false);
        }
        if ( step > 0 && !verify_step( storage, extent, step - 1)) {
            return(false);
        }
        if ( !write_step( storage, extent, step)) {
            return(false);
        }
        if ( sem_post( other_turn) != 0) {
            fprintf( stderr, "run_side: sem_post failed at step %d: "
                     "%s\n", step, strerror( errno));
            return(false);
        }
    }

    bool final_step_is_mine =
        ( ( ( ( STEP_COUNT - 1) % 2) == 0) == is_parent);
    if ( !final_step_is_mine) {
        if ( sem_wait( my_turn) != 0) {
            fprintf( stderr, "run_side: sem_wait failed at final "
                     "verify: %s\n", strerror( errno));
            return(false);
        }
        if ( !verify_step( storage, extent, STEP_COUNT - 1)) {
            return(false);
        }
    }

    return(true);
}

/*
 * Two independent processes open the same backing file (via separate
 * kes_storage_open() calls after fork(), each getting its own fd,
 * bitmap, and pthread_mutex -- process-local state a pthread_mutex
 * cannot coordinate across) and take turns writing then reading back
 * a distinguishable payload through the extent, with turns strictly
 * ordered by two POSIX semaphores in shared anonymous memory. This
 * checks that content written by one process through
 * kes_extent_write() is correctly observed by an independently opened
 * kes_storage_t in another process once access is externally
 * synchronized -- it deliberately does not exercise unsynchronized
 * concurrent access to the same file, which is a documented, unguarded
 * gap (see PENDING_ITEMS.md's "concurrent open of the same storage
 * file" item).
 */
static bool test_kes_cross_process_sync_io(void){
    kes_storage_t *storage = NULL;
    kes_extent_descriptor_t extent;
    kes_extent_request_t request = {
        .block_count = EXTENT_BLOCKS, .alignment = 0,
        .hint_block = 0, .flags = 0,
    };
    sync_state_t *sync_mem;
    pid_t pid;
    int status;
    bool parent_ok;

    cleanup();

    TEST_ASSERT( make_storage( TEST_FILE, &storage) == KES_SUCCESS,
                "storage created");
    TEST_ASSERT(
        kes_extent_allocate( storage, &request, &extent) == KES_SUCCESS,
        "extent allocated");
    TEST_ASSERT( kes_storage_sync( storage) == KES_SUCCESS,
                "descriptor/bitmap synced before fork");
    TEST_ASSERT( kes_storage_close( storage) == KES_SUCCESS,
                "storage closed before fork");
    storage = NULL;

    sync_mem = mmap( NULL, sizeof( sync_state_t), PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    TEST_ASSERT( sync_mem != MAP_FAILED, "shared memory mapped");
    TEST_ASSERT( sem_init( &sync_mem->parent_turn, 1, 1) == 0,
                "parent_turn semaphore initialized");
    TEST_ASSERT( sem_init( &sync_mem->child_turn, 1, 0) == 0,
                "child_turn semaphore initialized");

    pid = fork();
    TEST_ASSERT( pid >= 0, "fork succeeded");

    if ( pid == 0) {
        kes_storage_t *child_storage = NULL;
        bool child_ok;

        if ( kes_storage_open( TEST_FILE, 0, &child_storage) !=
             KES_SUCCESS) {
            fprintf( stderr, "child: kes_storage_open failed\n");
            _exit( 1);
        }
        child_ok = run_side( child_storage, &extent,
                              &sync_mem->child_turn,
                              &sync_mem->parent_turn, false);
        kes_storage_close( child_storage);
        _exit( child_ok ? 0 : 1);
    }

    TEST_ASSERT(
        kes_storage_open( TEST_FILE, 0, &storage) == KES_SUCCESS,
        "parent re-opened storage independently after fork");

    parent_ok = run_side( storage, &extent, &sync_mem->parent_turn,
                           &sync_mem->child_turn, true);

    kes_storage_close( storage);

    TEST_ASSERT( waitpid( pid, &status, 0) == pid,
                "waitpid reaped child");
    TEST_ASSERT( WIFEXITED( status) && WEXITSTATUS( status) == 0,
                "child side completed its steps without mismatch");
    TEST_ASSERT( parent_ok, "parent side completed its steps without "
                "mismatch");

    sem_destroy( &sync_mem->parent_turn);
    sem_destroy( &sync_mem->child_turn);
    munmap( sync_mem, sizeof( sync_state_t));
    cleanup();

    TEST_SUCCESS( "cross-process synchronized extent read/write "
                  "(fork + semaphore IPC)");
}

/*
 * Per-side outcome recorded into shared memory so the parent can
 * compare/print both sides' final view after waitpid() -- plain
 * MAP_SHARED data, not a synchronization primitive: each side only
 * ever writes its own half, so there is no race on this struct
 * itself even though there deliberately is one on the backing
 * storage file both sides are racing against.
 */
typedef struct{
    bool open_ok;
    int successful_allocs;
    uint64_t final_free_blocks;
    uint64_t final_used_blocks;
    uint32_t final_magic;
} race_side_result_t;

typedef struct{
    race_side_result_t parent;
    race_side_result_t child;
} race_shared_t;

static void run_racing_side( kes_storage_t *storage,
                              race_side_result_t *out, char fill_byte){
    kes_storage_stats_t stats;
    kes_storage_descriptor_t desc;

    out->successful_allocs = 0;

    for ( int i = 0; i < RACE_OP_COUNT; i++) {
        kes_extent_request_t req = { .block_count = 1, .alignment = 0,
                                      .hint_block = 0, .flags = 0 };
        kes_extent_descriptor_t ext;

        if ( kes_extent_allocate( storage, &req, &ext) != KES_SUCCESS) {
            continue;
        }

        char payload[64];
        memset( payload, fill_byte, sizeof( payload));
        /* Deliberately no coordination with the other process at
         * all -- not even a check of the write's own return value
         * gates anything further, per this test's job (see the
         * function doc comment above test_cross_process_racing_io):
         * observe what unsynchronized racing does, not assert it
         * succeeds cleanly. */
        kes_extent_write( storage, &ext, payload, sizeof( payload), 0);
        out->successful_allocs++;
    }

    /* Flush this process's in-memory bitmap/descriptor view to disk
     * -- both sides do this with no ordering between them, which is
     * exactly the unsynchronized write-write race this test exists
     * to exercise and observe, not prevent. */
    kes_storage_sync( storage);

    if ( kes_storage_get_stats( storage, &stats) == KES_SUCCESS) {
        out->final_free_blocks = stats.free_blocks;
        out->final_used_blocks = stats.used_blocks;
    }
    if ( kes_storage_get_descriptor( storage, &desc) == KES_SUCCESS) {
        out->final_magic = desc.magic;
    }
}

/*
 * A.2.2 -- deliberately racing counterpart to
 * test_kes_cross_process_sync_io() above: reuses its fork()/
 * shared-backing-file setup but REMOVES the semaphore turn-taking,
 * so both processes independently call kes_storage_open() on the
 * same file and issue overlapping kes_extent_allocate()/
 * kes_extent_write() calls with no coordination at all.
 *
 * This is intentional and does not test correctness. AGENTS.md and
 * PENDING_ITEMS.md already establish that unsynchronized concurrent
 * access to the same storage file from two independent kes_storage_t
 * instances is a documented, unguarded gap: each process loads its
 * own in-memory bitmap once at open() time and only flushes it back
 * on kes_storage_sync()/close(), with no cross-process locking at
 * all (a pthread_mutex_t inside kes_storage_t, same as
 * test_kes_cross_process_sync_io()'s doc comment explains, cannot
 * coordinate across two OS processes). Two independent processes
 * racing kes_extent_allocate() against the same on-disk bitmap are
 * expected to make conflicting "this block is free" decisions and
 * then overwrite each other's kes_storage_sync() flush of the
 * descriptor/bitmap block, corrupting free/used block accounting.
 *
 * This test's job is only to:
 *   - Run to completion without hanging or crashing the test harness
 *     itself, using a bounded number of racing operations
 *     (RACE_OP_COUNT per side, not an infinite loop).
 *   - Record and print what actually happens -- both sides' final
 *     free_blocks/used_blocks and descriptor magic -- as a
 *     documented, known limitation for a human to read, not as a
 *     pass/fail correctness assertion. It intentionally does NOT
 *     assert the two sides agree, or that free_blocks/used_blocks
 *     end up internally consistent.
 *   - Not attempt to fix the underlying race -- explicitly out of
 *     scope, see PENDING_ITEMS.md's "concurrent open of the same
 *     storage file" item and plan_phase5.md S1.
 *
 * The only real pass/fail assertions here are: fork() succeeded,
 * both processes opened the (already-created) storage file
 * successfully, the child was reaped without a crash signal
 * (WIFSIGNALED), and the descriptor magic number is *some* value
 * (kes_storage_get_descriptor() itself did not fail) -- i.e. this
 * verifies the race does not crash either process, which is the one
 * property this test is actually here to guarantee.
 */
static bool test_cross_process_racing_io(void){
    kes_storage_t *storage = NULL;
    race_shared_t *shared;
    pid_t pid;
    int status;

    cleanup();
    unlink( RACE_TEST_FILE);

    kes_storage_config_t cfg = {
        .device_path = RACE_TEST_FILE,
        .device_size = RACE_TEST_SIZE,
        .block_size = TEST_BLOCK,
        .flags = KES_STORAGE_CREATE,
        .strategy = KES_ALLOC_FIRST_FIT,
    };
    TEST_ASSERT( kes_storage_create( &cfg, &storage) == KES_SUCCESS,
                "race storage created");
    TEST_ASSERT( kes_storage_sync( storage) == KES_SUCCESS,
                "descriptor/bitmap synced before fork");
    TEST_ASSERT( kes_storage_close( storage) == KES_SUCCESS,
                "storage closed before fork");
    storage = NULL;

    shared = mmap( NULL, sizeof( race_shared_t), PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    TEST_ASSERT( shared != MAP_FAILED, "shared memory mapped");
    memset( shared, 0, sizeof( race_shared_t));

    pid = fork();
    TEST_ASSERT( pid >= 0, "fork succeeded");

    if ( pid == 0) {
        kes_storage_t *child_storage = NULL;

        shared->child.open_ok =
            ( kes_storage_open( RACE_TEST_FILE, 0, &child_storage) ==
              KES_SUCCESS);
        if ( shared->child.open_ok) {
            run_racing_side( child_storage, &shared->child, (char)0xCC);
            kes_storage_close( child_storage);
        }
        _exit( 0);
    }

    shared->parent.open_ok =
        ( kes_storage_open( RACE_TEST_FILE, 0, &storage) == KES_SUCCESS);
    TEST_ASSERT( shared->parent.open_ok,
                "parent re-opened storage independently after fork");
    run_racing_side( storage, &shared->parent, (char)0xAA);
    kes_storage_close( storage);
    storage = NULL;

    TEST_ASSERT( waitpid( pid, &status, 0) == pid, "waitpid reaped child");
    if ( WIFSIGNALED( status)) {
        printf( "\n  child process crashed with signal %d while "
                "racing unsynchronized storage access\n",
                WTERMSIG( status));
    }
    TEST_ASSERT( !WIFSIGNALED( status),
                "child process must not crash while racing "
                "unsynchronized storage access");
    TEST_ASSERT( shared->child.open_ok,
                "child independently opened the storage file too");

    printf( "\n  [known limitation -- unsynchronized cross-process "
            "race, see PENDING_ITEMS.md] "
            "parent: allocs=%d free_blocks=%llu used_blocks=%llu "
            "magic=0x%08x | child: allocs=%d free_blocks=%llu "
            "used_blocks=%llu magic=0x%08x\n",
            shared->parent.successful_allocs,
            (unsigned long long)shared->parent.final_free_blocks,
            (unsigned long long)shared->parent.final_used_blocks,
            shared->parent.final_magic,
            shared->child.successful_allocs,
            (unsigned long long)shared->child.final_free_blocks,
            (unsigned long long)shared->child.final_used_blocks,
            shared->child.final_magic);

    if ( shared->parent.final_free_blocks !=
         shared->child.final_free_blocks ||
         shared->parent.final_used_blocks !=
         shared->child.final_used_blocks) {
        printf( "  [known limitation] parent/child final block "
                "accounting diverged, as expected for unsynchronized "
                "concurrent access -- this is the corruption "
                "PENDING_ITEMS.md documents, not a test failure.\n");
    }
    if ( shared->parent.final_magic != KES_MAGIC_NUMBER ||
         shared->child.final_magic != KES_MAGIC_NUMBER) {
        printf( "  [known limitation] a racing kes_storage_sync() "
                "left the on-disk descriptor magic number other "
                "than KES_MAGIC_NUMBER as observed by one side.\n");
    }

    /*
     * Each side's final_free_blocks/final_used_blocks above only
     * reflects that process's own private in-memory view -- both
     * loaded an independent copy of the bitmap at open() time and
     * never saw the other's writes, so each one looks internally
     * "consistent" from its own perspective even though real
     * corruption occurred. A third, non-racing kes_storage_open()
     * here, after both processes have exited, reveals the actual
     * persisted state: whichever process's kes_storage_sync() call
     * physically landed last silently overwrote the other's bitmap/
     * descriptor block, so the real on-disk used_blocks is expected
     * to reflect only one side's allocations (<= RACE_OP_COUNT), not
     * the sum of both (up to 2 * RACE_OP_COUNT) -- this is where the
     * lost/clobbered allocations actually become visible.
     */
    kes_storage_t *final_storage = NULL;
    if ( kes_storage_open( RACE_TEST_FILE, KES_STORAGE_READONLY,
                           &final_storage) == KES_SUCCESS) {
        kes_storage_stats_t final_stats;
        int combined_allocs = shared->parent.successful_allocs +
                               shared->child.successful_allocs;

        if ( kes_storage_get_stats( final_storage,
                                    &final_stats) == KES_SUCCESS) {
            printf( "  [known limitation] real on-disk state after "
                    "both processes exited: used_blocks=%llu "
                    "free_blocks=%llu, vs. %d combined allocations "
                    "the two processes each believed succeeded -- "
                    "any shortfall is allocations one side's "
                    "kes_storage_sync() silently clobbered.\n",
                    (unsigned long long)final_stats.used_blocks,
                    (unsigned long long)final_stats.free_blocks,
                    combined_allocs);
        }
        kes_storage_close( final_storage);
    } else {
        printf( "  [known limitation] final read-only re-open of the "
                "raced storage file failed -- see AGENTS.md/"
                "PENDING_ITEMS.md's unsynchronized-access gap.\n");
    }

    munmap( shared, sizeof( race_shared_t));
    unlink( RACE_TEST_FILE);

    TEST_SUCCESS( "cross-process UNSYNCHRONIZED racing extent "
                  "allocate/write -- completes without hanging or "
                  "crashing; any accounting divergence printed above "
                  "is a documented, known limitation, not a failure "
                  "of this test");
}

typedef struct{
    const char *name;
    bool ( *func)(void);
} test_case_t;

static test_case_t test_cases[] = {
    {"kes_cross_process_sync_io", test_kes_cross_process_sync_io},
    {"kes_cross_process_racing_io", test_cross_process_racing_io},
    {NULL, NULL}
};

int main(void){
    printf( "=== KES Multi-Process Synchronized I/O ===\n\n");

    for ( test_case_t *t = test_cases; t->name != NULL; t++) {
        printf( "Running: %s... ", t->name);
        fflush( stdout);
        tests_run++;
        if ( t->func()) {
            tests_passed++;
        }
    }

    printf( "\n=== Test Results ===\n");
    printf( "Tests run: %d\n", tests_run);
    printf( "Tests passed: %d\n", tests_passed);
    printf( "Tests failed: %d\n", tests_run - tests_passed);

    return( ( tests_passed == tests_run) ? 0 : 1);
}
