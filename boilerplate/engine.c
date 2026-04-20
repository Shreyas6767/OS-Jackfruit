/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * Intentionally partial starter:
 *   - command-line shape is defined
 *   - key runtime data structures are defined
 *   - bounded-buffer skeleton is defined
 *   - supervisor / client split is outlined
 *
 * Students are expected to design:
 *   - the control-plane IPC implementation
 *   - container lifecycle and metadata synchronization
 *   - clone + namespace setup for each container
 *   - producer/consumer behavior for log buffering
 *   - signal handling and graceful shutdown
 */

#define _GNU_SOURCE
#define CGROUP_ROOT "/sys/fs/cgroup/jackfruit"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <stdint.h>
#include <getopt.h>
#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define CONTROL_MESSAGE_LEN 256
#define CHILD_COMMAND_LEN 256
#define LOG_CHUNK_SIZE 4096
#define BUFFER_SIZE 16
#define DEFAULT_SOFT_LIMIT (40UL << 20)
#define DEFAULT_HARD_LIMIT (64UL << 20)

static int cmd_logs(int argc, char *argv[]);

void crash_handler(int sig) {
    fprintf(stderr, "\nFATAL: Supervisor caught signal %d (Crash/Exit)\n", sig);
    exit(sig);
}

// Global flag to control the supervisor loop
volatile sig_atomic_t keep_running = 1;

// Handler for SIGINT (Ctrl+C) and SIGTERM
static void supervisor_shutdown_handler(int sig) {
    (void)sig; // Suppress unused warning
    keep_running = 0;
}

typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    bool stop_requested;
    pthread_t logger_tid;   // Handle to the per-container producer thread
    int log_read_fd;        // The read-end of the pipe (useful for cleanup)
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int exit_code;
    int exit_signal;
    char log_path[PATH_MAX];
    struct container_record *next;
} container_record_t;

typedef struct {
    char container_id[CONTAINER_ID_LEN];
    size_t length;
    char data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    int read_fd;
    char id[CONTAINER_ID_LEN];
} logger_args_t;

typedef struct {
    log_item_t items[BUFFER_SIZE];
    size_t head;
    size_t tail;
    size_t count;
    int shutdown;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} bounded_buffer_t;

// A global buffer that the Consumer thread will watch
bounded_buffer_t log_buffer;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[16][64];
    //char command[CHILD_COMMAND_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
} control_request_t;

typedef struct {
    int status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int nice_value;
    int log_write_fd;
} child_config_t;

typedef struct {
    int server_fd;
    int monitor_fd;
    int should_stop;
    pthread_t logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}

static int parse_mib_flag(const char *flag,
                          const char *value,
                          unsigned long *target_bytes)
{
    char *end = NULL;
    unsigned long mib;

    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }

    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value for %s is too large: %s\n", flag, value);
        return -1;
    }

    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req,
                                int argc,
                                char *argv[],
                                int start_index)
{
    int i;

    for (i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long nice_value;

        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }

        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i + 1], &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i + 1], &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nice_value = strtol(argv[i + 1], &end, 10);
            if (errno != 0 || end == argv[i + 1] || *end != '\0' ||
                nice_value < -20 || nice_value > 19) {
                fprintf(stderr,
                        "Invalid value for --nice (expected -20..19): %s\n",
                        argv[i + 1]);
                return -1;
            }
            req->nice_value = (int)nice_value;
            continue;
        }

        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }

    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "Invalid limits: soft limit cannot exceed hard limit\n");
        return -1;
    }

    return 0;
}

static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING:
        return "starting";
    case CONTAINER_RUNNING:
        return "running";
    case CONTAINER_STOPPED:
        return "stopped";
    case CONTAINER_KILLED:
        return "killed";
    case CONTAINER_EXITED:
        return "exited";
    default:
        return "unknown";
    }
}

