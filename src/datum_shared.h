/*
 * xorpool shared payout ("tally") mode for DATUM Gateway, non-pooled operation only.
 *
 * A ledger process outside the gateway keeps a PPLNS-style window of accepted share
 * difficulty per payout address and writes it as JSON:
 *
 *   {"payees": [{"address": "bc1q...", "weight": 123456}, ...]}
 *
 * Whenever a job is built the gateway rereads that file (if it changed) and pays the
 * whole coinbase to those addresses in proportion to weight, minus mining.pool_fee_bps
 * to mining.pool_address. Every miner on the gateway gets the identical coinbase, so
 * a block found by anyone pays everyone in the window straight from the coinbase.
 * Nothing is ever held by the pool.
 *
 * The gateway also writes one log line per miner per minute (and on disconnect):
 *   xorpool: shares <username> <accepted difficulty since last line>
 * which is what the ledger consumes.
 */
#ifndef _DATUM_SHARED_H_
#define _DATUM_SHARED_H_

#include <stdbool.h>
#include <stdint.h>
#include "datum_stratum.h"

bool datum_shared_enabled(void);

// Validate config and load the table once. Returns 0 on success.
int datum_shared_init(void);

// Reread the payout file if its mtime changed. Cheap; call once per job build.
void datum_shared_reload(void);

// Replace cb->coinb2 with the shared payout outputs paying `value` sats in total.
// cb->coinb1 must be complete and end with the in-coinbase extranonce push
// (space_for_en_in_coinbase). max_bytes caps the binary size of coinb2.
// Returns false (and leaves cb untouched) if the table is empty.
bool datum_shared_write_coinb2(T_DATUM_STRATUM_JOB *s, T_DATUM_STRATUM_COINBASE *cb, uint64_t value, bool with_witness, int max_bytes);

// Share accounting log lines for the ledger.
void datum_shared_note_accepted(T_DATUM_MINER_DATA *m, uint64_t diff, uint64_t now_tsms);
void datum_shared_flush(T_DATUM_MINER_DATA *m, const char *why);

#endif
