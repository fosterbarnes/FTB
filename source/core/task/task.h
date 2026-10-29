#pragma once

// Shared by a background task and the UI that started it, embedded as `state` in the task's data.
// The worker's return value becomes result; finished is set last, after a barrier, so once
// task_is_finished is true the UI may read result and everything the worker wrote. Set
// finished = true before a task first starts so "not running" reads as finished.
typedef struct task_state_s {
    volatile bool finished;
    volatile bool cancelled;
    Result result;
} task_state;

void task_init();
// Stops every task and waits (up to a few seconds) for the workers to end, so services can close.
void task_exit();
bool task_is_quit_all();
Handle task_get_pause_event();
Handle task_get_suspend_event();

// Runs func(arg) on a new thread. On failure state is finished, result stays 0, and func never runs.
Result task_start(task_state* state, Result (*func)(void* arg), void* arg, s32 priority, int core);
// UI side: read finished through this; it pairs with the worker's barrier.
bool task_is_finished(task_state* state);
// Worker side: waits while the console sleeps, then true once the task is cancelled or the app quits.
bool task_is_cancelled(task_state* state);
// UI side: asks the worker to stop at its next check.
void task_cancel(task_state* state);
// UI side: cancels and blocks until the worker finished; returns at once when it is not running.
void task_cancel_and_wait(task_state* state);

// Sleep stays blocked while any caller holds a block (data ops block it while they run).
void task_sleep_block();
void task_sleep_unblock();

#include "capturecam.h"
#include "dataop.h"
