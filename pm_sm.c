#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <stdarg.h>

#define MAX_PROCESSES   64
#define MAX_CHILDREN    64

typedef enum {
    STATE_RUNNING    = 0,
    STATE_BLOCKED    = 1,
    STATE_ZOMBIE     = 2,
    STATE_TERMINATED = 3
} ProcessState;

static const char *state_name(ProcessState s) {
    switch (s) {
        case STATE_RUNNING:    return "RUNNING";
        case STATE_BLOCKED:    return "BLOCKED";
        case STATE_ZOMBIE:     return "ZOMBIE";
        case STATE_TERMINATED: return "TERMINATED";
        default:               return "UNKNOWN";
    }
}

typedef struct PCB {
    int           pid;
    int           ppid;
    ProcessState  state;
    int           exit_status;
    int           children[MAX_CHILDREN];
    int           child_count;

    // parent sleeps on this waiting for a child to die
    pthread_cond_t  wait_cond;
    pthread_mutex_t wait_mutex;
} PCB;

static PCB             process_table[MAX_PROCESSES];
static int             process_count = 0;
static int             next_pid      = 1;
static pthread_mutex_t table_mutex   = PTHREAD_MUTEX_INITIALIZER;

static FILE           *snap_file     = NULL;

// snapshot queue — each notify call enqueues one entry
// monitor dequeues and prints them in order, none get lost
#define QUEUE_SIZE 256

typedef struct {
    char label[256];
    char table_snap[MAX_PROCESSES * 80]; // pre-rendered table at time of event
} SnapEntry;

static SnapEntry       snap_queue[QUEUE_SIZE];
static int             snap_head    = 0;
static int             snap_tail    = 0;
static pthread_mutex_t snap_mutex   = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  snap_cond    = PTHREAD_COND_INITIALIZER;

// render table into a string buffer right now, caller holds table_mutex
static void render_table(char *buf, int bufsz) {
    int pos = 0;
    pos += snprintf(buf + pos, bufsz - pos,
                    "PID\t\tPPID\t\tSTATE\t\tEXIT_STATUS\n");
    pos += snprintf(buf + pos, bufsz - pos,
                    "----------------------------------------------\n");
    for (int i = 0; i < MAX_PROCESSES && pos < bufsz; i++) {
        PCB *p = &process_table[i];
        if (p->pid == 0 || p->state == STATE_TERMINATED)
            continue;
        if (p->exit_status == -1)
            pos += snprintf(buf + pos, bufsz - pos, "%d\t\t%d\t\t%s\t\t-\n",
                            p->pid, p->ppid, state_name(p->state));
        else
            pos += snprintf(buf + pos, bufsz - pos, "%d\t\t%d\t\t%s\t\t%d\n",
                            p->pid, p->ppid, state_name(p->state), p->exit_status);
    }
}

// call with table_mutex held — snapshots the table right now so no race later
static void enqueue_snapshot(const char *label) {
    pthread_mutex_lock(&snap_mutex);

    int next = (snap_tail + 1) % QUEUE_SIZE;
    if (next == snap_head) {
        // queue full, drop — shouldn't happen in normal use
        pthread_mutex_unlock(&snap_mutex);
        return;
    }

    SnapEntry *e = &snap_queue[snap_tail];
    strncpy(e->label, label, sizeof(e->label) - 1);
    e->label[sizeof(e->label) - 1] = '\0';
    render_table(e->table_snap, sizeof(e->table_snap));
    snap_tail = next;

    pthread_cond_signal(&snap_cond);
    pthread_mutex_unlock(&snap_mutex);
}

static void pm_ps_to(FILE *f) {
    fprintf(f, "PID\t\tPPID\t\tSTATE\t\tEXIT_STATUS\n");
    fprintf(f, "----------------------------------------------\n");
    for (int i = 0; i < MAX_PROCESSES; i++) {
        PCB *p = &process_table[i];
        if (p->pid == 0 || p->state == STATE_TERMINATED)
            continue;
        if (p->exit_status == -1)
            fprintf(f, "%d\t\t%d\t\t%s\t\t-\n",
                    p->pid, p->ppid, state_name(p->state));
        else
            fprintf(f, "%d\t\t%d\t\t%s\t\t%d\n",
                    p->pid, p->ppid, state_name(p->state), p->exit_status);
    }
}

void pm_ps(void) {
    pthread_mutex_lock(&table_mutex);
    pm_ps_to(stdout);
    pthread_mutex_unlock(&table_mutex);
}

// pid==0 means the slot is free
static PCB *find_pcb(int pid) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (process_table[i].state != STATE_TERMINATED &&
            process_table[i].pid == pid)
            return &process_table[i];
    }
    return NULL;
}

static PCB *alloc_slot(void) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (process_table[i].state == STATE_TERMINATED &&
            process_table[i].pid == 0)
            return &process_table[i];
    }
    return NULL;
}

