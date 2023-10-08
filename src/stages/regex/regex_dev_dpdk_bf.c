/*
 * Copyright (c) 2021-2022 NVIDIA CORPORATION & AFFILIATES, ALL RIGHTS RESERVED.
 *
 * This software product is a proprietary product of NVIDIA CORPORATION &
 * AFFILIATES (the "Company") and all right, title, and interest in and to the
 * software product, including all associated intellectual property rights, are
 * and shall remain exclusively with the Company.
 *
 * This software product is governed by the End User License Agreement
 * provided with the software product.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <pthread.h>

#include <rte_malloc.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_regexdev.h>
#include <rte_timer.h>


#include "regex_dev.h"
#include "regex_bf.h"
#include "rules_file_utils.h"

#include "../../runtime/meili_runtime.h"
#include "../../packet_timestamping/packet_timestamping.h"

#include "../../utils/dpdk_live_shared.h"
#include "../utils/utils.h"
#include "../../utils/utils_temp.h"

/* Number of dpdk queue descriptors is 1024 so need more mbuf pool entries. */
// #define MBUF_POOL_SIZE		     2047 /* Should be n = (2^q - 1)*/
// #define MBUF_CACHE_SIZE		     256
// #define MBUF_SIZE		     (1 << 8)

/* add missing definitions */
#define 	RTE_REGEX_OPS_RSP_RESOURCE_LIMIT_REACHED_F   (1 << 4)

/* Mbuf has 9 dynamic entries (dynfield1) that we can use. */
#define DF_USER_ID_HIGH		     0
#define DF_USER_ID_LOW		     1
#define DF_TIME_HIGH		     2
#define DF_TIME_LOW		     3
#define DF_PAY_OFF		     4
#define DF_EGRESS_PORT		     5

bool regex_bf_initialized;
pthread_mutex_t mutex_regex_bf = PTHREAD_MUTEX_INITIALIZER;


struct per_core_globals {
	union {
		struct {
			uint64_t last_idle_time;
			uint64_t total_enqueued;
			uint64_t total_dequeued;
			uint64_t buf_id;
			uint16_t op_offset;
		};
		unsigned char cache_align[CACHE_LINE_SIZE];
	};
};

//static struct rte_mbuf_ext_shared_info shinfo;
static struct per_core_globals *core_vars;
/* First 'batch' ops_arr_tx entries are queue 0, next queue 1 etc. */
static struct rte_regex_ops **ops_arr_tx;
static struct rte_regex_ops **ops_arr_rx;
//static struct rte_mempool **mbuf_pool;
static uint8_t regex_dev_id;
static int max_batch_size;
static bool verbose;
static char *rules;

/* Job format specific arrays. */
static uint16_t **input_subset_ids;
static uint64_t *input_job_ids;
static uint32_t input_total_jobs;
static exp_matches_t *input_exp_matches;

static bool lat_mode;

static void regex_dev_dpdk_bf_clean(rb_conf *run_conf);


static int
regex_dev_dpdk_bf_config(rb_conf *run_conf, uint8_t dev_id, struct rte_regexdev_config *dev_cfg, const char *rules_file,
			 int num_queues)
{
	struct rte_regexdev_info dev_info = {};
	struct rte_regexdev_qp_conf qp_conf;
	uint64_t rules_len;
	int ret, i;

	memset(dev_cfg, 0, sizeof(*dev_cfg));
	memset(&qp_conf, 0, sizeof(qp_conf));
	qp_conf.nb_desc = 1024;
	/* Accept out of order results. */
	qp_conf.qp_conf_flags = RTE_REGEX_QUEUE_PAIR_CFG_OOS_F;

	ret = rte_regexdev_info_get(dev_id, &dev_info);
	if (ret) {
		MEILI_LOG_ERR("Failed to get BF regex device info.");
		return -EINVAL;
	}

	/*
	 * Note that the currently targeted DPDK version does not support card
	 * configuration of variables/flags (config warns on unsupported
	 * inputs, e.g. rxp-max-matches).
	 * Therefore, the default values will be added to dev_cfg in the
	 * following code.
	 */

	if (num_queues > dev_info.max_queue_pairs) {
		MEILI_LOG_ERR("Requested queues/cores (%d) exceeds device max (%d)", num_queues,
			     dev_info.max_queue_pairs);
		return -EINVAL;
	}

	if (run_conf->rxp_max_matches > dev_info.max_matches) {
		MEILI_LOG_ERR("Requested max matches > device supports.");
		return -EINVAL;
	}
	dev_cfg->nb_max_matches = run_conf->rxp_max_matches ? run_conf->rxp_max_matches : dev_info.max_matches;
	run_conf->rxp_max_matches = dev_cfg->nb_max_matches;
	dev_cfg->nb_queue_pairs = num_queues;
	dev_cfg->nb_groups = 1;
	dev_cfg->nb_rules_per_group = dev_info.max_rules_per_group;

	if (dev_info.regexdev_capa & RTE_REGEXDEV_SUPP_MATCH_AS_END_F)
		dev_cfg->dev_cfg_flags |= RTE_REGEXDEV_CFG_MATCH_AS_END_F;

	/* Load in rules file. */
	ret = util_load_file_to_buffer(rules_file, &rules, &rules_len, 0);
	if (ret) {
		MEILI_LOG_ERR("Failed to read in rules file.");
		return ret;
	}

	dev_cfg->rule_db = rules;
	dev_cfg->rule_db_len = rules_len;

	MEILI_LOG_INFO("Programming card memories....");
	/* Configure will program the rules to the card. */
	ret = rte_regexdev_configure(dev_id, dev_cfg);
	if (ret) {
		MEILI_LOG_ERR("Failed to configure BF regex device.");
		rte_free(rules);
		return ret;
	}
	MEILI_LOG_INFO("Card configured");

	for (i = 0; i < num_queues; i++) {
		ret = rte_regexdev_queue_pair_setup(dev_id, i, &qp_conf);
		if (ret) {
			MEILI_LOG_ERR("Failed to configure queue pair %u on dev %u.", i, dev_id);
			rte_free(rules);
			return ret;
		}
	}

	return 0;
}