static int bounded_buffer_init(bounded_buffer_t *buffer)
{
    int rc;

    memset(buffer, 0, sizeof(*buffer));

    rc = pthread_mutex_init(&buffer->mutex, NULL);
    if (rc != 0)
        return rc;

    rc = pthread_cond_init(&buffer->not_empty, NULL);
    if (rc != 0) {
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    rc = pthread_cond_init(&buffer->not_full, NULL);
    if (rc != 0) {
        pthread_cond_destroy(&buffer->not_empty);
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buffer)
{
    pthread_cond_destroy(&buffer->not_full);
    pthread_cond_destroy(&buffer->not_empty);
    pthread_mutex_destroy(&buffer->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buffer)
{
    pthread_mutex_lock(&buffer->mutex);
    buffer->shutdown = 1;
    pthread_cond_broadcast(&buffer->not_empty);
    pthread_cond_broadcast(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
}

void apply_cgroup_limits(const char *container_id, pid_t pid, unsigned long limit_bytes) {
   char path[512];
    char buf[64];

    // 1. Create directory
    snprintf(path, sizeof(path), "/sys/fs/cgroup/%s", container_id);
    mkdir(path, 0755);

    // 2. Write Memory Limit
    if (limit_bytes > 0) {
        snprintf(path, sizeof(path), "/sys/fs/cgroup/%s/memory.max", container_id);
        int fd = open(path, O_WRONLY);
        if (fd >= 0) {
            // Write the bytes directly as a string
            int len = snprintf(buf, sizeof(buf), "%lu", limit_bytes);
            write(fd, buf, len);
            close(fd);
        }
    }

    // 3. Attach PID
    snprintf(path, sizeof(path), "/sys/fs/cgroup/%s/cgroup.procs", container_id);
    int fd = open(path, O_WRONLY);
    if (fd >= 0) {
        int len = snprintf(buf, sizeof(buf), "%d", pid);
        write(fd, buf, len);
        close(fd);
    }
}


/*
 * TODO:
 * Implement producer-side insertion into the bounded buffer.
 *
 * Requirements:
 *   - block or fail according to your chosen policy when the buffer is full
 *   - wake consumers correctly
 *   - stop cleanly if shutdown begins
 */
int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
   pthread_mutex_lock(&buffer->mutex);

    // 1. Wait while full
    while (buffer->count == BUFFER_SIZE && !buffer->shutdown) {
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);
    }

    if (buffer->shutdown) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1; 
    }

    // 2. Copy the item into the buffer (Deep Copy)
    // Assuming items[tail] is the destination
    buffer->items[buffer->tail] = *item; 
    
    buffer->tail = (buffer->tail + 1) % BUFFER_SIZE;
    buffer->count++;

    // 3. Signal consumer
    pthread_cond_signal(&buffer->not_empty);
    pthread_mutex_unlock(&buffer->mutex);

    return 0;
}

/*
 * TODO:
 * Implement consumer-side removal from the bounded buffer.
 *
 * Requirements:
 *   - wait correctly while the buffer is empty
 *   - return a useful status when shutdown is in progress
 *   - avoid races with producers and shutdown
 */
int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *out_item)
{
    pthread_mutex_lock(&buffer->mutex);

    // 1. Wait while empty
    while (buffer->count == 0 && !buffer->shutdown) {
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);
    }

    if (buffer->count == 0 && buffer->shutdown) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    // 2. Retrieve the item
    *out_item = buffer->items[buffer->head];
    buffer->head = (buffer->head + 1) % BUFFER_SIZE;
    buffer->count--;

    // 3. Signal producer
    pthread_cond_signal(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);

    return 0;
}

/*
 * TODO:
 * Implement the logging consumer thread.
 *
 * Suggested responsibilities:
 *   - remove log chunks from the bounded buffer
 *   - route each chunk to the correct per-container log file
 *   - exit cleanly when shutdown begins and pending work is drained
 */
void *logging_consumer_thread(void *arg)
{
   bounded_buffer_t *bb = (bounded_buffer_t *)arg;
    log_item_t item;

    while (1) {
        if (bounded_buffer_pop(bb, &item) != 0) 
        	break;

        char path[PATH_MAX];
        snprintf(path, sizeof(path), "/tmp/container_%s.log", item.container_id);

        int fd = open(path, O_CREAT | O_WRONLY | O_APPEND, 0644);
        if (fd >= 0) {
            write(fd, item.data, item.length); // Write the exact length
            close(fd);
        }
    }
    return NULL;
}

