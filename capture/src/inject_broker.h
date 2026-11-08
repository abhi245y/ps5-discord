// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "inject.h"

#define SL_RPC_BYTES 0x100000u
struct sl_rpc;
typedef struct sl_broker {
    struct sl_rpc *shared;
    int channel[2];
    sl_inject context; // PRIVATE supervisor memory: worker only receives a diagnostic view
    unsigned sequence;
    char stage[64];
} sl_broker;

enum sl_rpc_op { SL_RPC_BEGIN = 1, SL_RPC_READ, SL_RPC_WRITE, SL_RPC_CODE, SL_RPC_CALL, SL_RPC_END, SL_RPC_STOP, SL_RPC_CLEANUP_CALL };
int sl_broker_open(sl_broker *b);
void sl_broker_worker(sl_broker *b);
void sl_broker_parent(sl_broker *b);
int sl_broker_poll(sl_broker *b, int timeout_ms); // one whole operation, 0 idle, -1 closed/invalid
int sl_broker_recover(sl_broker *b); // worker must be reaped first; same private context and tracer
int sl_broker_close(sl_broker *b); // refuses to discard an attached target
int sl_broker_client(void);
int64_t sl_broker_request(sl_inject *view, enum sl_rpc_op op, intptr_t address, void *bytes,
                          size_t len, const uint64_t args[6], int save_fp, const char *name);