static int
regex_dev_init_ops(rb_conf *run_conf,int batch_size, int max_matches, int num_queues)
{
	size_t per_core_var_sz;
	int match_mem_size;
	int num_entries;
	char pool_n[50];
	int i;

	/* Set all to NULL to ensure cleanup doesn't free unallocated memory. */
	ops_arr_tx = NULL;
	ops_arr_rx = NULL;
	run_conf->mbuf_pool = NULL;
	core_vars = NULL;
	verbose = false;

	num_entries = batch_size * num_queues;

	/* Allocate space for rx/tx batches per core/queue. */
	ops_arr_tx = rte_malloc(NULL, sizeof(*ops_arr_tx) * num_entries, 0);
	if (!ops_arr_tx)
		goto err_out;

	ops_arr_rx = rte_malloc(NULL, sizeof(*ops_arr_rx) * num_entries, 0);
	if (!ops_arr_rx)
		goto err_out;

	/* Size of rx regex_ops is extended by potentially MAX match fields. */
	match_mem_size = max_matches * sizeof(struct rte_regexdev_match);
	for (i = 0; i < num_entries; i++) {
		ops_arr_tx[i] = rte_malloc(NULL, sizeof(*ops_arr_tx[0]), 0);
		if (!ops_arr_tx[i])
			goto err_out;

		ops_arr_rx[i] = rte_malloc(NULL, sizeof(*ops_arr_rx[0]) + match_mem_size, 0);
		if (!ops_arr_rx[i])
			goto err_out;
	}

	/* Create mbuf pool for each queue. */
	// run_conf->mbuf_pool = rte_malloc(NULL, sizeof(*(run_conf->mbuf_pool)) * num_queues, 0);
	// if (!run_conf->mbuf_pool)
	// 	goto err_out;

	// for (i = 0; i < num_queues; i++) {
	// 	sprintf(pool_n, "REGEX_MUF_POOL_%u", i);
	// 	/* Pool size should be > dpdk descriptor queue. */
	// 	run_conf->mbuf_pool[i] =
	// 		rte_pktmbuf_pool_create(pool_n, MBUF_POOL_SIZE, MBUF_CACHE_SIZE, 0, MBUF_SIZE, rte_socket_id());
	// 	if (!run_conf->mbuf_pool[i]) {
	// 		MEILI_LOG_ERR("Failed to create mbuf pool.");
	// 		goto err_out;
	// 	}
	// }
	
	// run_conf->shinfo.free_cb = extbuf_free_cb;
	max_batch_size = batch_size;

	/* Maintain global variables operating on each queue (per lcore). */
	per_core_var_sz = sizeof(struct per_core_globals) * num_queues;
	core_vars = rte_zmalloc(NULL, per_core_var_sz, 64);
	if (!core_vars)
		goto err_out;

	return 0;

err_out:
	/* Clean happens in calling function. */
	MEILI_LOG_ERR("Mem failure initiating dpdk ops.");

	return -ENOMEM;
}

static int
regex_dev_dpdk_bf_init(rb_conf *run_conf)
{
	const int num_queues = run_conf->cores;
	struct rte_regexdev_config dev_cfg;
	rxp_stats_t *stats;
	regex_dev_id = 0;
	int ret = 0;
	int i;

	/* Current implementation supports a single regex device */
	if (rte_regexdev_count() != 1) {
		MEILI_LOG_ERR("%u regex devices detected - should be 1.", rte_regexdev_count());
		return -ENOTSUP;
	}

	ret = regex_dev_dpdk_bf_config(run_conf, regex_dev_id, &dev_cfg, run_conf->compiled_rules_file, num_queues);
	if (ret)
		return ret;

	ret = regex_dev_init_ops(run_conf, run_conf->input_batches, dev_cfg.nb_max_matches, num_queues);
	if (ret) {
		regex_dev_dpdk_bf_clean(run_conf);
		return ret;
	}

	verbose = run_conf->verbose;
	if (verbose) {
		ret = regex_dev_open_match_file(run_conf);
		if (ret) {
			regex_dev_dpdk_bf_clean(run_conf);
			return ret;
		}
	}

	/* Init min latency stats to large value. */
	// for (i = 0; i < num_queues; i++) {
	// 	stats = (rxp_stats_t *)(run_conf->stats->regex_stats[i].custom);
	// 	stats->min_lat = UINT64_MAX;
	// }

	/* Grab a copy of job format specific arrays. */
	input_subset_ids = run_conf->input_subset_ids;
	input_job_ids = run_conf->input_job_ids;
	input_total_jobs = run_conf->input_len_cnt;
	input_exp_matches = run_conf->input_exp_matches;

	lat_mode = run_conf->latency_mode;

	return ret;
}

