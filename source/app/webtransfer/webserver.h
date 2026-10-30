#pragma once

#include "../../core/linkedlist.h"
#include "../../core/task/dataop.h"

// Web Transfer's worker: serves the Web Transfer page (HTTP on 80, or 8080 when 80 is taken) and its
// API: SD listings and downloads, upload pre-checks, and uploads. It serves one connection at a time
// and runs each transfer through the data op engine on its own thread. The screen (webtransfer.c)
// only draws this state and asks for cancels. FBI's port 5000 URL protocol is the FBI Legacy screen
// (legacy/receiveurls.c).

typedef struct web_job_s web_job;

typedef struct {
    task_state state; // the worker; task_cancel_and_wait stops it

    // Written by the worker, read by the UI each frame.
    volatile u32 ip; // 0 while there is no network
    volatile u16 webPort; // 0 while the page is not served

    // The running transfer: job is set while op runs and only changes under jobLock, so the UI may
    // read the job's peer and item names while it holds the lock. task_cancel(&op.state) stops it.
    LightLock jobLock;
    web_job* job;
    data_op_data op;

    // Log rows (calloc'd list_item) for the UI, oldest first; add and take only under logLock.
    LightLock logLock;
    linked_list rows;

    // The page served at "/", owned by the screen.
    const char* page;
    u32 pageSize;
} web_state;

// Call with state zeroed except page; inits the locks and the row list, then starts the worker.
Result webserver_start(web_state* state);
// UI side, under jobLock: what the running transfer is moving ("From 192.168.1.20: a.cia").
void webserver_job_line(const web_job* job, u32 index, char* out, size_t size);
