#pragma once

#include "../../core/linkedlist.h"
#include "../../core/task/dataop.h"

// Receive's worker: listens for FBI's URL protocol (TCP 5000), the Receive page (HTTP on 80, or 8080
// when 80 is taken), and ftbsend.py's discovery broadcast (UDP 5000). It serves one connection at a
// time and runs each accepted batch on its own thread through the data op engine. The screen
// (receive.c) only draws this state and asks for cancels.

typedef struct receive_job_s receive_job;

typedef struct {
    task_state state; // the worker; task_cancel_and_wait stops it

    // Written by the worker, read by the UI each frame.
    volatile u32 ip; // 0 while there is no network
    volatile u16 webPort; // 0 while the page is not served
    volatile bool fbiListening;

    // The running batch: job is set while op runs and only changes under jobLock, so the UI may read
    // the job's sender and item names while it holds the lock. task_cancel(&op.state) stops the batch.
    LightLock jobLock;
    receive_job* job;
    data_op_data op;

    // Log rows (calloc'd list_item) for the UI, oldest first; add and take only under logLock.
    LightLock logLock;
    linked_list rows;

    // The page served at "/", owned by the screen.
    const char* page;
    u32 pageSize;
} receive_state;

// Call with state zeroed except page; inits the locks and the row list, then starts the worker.
Result receiver_start(receive_state* state);
// UI side, under jobLock: the running batch's sender ("192.168.1.20") and current item name.
const char* receiver_job_from(const receive_job* job);
const char* receiver_job_item_name(const receive_job* job, u32 index);
