#include "spdk/stdinc.h"
#include "spdk/nvme.h"
#include "spdk/thread.h"
#include "spdk/vmd.h"
#include "spdk/nvme_zns.h"
#include "spdk/env.h"
#include "spdk/string.h"
#include "spdk/log.h"

#include "spdk_nvme.h"

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

void print_info(struct spdk_nvme_ns *ns)
{
	uint64_t sys_sector_count = spdk_nvme_ns_get_num_sectors(ns);
	uint32_t sys_sector_size = spdk_nvme_ns_get_sector_size(ns);
	uint64_t sys_byte_size = sys_sector_count * sys_sector_size;

	printf("Namespace ID: %d\n", spdk_nvme_ns_get_id(ns));
	printf("Number of Sectors: %lu\n", sys_sector_count);
	printf("Sector Size: %u bytes\n", sys_sector_size);
	printf("Namespace Size: %lu bytes %.2f GB\n", sys_byte_size, (float)sys_byte_size / (1024 * 1024 * 1024));
	return;
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

int sel_operation(void) {
	int op = -1;
	while (true)
	{
		printf("%sChoose the operation among the following...\n", KNRM);
		printf("0: Write\n");
		printf("1: Read\n");
		printf("Operation: ");
		op = pos_int_input();

		if (op == 0 || op == 1) {
			break;
		}
		else {
			printf("%sInvalid operation\n", KRED);
			continue;
		}
	}
	return op;
}

int sel_pattern(void) {
	int patt = -1;
	while (true)
	{
		printf("Choose the pattern to write among the following...\n");
		printf("0: Fill zeroes\n");
		printf("1: Fill ones\n");
		printf("2: Incremental\n");
		printf("3: Without pattern\n");
		// printf("4: Back\n");
		printf("Pattern: ");
		patt = pos_int_input();
		// if (patt == 4) {
		// 	operation();

		// }
		if (patt < 0 || patt > 3) {
			printf("%sInvalid pattern\n", KRED);
			continue;
		}
		break;
	}
	return patt;
}

uint64_t get_lba_current(uint64_t lba_start, size_t buffer_sz, uint64_t lba_c_out) {
    return lba_start + (lba_c_out * buffer_sz);
}