void* logger_producer_thread(void* arg) {
logger_args_t *args = (logger_args_t*)arg;
    int rfd = args->read_fd;
    char cid[CONTAINER_ID_LEN];
    strncpy(cid, args->id, CONTAINER_ID_LEN);
    free(args); // We've copied what we need

    log_item_t item;
    ssize_t bytes_read;

    // Read up to LOG_CHUNK_SIZE from the pipe
    while ((bytes_read = read(rfd, item.data, LOG_CHUNK_SIZE)) > 0) {
        // Fill the metadata
        strncpy(item.container_id, cid, CONTAINER_ID_LEN);
        item.length = (size_t)bytes_read;

        // Push the chunk into the buffer
        bounded_buffer_push(&log_buffer, &item);
        
        // Clear the item for the next read
        memset(&item, 0, sizeof(item));
    }

    close(rfd);
    return NULL;
}


static void register_with_kernel_monitor(pid_t pid, const char *id, uint64_t soft_mib, uint64_t hard_mib) {
    int fd = open("/dev/container_monitor", O_RDWR);
    if (fd < 0) {
        // Log to stderr but don't stop the engine; maybe the module isn't loaded
        fprintf(stderr, "[Supervisor] Note: Kernel monitor device not found.\n");
        return;
    }

    struct monitor_request req;
    req.pid = pid;
    // We convert MiB from the CLI to Bytes for the Kernel
    req.soft_limit_bytes = soft_mib * 1024 * 1024;
    req.hard_limit_bytes = hard_mib * 1024 * 1024;
    strncpy(req.container_id, id, sizeof(req.container_id) - 1);
    req.container_id[sizeof(req.container_id) - 1] = '\0';

    if (ioctl(fd, MONITOR_REGISTER, &req) < 0) {
        perror("[Supervisor] ioctl MONITOR_REGISTER failed");
    } else {
        printf("[Supervisor] Successfully registered PID %d with Kernel Monitor\n", pid);
    }

    close(fd);
}

/*
 * TODO:
 * Implement the clone child entrypoint.
 *
 * Required outcomes:
 *   - isolated PID / UTS / mount context
 *   - chroot or pivot_root into rootfs
 *   - working /proc inside container
 *   - stdout / stderr redirected to the supervisor logging path
 *   - configured command executed inside the container
 */
static int child_fn(void *arg)
{
   child_config_t *config = (child_config_t *)arg;
   
    // 4. Redirect output to a log file (Task 2 style)
    //char log_path[PATH_MAX];
    //snprintf(log_path, sizeof(log_path), "/tmp/container_%s.log", config->id);
    
    // Open the file (create if it doesn't exist, append if it does)
    //int log_fd = open(log_path, O_CREAT | O_WRONLY | O_APPEND, 0644);
   /* if (log_fd >= 0) {
        dup2(log_fd, STDOUT_FILENO);
        dup2(log_fd, STDERR_FILENO);
        close(log_fd);
    } else {
        perror("Failed to open log file in child");
    }
    
    */

    // 1. Set Hostname (UTS Isolation)
    sethostname(config->id, strlen(config->id));

    // 2. Filesystem Isolation (chroot)
    if (chdir(config->rootfs) != 0 || chroot(".") != 0 || chdir("/") != 0) {
        perror("chroot failed");
        return 1;
    }

    // 3. Mount /proc so 'ps' works inside the container
    if (mount("proc", "/proc", "proc", 0, NULL) != 0) {
        perror("proc mount failed");
        return 1;
    }

    //4. Redirect output to the supervisor pipe (for Task 3 later)
    if (config->log_write_fd >= 0) {
    dup2(config->log_write_fd, STDOUT_FILENO);
    dup2(config->log_write_fd, STDERR_FILENO);
    close(config->log_write_fd);
    }
   

    // 5. Execute the command
    char *argv[] = { "/bin/sh", "-c", config->command, NULL };
    execvp(argv[0], argv);

    perror("execvp failed");
    return 1;
}