int pm_fork(int parent_pid, int thread_id) {
    pthread_mutex_lock(&table_mutex);

    PCB *parent = find_pcb(parent_pid);
    if (!parent) {
        fprintf(stderr, "[pm_fork] parent PID %d not found\n", parent_pid);
        pthread_mutex_unlock(&table_mutex);
        return -1;
    }
    if (process_count >= MAX_PROCESSES) {
        fprintf(stderr, "[pm_fork] table full\n");
        pthread_mutex_unlock(&table_mutex);
        return -1;
    }

    PCB *child = alloc_slot();
    if (!child) {
        pthread_mutex_unlock(&table_mutex);
        return -1;
    }

    int new_pid = next_pid++;
    child->pid         = new_pid;
    child->ppid        = parent_pid;
    child->state       = STATE_RUNNING;
    child->exit_status = -1;
    child->child_count = 0;
    memset(child->children, 0, sizeof(child->children));
    pthread_cond_init(&child->wait_cond,  NULL);
    pthread_mutex_init(&child->wait_mutex, NULL);

    if (parent->child_count < MAX_CHILDREN)
        parent->children[parent->child_count++] = new_pid;

    process_count++;

    // snapshot taken here while we still hold table_mutex — label is final
    char label[256];
    snprintf(label, sizeof(label), "Thread %d calls pm_fork %d", thread_id, parent_pid);
    enqueue_snapshot(label);

    pthread_mutex_unlock(&table_mutex);
    return new_pid;
}

void pm_exit(int pid, int status, int thread_id) {
    pthread_mutex_lock(&table_mutex);

    PCB *proc = find_pcb(pid);
    if (!proc) {
        fprintf(stderr, "[pm_exit] PID %d not found\n", pid);
        pthread_mutex_unlock(&table_mutex);
        return;
    }

    proc->state       = STATE_ZOMBIE;
    proc->exit_status = status;

    // snapshot while table is still locked — exit is fully applied
    char label[256];
    snprintf(label, sizeof(label), "Thread %d calls pm_exit %d %d", thread_id, pid, status);
    enqueue_snapshot(label);

    // wake up parent if its waiting
    PCB *parent = find_pcb(proc->ppid);
    pthread_mutex_unlock(&table_mutex);

    if (parent) {
        pthread_mutex_lock(&parent->wait_mutex);
        pthread_cond_broadcast(&parent->wait_cond);
        pthread_mutex_unlock(&parent->wait_mutex);
    }
}

int pm_wait(int parent_pid, int child_pid, int thread_id) {
    pthread_mutex_lock(&table_mutex);

    PCB *parent = find_pcb(parent_pid);
    if (!parent) {
        pthread_mutex_unlock(&table_mutex);
        return -1;
    }
    if (parent->child_count == 0) {
        pthread_mutex_unlock(&table_mutex);
        return 0; // no children, nothing to wait on
    }

    parent->state = STATE_BLOCKED;

    char label[256];
    snprintf(label, sizeof(label), "Thread %d calls pm_wait %d %d", thread_id, parent_pid, child_pid);
    enqueue_snapshot(label);

    // have to release table_mutex before sleeping or we deadlock
    pthread_mutex_unlock(&table_mutex);

    pthread_mutex_lock(&parent->wait_mutex);
    for (;;) {
        pthread_mutex_lock(&table_mutex);

        PCB *zombie = NULL;
        for (int i = 0; i < parent->child_count; i++) {
            PCB *c = find_pcb(parent->children[i]);
            if (!c) continue;
            if (c->state == STATE_ZOMBIE) {
                if (child_pid == -1 || c->pid == child_pid) {
                    zombie = c;
                    break;
                }
            }
        }

        if (zombie) {
            int reaped_pid    = zombie->pid;
            int reaped_status = zombie->exit_status;

            // remove from parent's list
            for (int i = 0; i < parent->child_count; i++) {
                if (parent->children[i] == reaped_pid) {
                    parent->children[i] = parent->children[--parent->child_count];
                    break;
                }
            }

            zombie->state = STATE_TERMINATED;
            zombie->pid   = 0;
            process_count--;

            parent->state = STATE_RUNNING;

            // snapshot the reap before unlocking
            char rlabel[256];
            snprintf(rlabel, sizeof(rlabel),
                     "Thread %d calls pm_wait %d %d (reaped child %d, status=%d)",
                     thread_id, parent_pid, child_pid, reaped_pid, reaped_status);
            enqueue_snapshot(rlabel);

            pthread_mutex_unlock(&table_mutex);
            pthread_mutex_unlock(&parent->wait_mutex);
            return reaped_status;
        }

        pthread_mutex_unlock(&table_mutex);
        pthread_cond_wait(&parent->wait_cond, &parent->wait_mutex);
    }
}