static void
regex_dev_dpdk_bf_release_mbuf_deprecated(struct rte_mbuf *mbuf, regex_stats_t *stats, uint64_t recv_time, int sample)
{
	rxp_stats_t *rxp_stats = (rxp_stats_t *)stats->custom;
	uint64_t time_mbuf, time_diff;

	/* Calculate and store latency of packet through HW. */
	time_mbuf = util_get_64_bit_from_2_32(&mbuf->dynfield1[DF_TIME_HIGH]);

	time_diff = (recv_time - time_mbuf);

	rxp_stats->tot_lat += time_diff;
	if (time_diff < rxp_stats->min_lat)
		rxp_stats->min_lat = time_diff;
	if (time_diff > rxp_stats->max_lat)
		rxp_stats->max_lat = time_diff;
	// if(sample){
	// 	rxp_stats->time_diff_sample[nb_sampled%NUMBER_OF_SAMPLE] = time_diff;
	// 	nb_sampled++;
	// }

	/* Mbuf refcnt will be 1 if created by local mempool. */
	if (rte_mbuf_refcnt_read(mbuf) == 1) {
		rte_pktmbuf_detach_extbuf(mbuf);
		rte_pktmbuf_free(mbuf);
	} else {
		/* Packet is created elsewhere - may have to update data ptr. */
		if (mbuf->dynfield1[DF_PAY_OFF])
			rte_pktmbuf_prepend(mbuf, mbuf->dynfield1[DF_PAY_OFF]);

		rte_mbuf_refcnt_update(mbuf, -1);
	}
}

static inline int
regex_dev_dpdk_bf_get_array_offset(uint64_t job_id)
{
	/* Job ids start at 1 while array starts at 0 so need to decrement before wrap. */
	return (job_id - 1) % input_total_jobs;
}

static void
regex_dev_dpdk_bf_matches(int qid, struct rte_mbuf *mbuf, uint16_t num_matches, struct rte_regexdev_match *matches)
{
	uint64_t job_id;
	uint16_t offset;
	char *data;
	int i;

	/* Extract job id from mbuf metadata. */
	job_id = util_get_64_bit_from_2_32(&mbuf->dynfield1[DF_USER_ID_HIGH]);

	/* May have to convert the incrementing rule id to user input ID. */
	if (input_job_ids)
		job_id = input_job_ids[regex_dev_dpdk_bf_get_array_offset(job_id)];

	for (i = 0; i < num_matches; i++) {
		offset = matches[i].start_offset;
		data = rte_pktmbuf_mtod_offset(mbuf, char *, offset);
		regex_dev_write_to_match_file(qid, job_id, matches[i].rule_id, offset, matches[i].len, data);
	}
}

static void
regex_dev_dpdk_bf_exp_matches(struct rte_regex_ops *resp, rxp_stats_t *rxp_stats, bool max)
{
	const uint16_t num_matches = resp->nb_matches;
	struct rte_mbuf *mbuf = resp->user_ptr;
	exp_match_t actual_match[num_matches];
	struct rte_regexdev_match *matches;
	exp_matches_t actual_matches;
	rxp_exp_match_stats_t *stats;
	exp_matches_t *exp_matches;
	uint64_t job_id;
	uint16_t i;

	/* Copy matches to shared type - exp matches are for validation so perf is not a priority here. */
	matches = resp->matches;
	for (i = 0; i < num_matches; i++) {
		actual_match[i].rule_id = matches[i].rule_id;
		actual_match[i].start_ptr = matches[i].start_offset;
		actual_match[i].length = matches[i].len;
	}

	actual_matches.num_matches = num_matches;
	actual_matches.matches = &actual_match[0];

	stats = max ? &rxp_stats->max_exp : &rxp_stats->exp;
	job_id = util_get_64_bit_from_2_32(&mbuf->dynfield1[DF_USER_ID_HIGH]);
	exp_matches = &input_exp_matches[regex_dev_dpdk_bf_get_array_offset(job_id)];

	regex_dev_verify_exp_matches(exp_matches, &actual_matches, stats);
}

