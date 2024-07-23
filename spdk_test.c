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
#include <stdio.h>
#include <stdlib.h>
#include "spdk/stdinc.h"

#include "spdk/nvme.h"
#include "spdk/thread.h"
#include "spdk/vmd.h"
#include "spdk/nvme_zns.h"
#include "spdk/env.h"
#include "spdk/string.h"
#include "spdk/log.h"

#include "spdk_nvme.h"


/* Limitation:
 * 1. Only one namespace is supported.
 * 2. Only one controller is supported.
 * 3. 
 */

/* input */
static uint64_t user_sector_start = 59392;
static uint64_t user_sector_count = 1024;
static size_t user_buffer_size = 512 * 1024;
static int user_pattern = -1; //read
static uint64_t user_qdepth = 64;
static uint64_t max_sector_count = 1;

/* define */
#define KNRM  "\x1B[0m"
#define KRED  "\x1B[31m"
#define MEM_SIZE ((uint64_t)1ULL  << 40) // 1TBs
// #define BUF_SIZE 4294967296 // 4GB
void reset_zone_complete(void *arg, const struct spdk_nvme_cpl *completion)
{
	struct sequence *sequence = arg;

	/* Assume the I/O was successful */
	sequence->is_completed = 1;
	/* See if an error occurred. If so, display information  about it, and set completion value so that I/O caller is aware that an error occurred. */
	if (spdk_nvme_cpl_is_error(completion)) {
		spdk_nvme_qpair_print_completion(sequence->ns_entry->qpair, (struct spdk_nvme_cpl *)completion);
		fprintf(stderr, "I/O error status: %s\n", spdk_nvme_cpl_get_status_string(&completion->status));
		fprintf(stderr, "Reset zone I/O failed, aborting run\n");
		sequence->is_completed = 2;
		exit(1);
	}
}

void reset_zone_and_wait_for_completion(struct sequence *sequence)
{
	if (spdk_nvme_zns_reset_zone(sequence->ns_entry->ns, sequence->ns_entry->qpair,
				     0, /* starting LBA of the zone to reset */
				     false, /* don't reset all zones */
				     reset_zone_complete,
				     sequence)) {
		fprintf(stderr, "starting reset zone I/O failed\n");
		exit(1);
	}
	while (!sequence->is_completed) {
		spdk_nvme_qpair_process_completions(sequence->ns_entry->qpair, 0);
	}
	sequence->is_completed = 0; // Reset for next I/O
}

void get_info(struct spdk_nvme_ns *ns)
{
	max_sector_count = spdk_nvme_ns_get_num_sectors(ns);
	uint32_t sector_size = spdk_nvme_ns_get_sector_size(ns);
	uint64_t size_in_bytes = max_sector_count * sector_size;

	printf("Namespace ID: %d\n", spdk_nvme_ns_get_id(ns));
	printf("Number of Sectors: %lu\n", max_sector_count);
	printf("Sector Size: %u bytes\n", sector_size);
	printf("Namespace Size: %lu bytes %.2f GB\n", size_in_bytes, (float)size_in_bytes / (1024 * 1024 * 1024));
	return;
}

TAILQ_HEAD(g_ctrs, ctrlr_entry) g_controllers = TAILQ_HEAD_INITIALIZER(g_controllers);
TAILQ_HEAD(g_nsps, ns_entry) g_namespaces     = TAILQ_HEAD_INITIALIZER(g_namespaces);

/* usage */

static uint64_t lba_count = 0;
uint64_t get_lba_current(uint64_t lba_start, size_t buffer_sz) {
    return lba_start + (lba_count++ * buffer_sz);
}

