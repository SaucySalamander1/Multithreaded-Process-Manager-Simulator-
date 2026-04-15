// Process Manager Simulation 


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

#define MAX_PROCESSES 64
#define MAX_CHILDREN 64

typedef enum {
    PM_RUNNING = 0,
    PM_WAITING = 1,
    PM_ZOMBIE  = 2
} pm_state_t;

typedef struct {
    int in_use;
    int pid;
    int ppid;
    pm_state_t state;
    int exit_status;

    int children[MAX_CHILDREN];
    int child_count;
} pcb_t;

static pcb_t table[MAX_PROCESSES];
static int next_pid = 2;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t child_cv = PTHREAD_COND_INITIALIZER;
static pthread_cond_t monitor_cv = PTHREAD_COND_INITIALIZER;

static int table_changed = 0;
static int shutdown_flag = 0;

static FILE *snap_fp = NULL;
static char last_event[256] = "Initial Process Table";

/* helper functions */

static const char *state_str(pm_state_t s) {
    if (s == PM_RUNNING) return "RUNNING";
    if (s == PM_WAITING) return "WAITING";
    if (s == PM_ZOMBIE)  return "ZOMBIE";
    return "UNKNOWN";
}

static pcb_t *find_pcb(int pid) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (table[i].in_use && table[i].pid == pid)
            return &table[i];
    }
    return NULL;
}

static pcb_t *alloc_slot() {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (!table[i].in_use)
            return &table[i];
    }
    return NULL;
}

static void remove_child(pcb_t *parent, int cpid) {
    for (int i = 0; i < parent->child_count; i++) {
        if (parent->children[i] == cpid) {
            for (int j = i; j < parent->child_count - 1; j++)
                parent->children[j] = parent->children[j + 1];
            parent->child_count--;
            return;
        }
    }
}

static void notify_change() {
    table_changed = 1;
    pthread_cond_signal(&monitor_cv);
}

/* print process table snapshot */

static void pm_ps(FILE *fp) {
    fprintf(fp, "PID\t\tPPID\t\tSTATE\t\tEXIT_STATUS\n");
    fprintf(fp, "----------------------------------------------\n");

    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (!table[i].in_use) continue;

        pcb_t *p = &table[i];
        fprintf(fp, "%d\t\t%d\t\t%s\t\t", p->pid, p->ppid, state_str(p->state));

        if (p->state == PM_ZOMBIE)
            fprintf(fp, "%d\n", p->exit_status);
        else
            fprintf(fp, "-\n");
    }
}

/* process operations */

static int pm_fork(int ppid, int tid) {
    pthread_mutex_lock(&lock);

    pcb_t *parent = find_pcb(ppid);
    if (!parent || parent->child_count >= MAX_CHILDREN) {
        pthread_mutex_unlock(&lock);
        return -1;
    }

    pcb_t *child = alloc_slot();
    if (!child) {
        pthread_mutex_unlock(&lock);
        return -1;
    }

    memset(child, 0, sizeof(*child));
    child->in_use = 1;
    child->pid = next_pid++;
    child->ppid = ppid;
    child->state = PM_RUNNING;

    parent->children[parent->child_count++] = child->pid;

    snprintf(last_event, sizeof(last_event),
             "Thread %d calls pm_fork %d", tid, ppid);

    notify_change();
    pthread_mutex_unlock(&lock);
    return child->pid;
}

static void pm_exit(int pid, int status, int tid) {
    pthread_mutex_lock(&lock);

    pcb_t *p = find_pcb(pid);
    if (!p) {
        pthread_mutex_unlock(&lock);
        return;
    }

    if (p->state != PM_ZOMBIE) {
        p->state = PM_ZOMBIE;
        p->exit_status = status;
        pthread_cond_broadcast(&child_cv);
    }

    snprintf(last_event, sizeof(last_event),
             "Thread %d calls pm_exit %d %d", tid, pid, status);

    notify_change();
    pthread_mutex_unlock(&lock);
}