static void
regex_dev_dpdk_bf_process_resp(int qid, struct rte_regex_ops *resp, regex_stats_t *stats)
{
	rxp_stats_t *rxp_stats = (rxp_stats_t *)stats->custom;
	const uint16_t res_flags = resp->rsp_flags;

	/* Only DPDK error flags are supported on BF dev. */
	if (res_flags) {
		if (res_flags & RTE_REGEX_OPS_RSP_MAX_SCAN_TIMEOUT_F)
			rxp_stats->rx_timeout++;
		else if (res_flags & RTE_REGEX_OPS_RSP_MAX_MATCH_F)
			rxp_stats->rx_max_match++;
		else if (res_flags & RTE_REGEX_OPS_RSP_MAX_PREFIX_F)
			rxp_stats->rx_max_prefix++;
		else if (res_flags & RTE_REGEX_OPS_RSP_RESOURCE_LIMIT_REACHED_F)
			rxp_stats->rx_resource_limit++;
		rxp_stats->rx_invalid++;

		/* Still check expected matches if job failed. */
		if (input_exp_matches)
			regex_dev_dpdk_bf_exp_matches(resp, rxp_stats, res_flags);

		return;
	}

	stats->rx_valid++;

	const uint16_t num_matches = resp->nb_matches;
	if (num_matches) {
		stats->rx_buf_match_cnt++;
		stats->rx_total_match += num_matches;

		if (verbose)
			regex_dev_dpdk_bf_matches(qid, resp->user_ptr, num_matches, resp->matches);
	}

	if (input_exp_matches)
		regex_dev_dpdk_bf_exp_matches(resp, rxp_stats, res_flags);
}

static void
regex_dev_dpdk_bf_dequeue_deprecated(int qid, regex_stats_t *stats, bool live, dpdk_egress_t *dpdk_tx, uint16_t wait_on_dequeue)
{
	rxp_stats_t *rxp_stats = (rxp_stats_t *)stats->custom;
	int q_offset = qid * max_batch_size;
	struct rte_regex_ops **ops;
	uint16_t tot_dequeued = 0;
	int port1_cnt, port2_cnt;
	struct rte_mbuf *mbuf;
	uint16_t num_dequeued;
	int egress_idx;
	uint64_t time;
	int i;
	int sample = 0;

	/* Determine rx ops for this queue/lcore. */
	ops = &ops_arr_rx[q_offset];

	/* Poll the device until no more matches are received. */
	do {
		if (live) {
			port1_cnt = dpdk_tx->port_cnt[PRIM_PORT_IDX];
			port2_cnt = dpdk_tx->port_cnt[SEC_PORT_IDX];

			/* Don't do a pull if can't process the max size */
			if (port1_cnt + max_batch_size > TX_RING_SIZE || port2_cnt + max_batch_size > TX_RING_SIZE)
				break;
		}

		num_dequeued = rte_regexdev_dequeue_burst(0, qid, ops, max_batch_size);
		time = rte_get_timer_cycles();

		/* sample one packets from dequeued batch */
		//sample = rand()%num_dequeued;

		/* Handle idle timers (periods with no matches). */
		if (num_dequeued == 0) {
			if ((core_vars[qid].last_idle_time == 0) && (core_vars[qid].total_enqueued > 0)) {
				core_vars[qid].last_idle_time = time;
			}
		} else {
			if (core_vars[qid].last_idle_time != 0) {
				rxp_stats->rx_idle += time - core_vars[qid].last_idle_time;
				core_vars[qid].last_idle_time = 0;
			}
		}

		for (i = 0; i < num_dequeued; i++) {
			mbuf = ops[i]->user_ptr;
			/* put mbuf to out buffer */
			regex_dev_dpdk_bf_process_resp(qid, ops[i], stats);
			//regex_dev_dpdk_bf_release_mbuf(mbuf, stats, time, i==sample? 1:0);
			regex_dev_dpdk_bf_release_mbuf_deprecated(mbuf, stats, time, 0);
			// if (live) {
			// 	egress_idx = mbuf->dynfield1[DF_EGRESS_PORT];
			// 	dpdk_live_add_to_tx(dpdk_tx, egress_idx, mbuf);
			// }
		}

		core_vars[qid].total_dequeued += num_dequeued;
		tot_dequeued += num_dequeued;
	} while (num_dequeued || tot_dequeued < wait_on_dequeue);
}

static inline int
regex_dev_dpdk_bf_send_ops_deprecated(int qid, regex_stats_t *stats, bool live, dpdk_egress_t *dpdk_tx)
{
	rxp_stats_t *rxp_stats = (rxp_stats_t *)stats->custom;
	uint16_t to_enqueue = core_vars[qid].op_offset;
	int q_offset = qid * max_batch_size;
	struct rte_regex_ops **ops;
	uint16_t num_enqueued = 0;
	uint64_t tx_busy_time = 0;
	bool tx_full = false;
	uint32_t *m_time;
	uint16_t num_ops;
	uint64_t time;
	uint16_t ret;
	int i;

    /* do not turn on lat mode so do not wait until all ops of this round to dequeue and process in a pipeline manner */
	lat_mode = 0;

	/* Loop until all ops are enqueued. */
	while (num_enqueued < to_enqueue) {
		ops = &ops_arr_tx[num_enqueued + q_offset];
		num_ops = to_enqueue - num_enqueued;
		ret = rte_regexdev_enqueue_burst(0, qid, ops, num_ops);
		if (ret) {
			time = rte_get_timer_cycles();
			/* Put the timestamps in dynfield of mbufs sent. */
			for (i = 0; i < ret; i++) {
				m_time = &ops[i]->mbuf->dynfield1[DF_TIME_HIGH];
				util_store_64_bit_as_2_32(m_time, time);
			}

			/* Queue is now free so note any tx busy time. */
			if (tx_full) {
				rxp_stats->tx_busy += rte_get_timer_cycles() - tx_busy_time;
				tx_full = false;
			}
		} 
		else if (!tx_full) {
			/* Record time when the queue cannot be written to. */
			tx_full = true;
			tx_busy_time = rte_get_timer_cycles();
		}

		num_enqueued += ret;
		regex_dev_dpdk_bf_dequeue_deprecated(qid, stats, live, dpdk_tx, lat_mode ? ret : 0);
	}
	
	core_vars[qid].total_enqueued += num_enqueued;
	/* Reset the offset for next batch. */
	core_vars[qid].op_offset = 0;

	return 0;
}