int register_with_monitor(int monitor_fd,
                          const char *container_id,
                          pid_t host_pid,
                          unsigned long soft_limit_bytes,
                          unsigned long hard_limit_bytes)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0)
        return -1;

    return 0;
}

int unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0)
        return -1;

    return 0;
}

/*
 * TODO:
 * Implement the long-running supervisor process.
 *
 * Suggested responsibilities:
 *   - create and bind the control-plane IPC endpoint
 *   - initialize shared metadata and the bounded buffer
 *   - start the logging thread
 *   - accept control requests and update container state
 *   - reap children and respond to signals
 */
static int run_supervisor(const char *rootfs)
{
    // Catch crashes so they aren't silent
    signal(SIGPIPE, SIG_IGN);
    signal(SIGSEGV, crash_handler);
    signal(SIGILL,  crash_handler);
    signal(SIGABRT, crash_handler);

    supervisor_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.containers = NULL;
    pthread_mutex_init(&ctx.metadata_lock, NULL);
    
    signal(SIGINT, supervisor_shutdown_handler);
    signal(SIGTERM, supervisor_shutdown_handler);

    int sfd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr;
    unlink(CONTROL_PATH);
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path)-1);
    
    bind(sfd, (struct sockaddr *)&addr, sizeof(addr));
    listen(sfd, 5);
    fcntl(sfd, F_SETFL, O_NONBLOCK);
    
    bounded_buffer_init(&log_buffer);
    pthread_t consumer_tid;
    pthread_create(&consumer_tid, NULL, logging_consumer_thread, &log_buffer);
    
    mkdir(CGROUP_ROOT, 0755);

   printf("Supervisor: Active and listening (Press Ctrl+C to shut down)...\n");

    while (keep_running) {
        // --- 1. REAPER ---
        int status;
        pid_t reaped;
        while ((reaped = waitpid(-1, &status, WNOHANG)) > 0) {
            printf("DEBUG: Reaper caught PID %d\n", reaped);
            pthread_mutex_lock(&ctx.metadata_lock);
            container_record_t *it = ctx.containers;
            while (it) {
                if (it->host_pid == reaped) {
                
                if (WIFSIGNALED(status)) {
		// If we didn't explicitly request a stop, it was likely the monitor
		int sig = WTERMSIG(status);
       		it->exit_signal = sig;
       		
       		if (sig == SIGKILL && !it->stop_requested) {
		    it->state = CONTAINER_KILLED; 
		    printf("Supervisor: Container '%s' was KILLED by the Kernel Monitor (Hard Limit reached).\n", it->id);
		} else {
		    it->state = CONTAINER_EXITED;
		}
    	    }
                    else {
        	    it->state = CONTAINER_EXITED;
        	    it->exit_code = WEXITSTATUS(status);
                    }
                    it->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
                    char cgroup_path[512];
                    snprintf(cgroup_path, sizeof(cgroup_path), "/sys/fs/cgroup/%s", it->id);
                    if (rmdir(cgroup_path) == 0) {
                        printf("Supervisor: Cgroup for '%s' cleaned up.\n", it->id);
                    } else {
                        // It's okay if this fails on a double-delete or manual removal
                        // but it's good to see the message for debugging.
                        perror("Supervisor: Cgroup cleanup failed");
                    }
                    
                    printf("Supervisor: Process %d finished.\n", reaped);
                }
                it = it->next;
            }
            pthread_mutex_unlock(&ctx.metadata_lock);
        }

        // --- 2. COMMANDS ---
        int cfd = accept(sfd, NULL, NULL);
        if (cfd >= 0) {
           printf("Supervisor: CLI connected! Waiting for data...\n");
            control_request_t req;
            memset(&req, 0, sizeof(req));
            
            ssize_t n = read(cfd, &req, sizeof(req));
            printf("Supervisor: Read %zd bytes (Expected %zu)\n", n, sizeof(req));
            
            if (n <= 0) {
                perror("Supervisor: Read failed");
            } else {
                printf("Supervisor: Received Kind=%d, ID=[%s]\n", req.kind, req.container_id);
                }
            if (n == sizeof(req)) {
                if (req.kind == CMD_START) {
                   
                    int pipefd[2];
		    if (pipe(pipefd) < 0) {
			perror("Failed to create pipe");
			// handle error...
		    }
                
                    child_config_t *config = malloc(sizeof(child_config_t));
                    strncpy(config->id, req.container_id, CONTAINER_ID_LEN);
                    strncpy(config->rootfs, req.rootfs, PATH_MAX);
                    strncpy(config->command, req.command, CHILD_COMMAND_LEN);
                    
                    config->log_write_fd = pipefd[1];  // Give the WRITE end to the child

                    char *stack = malloc(STACK_SIZE);
                    pid_t pid = clone(child_fn, stack + STACK_SIZE, 
                                     CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | SIGCHLD, 
                                     config);
                    
                    if (pid > 0) {
                    
                     pthread_t logger_tid;
                    
                    // SUPERVISOR SIDE
			close(pipefd[1]); // Close the write end; only child uses it

			logger_args_t *largs = malloc(sizeof(logger_args_t));
		        largs->read_fd = pipefd[0];
		        strncpy(largs->id, config->id, CONTAINER_ID_LEN);
			
			// 'logger_producer_thread' function called
			pthread_create(&logger_tid, NULL, logger_producer_thread, largs);
                    
                    
                        pthread_mutex_lock(&ctx.metadata_lock);
                        container_record_t *rec = malloc(sizeof(container_record_t));
                        strncpy(rec->id, config->id, CONTAINER_ID_LEN);
                        rec->host_pid = pid;
                        rec->started_at = time(NULL);
                        rec->state = CONTAINER_RUNNING;
                        rec->soft_limit_bytes = (uint64_t)req.soft_limit_bytes;
                        rec->hard_limit_bytes = (uint64_t)req.hard_limit_bytes;
                        rec->exit_code = 0;
                        rec->exit_signal = 0;
                        snprintf(rec->log_path, PATH_MAX, "/tmp/container_%s.log", rec->id);
                        
                        rec->logger_tid = logger_tid;
                        rec->next = ctx.containers;
                        rec->stop_requested = false;
                        ctx.containers = rec;
                        pthread_mutex_unlock(&ctx.metadata_lock);
                        
                        printf("Supervisor: Saved metadata for '%s' (PID: %d)\n", rec->id, rec->host_pid);
                        
                        register_with_kernel_monitor(pid, rec->id, rec->soft_limit_bytes, rec->hard_limit_bytes);
                        
                        // 2. Apply the limits from the request
		        // req.hard_limit_mib comes from your CLI arguments
		        apply_cgroup_limits(req.container_id, pid, req.hard_limit_bytes);
		    
		    printf("Supervisor: Container %s (PID %d) is now resource-constrained.\n", req.container_id, pid);
                    } 
                    else {
                        perror("clone failed");
                        free(stack);
                        free(config);
                    }
                } 
                else if (req.kind == CMD_PS) {
                    pthread_mutex_lock(&ctx.metadata_lock);
                    container_record_t *p = ctx.containers;
                    while (p) {
                        write(cfd, p, sizeof(container_record_t));
                        p = p->next;
                    }
                    pthread_mutex_unlock(&ctx.metadata_lock);
                }
                else if (req.kind == CMD_STOP) {
                
                 pthread_mutex_lock(&ctx.metadata_lock);
	    container_record_t *s = ctx.containers;
	    int ok = 0;

	    // Use a fixed buffer to ensure null termination for printing
	    char req_id[CONTAINER_ID_LEN];
	    memset(req_id, 0, CONTAINER_ID_LEN);
	    strncpy(req_id, req.container_id, CONTAINER_ID_LEN - 1);

	    printf("Supervisor: Request to stop ID: [%s] (len: %zu)\n", req_id, strlen(req_id));

	    while (s != NULL) {
		printf("Supervisor: Comparing against: [%s] (len: %zu)\n", s->id, strlen(s->id));
		
		// Use strncmp for a precise match up to the buffer limit
		if (strncmp(s->id, req_id, CONTAINER_ID_LEN) == 0) {
		    printf("Supervisor: Match found! Killing PID %d\n", s->host_pid);
		    
		    // --- CRITICAL TASK 4 LOGIC ---
		    s->stop_requested = true;  // Mark that this was a manual stop
		    s->state = CONTAINER_STOPPED;
		    
		    kill(s->host_pid, SIGTERM);
		    usleep(50000);
		    kill(s->host_pid, SIGKILL);
		    ok = 1;
		    break;
		}
		s = s->next;
	    }
	    pthread_mutex_unlock(&ctx.metadata_lock);
	    
	    control_response_t res;
	    memset(&res, 0, sizeof(res));
	    res.status = ok ? 0 : -1;
	    strncpy(res.message, ok ? "Stopped successfully" : "Container not found", CONTROL_MESSAGE_LEN - 1);
	    write(cfd, &res, sizeof(res));
           }
                
                else
                {
                	// THIS IS THE KEY
    			printf("Supervisor: Received unknown command kind: %d\n", req.kind);
                }
            }
            close(cfd);
        }
        usleep(10000);
    }
    
 printf("\nSupervisor: Shutting down gracefully...\n");