void usage(const char *program_name)
{
	printf("%s [options]", program_name);
	printf("\t\n");
	printf("options:\n");
	printf("\t[-d DPDK huge memory size in MB]\n");
	printf("\t[-g use single file descriptor for DPDK memory segments]\n");
	printf("\t[-i shared memory group ID]\n");
	printf("\t[-r remote NVMe over Fabrics target address]\n");
	printf("\t[-V enumerate VMD]\n");
#ifdef DEBUG
	printf("\t[-L enable debug logging]\n");
#else
	printf("\t[-L enable debug logging (flag disabled, must reconfigure with --enable-debug)\n");
#endif
}

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
	get_info(ns);
	// printf("Namespace ID: %d size: %juGB\n", spdk_nvme_ns_get_id(ns, spdk_nvme_ns_get_size(ns) / 1000000000));
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

void read_complete(void *arg, const struct spdk_nvme_cpl *completion)
{
	struct sequence *sequence = arg;

	/* Assume the I/O was successful */
	sequence->is_completed = 1;
	/* See if an error occurred. If so, display information
	 * about it, and set completion value so that I/O
	 * caller is aware that an error occurred.
	 */
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
	printf("read: %s\n", sequence->buf);
	spdk_free(sequence->buf);
}

void write_complete(void *arg, const struct spdk_nvme_cpl *completion)
{
	struct sequence	*sequence = arg;
	struct ns_entry			    *ns_entry = sequence->ns_entry;

	/* See if an error occurred. If so, display information
	 * about it, and set completion value so that I/O
	 * caller is aware that an error occurred.
	 */
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
	
	// int				            rc;
	// sequence->buf = spdk_zmalloc(buffer_sz, buffer_sz, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
    // uint32_t sys_lba_size = spdk_nvme_ns_get_sector_size(ns_entry->ns);
    // uint32_t lba_num = buffer_sz/sys_lba_size;
	// rc = spdk_nvme_ns_cmd_read(ns_entry->ns, ns_entry->qpair, sequence->buf,
	// 			   user_sector_start, /* LBA start */
	// 			   lba_num, /* number of LBAs */
	// 			   read_complete, (void *)sequence, 0);
	// if (rc != 0) {
	// 	fprintf(stderr, "starting read I/O failed\n");
	// 	exit(1);
	// }
}

// void spdk_write(void)
// {
// 	struct ns_entry			    *ns_entry;
// 	struct sequence	            sequence;
// 	int				            rc;
//     int                         offset;
// 	size_t				        sz;

// 	TAILQ_FOREACH(ns_entry, &g_namespaces, link) {

// 		ns_entry->qpair = spdk_nvme_ctrlr_alloc_io_qpair(ns_entry->ctrlr, NULL, 0);
// 		if (ns_entry->qpair == NULL) {
// 			printf("ERROR: spdk_nvme_ctrlr_alloc_io_qpair() failed\n");
// 			return;
// 		}
// 		sequence.using_cmb_io = 1;
// 		sequence.buf = spdk_nvme_ctrlr_map_cmb(ns_entry->ctrlr, &sz);
// 		if (sequence.buf == NULL || sz < buffer_sz) {
// 			sequence.using_cmb_io = 0;
// 			sequence.buf = spdk_zmalloc(buffer_sz, buffer_sz, NULL, 
//             SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
// 		}
// 		if (sequence.buf == NULL) {
// 			printf("ERROR: write buffer allocation failed\n");
// 			return;
// 		}
// 		if (sequence.using_cmb_io) {
// 			printf("INFO: using controller memory buffer for IO\n");
// 		} else {
// 			printf("INFO: using host memory buffer for IO\n");
// 		}
// 		sequence.is_completed = 0;
// 		sequence.ns_entry = ns_entry;
// 		if (spdk_nvme_ns_get_csi(ns_entry->ns) == SPDK_NVME_CSI_ZNS) {
// 			reset_zone_and_wait_for_completion(&sequence);
// 		}

