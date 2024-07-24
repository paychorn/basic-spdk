/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "spdk/stdinc.h"

#include "spdk/nvme.h"
#include "spdk/thread.h"
#include "spdk/vmd.h"
#include "spdk/nvme_zns.h"
#include "spdk/env.h"
#include "spdk/string.h"
#include "spdk/log.h"

#include "spdk_nvme.c"



/* Limitation:
 * 1. Only one namespace is supported.
 * 2. Only one controller is supported.
 */
/* define */

TAILQ_HEAD(g_ctrs, ctrlr_entry) g_controllers = TAILQ_HEAD_INITIALIZER(g_controllers);
TAILQ_HEAD(g_nsps, ns_entry) g_namespaces     = TAILQ_HEAD_INITIALIZER(g_namespaces);

/* usage */

int parse_args(int argc, char **argv, struct spdk_env_opts *env_opts)
{
	int op, rc;

	spdk_nvme_trid_populate_transport(&g_trid, SPDK_NVME_TRANSPORT_PCIE);
	snprintf(g_trid.subnqn, sizeof(g_trid.subnqn), "%s", SPDK_NVMF_DISCOVERY_NQN);

	while ((op = getopt(argc, argv, "d:gi:r:L:V")) != -1) {
		switch (op) {
		case 'V':
			g_vmd = true;
			break;
		case 'i':
			env_opts->shm_id = spdk_strtol(optarg, 10);
			if (env_opts->shm_id < 0) {
				fprintf(stderr, "Invalid shared memory ID\n");
				return env_opts->shm_id;
			}
			break;
		case 'g':
			env_opts->hugepage_single_segments = true;
			break;
		case 'r':
			if (spdk_nvme_transport_id_parse(&g_trid, optarg) != 0) {
				fprintf(stderr, "Error parsing transport address\n");
				return 1;
			}
			break;
		case 'd':
			env_opts->mem_size = spdk_strtol(optarg, 10);
			if (env_opts->mem_size < 0) {
				fprintf(stderr, "Invalid DPDK memory size\n");
				return env_opts->mem_size;
			}
			break;
		case 'L':
			rc = spdk_log_set_flag(optarg);
			if (rc < 0) {
				fprintf(stderr, "unknown flag\n");
				usage(argv[0]);
				exit(EXIT_FAILURE);
			}
#ifdef DEBUG
			spdk_log_set_print_level(SPDK_LOG_DEBUG);
#endif
			break;
		default:
			usage(argv[0]);
			return 1;
		}
	}
	return 0;
}

void cleanup(void)
{
	struct ns_entry *ns_entry, *tmp_ns_entry;
	struct ctrlr_entry *ctrlr_entry, *tmp_ctrlr_entry;
	struct spdk_nvme_detach_ctx *detach_ctx = NULL;

	TAILQ_FOREACH_SAFE(ns_entry, &g_namespaces, link, tmp_ns_entry) {
		TAILQ_REMOVE(&g_namespaces, ns_entry, link);
		free(ns_entry);
	}

	TAILQ_FOREACH_SAFE(ctrlr_entry, &g_controllers, link, tmp_ctrlr_entry) {
		TAILQ_REMOVE(&g_controllers, ctrlr_entry, link);
		spdk_nvme_detach_async(ctrlr_entry->ctrlr, &detach_ctx);
		free(ctrlr_entry);
	}

	if (detach_ctx) {
		spdk_nvme_detach_poll(detach_ctx);
	}
}

/* set up namespace */
void register_ns(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_ns *ns)
{
	struct ns_entry *entry;

	if (!spdk_nvme_ns_is_active(ns)) {
		return;
	}

	entry = malloc(sizeof(struct ns_entry));
	if (entry == NULL) {
		perror("ns_entry malloc");
		exit(1);
	}
	entry->ctrlr = ctrlr;
	entry->ns = ns;
	TAILQ_INSERT_TAIL(&g_namespaces, entry, link);

	return;
}

bool probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid, struct spdk_nvme_ctrlr_opts *opts)
{
	printf("Attaching to %s\n", trid->traddr);
	return true;
}

void attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid, struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	int nsid;
	struct ctrlr_entry *entry;
	struct spdk_nvme_ns *ns;
	const struct spdk_nvme_ctrlr_data *cdata;

	entry = malloc(sizeof(struct ctrlr_entry));
	if (entry == NULL) {
		perror("ctrlr_entry malloc");
		exit(1);
	}

	printf("Attached to %s\n", trid->traddr);

	/*
	 * spdk_nvme_ctrlr is the logical abstraction in SPDK for an NVMe controller.  During initialization, the IDENTIFY data for the controller 
	 * is read using an NVMe admin command, and that data can be retrieved using spdk_nvme_ctrlr_get_data() 
	 * to get detailed information on the controller. Refer to the NVMe specification for more details on IDENTIFY for NVMe controllers.
	 */
	cdata = spdk_nvme_ctrlr_get_data(ctrlr);

	snprintf(entry->name, sizeof(entry->name), "%-20.20s (%-20.20s)", cdata->mn, cdata->sn);

	entry->ctrlr = ctrlr;
	TAILQ_INSERT_TAIL(&g_controllers, entry, link);

	/*
	 * Each controller has one or more namespaces.  An NVMe namespace is basically equivalent to a SCSI LUN. The controller's IDENTIFY data tells us 
	 * how many namespaces exist on the controller.  For Intel(R) P3X00 controllers, it will just be one namespace.
	 * Note that in NVMe, namespace IDs start at 1, not 0.
	 */
	for (nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr); nsid != 0;
	     nsid = spdk_nvme_ctrlr_get_next_active_ns(ctrlr, nsid)) {
		ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
		if (ns == NULL) {
			// continue;
            return;
		}
		register_ns(ctrlr, ns);
	}
}


void read_complete(void *arg, const struct spdk_nvme_cpl *completion)
{
	struct sequence *sequence = arg;

	/* Assume the I/O was successful */
	sequence->is_completed = 1;

	// /* See if an error occurred. If so, display information
	//  * about it, and set completion value so that I/O
	//  * caller is aware that an error occurred.
	//  */
	if (spdk_nvme_cpl_is_error(completion)) {
		spdk_nvme_qpair_print_completion(sequence->ns_entry->qpair, (struct spdk_nvme_cpl *)completion);
		fprintf(stderr, "I/O error status: %s\n", spdk_nvme_cpl_get_status_string(&completion->status));
		fprintf(stderr, "Read I/O failed, aborting run\n");
		sequence->is_completed = 2;
		exit(1);
	}

	/*
	 * The read I/O has completed.  Print the contents of the
	 *  buffer, free the buffer, then mark the sequence as
	 *  completed.  This will trigger the hello_world() function
	 *  to exit its polling loop.
	 */
	// printf("read: %s\n", sequence->buf);
	spdk_free(sequence->buf);
}

void write_complete(void *arg, const struct spdk_nvme_cpl *completion)
{
	struct sequence				*sequence = arg;
	struct ns_entry			    *ns_entry = sequence->ns_entry;

	/* See if an error occurred. If so, display information
	 * about it, and set completion value so that I/O
	 * caller is aware that an error occurred.
	 */
	sequence->is_completed = 1;
	
	if (spdk_nvme_cpl_is_error(completion)) {
		spdk_nvme_qpair_print_completion(sequence->ns_entry->qpair, (struct spdk_nvme_cpl *)completion);
		fprintf(stderr, "I/O error status: %s\n", spdk_nvme_cpl_get_status_string(&completion->status));
		fprintf(stderr, "Write I/O failed, aborting run\n");
		sequence->is_completed = 2;
		exit(1);
	}
	/*
	 * The write I/O has completed.  Free the buffer associated with
	 *  the write I/O and allocate a new zeroed buffer for reading
	 *  the data back from the NVMe namespace.
	 */
	if (sequence->using_cmb_io) {
		spdk_nvme_ctrlr_unmap_cmb(ns_entry->ctrlr);
	} else {
		spdk_free(sequence->buf);
	}
}

