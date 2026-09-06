/*
 * xorpool shared payout ("tally") mode. See datum_shared.h.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <pthread.h>
#include <sys/stat.h>
#include <jansson.h>

#include "datum_conf.h"
#include "datum_logger.h"
#include "datum_utils.h"
#include "datum_stratum.h"
#include "datum_blocktemplates.h"
#include "datum_shared.h"

#define SHARED_MAX_PAYEES 512
#define SHARED_FLUSH_MS 60000

typedef struct {
	unsigned char script[64];
	int script_len;
	uint64_t weight;
	char address[128];
} T_SHARED_PAYEE;

static T_SHARED_PAYEE payees[SHARED_MAX_PAYEES];
static int npayees = 0;
static uint64_t total_weight = 0;
static time_t loaded_mtime = 0;
static off_t loaded_size = -1;
static pthread_rwlock_t table_lock = PTHREAD_RWLOCK_INITIALIZER;
static pthread_mutex_t reload_lock = PTHREAD_MUTEX_INITIALIZER;

bool datum_shared_enabled(void) {
	return datum_config.mining_shared_payout_file[0] != 0;
}

static int payee_cmp(const void *a, const void *b) {
	const T_SHARED_PAYEE *pa = a, *pb = b;
	if (pa->weight > pb->weight) return -1;
	if (pa->weight < pb->weight) return 1;
	return strcmp(pa->address, pb->address);
}

// Parse the file into a fresh table. Returns payee count, or -1 on a parse error.
static int load_table(const char *path, T_SHARED_PAYEE *out, int max, uint64_t *total) {
	json_error_t err;
	json_t *root = json_load_file(path, 0, &err);
	if (!root) {
		DLOG_ERROR("xorpool shared: cannot parse %s: %s (line %d)", path, err.text, err.line);
		return -1;
	}
	json_t *arr = json_object_get(root, "payees");
	if (!json_is_array(arr)) {
		DLOG_ERROR("xorpool shared: %s has no \"payees\" array", path);
		json_decref(root);
		return -1;
	}
	int n = 0;
	uint64_t tw = 0;
	size_t i;
	json_t *item;
	json_array_foreach(arr, i, item) {
		const char *addr = json_string_value(json_object_get(item, "address"));
		json_t *w = json_object_get(item, "weight");
		if (!addr || !json_is_integer(w) || json_integer_value(w) <= 0) continue;
		if (n >= max) { DLOG_WARN("xorpool shared: more than %d payees in %s; the rest are ignored", max, path); break; }
		T_SHARED_PAYEE *p = &out[n];
		memset(p, 0, sizeof(*p));
		int l = addr_2_output_script(addr, p->script, sizeof(p->script));
		if (l <= 0 || l > 64) { DLOG_WARN("xorpool shared: skipping payee with invalid address %s", addr); continue; }
		p->script_len = l;
		p->weight = (uint64_t)json_integer_value(w);
		strncpy(p->address, addr, sizeof(p->address) - 1);
		tw += p->weight;
		n++;
	}
	json_decref(root);
	qsort(out, n, sizeof(T_SHARED_PAYEE), payee_cmp);
	*total = tw;
	return n;
}

void datum_shared_reload(void) {
	if (!datum_shared_enabled()) return;
	const char *path = datum_config.mining_shared_payout_file;
	struct stat st;
	static T_SHARED_PAYEE fresh[SHARED_MAX_PAYEES];

	pthread_mutex_lock(&reload_lock);
	if (stat(path, &st) != 0) {
		if (npayees || loaded_size != 0) {
			DLOG_WARN("xorpool shared: payout file %s is missing; paying the whole coinbase to pool_address until it appears", path);
			pthread_rwlock_wrlock(&table_lock);
			npayees = 0; total_weight = 0;
			pthread_rwlock_unlock(&table_lock);
			loaded_size = 0; loaded_mtime = 0;
		}
		pthread_mutex_unlock(&reload_lock);
		return;
	}
	if (st.st_mtime == loaded_mtime && st.st_size == loaded_size) {
		pthread_mutex_unlock(&reload_lock);
		return;
	}
	uint64_t tw = 0;
	int n = load_table(path, fresh, SHARED_MAX_PAYEES, &tw);
	if (n >= 0) {
		pthread_rwlock_wrlock(&table_lock);
		memcpy(payees, fresh, sizeof(T_SHARED_PAYEE) * (size_t)n);
		npayees = n; total_weight = tw;
		pthread_rwlock_unlock(&table_lock);
		DLOG_INFO("xorpool shared: payout table loaded: %d payees, total weight %" PRIu64 "%s", n, tw, n ? "" : " (empty: coinbase pays pool_address)");
	}
	// remember the file state even when parsing failed, so a broken file is not reparsed every job
	loaded_mtime = st.st_mtime; loaded_size = st.st_size;
	pthread_mutex_unlock(&reload_lock);
}

int datum_shared_init(void) {
	if (!datum_shared_enabled()) return 0;
	if (datum_config.mining_per_miner_payout) {
		DLOG_FATAL("xorpool shared: mining.shared_payout_file and mining.per_miner_payout cannot both be set");
		return -1;
	}
	if (datum_config.datum_pool_host[0]) {
		DLOG_FATAL("xorpool shared: shared payout is for non-pooled mode only; clear datum.pool_host");
		return -1;
	}
	if (datum_config.mining_pool_fee_bps < 0 || datum_config.mining_pool_fee_bps > 5000) {
		DLOG_FATAL("xorpool shared: mining.pool_fee_bps must be between 0 and 5000");
		return -1;
	}
	unsigned char tmp[64];
	if (addr_2_output_script(datum_config.mining_pool_address, tmp, sizeof(tmp)) <= 0) {
		DLOG_FATAL("xorpool shared: mining.pool_address is not a valid address");
		return -1;
	}
	datum_shared_reload();
	DLOG_INFO("xorpool shared: enabled, table %s, fee %d bps, min payout %d sats", datum_config.mining_shared_payout_file, datum_config.mining_pool_fee_bps, datum_config.mining_shared_min_payout_sats);
	return 0;
}

static int hex_output(char *dst, uint64_t sats, const unsigned char *script, int script_len) {
	int n = sprintf(dst, "%016llx", (unsigned long long)__builtin_bswap64(sats));
	n += append_bitcoin_varint_hex((uint64_t)script_len, dst + n);
	for (int i = 0; i < script_len; i++) { uchar_to_hex(dst + n, script[i]); n += 2; }
	return n;
}

bool datum_shared_write_coinb2(T_DATUM_STRATUM_JOB *s, T_DATUM_STRATUM_COINBASE *cb, uint64_t value, bool with_witness, int max_bytes) {
	if (!datum_shared_enabled()) return false;

	pthread_rwlock_rdlock(&table_lock);
	if (!npayees || !total_weight) {
		pthread_rwlock_unlock(&table_lock);
		return false;
	}

	const uint64_t fee = (uint64_t)(((unsigned __int128)value * (uint64_t)datum_config.mining_pool_fee_bps) / 10000);
	const uint64_t payable = value - fee;
	const uint64_t min_sats = datum_config.mining_shared_min_payout_sats > 0 ? (uint64_t)datum_config.mining_shared_min_payout_sats : 1;

	// fixed bytes: sequence(4) + count(up to 3) + pool output(9+len) + witness(46) + locktime(4)
	const int witness_bytes = with_witness ? (8 + 1 + (int)(strlen(s->block_template->default_witness_commitment) >> 1)) : 0;
	int budget = max_bytes - (4 + 3 + 9 + s->pool_addr_script_len + witness_bytes + 4);

	// first pass: decide which payees fit, largest weight first
	static __thread uint64_t sats_of[SHARED_MAX_PAYEES];
	int included = 0, dropped_small = 0, dropped_space = 0;
	uint64_t paid = 0;
	for (int k = 0; k < npayees; k++) {
		uint64_t sats = (uint64_t)(((unsigned __int128)payable * payees[k].weight) / total_weight);
		if (sats < min_sats) { dropped_small++; sats_of[k] = 0; continue; }
		int need = 8 + 1 + payees[k].script_len;
		if (need > budget) { dropped_space++; sats_of[k] = 0; continue; }
		budget -= need;
		sats_of[k] = sats;
		paid += sats;
		included++;
	}
	if (paid > value) { // cannot happen, but never build a coinbase that overpays
		pthread_rwlock_unlock(&table_lock);
		DLOG_ERROR("xorpool shared: payout table would overpay (%" PRIu64 " > %" PRIu64 "); paying pool_address only", paid, value);
		return false;
	}

	// second pass: write coinb2 = sequence, count, payee outputs, pool remainder, [witness], locktime
	char *o = cb->coinb2;
	int n = 0;
	n += sprintf(o + n, "ffffffff");
	n += append_bitcoin_varint_hex((uint64_t)included + 1 + (with_witness ? 1 : 0), o + n);
	for (int k = 0; k < npayees; k++) {
		if (!sats_of[k]) continue;
		n += hex_output(o + n, sats_of[k], payees[k].script, payees[k].script_len);
	}
	n += hex_output(o + n, value - paid, s->pool_addr_script, s->pool_addr_script_len);
	if (with_witness) {
		n += sprintf(o + n, "0000000000000000%2.2x%s", (unsigned int)strlen(s->block_template->default_witness_commitment) >> 1, s->block_template->default_witness_commitment);
	}
	n += sprintf(o + n, "00000000");
	o[n] = 0;
	pthread_rwlock_unlock(&table_lock);

	// keep the binary copy in step with the hex
	cb->coinb2_len = 0;
	for (int j = 0; j < n; j += 2) { cb->coinb2_bin[j >> 1] = hex2bin_uchar(&cb->coinb2[j]); cb->coinb2_len++; }

	DLOG_DEBUG("xorpool shared: height %" PRIu64 " %s: %d payees paid %" PRIu64 " of %" PRIu64 " sats (fee %" PRIu64 ", %d below min, %d out of space), coinb2 %d bytes",
		s->height, with_witness ? "full" : "subsidy-only", included, paid, value, value - paid, dropped_small, dropped_space, cb->coinb2_len);
	return true;
}

void datum_shared_flush(T_DATUM_MINER_DATA *m, const char *why) {
	if (!m || !m->shared_diff_unflushed) return;
	DLOG_INFO("xorpool: shares %s %" PRIu64 " (%s)", m->last_auth_username, m->shared_diff_unflushed, why);
	m->shared_diff_unflushed = 0;
}

void datum_shared_note_accepted(T_DATUM_MINER_DATA *m, uint64_t diff, uint64_t now_tsms) {
	if (!datum_shared_enabled()) return;
	m->shared_diff_unflushed += diff;
	if (!m->shared_flush_tsms) m->shared_flush_tsms = now_tsms;
	if (now_tsms >= m->shared_flush_tsms + SHARED_FLUSH_MS) {
		datum_shared_flush(m, "minute");
		m->shared_flush_tsms = now_tsms;
	}
}