//         /* Write from file to buffer 
//          * Open the file then read and print the file contents
//          */
//         //// FILE *file;
//         // char buffer[get_buffer_sz()];
//         // size_t bytesRead;
//         // file = fopen("4k_text.txt", "r");
//         // if (file == NULL) {
//         //     perror("Error opening file");
//         //     return;
//         // }
//         // while ((bytesRead = fread(buffer, 1, sizeof(buffer), file)) > 0) {
//         //     fwrite(buffer, 1, bytesRead, stdout);
//         // }
//         // Close the file
//         // fclose(file);
        
//         char buffer[buffer_sz];
//         for (int i = 0; i < buffer_sz - 1; i++) {
//             buffer[i] = 'C'; // Use 'A' for simplicity
//         }
//         buffer[buffer_sz - 1] = '\0'; // Null-terminate the string for safety
        
//         /* Check the LBA size */
//         uint32_t sys_lba_size = spdk_nvme_ns_get_sector_size(ns_entry->ns);
//         uint32_t lba_num = buffer_sz/sys_lba_size;

//         snprintf(sequence.buf, buffer_sz, "%s", buffer);

//         rc = spdk_nvme_ns_cmd_write(ns_entry->ns, ns_entry->qpair, sequence.buf,
// 					    user_sector_start, /* LBA start */
// 					    floor(lba_num), /* number of LBAs */
// 					    write_complete, &sequence, 0);
//         if (rc != 0) {
//             fprintf(stderr, "starting write I/O failed\n");
//             exit(1);
//         }
        
//         while (!sequence.is_completed) {
//             spdk_nvme_qpair_process_completions(ns_entry->qpair, 0);
//         }
// 		spdk_nvme_ctrlr_free_io_qpair(ns_entry->qpair);
// 	}
// }