static inline void
regex_dev_dpdk_bf_prep_op(int qid, struct rte_regex_ops *op)
{
	/* Store the buffer id in the mbuf metadata. */
	//util_store_64_bit_as_2_32(&op->mbuf->dynfield1[DF_USER_ID_HIGH], ++(core_vars[qid].buf_id));

	++(core_vars[qid].buf_id);

	if (input_subset_ids) {
		const int job_offset = regex_dev_dpdk_bf_get_array_offset(core_vars[qid].buf_id);

		op->group_id0 = input_subset_ids[job_offset][0];
		op->group_id1 = input_subset_ids[job_offset][1];
		op->group_id2 = input_subset_ids[job_offset][2];
		op->group_id3 = input_subset_ids[job_offset][3];
		op->req_flags = RTE_REGEX_OPS_REQ_GROUP_ID0_VALID_F | RTE_REGEX_OPS_REQ_GROUP_ID1_VALID_F |
				RTE_REGEX_OPS_REQ_GROUP_ID2_VALID_F | RTE_REGEX_OPS_REQ_GROUP_ID3_VALID_F;
	} else {
		op->group_id0 = 1;
		op->req_flags = RTE_REGEX_OPS_REQ_GROUP_ID0_VALID_F;
	}
	/* User id of the job is the address of it's mbuf - mbuf is not released until response is handled. */
	op->user_ptr = op->mbuf;
}

/* search function for preloaded mode */
static int
regex_dev_dpdk_bf_search(int qid, struct rte_mbuf *mbuf, int buf_len, bool push_batch, regex_stats_t *stats)
{
	uint16_t per_q_offset = core_vars[qid].op_offset;
	int q_offset = qid * max_batch_size;
	struct rte_regex_ops *op;

	/* Get the next free op for this queue and prep request. */
	op = ops_arr_tx[q_offset + per_q_offset];
	//op->mbuf = rte_pktmbuf_alloc(mbuf_pool[qid]);
	op->mbuf = mbuf;
	// if (!op->mbuf) {
	// 	MEILI_LOG_ERR("Failed to get mbuf from pool.");
	// 	return -ENOMEM;
	// }
	// rte_pktmbuf_attach_extbuf(op->mbuf, buf, 0, buf_len, &shinfo);
	// op->mbuf->data_len = buf_len;
	// op->mbuf->pkt_len = buf_len;
	regex_dev_dpdk_bf_prep_op(qid, op);

	(core_vars[qid].op_offset)++;/* set the offset, which is the total number of ops to enqueue this round */

	/* Send the batched ops if flag is set - this resets the ops array. */
	if (push_batch){
		regex_dev_dpdk_bf_send_ops_deprecated(qid, stats, false, NULL);
	}

	return 0;
}

static int
regex_dev_dpdk_bf_search_live(int qid, struct rte_mbuf *mbuf, int pay_off, uint16_t rx_port __rte_unused,
			      uint16_t tx_port, dpdk_egress_t *dpdk_tx __rte_unused, regex_stats_t *stats __rte_unused)
{
	uint16_t per_q_offset = core_vars[qid].op_offset;
	int q_offset = qid * max_batch_size;
	struct rte_regex_ops *op;

	op = ops_arr_tx[q_offset + per_q_offset];

	/* Mbuf already prepared so just add to the ops. */
	op->mbuf = mbuf;
	if (!op->mbuf) {
		MEILI_LOG_ERR("Failed to get mbuf from pool.");
		return -ENOMEM;
	}

	/* Mbuf is used elsewhere so increase ref cnt before using here. */
	rte_mbuf_refcnt_update(mbuf, 1);

	/* Adjust and store the data position to the start of the payload. */
	if (pay_off) {
		mbuf->dynfield1[DF_PAY_OFF] = pay_off;
		rte_pktmbuf_adj(mbuf, pay_off);
	} else {
		mbuf->dynfield1[DF_PAY_OFF] = 0;
	}

	mbuf->dynfield1[DF_EGRESS_PORT] = tx_port;
	regex_dev_dpdk_bf_prep_op(qid, op);

	(core_vars[qid].op_offset)++;

	/* Enqueue should be called by the force batch function. */

	return 0;
}

static void
regex_dev_dpdk_bf_force_batch_push(int qid, uint16_t rx_port __rte_unused, dpdk_egress_t *dpdk_tx, regex_stats_t *stats)
{
	regex_dev_dpdk_bf_send_ops_deprecated(qid, stats, true, dpdk_tx);
}