void spdk_write(
	uint64_t 							user_sector_start,
	uint64_t 							user_sector_count,
	size_t 								user_buffer_size,
	int 								user_pattern
)
{
	struct ns_entry 					*ns_entry;
    struct sequence 					sequence;
    int 								rc;
	uint32_t 							buffer[user_buffer_size];
	

	/* generate buffer as pattern 
	 * 0: fill zeroes, 1: fill ones, 2: increment
	 */
	if (user_pattern == 2) {
		for (size_t i = 0; i < user_buffer_size - 1; i++) {
			buffer[i] = (user_sector_start + i);
		}
	}
	else {
		for (size_t i = 0; i < user_buffer_size - 1; i++) {
			buffer[i] = (uint32_t)user_pattern;
		}
	}
	buffer[user_buffer_size - 1] = '\0'; // Null-terminate the string for safety

	uint32_t *ptr = buffer;

	for (size_t i = 0; i < user_buffer_size - 1; i++) {
		printf("%d ", *(ptr + i));
	}

    TAILQ_FOREACH(ns_entry, &g_namespaces, link) {
		uint64_t current_sector = user_sector_start;		
		uint64_t end_sector = user_sector_start + user_sector_count;

		uint32_t max_xfer_sector = spdk_nvme_ctrlr_get_max_xfer_size(ns_entry->ctrlr) / spdk_nvme_ns_get_sector_size(ns_entry->ns);
		
		time_t start_time, end_time;
		start_time = clock();
		while (current_sector < end_sector)
		{
			size_t sz;

			// printf("Sector current: %lu\n", current_sector);
			ns_entry->qpair = spdk_nvme_ctrlr_alloc_io_qpair(ns_entry->ctrlr, NULL, 0);
			if (ns_entry->qpair == NULL) {
				printf("ERROR: spdk_nvme_ctrlr_alloc_io_qpair() failed\n");
				exit(1);
			}
			sequence.using_cmb_io = 1;
			sequence.buf = spdk_nvme_ctrlr_map_cmb(ns_entry->ctrlr, &sz);
			sequence.buffer_sz = sz;
			
			uint32_t looping_sector = (end_sector - current_sector) > max_xfer_sector ? max_xfer_sector : (end_sector - current_sector);
			size_t seq_buffer_size = pow(2, ceil(log2(looping_sector * spdk_nvme_ns_get_sector_size(ns_entry->ns))));
			
			if (sequence.buf == NULL || sz < seq_buffer_size) {
				sequence.using_cmb_io = 0;
				sequence.buf = spdk_malloc(seq_buffer_size, seq_buffer_size, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
				sequence.buffer_sz = seq_buffer_size;
			}
			if (sequence.buf == NULL) {
				printf("ERROR: buffer allocation failed\n");
				exit(1);

			}
			// if (sequence.using_cmb_io) {
			// 	printf("INFO: using controller memory buffer for IO\n");
			// } else {
			// 	printf("INFO: using host memory buffer for IO\n");
			// }
			sequence.is_completed = 0;
			sequence.ns_entry = ns_entry;
			if (spdk_nvme_ns_get_csi(ns_entry->ns) == SPDK_NVME_CSI_ZNS) {
				reset_zone_and_wait_for_completion(&sequence);
			}

			snprintf(sequence.buf, seq_buffer_size,"%d", *buffer);
			

			// printf("Reading sector: %u\n", looping_sector);	
			rc = spdk_nvme_ns_cmd_write(ns_entry->ns, ns_entry->qpair, sequence.buf,
                    current_sector, looping_sector, write_complete, &sequence, 0);

			if (rc != 0) {
				fprintf(stderr, "ERROR: starting read I/O failed\n");
				spdk_nvme_ctrlr_free_io_qpair(ns_entry->qpair);
				return;
			}
		
			current_sector += looping_sector;

			printf("Sector current: %lu\n", current_sector);
			// current_buffer = (void *)((uintptr_t)current_buffer + looping_sector * spdk_nvme_ns_get_sector_size(ns_entry->ns));

			while (!sequence.is_completed) {
				spdk_nvme_qpair_process_completions(ns_entry->qpair, 0);
			}
        	spdk_nvme_ctrlr_free_io_qpair(ns_entry->qpair);

		}
		end_time = clock();
		printf("Time: %.10f sec\n", ((double)end_time-start_time)/CLOCKS_PER_SEC);
		// sequence.buf = current_buffer;
    }
}

void spdk_read(	
	uint64_t 							user_sector_start,
	uint64_t 							user_sector_count,
	size_t 								user_buffer_size
)
{
    struct ns_entry 					*ns_entry;
    struct sequence 					sequence;
    int 								rc;
	

    TAILQ_FOREACH(ns_entry, &g_namespaces, link) {
		uint64_t current_sector = user_sector_start;		
		uint64_t end_sector = user_sector_start + user_sector_count;

		uint32_t max_xfer_sector = spdk_nvme_ctrlr_get_max_xfer_size(ns_entry->ctrlr) / spdk_nvme_ns_get_sector_size(ns_entry->ns);
		
		time_t start_time, end_time;
		start_time = clock();
		while (current_sector < end_sector)
		{
			size_t sz;

			// printf("Sector current: %lu\n", current_sector);
			ns_entry->qpair = spdk_nvme_ctrlr_alloc_io_qpair(ns_entry->ctrlr, NULL, 0);
			if (ns_entry->qpair == NULL) {
				printf("ERROR: spdk_nvme_ctrlr_alloc_io_qpair() failed\n");
				exit(1);
			}
			sequence.using_cmb_io = 1;
			sequence.buf = spdk_nvme_ctrlr_map_cmb(ns_entry->ctrlr, &sz);
			sequence.buffer_sz = sz;
			uint32_t looping_sector = (end_sector - current_sector) > max_xfer_sector ? max_xfer_sector : (end_sector - current_sector);
			size_t seq_buffer_size = pow(2, ceil(log2(looping_sector * spdk_nvme_ns_get_sector_size(ns_entry->ns))));
			if (sequence.buf == NULL || sz < seq_buffer_size) {
				sequence.using_cmb_io = 0;
				sequence.buf = spdk_zmalloc(seq_buffer_size, seq_buffer_size, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
				sequence.buffer_sz = seq_buffer_size;
			}
			if (sequence.buf == NULL) {
				printf("ERROR: buffer allocation failed\n");
				exit(1);

			}
			// if (sequence.using_cmb_io) {
			// 	printf("INFO: using controller memory buffer for IO\n");
			// } else {
			// 	printf("INFO: using host memory buffer for IO\n");
			// }
			sequence.is_completed = 0;
			sequence.ns_entry = ns_entry;
			if (spdk_nvme_ns_get_csi(ns_entry->ns) == SPDK_NVME_CSI_ZNS) {
				reset_zone_and_wait_for_completion(&sequence);
			}

			// printf("Reading sector: %u\n", looping_sector);	
			rc = spdk_nvme_ns_cmd_read(ns_entry->ns, ns_entry->qpair, sequence.buf,
                    current_sector, looping_sector, read_complete, &sequence, 0);

			if (rc != 0) {
				fprintf(stderr, "ERROR: starting read I/O failed\n");
				spdk_nvme_ctrlr_free_io_qpair(ns_entry->qpair);
				return;
			}
		
			current_sector += looping_sector;

			// printf("Sector current: %lu\n", current_sector);
			// current_buffer = (void *)((uintptr_t)current_buffer + looping_sector * spdk_nvme_ns_get_sector_size(ns_entry->ns));

			while (!sequence.is_completed) {
				spdk_nvme_qpair_process_completions(ns_entry->qpair, 0);
			}
        	spdk_nvme_ctrlr_free_io_qpair(ns_entry->qpair);

		}
		end_time = clock();
		printf("Time: %.10f sec\n", ((double)end_time-start_time)/CLOCKS_PER_SEC);
		// sequence.buf = current_buffer;
		// spdk_free(current_buffer);
    }
}

int main_loop(void) {
	struct ns_entry 					*ns_entry;
	int 								user_pattern = -1;
	int									op = -1;
	uint64_t 							user_sector_start;
	uint64_t 							user_sector_count;
	size_t 								user_buffer_size;

	TAILQ_FOREACH(ns_entry, &g_namespaces, link) {
		uint64_t sys_sector_count = spdk_nvme_ns_get_num_sectors(ns_entry->ns);		
		uint32_t sys_sector_size = spdk_nvme_ns_get_sector_size(ns_entry->ns);

		printf("Max transfer: %u\n", spdk_nvme_ctrlr_get_max_xfer_size(ns_entry->ctrlr));
		print_info(ns_entry->ns);
		printf("...\n");
		
		/* pattern */
		while (true)
		{
			printf("%sChoose the operation among the following...\n", KNRM);
			printf("0: Write\n");
			printf("1: Read\n");
			printf("Operation: ");
			op = pos_int_input();
			
			if (op == 0) {
				int patt = sel_pattern();
				if (patt == 3) {
					continue;
				}
				else {
					user_pattern = (int)patt;
					break;
				}
			}
			else if (op == 1) {
				user_pattern = -1;
				break;
			}
			else
			{
				printf("%sInvalid operation\n", KRED);
			}
		}

		/* start */
		while (true)
		{
			if (user_pattern == -1) {
				printf("%sStart reading at sector: ", KNRM);
			} else {
				printf("%sStart writing at sector: ", KNRM);
			}

			uint64_t start = llu_input();
			if ((int)start == -1) {
				printf("%sInvalid start\n", KRED);
				continue;
			}
			if (start > sys_sector_count) {
				printf("%sStart sector exceeds the SSD size\n", KRED);
				continue;
			} else {
				user_sector_start = start;
				break;
			}
		}

		/* length */
		while (true)
		{
			if (user_pattern == -1) {
				printf("%sReading length (aka sector count): ", KNRM);
			} else {
				printf("%sWriting length (aka sector count): ", KNRM);
			}

			uint64_t length = llu_input();
			if ((int)length == -1) {
				printf("%sInvalid length\n", KRED);
				continue;
			}
			if (length > sys_sector_count - user_sector_start) {
				printf("%sLength exceeds the SSD size\n", KRED);
				continue;
			} else {
				user_sector_count = length;
				user_buffer_size = length * sys_sector_size;
				printf("Buffer size: %lu\n", user_buffer_size);
				break;
			}
		}
			
		// printf("How many queues depth do you want to use?\n");
		// rt = scanf("%lu\n", qdepth);

		printf("-----------------------------------\n");
		printf("LBA start: %lu\n", user_sector_start);
		printf("LBA count: %lu\n", user_sector_count);
		printf("LBA end  : %lu\n", user_sector_start + user_sector_count);

		time_t start_time, end_time;
		if (user_pattern == -1)
		{
			spdk_read(user_sector_start, user_sector_count, user_buffer_size);
		} 
		else
		{
			spdk_write(user_sector_start, user_sector_count, user_buffer_size, user_pattern);
		}
		printf("...\n");
	}
	return 0;
}

int main(int argc, char **argv)
{
	// int rc, user_pattern = -1, op = -1, patt = -1;
	int rc;
	struct spdk_env_opts opts;
	// uint64_t start=0, length=1, qdepth;
	// static uint64_t user_sector_start, user_sector_count, user_qdepth;
	// static size_t user_buffer_size = 512 * 1024;
	// int NUM_THREADS = 4;
    // struct spdk_thread *threads[NUM_THREADS];
    // int thread_ids[NUM_THREADS];

	/* environment initialization */
	spdk_env_opts_init(&opts);
	rc = parse_args(argc, argv, &opts);
	if (rc != 0) {
		return rc;
	}
	opts.name = "spdk_testS";
	if (spdk_env_init(&opts) < 0) {
		fprintf(stderr, "Unable to initialize SPDK env\n");
		return 1;
	}
	printf("Initializing NVMe Controllers\n");
	if (g_vmd && spdk_vmd_init()) {
		fprintf(stderr, "Failed to initialize VMD."
			" Some NVMe devices can be unavailable.\n");
	}
	rc = spdk_nvme_probe(&g_trid, NULL, probe_cb, attach_cb, NULL);
	if (rc != 0) {
		fprintf(stderr, "spdk_nvme_probe() failed\n");
		rc = 1;
		goto exit;
	}
	if (TAILQ_EMPTY(&g_controllers)) {
		fprintf(stderr, "no NVMe controllers found\n");
		rc = 1;
		goto exit;
	}
	printf("SPDK Initialization complete.\n");

	/* application */
	main_loop();

	cleanup();
	if (g_vmd) {
		spdk_vmd_fini();
	}

	exit:
		cleanup();
		spdk_env_fini();
		return rc;
}
