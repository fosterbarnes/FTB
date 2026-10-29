#include <malloc.h>

#include <3ds.h>

#include "task.h"
#include "../error.h"

#define TASK_STACK_SIZE 0x10000
#define TASK_EXIT_TIMEOUT_MS 5000

static volatile bool task_quit;

static Handle task_pause_event;
static Handle task_suspend_event;

static aptHookCookie cookie;

// Running workers (for task_exit) and sleep blocks, both under task_lock.
static LightLock task_lock;
static CondVar task_idle;
static u32 task_running;
static u32 task_sleep_blocks;

static void task_apt_hook(APT_HookType hook, void* param) {
    switch(hook) {
        case APTHOOK_ONRESTORE:
            svcSignalEvent(task_suspend_event);
            __attribute__((fallthrough)); // a restored app is awake too
        case APTHOOK_ONWAKEUP:
            svcSignalEvent(task_pause_event);
            break;
        case APTHOOK_ONSUSPEND:
            svcClearEvent(task_suspend_event);
            __attribute__((fallthrough)); // a suspended app is paused too
        case APTHOOK_ONSLEEP:
            svcClearEvent(task_pause_event);
            break;
        default:
            break;
    }
}

void task_init() {
    task_quit = false;

    LightLock_Init(&task_lock);
    CondVar_Init(&task_idle);
    task_running = 0;
    task_sleep_blocks = 0;

    Result res = 0;

    if(R_FAILED(res = svcCreateEvent(&task_pause_event, RESET_STICKY))) {
        error_panic("Failed to create task pause event: 0x%08lX", res);
        return;
    }

    if(R_FAILED(res = svcCreateEvent(&task_suspend_event, RESET_STICKY))) {
        svcCloseHandle(task_pause_event);

        error_panic("Failed to create task suspend event: 0x%08lX", res);
        return;
    }

    svcSignalEvent(task_pause_event);
    svcSignalEvent(task_suspend_event);

    aptHook(&cookie, task_apt_hook, NULL);
}

void task_exit() {
    task_quit = true;

    aptUnhook(&cookie);

    // A paused worker would never see the quit flag.
    if(task_pause_event != 0) {
        svcSignalEvent(task_pause_event);
    }

    // Workers stop at their next cancel check (an install cancels through AM_CancelCIAInstall).
    // One blocked on a prompt nobody can answer any more must not keep the app from closing.
    u64 deadline = osGetTime() + TASK_EXIT_TIMEOUT_MS;

    LightLock_Lock(&task_lock);
    while(task_running > 0) {
        u64 now = osGetTime();
        if(now >= deadline) {
            break;
        }

        CondVar_WaitTimeout(&task_idle, &task_lock, (s64) (deadline - now) * 1000000);
    }
    LightLock_Unlock(&task_lock);

    if(task_pause_event != 0) {
        svcCloseHandle(task_pause_event);
        task_pause_event = 0;
    }

    if(task_suspend_event != 0) {
        svcCloseHandle(task_suspend_event);
        task_suspend_event = 0;
    }
}

bool task_is_quit_all() {
    return task_quit;
}

Handle task_get_pause_event() {
    return task_pause_event;
}

Handle task_get_suspend_event() {
    return task_suspend_event;
}

typedef struct {
    task_state* state;
    Result (*func)(void* arg);
    void* arg;
} task_start_data;

static void task_thread(void* arg) {
    task_start_data start = *(task_start_data*) arg;
    free(arg);

    start.state->result = start.func(start.arg);

    // The owner may free the state once it sees finished, so nothing touches it after this.
    __dmb();
    start.state->finished = true;

    LightLock_Lock(&task_lock);
    if(--task_running == 0) {
        CondVar_Broadcast(&task_idle);
    }
    LightLock_Unlock(&task_lock);
}

Result task_start(task_state* state, Result (*func)(void* arg), void* arg, s32 priority, int core) {
    state->finished = false;
    state->cancelled = false;
    state->result = 0;

    task_start_data* start = (task_start_data*) malloc(sizeof(task_start_data));
    if(start == NULL) {
        state->finished = true;
        return R_APP_OUT_OF_MEMORY;
    }

    start->state = state;
    start->func = func;
    start->arg = arg;

    LightLock_Lock(&task_lock);
    task_running++;
    LightLock_Unlock(&task_lock);

    if(threadCreate(task_thread, start, TASK_STACK_SIZE, priority, core, true) == NULL) {
        LightLock_Lock(&task_lock);
        if(--task_running == 0) {
            CondVar_Broadcast(&task_idle);
        }
        LightLock_Unlock(&task_lock);

        free(start);
        state->finished = true;
        return R_APP_THREAD_CREATE_FAILED;
    }

    return 0;
}

bool task_is_finished(task_state* state) {
    bool finished = state->finished;
    if(finished) {
        __dmb();
    }

    return finished;
}

bool task_is_cancelled(task_state* state) {
    svcWaitSynchronization(task_pause_event, U64_MAX);
    return task_quit || state->cancelled;
}

void task_cancel(task_state* state) {
    state->cancelled = true;
}

void task_cancel_and_wait(task_state* state) {
    state->cancelled = true;

    while(!task_is_finished(state)) {
        svcSleepThread(1000000);
    }
}

void task_sleep_block() {
    LightLock_Lock(&task_lock);
    if(task_sleep_blocks++ == 0) {
        aptSetSleepAllowed(false);
    }
    LightLock_Unlock(&task_lock);
}

void task_sleep_unblock() {
    LightLock_Lock(&task_lock);
    if(task_sleep_blocks > 0 && --task_sleep_blocks == 0) {
        aptSetSleepAllowed(true);
    }
    LightLock_Unlock(&task_lock);
}