static void
regex_dev_dpdk_bf_force_batch_pull(int qid, dpdk_egress_t *dpdk_tx, regex_stats_t *stats)
{
	/* Async dequeue is only needed if not in latency mode so set 'wait on' value to 0. */
	regex_dev_dpdk_bf_dequeue_deprecated(qid, stats, true, dpdk_tx, 0);
}

/* Ensure all ops in flight are received before exiting. */
static void
regex_dev_dpdk_bf_post_search(int qid, regex_stats_t *stats)
{
	uint64_t start, diff;

	start = rte_rdtsc();
	while (core_vars[qid].total_enqueued > core_vars[qid].total_dequeued) {
		regex_dev_dpdk_bf_dequeue_deprecated(qid, stats, false, NULL, 0);

		/* Prevent infinite loops. */
		diff = rte_rdtsc() - start;
		if (diff > MAX_POST_SEARCH_DEQUEUE_CYCLES) {
			MEILI_LOG_ALERT("Post-processing appears to be in an infinite loop. Breaking...");
			break;
		}
	}
}

static void
regex_dev_dpdk_bf_clean(rb_conf *run_conf)
{
	uint32_t batches = run_conf->input_batches;
	uint32_t queues = run_conf->cores;
	uint32_t ops_size;
	uint32_t i;

	ops_size = batches * queues;
	if (ops_arr_tx) {
		for (i = 0; i < ops_size; i++)
			if (ops_arr_tx[i])
				rte_free(ops_arr_tx[i]);
		rte_free(ops_arr_tx);
	}

	if (ops_arr_rx) {
		for (i = 0; i < ops_size; i++)
			if (ops_arr_rx[i])
				rte_free(ops_arr_rx[i]);
		rte_free(ops_arr_rx);
	}

	if (run_conf->mbuf_pool) {
		for (i = 0; i < queues; i++)
			if (run_conf->mbuf_pool[i])
				rte_mempool_free(run_conf->mbuf_pool[i]);
		rte_free(run_conf->mbuf_pool);
	}

	rte_free(core_vars);

	if (verbose)
		regex_dev_close_match_file(run_conf);
	rte_free(rules);

	/* Free queue-pair memory. */
	rte_regexdev_stop(regex_dev_id);
}

static int
regex_dev_dpdk_bf_compile(rb_conf *run_conf)
{
	return rules_file_compile_for_rxp(run_conf);
}

int
regex_dev_dpdk_bf_reg(regex_func_t *funcs)
{
	funcs->init_regex_dev = regex_dev_dpdk_bf_init;
	funcs->search_regex = regex_dev_dpdk_bf_search;
	funcs->search_regex_live = regex_dev_dpdk_bf_search_live;
	funcs->force_batch_push = regex_dev_dpdk_bf_force_batch_push;
	funcs->force_batch_pull = regex_dev_dpdk_bf_force_batch_pull;
	funcs->post_search_regex = regex_dev_dpdk_bf_post_search;
	funcs->clean_regex_dev = regex_dev_dpdk_bf_clean;
	funcs->compile_regex_rules = regex_dev_dpdk_bf_compile;

	return 0;
}

int
regex_bf_init(struct pipeline_stage *self)
{
	int ret;
    /* allocate space for pipeline state */
    self->state = (struct regex_bf_state *)malloc(sizeof(struct regex_bf_state));
    struct regex_bf_state *mystate = (struct regex_bf_state *)self->state;

	struct pipeline *pl = (struct pipeline *)self->pl;
	struct pipeline_conf *pl_conf = &(pl->pl_conf);

	pthread_mutex_lock(&mutex_regex_bf);
	if(!regex_bf_initialized){
		ret = regex_dev_register(pl_conf);
		if (ret) {
			//snprintf(err, ERR_STR_SIZE, "Regex dev registration error");
			MEILI_LOG_ERR("Regex dev registration error");
			return ret;
		}

		ret = regex_dev_compile_rules(pl_conf);
		if (ret) {
			//snprintf(err, ERR_STR_SIZE, "Regex dev rule compilation error");
			MEILI_LOG_ERR("Regex dev rule compilation error");
			return ret;
		}

		ret = regex_dev_init(pl_conf);
		if (ret) {
			//snprintf(err, ERR_STR_SIZE, "Failed initialising regex device");	
			MEILI_LOG_ERR("Failed initialising regex device");
			return ret;
		}
		regex_bf_initialized = true;
		MEILI_LOG_INFO("Regex device configuration finished...");
	}
	pthread_mutex_unlock(&mutex_regex_bf);


	/* private states */
    if(!mystate){
        return -ENOMEM;
    }
    //memset(self->state, 0x00, sizeof(struct regex_bf_state));

	mystate->wait_on_dequeue = 0;
	mystate->regex_stats  = (struct regex_dev_stats *)malloc(sizeof(struct regex_dev_stats));
	mystate->regex_stats->custom = (struct regex_custom_rxp *)malloc(sizeof(struct regex_custom_rxp));
	/* Init min latency stats to large value. */

	struct regex_custom_rxp *custom = (struct regex_custom_rxp *)mystate->regex_stats->custom;
	custom->min_lat = UINT64_MAX;
	custom->max_lat = 0;

    return 0;
}

