#define KNRM  "\x1B[0m"
#define KRED  "\x1B[31m"

struct ctrlr_entry 
{
	struct spdk_nvme_ctrlr		*ctrlr;
	TAILQ_ENTRY(ctrlr_entry)	link;
	char				        name[1024];
};
struct ns_entry 
{
	struct spdk_nvme_ctrlr	*ctrlr;
	struct spdk_nvme_ns	    *ns;
	TAILQ_ENTRY(ns_entry)	link;
	struct spdk_nvme_qpair	*qpair;
};

struct spdk_nvme_transport_id g_trid = {};
bool g_vmd = false;

struct sequence {
	struct ns_entry	*ns_entry;
	uint32_t		*buf;
    size_t      	buffer_sz;
	unsigned    	using_cmb_io;
	int		    	is_completed;
	int 			user_pattern;
	uint64_t 		user_sector_start;
};

/* spdk_nvme*/
/** provided **/
void reset_zone_and_wait_for_completion(struct sequence *sequence);
void reset_zone_complete(void *arg, const struct spdk_nvme_cpl *completion);
void usage(const char *program_name);

/** added **/
void print_info(struct spdk_nvme_ns *ns);
int pos_int_input(void);
uint64_t llu_input(void);
int sel_pattern(void);
uint64_t get_lba_current(
	uint64_t 							lba_start, 
	size_t 								buffer_sz, 
	uint64_t 							lba_c_out
);
struct sequence sequence_setup(
	size_t 								seq_buffer_size,
	struct ns_entry 					*ns_entry,
	struct sequence 					sequence
);
/*************************************************************************/

/* spdk_test */
/** provided **/
int parse_args(
	int 								argc, 
	char 								**argv, 
	struct spdk_env_opts 				*env_opts);
void register_ns(
	struct spdk_nvme_ctrlr 				*ctrlr, 
	struct spdk_nvme_ns 				*ns);
bool probe_cb(
	void 								*cb_ctx, 
	const struct spdk_nvme_transport_id *trid, 
	struct spdk_nvme_ctrlr_opts 		*opts);
void attach_cb(
	void 								*cb_ctx, 
	const struct spdk_nvme_transport_id *trid, 
	struct spdk_nvme_ctrlr 				*ctrlr, 
	const struct spdk_nvme_ctrlr_opts 	*opts);
void read_complete(
	void 								*arg, 
	const struct spdk_nvme_cpl 			*completion);
void write_complete(
	void 								*arg, 
	const struct spdk_nvme_cpl 			*completion);
void cleanup(void);

/** added **/

void spdk_read(	
	uint64_t 							user_sector_start,
	uint64_t 							user_sector_count,
	size_t								user_buffer_size,
	int 								user_pattern,
	uint32_t 							user_command_size,
	uint64_t 							user_qdepth,
	uint32_t 							max_xfer_sector
);		
void spdk_write(
	uint64_t 							user_sector_start,
	uint64_t 							user_sector_count,
	size_t								user_buffer_size,
	int 								user_pattern,
	uint32_t 							user_command_size,
	uint64_t 							user_qdepth,
	uint32_t 							max_xfer_sector
);
int sel_operation(void);
void read_verify(int user_pattern, uint32_t *buffer, size_t user_buffer_size, uint64_t user_sector_start);
int main_loop(void);
int main(int argc, char **argv);
/*************************************************************************/


// int random_lba_start(uint64_t MEM_SIZE, uint64_t LBA_SIZE) {
//     int lba_sectors = (MEM_SIZE / LBA_SIZE);
//     int rnd = rand() % lba_sectors;
//     printf("LBA sector: %d\n", rnd);
//     return rnd;
// }