static int pm_wait(int ppid, int cpid, int tid) {
    pthread_mutex_lock(&lock);

    pcb_t *parent = find_pcb(ppid);
    if (!parent) {
        pthread_mutex_unlock(&lock);
        return -1;
    }

    if (parent->child_count == 0) {
        pthread_mutex_unlock(&lock);
        return -1;
    }

    if (cpid != -1) {
        int found = 0;
        for (int i = 0; i < parent->child_count; i++) {
            if (parent->children[i] == cpid) {
                found = 1;
                break;
            }
        }
        if (!found) {
            pthread_mutex_unlock(&lock);
            return -1;
        }
    }

    while (1) {
        for (int i = 0; i < parent->child_count; i++) {
            int id = parent->children[i];
            pcb_t *child = find_pcb(id);
            if (!child) continue;

            if ((cpid == -1 || child->pid == cpid) &&
                child->state == PM_ZOMBIE) {

                int status = child->exit_status;

                remove_child(parent, child->pid);
                memset(child, 0, sizeof(*child));

                snprintf(last_event, sizeof(last_event),
                         "Thread %d calls pm_wait %d %d", tid, ppid, cpid);

                notify_change();
                pthread_mutex_unlock(&lock);
                return status;
            }
        }

        parent->state = PM_WAITING;

        snprintf(last_event, sizeof(last_event),
                 "Thread %d calls pm_wait %d %d (blocking)", tid, ppid, cpid);

        notify_change();

        pthread_cond_wait(&child_cv, &lock);

        parent->state = PM_RUNNING;
    }
}

static void pm_kill(int pid, int tid) {
    pthread_mutex_lock(&lock);

    snprintf(last_event, sizeof(last_event),
             "Thread %d calls pm_kill %d", tid, pid);

    notify_change();
    pthread_mutex_unlock(&lock);

    pm_exit(pid, -1, tid);
}

/* monitor thread */

static void *monitor_thread(void *arg) {
    (void)arg;

    pthread_mutex_lock(&lock);

    fprintf(snap_fp, "%s\n", last_event);
    pm_ps(snap_fp);
    fprintf(snap_fp, "\n");
    fflush(snap_fp);

    while (!shutdown_flag) {
        while (!table_changed && !shutdown_flag)
            pthread_cond_wait(&monitor_cv, &lock);

        if (shutdown_flag) break;

        table_changed = 0;

        fprintf(snap_fp, "%s\n", last_event);
        pm_ps(snap_fp);
        fprintf(snap_fp, "\n");
        fflush(snap_fp);
    }

    pthread_mutex_unlock(&lock);
    return NULL;
}

/* worker thread */

typedef struct {
    int tid;
    const char *file;
} worker_arg;

static void *worker(void *arg) {
    worker_arg *wa = (worker_arg *)arg;

    FILE *fp = fopen(wa->file, "r");
    if (!fp) return NULL;

    char line[256];

    while (fgets(line, sizeof(line), fp)) {
        char cmd[32];
        if (sscanf(line, "%s", cmd) != 1) continue;

        if (strcmp(cmd, "fork") == 0) {
            int p;
            if (sscanf(line, "%*s %d", &p) == 1)
                pm_fork(p, wa->tid);
        }
        else if (strcmp(cmd, "exit") == 0) {
            int p, s;
            if (sscanf(line, "%*s %d %d", &p, &s) == 2)
                pm_exit(p, s, wa->tid);
        }
        else if (strcmp(cmd, "wait") == 0) {
            int p, c;
            if (sscanf(line, "%*s %d %d", &p, &c) == 2)
                pm_wait(p, c, wa->tid);
        }
        else if (strcmp(cmd, "kill") == 0) {
            int p;
            if (sscanf(line, "%*s %d", &p) == 1)
                pm_kill(p, wa->tid);
        }
        else if (strcmp(cmd, "sleep") == 0) {
            int ms;
            if (sscanf(line, "%*s %d", &ms) == 1)
                usleep(ms * 1000);
        }
    }

    fclose(fp);
    return NULL;
}

/* init */

static void pm_init() {
    memset(table, 0, sizeof(table));

    table[0].in_use = 1;
    table[0].pid = 1;
    table[0].ppid = 0;
    table[0].state = PM_RUNNING;
}

/* main */

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: %s thread0.txt ...\n", argv[0]);
        return 1;
    }

    snap_fp = fopen("snapshots.txt", "w");
    if (!snap_fp) {
        perror("snapshots.txt");
        return 1;
    }

    pm_init();

    pthread_t monitor;
    pthread_create(&monitor, NULL, monitor_thread, NULL);

    int n = argc - 1;
    pthread_t th[n];
    worker_arg args[n];

    for (int i = 0; i < n; i++) {
        args[i].tid = i;
        args[i].file = argv[i + 1];
        pthread_create(&th[i], NULL, worker, &args[i]);
    }

    for (int i = 0; i < n; i++)
        pthread_join(th[i], NULL);

    pthread_mutex_lock(&lock);
    shutdown_flag = 1;
    pthread_cond_signal(&monitor_cv);
    pthread_mutex_unlock(&lock);

    pthread_join(monitor, NULL);

    fclose(snap_fp);
    return 0;
}