int
regex_bf_free(struct pipeline_stage *self)
{
    struct regex_bf_state *mystate = (struct regex_bf_state *)self->state;
	struct pipeline *pl = (self->pl);
	struct pipeline_conf *run_conf = &(pl->pl_conf);

	//regex_dev_clean_regex(run_conf);

	free(mystate->regex_stats->custom);
	free(mystate->regex_stats);
    free(mystate);
    return 0;
}


static void
regex_dev_dpdk_bf_update_time_stats_deprecated(struct rte_mbuf *mbuf, regex_stats_t *stats, uint64_t recv_time, int sample)
{
	rxp_stats_t *rxp_stats = (rxp_stats_t *)stats->custom;
	uint64_t time_mbuf, time_diff;

	/* Calculate and store latency of packet through HW. */
	time_mbuf = util_get_64_bit_from_2_32(&mbuf->dynfield1[DF_TIME_HIGH]);

	time_diff = (recv_time - time_mbuf);

	rxp_stats->tot_lat += time_diff;
	if (time_diff < rxp_stats->min_lat)
		rxp_stats->min_lat = time_diff;
	if (time_diff > rxp_stats->max_lat)
		rxp_stats->max_lat = time_diff;
	// if(sample){
	// 	rxp_stats->time_diff_sample[nb_sampled%NUMBER_OF_SAMPLE] = time_diff;
	// 	nb_sampled++;
	// }
}

static void
regex_dev_dpdk_bf_dequeue(struct pipeline_stage *self, int nb_wait_on_dequeue,
                            struct rte_mbuf **mbuf_out,
                            int *nb_deq)
//(int qid, regex_stats_t *stats, bool live, dpdk_egress_t *dpdk_tx, uint16_t wait_on_dequeue)
{
	int qid = self->worker_qid;
	struct regex_bf_state *mystate = (struct regex_bf_state*)self->state;
	regex_stats_t *stats = mystate->regex_stats;

	rxp_stats_t *rxp_stats = (rxp_stats_t *)stats->custom;

	int q_offset = qid * max_batch_size;
	struct rte_regex_ops **ops;
	uint16_t tot_dequeued = 0;

	struct rte_mbuf *mbuf;

	int batch_size = self->batch_size;

	/* lat mode off, nb_enq should always be zero */
	uint16_t wait_on_dequeue = nb_wait_on_dequeue;
	

	uint16_t num_dequeued;

	int out_offset = *nb_deq;

	uint64_t time;

	int sample = 0;

	/* Determine rx ops for this queue/lcore. */
	ops = &ops_arr_rx[q_offset];

	/* Poll the device until no more matches are received. */
	/* poll at least once */
	do {
		
		num_dequeued = rte_regexdev_dequeue_burst(0, qid, ops, batch_size);
		//printf("dequeue=%d\n",num_dequeued);
		//time = rte_get_timer_cycles();

		/* sample one packets from dequeued batch */
		//sample = rand()%num_dequeued;

		/* Handle idle timers (periods with no matches). */
		if (num_dequeued == 0) {
			if ((core_vars[qid].last_idle_time == 0) && (core_vars[qid].total_enqueued > 0)) {
				core_vars[qid].last_idle_time = time;
			}
		} else {
			if (core_vars[qid].last_idle_time != 0) {
				rxp_stats->rx_idle += time - core_vars[qid].last_idle_time;
				core_vars[qid].last_idle_time = 0;
			}
		}

		for (int i = 0; i < num_dequeued; i++) {
			mbuf = ops[i]->user_ptr;
			/* put mbuf into out buffer */
			mbuf_out[out_offset + i] = mbuf;
			
			regex_dev_dpdk_bf_process_resp(qid, ops[i], stats);
			
		}

		core_vars[qid].total_dequeued += num_dequeued;
		tot_dequeued += num_dequeued;
		out_offset += num_dequeued;

	//} while (num_dequeued || tot_dequeued < wait_on_dequeue);
	} while (tot_dequeued < wait_on_dequeue);

	/* update # of dequeued mbufs, out_offset is assigned as nb_deq previously, so assign updated value back to nb_deq */
	*nb_deq = out_offset;
}