// 1. FIRST: Signal the buffer to stop accepting new logs
bounded_buffer_begin_shutdown(&log_buffer); 

// 2. SECOND: Kill all containers. This is CRITICAL.
// Closing the containers closes the pipes, which unblocks the Producer threads.
pthread_mutex_lock(&ctx.metadata_lock);
container_record_t *it = ctx.containers;
while (it) {
    if (it->state == CONTAINER_RUNNING) {
        printf("Supervisor: Stopping container '%s' (PID %d)...\n", it->id, it->host_pid);
        kill(it->host_pid, SIGKILL); // Use SIGKILL for a guaranteed pipe closure
        
        int status;
        waitpid(it->host_pid, &status, 0); 
    }
    it = it->next;
}
pthread_mutex_unlock(&ctx.metadata_lock);

// 3. THIRD: Join the Producer threads
pthread_mutex_lock(&ctx.metadata_lock);
it = ctx.containers;
while (it) {
    if (it->logger_tid != 0) {
        pthread_join(it->logger_tid, NULL);
        printf("Supervisor: Producer thread for %s joined.\n", it->id);
    }
    
    // Cleanup cgroup and memory
    char cgroup_path[512];
    snprintf(cgroup_path, sizeof(cgroup_path), "/sys/fs/cgroup/%s", it->id);
    rmdir(cgroup_path);
    
    container_record_t *next = it->next;
    free(it);
    it = next;
}
pthread_mutex_unlock(&ctx.metadata_lock);