void pm_kill(int pid, int thread_id) {
    pthread_mutex_lock(&table_mutex);
    PCB *proc = find_pcb(pid);
    if (!proc) {
        fprintf(stderr, "[pm_kill] PID %d not found\n", pid);
        pthread_mutex_unlock(&table_mutex);
        return;
    }
    int ppid = proc->ppid;
    proc->state       = STATE_ZOMBIE;
    proc->exit_status = 0;

    char label[256];
    snprintf(label, sizeof(label), "Thread %d calls pm_kill %d", thread_id, pid);
    enqueue_snapshot(label);

    PCB *parent = find_pcb(ppid);
    pthread_mutex_unlock(&table_mutex);

    if (parent) {
        pthread_mutex_lock(&parent->wait_mutex);
        pthread_cond_broadcast(&parent->wait_cond);
        pthread_mutex_unlock(&parent->wait_mutex);
    }
}

static void *monitor_thread(void *arg) {
    (void)arg;

    // print initial snapshot
    pthread_mutex_lock(&table_mutex);
    fprintf(snap_file, "Initial Process Table\n\n");
    pm_ps_to(snap_file);
    fprintf(snap_file, "\n");
    fflush(snap_file);
    pthread_mutex_unlock(&table_mutex);

    for (;;) {
        pthread_mutex_lock(&snap_mutex);
        while (snap_head == snap_tail)
            pthread_cond_wait(&snap_cond, &snap_mutex);

        SnapEntry e = snap_queue[snap_head];
        snap_head = (snap_head + 1) % QUEUE_SIZE;
        pthread_mutex_unlock(&snap_mutex);

        fprintf(snap_file, "%s\n\n", e.label);
        fprintf(snap_file, "%s\n", e.table_snap);
        fflush(snap_file);
    }
    return NULL;
}

typedef struct {
    int   thread_id;
    char *filename;
} WorkerArgs;

static void *worker_thread(void *arg) {
    WorkerArgs *wa = (WorkerArgs *)arg;
    FILE *f = fopen(wa->filename, "r");
    if (!f) {
        fprintf(stderr, "Thread %d: cannot open %s\n", wa->thread_id, wa->filename);
        return NULL;
    }

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0') continue;

        char cmd[64];
        int a, b;
        if (sscanf(line, "%63s", cmd) < 1) continue;

        if (strcmp(cmd, "fork") == 0) {
            if (sscanf(line, "%*s %d", &a) == 1)
                pm_fork(a, wa->thread_id);
        } else if (strcmp(cmd, "exit") == 0) {
            if (sscanf(line, "%*s %d %d", &a, &b) == 2)
                pm_exit(a, b, wa->thread_id);
        } else if (strcmp(cmd, "wait") == 0) {
            if (sscanf(line, "%*s %d %d", &a, &b) == 2)
                pm_wait(a, b, wa->thread_id);
        } else if (strcmp(cmd, "kill") == 0) {
            if (sscanf(line, "%*s %d", &a) == 1)
                pm_kill(a, wa->thread_id);
        } else if (strcmp(cmd, "sleep") == 0) {
            if (sscanf(line, "%*s %d", &a) == 1)
                usleep((useconds_t)a * 1000);
        } else {
            fprintf(stderr, "Thread %d: unknown command '%s'\n", wa->thread_id, cmd);
        }
    }

    fclose(f);
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s thread0.txt [thread1.txt ...]\n", argv[0]);
        return 1;
    }

    memset(process_table, 0, sizeof(process_table));
    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_table[i].pid   = 0;
        process_table[i].state = STATE_TERMINATED;
    }

    // init process, always PID 1
    PCB *init = &process_table[0];
    init->pid         = next_pid++;
    init->ppid        = 0;
    init->state       = STATE_RUNNING;
    init->exit_status = -1;
    init->child_count = 0;
    pthread_cond_init(&init->wait_cond,  NULL);
    pthread_mutex_init(&init->wait_mutex, NULL);
    process_count = 1;

    snap_file = fopen("snapshots.txt", "w");
    if (!snap_file) {
        perror("fopen snapshots.txt");
        return 1;
    }

    pthread_t mon;
    pthread_create(&mon, NULL, monitor_thread, NULL);
    usleep(10000); // let monitor print initial snapshot before workers start

    int n_workers = argc - 1;
    pthread_t  *workers = malloc(n_workers * sizeof(pthread_t));
    WorkerArgs *wargs   = malloc(n_workers * sizeof(WorkerArgs));

    for (int i = 0; i < n_workers; i++) {
        wargs[i].thread_id = i;
        wargs[i].filename  = argv[i + 1];
        pthread_create(&workers[i], NULL, worker_thread, &wargs[i]);
    }

    for (int i = 0; i < n_workers; i++)
        pthread_join(workers[i], NULL);

    usleep(50000); // give monitor time to flush the last snapshot

    printf("\n=== Final Process Table ===\n");
    pm_ps();

    fclose(snap_file);
    free(workers);
    free(wargs);
    pthread_cancel(mon);
    pthread_join(mon, NULL);
    return 0;
}