static inline int
regex_dev_dpdk_bf_enq_deq_ops(struct pipeline_stage *self, int nb_enq,
                            struct rte_mbuf **mbuf_out,
                            int *nb_deq)

	//(int qid, regex_stats_t *stats, bool live, dpdk_egress_t *dpdk_tx)
{
	int qid = self->worker_qid;
	struct regex_bf_state *mystate = (struct regex_bf_state*)self->state;
	regex_stats_t *stats = mystate->regex_stats;
	int batch_size = self->batch_size;

	rxp_stats_t *rxp_stats = (rxp_stats_t *)stats->custom;
	//uint16_t to_enqueue = core_vars[qid].op_offset;
	uint16_t to_enqueue = nb_enq;

	int q_offset = qid * max_batch_size;
	struct rte_regex_ops **ops;
	uint16_t num_enqueued = 0;
	uint16_t num_ops;

	uint64_t tx_busy_time = 0;
	bool tx_full = false;
	uint32_t *m_time;
	uint64_t time;
	uint16_t ret;
	int i;

    /* do not turn on lat mode so do not wait until all ops of this round to dequeue and process in a pipeline manner */
	lat_mode = 0;

	//printf("nb_enq=%d, op_offset=%d, to_enqueue=%d\n",nb_enq,core_vars[qid].op_offset,to_enqueue);
	/* Loop until all ops are enqueued. */
	while (num_enqueued < nb_enq) {
		//printf("enqueuing");
		ops = &ops_arr_tx[num_enqueued + q_offset];
		//num_ops = to_enqueue - num_enqueued;
		
		/* Note that to_enqueue is always <= batch_size of this pl stage */
		num_ops = RTE_MIN(batch_size, to_enqueue);
		//printf("num_ops=%d\n",num_ops);
		ret = rte_regexdev_enqueue_burst(0, qid, ops, num_ops);
		//printf("ret=%d\n",ret);
		// if (ret) {
		// 	time = rte_get_timer_cycles();
		// 	/* Put the timestamps in dynfield of mbufs sent. */
		// 	/* timestamps will conflict with reorder seq, should use register function to use it*/
		// 	// for (i = 0; i < ret; i++) {
		// 	// 	m_time = &ops[i]->mbuf->dynfield1[DF_TIME_HIGH];
		// 	// 	util_store_64_bit_as_2_32(m_time, time);
		// 	// }

		// 	/* Queue is now free so note any tx busy time. */
		// 	if (tx_full) {
		// 		rxp_stats->tx_busy += rte_get_timer_cycles() - tx_busy_time;
		// 		tx_full = false;
		// 	}
		// } 
		// else if (!tx_full) {
		// 	/* Record time when the queue cannot be written to. */
		// 	tx_full = true;
		// 	tx_busy_time = rte_get_timer_cycles();
		// }

		num_enqueued += ret;
		to_enqueue -= ret;
		//regex_dev_dpdk_bf_dequeue(qid, stats, live, dpdk_tx, lat_mode ? ret : 0);
		/* dequeue operations and put them into mbuf_out, update *nb_deq at the same time */
		/* no waiting */
		regex_dev_dpdk_bf_dequeue(self, 0, mbuf_out, nb_deq);
	}

	
	core_vars[qid].total_enqueued += num_enqueued;
	/* Reset the offset for next batch. */
	core_vars[qid].op_offset = 0;

	return 0;
}



int
regex_bf_exec(struct pipeline_stage *self, struct rte_mbuf **mbuf, int nb_enq,
                            struct rte_mbuf ***mbuf_out,
                            int *nb_deq)
{
	
	int qid = self->worker_qid;

	struct regex_bf_state *mystate = (struct regex_bf_state*)self->state;
	regex_stats_t *stats = mystate->regex_stats;

	uint16_t per_q_offset = core_vars[qid].op_offset;
	int q_offset = qid * max_batch_size;
	struct rte_regex_ops *op;

	*nb_deq = 0;
	if(nb_enq <= 0 && mystate->wait_on_dequeue > 0){
		/* no enqueue, dequeue and return */
		regex_dev_dpdk_bf_dequeue(self, 0, *mbuf_out, nb_deq);
		//debug
		// if(*nb_deq > 0){
		// 	printf("no enqueue, dequeued %d requests\n",*nb_deq);
		// }
		mystate->wait_on_dequeue = mystate->wait_on_dequeue - *nb_deq;
		return 0;
	}else if(nb_enq <= 0 && mystate->wait_on_dequeue <= 0){
		return 0;
	}
	
	/* prepare regex operations */
	/* Get the next free op for this queue and prep request. */

	for(int i=0; i<nb_enq; i++){
		op = ops_arr_tx[q_offset + per_q_offset + i];
		op->mbuf = mbuf[i];
		regex_dev_dpdk_bf_prep_op(qid, op);
		(core_vars[qid].op_offset)++;/* set the offset, which is the total number of ops to enqueue this round */
	}

	//pkt_ts_exec(&pl->ts_start_stage, mbuf, nb_enq);
	
	/* Send the batched ops - this resets the ops array. */
	//printf("point 1\n");
	
	regex_dev_dpdk_bf_enq_deq_ops(self, nb_enq, *mbuf_out, nb_deq);
	mystate->wait_on_dequeue += nb_enq;
	//printf("1mystate->wait_on_dequeue = %d\n",mystate->wait_on_dequeue);

	//printf("2mystate->wait_on_dequeue = %d\n",mystate->wait_on_dequeue);

	//pkt_ts_exec(&pl->ts_end_stage, *mbuf_out, *nb_deq);
	
    return 0;
}



/* pipeline stage operations */
int regex_bf_pipeline_stage_func_reg(struct pipeline_stage *stage){

	stage->funcs->pipeline_stage_init = regex_bf_init;
	stage->funcs->pipeline_stage_free = regex_bf_free;
	stage->funcs->pipeline_stage_exec = regex_bf_exec;

	return 0;
}