// 4. FINALLY: Join the consumer thread
pthread_join(consumer_tid, NULL); 
printf("Supervisor: Logging consumer thread joined.\n");

printf("Supervisor: Shutdown complete.\n");
}

/*
 * TODO:
 * Implement the client-side control request path.
 *
 * The CLI commands should use a second IPC mechanism distinct from the
 * logging pipe. A UNIX domain socket is the most direct option, but a
 * FIFO or shared memory design is also acceptable if justified.
 */
static int send_control_request(const control_request_t *req)
{
   int sfd;
    struct sockaddr_un addr;

    // 1. Create socket
    if ((sfd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
        perror("socket");
        return 1;
    }

    // 2. Setup address (pointing to /tmp/mini_runtime.sock)
    memset(&addr, 0, sizeof(struct sockaddr_un));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    // 3. Connect to the supervisor
    if (connect(sfd, (struct sockaddr *)&addr, sizeof(struct sockaddr_un)) == -1) {
        fprintf(stderr, "Error: Could not connect to supervisor. Is it running?\n");
        close(sfd);
        return 1;
    }

    // 4. Send the request structure
    if (write(sfd, req, sizeof(control_request_t)) != sizeof(control_request_t)) {
        perror("write");
        close(sfd);
        return 1;
    }

    close(sfd);
    return 0;
}

static int cmd_start(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr,
                "Usage: %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[0], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[1], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[2], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 3) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
      control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;

    strncpy(req.rootfs, "/home/shreyas67/OS-Jackfruit/rootfs-base", PATH_MAX - 1);
    
    // 1. CLEAR & SIMPLE PARSING
    for (int i = 0; i < argc; i++) {
        // Look for the ID  
        if ((strcmp(argv[i], "--id") == 0 || strcmp(argv[i], "-i") == 0) && i + 1 < argc) {
            strncpy(req.container_id, argv[i+1], CONTAINER_ID_LEN - 1);
        } 
        // Look for Hard Limit
        else if ((strcmp(argv[i], "--hard-mib") == 0 || strcmp(argv[i], "-h") == 0) && i + 1 < argc) {
            req.hard_limit_bytes = (unsigned long)atoll(argv[i+1]);
        } 
        // Look for Soft Limit
        else if ((strcmp(argv[i], "--soft-mib") == 0 || strcmp(argv[i], "-s") == 0) && i + 1 < argc) {
            req.soft_limit_bytes = (unsigned long)atoll(argv[i+1]) * 1024 * 1024;
        }
    }

    // 2. FIND THE COMMAND (e.g., ./memory_hog 50)
    int cmd_idx = 0;
    for (int i = 0; i < argc; i++) {
        if (argv[i][0] != '-') {
            // Skip the values belonging to flags
            if (i > 0 && (strcmp(argv[i-1], "--id") == 0 || strcmp(argv[i-1], "-i") == 0 ||
                          strcmp(argv[i-1], "--hard-mib") == 0 || strcmp(argv[i-1], "-h") == 0 ||
                          strcmp(argv[i-1], "--soft-mib") == 0 || strcmp(argv[i-1], "-s") == 0)) {
                continue; 
            }
            // This is our command! Copy it into req.command[16][64]
            while (i < argc && cmd_idx < 16) {
                strncpy(req.command[cmd_idx++], argv[i++], 64 - 1);
            }
            break;
        }
    }

    // 3. SEND DIRECTLY TO SUPERVISOR
    int sfd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(sfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("Connect failed. Is supervisor running?");
        return 1;
    }

    write(sfd, &req, sizeof(req));
    
    control_response_t res;
    read(sfd, &res, sizeof(res));
    close(sfd);

    printf("Run: Waiting for container '%s' to finish...\n", req.container_id);

    // 4. POLLING LOOP (Check if it's still running)
    while (1) {
        sleep(1);
        control_request_t poll_req;
        memset(&poll_req, 0, sizeof(poll_req));
        poll_req.kind = CMD_PS;

        int ps_sfd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (connect(ps_sfd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            write(ps_sfd, &poll_req, sizeof(poll_req));
            container_record_t rec;
            int still_running = 0;
            int found = 0;

            while (read(ps_sfd, &rec, sizeof(rec)) > 0) {
                if (strncmp(rec.id, req.container_id, CONTAINER_ID_LEN) == 0) {
                    found = 1;
                    if (rec.state == CONTAINER_RUNNING) still_running = 1;
                    break;
                }
            }
            close(ps_sfd);

            if (!found || !still_running) {
                printf("Run: Container finished.\n");
                return 0; 
            }
        }
    }
}

static int cmd_ps(void)
{
   control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;

    // 1. Open connection to supervisor
    int sfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sfd == -1) { perror("socket"); return 1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(struct sockaddr_un));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(sfd, (struct sockaddr *)&addr, sizeof(struct sockaddr_un)) == -1) {
        fprintf(stderr, "Error: Supervisor is not running.\n");
        close(sfd);
        return 1;
    }

    // 2. Send the request
    if (write(sfd, &req, sizeof(req)) != sizeof(req)) {
        perror("write request");
        close(sfd);
        return 1;
    }

    // 3. Print table header
    printf("\n%-12s %-8s %-12s %-20s\n", "CONTAINER ID", "PID", "STATUS", "STARTED AT");
    printf("----------------------------------------------------------------------\n");

    // 4. Receive records until supervisor closes the socket
    container_record_t rec;
    int count = 0;
    while (read(sfd, &rec, sizeof(container_record_t)) > 0) {
        char time_buf[20];
        struct tm *tm_info = localtime(&rec.started_at);
        strftime(time_buf, sizeof(time_buf), "%H:%M:%S %Y-%m-%d", tm_info);
        
        char status_str[20];
        if (rec.state == CONTAINER_RUNNING) {
            strcpy(status_str, "RUNNING");
        } else if (rec.state == CONTAINER_EXITED) {
            snprintf(status_str, sizeof(status_str), "EXITED(%d)", rec.exit_code);
        } else if (rec.state == CONTAINER_KILLED) {
            snprintf(status_str, sizeof(status_str), "KILLED(%d)", rec.exit_signal);
        } else {
            strcpy(status_str, "STOPPED");
        }

        printf("%-12s %-8d %-12s %-20s\n",
               rec.id,
               rec.host_pid,
               (rec.state == CONTAINER_RUNNING ? "RUNNING" : "STOPPED"),
               time_buf);
        count++;
    }

    if (count == 0) {
        printf("(No containers currently tracked)\n");
    }
    printf("\n");

    close(sfd);
    return 0;
}