static void spdk_read(uint64_t lba_start, uint32_t lba_count, size_t buffer_sz, uint64_t qdepth)
{
    struct ns_entry 					*ns_entry;
    struct sequence 					sequence;
    int 								rc;
    size_t 								sz;

    TAILQ_FOREACH(ns_entry, &g_namespaces, link) {
        ns_entry->qpair = spdk_nvme_ctrlr_alloc_io_qpair(ns_entry->ctrlr, NULL, 0);
        if (ns_entry->qpair == NULL) {
            fprintf(stderr, "ERROR: spdk_nvme_ctrlr_alloc_io_qpair() failed\n");
            return;
        }

        sequence.using_cmb_io = 1;
        sequence.buf = spdk_nvme_ctrlr_map_cmb(ns_entry->ctrlr, &sz);
        if (sequence.buf == NULL || sz < buffer_sz) {
            sequence.using_cmb_io = 0;
            sequence.buf = spdk_zmalloc(buffer_sz, buffer_sz, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
        }
        if (sequence.buf == NULL) {
            fprintf(stderr, "ERROR: buffer allocation failed\n");
            spdk_nvme_ctrlr_free_io_qpair(ns_entry->qpair);
            return;
        }

        printf("INFO: Using %s memory buffer for I/O\n", sequence.using_cmb_io ? "controller" : "host");

        sequence.is_completed = 0;
        sequence.ns_entry = ns_entry;

        if (spdk_nvme_ns_get_csi(ns_entry->ns) == SPDK_NVME_CSI_ZNS) {
            reset_zone_and_wait_for_completion(&sequence);
        }

        rc = spdk_nvme_ns_cmd_read(ns_entry->ns, ns_entry->qpair, sequence.buf,
                    lba_start, lba_count, read_complete, &sequence, 0);

        if (rc != 0) {
            fprintf(stderr, "ERROR: starting read I/O failed\n");
            spdk_nvme_ctrlr_free_io_qpair(ns_entry->qpair);
            exit(1);
        }

        while (!sequence.is_completed) {
            spdk_nvme_qpair_process_completions(ns_entry->qpair, 0);
        }

        spdk_nvme_ctrlr_free_io_qpair(ns_entry->qpair);
    }
}


int pos_int_input(void) {
    int num;
    char buffer[100];

    if (fgets(buffer, sizeof(buffer), stdin) != NULL) {
        char *endptr;
        num = strtol(buffer, &endptr, 10);
        if (*endptr == '\0' || *endptr == '\n') {
            return num;
        } else {
            return -1;
        }
    } else {
        return -1;
    }
}

uint64_t llu_input(void) {
	uint64_t num;
	char buffer[100];

	if (fgets(buffer, sizeof(buffer), stdin) != NULL) {
		char *endptr;
		num = strtoull(buffer, &endptr, 10);
		if (*endptr == '\0' || *endptr == '\n') {
			return num;
		} else {
			return -1;
		}
	} else {
		return -1;
	}
}

int sel_pattern(void) {
	int patt = -1;
	while (patt == -1)
	{
		printf("Choose the pattern to write among the following...\n");
		printf("0: Fill zeroes\n");
		printf("1: Fill ones\n");
		printf("2: Incremental\n");
		printf("3: Back\n");
		printf("Pattern: ");
		patt = pos_int_input();
		if (patt == 3) {
			return 3;
			break;
		}
		else if (patt < 0 || patt > 2) {
			printf("%sInvalid pattern\n", KRED);
			continue;
		}
		return patt;
		break;
	}
}

int main(int argc, char **argv)
{
	int rc;
	int op = -1, patt = -1;
	struct spdk_env_opts opts;
	uint64_t start=0, length=1, qdepth;
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
	while (true)
	{
		printf("%sChoose the operation among the following...\n", KNRM);
		printf("0: Write\n");
		printf("1: Read\n");
		printf("Operation: ");
		op = pos_int_input();
		
		if (op == 0) {
			patt = sel_pattern();
			if (patt == 3) {
				continue;
			}
			else {
				user_pattern = patt;
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
	
	while (true)
	{
		if (user_pattern == -1) {
			printf("%sStart reading at sector: ", KNRM);
		} else {
			printf("%sStart writing at sector: ", KNRM);
		}

		start = llu_input();
		if ((int)start == -1) {
			printf("%sInvalid start\n", KRED);
			continue;
		}
		if (start > max_sector_count) {
			printf("%sStart sector exceeds the SSD size\n", KRED);
			continue;
		} else {
			user_sector_start = start;
			break;
		}
	}

	while (true)
	{
		if ((int)user_pattern == -1) {
			printf("%sReading length (aka sector count): ", KNRM);
		} else {
			printf("%sWriting length (aka sector count): ", KNRM);
		}

		length = llu_input();
		if ((int)length == -1) {
			printf("%sInvalid length\n", KRED);
			continue;
		}
		if (length > max_sector_count - user_sector_start) {
			printf("%sLength exceeds the SSD size\n", KRED);
			continue;
		} else if (length > 4294967295) {
			printf("%sLength exceeds the maximum size\n", KRED);
			continue;
		} else {
			user_sector_count = length;
			user_buffer_size = length * 512;
			break;
		}
	}

		
	// printf("How many queues depth do you want to use?\n");
	// rt = scanf("%lu\n", qdepth);

	time_t start_time, end_time;
	if ((int)user_pattern == -1)
	{
		printf("Start reading...\n");
		printf("LBA start: %lu\n", user_sector_start);
		printf("LBA count: %lu\n", user_sector_count);
		start_time = clock();
		spdk_read(user_sector_start, user_sector_count, user_buffer_size, user_qdepth);
		end_time = clock();

		printf("Time: %.10f sec\n", ((double)end_time-start_time)/CLOCKS_PER_SEC);
	} else
	{
		start_time = clock();
		// spdk_write();
		end_time = clock();

		printf("Time: %lu sec\n", (end_time-start_time)/CLOCKS_PER_SEC);
	}

    cleanup();
	if (g_vmd) {
		spdk_vmd_fini();
	}

exit:
	cleanup();
	spdk_env_fini();
	return rc;
}
