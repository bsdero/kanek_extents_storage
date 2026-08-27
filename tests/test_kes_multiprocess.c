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

typedef struct{
    const char *name;
    bool ( *func)(void);
} test_case_t;

static test_case_t test_cases[] = {
    {"kes_cross_process_sync_io", test_kes_cross_process_sync_io},
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