static int cmd_logs(int argc, char *argv[])
{
   if (argc < 1) {
        fprintf(stderr, "Usage: engine logs <container_id>\n");
        return 1;
    }

    char log_path[PATH_MAX];
    // This matches the path format used in your run_supervisor metadata
    snprintf(log_path, sizeof(log_path), "/tmp/container_%s.log", argv[0]);

    FILE *f = fopen(log_path, "r");
    if (!f) {
        // If the file isn't there, the container likely hasn't started or 
        // the supervisor failed to create the file.
        fprintf(stderr, "Error: Could not find logs for container '%s'.\n", argv[0]);
        return 1;
    }

    printf("--- Logs for '%s' ---\n", argv[0]);
    char buffer[1024];
    while (fgets(buffer, sizeof(buffer), f)) {
        printf("%s", buffer);
    }
    printf("--- End of logs ---\n");

    fclose(f);
    return 0;
}

static int cmd_stop(int argc, char *argv[])
{
   if (argc < 1) {
        fprintf(stderr, "Usage: engine stop <container_id>\n");
        return 1;
    }

    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[0], CONTAINER_ID_LEN-1);

    // Connect and send
    int sfd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(struct sockaddr_un));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(sfd, (struct sockaddr *)&addr, sizeof(struct sockaddr_un)) == -1) {
        perror("connect");
        return 1;
    }

    write(sfd, &req, sizeof(req));

    // CRITICAL: Wait for the supervisor to reply before closing!
    control_response_t res;
    if (read(sfd, &res, sizeof(res)) > 0) {
        printf("Stop request: %s (Status: %d)\n", res.message, res.status);
    }

    close(sfd);
    return 0;
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }
    
    if (strcmp(argv[1], "ps") == 0) {
        return cmd_ps(); // This should return the result of cmd_ps and exit main
    }

   if (strcmp(argv[1], "stop") == 0)
        // Shift by 2: skip './engine' and 'stop'
        return cmd_stop(argc - 2, argv + 2);

    if (strcmp(argv[1], "start") == 0)
        return cmd_start(argc - 2, argv + 2);

    // Do the same for logs if it takes arguments
    if (strcmp(argv[1], "logs") == 0)
        return cmd_logs(argc - 2, argv + 2);
        
     if (strcmp(argv[1], "run") == 0)
        return cmd_run(argc - 2, argv + 2);

    usage(argv[0]);
    return 1;
}
