/* Copyright 2009-2012 Thomas Schoebel-Theuer /  1&1 Internet AG
 *
 * Email: tst@1und1.de
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

/* Replay of blktrace traces.
 * 
 * The standard input must be in a special format (see docs).
 *
 * TODO: Reorganize / modularize code. It grew over a too long time, out
 * of a really _trivial_ initial version. Too many features were added
 * which were originally never envisioned.
 * Replace some global variables with helper structs for
 * parameters, runtime states, etc.
 *
 * MAYBE: Add reasonable getopt() in place of wild-grown parameter parsing,
 * but KEEP PORTABILITY IN MIND, even to very old UNICES.
 *
 * Exception: for good timer resolution, we use nanosleep() & friends.
 * But this can be easily abstracted/interfaced to
 * elder kernel interfaces when necessary.
 */

#define _FILE_OFFSET_BITS 64
#define _LARGEFILE64_SOURCE
#define _GNU_SOURCE
#include <config.h>

#include <ctype.h>

#include <stdio.h>

#include <errno.h>

#ifdef STDC_HEADERS
# include <stdlib.h>
# include <stddef.h>
#else
# ifdef HAVE_STDLIB_H
#  include <stdlib.h>
# endif
#endif

#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif

#ifdef HAVE_LIMITS_H
# include <limits.h>
#endif

#include <fcntl.h>

#ifdef HAVE_STRING_H
# if !defined STDC_HEADERS && defined HAVE_MEMORY_H
#  include <memory.h>
# endif
#include <string.h>
#endif
#ifdef HAVE_STRINGS_H
# include <strings.h>
#endif

#include <time.h>

#include <math.h>

#include <signal.h>

#ifdef HAVE_SYS_STAT_H
# include <sys/stat.h>
#endif

#ifdef HAVE_SYS_TYPES_H
# include <sys/types.h>
#endif

/**********************************************************
 *
 */

#ifndef HAVE_DECL_POSIX_MEMALIGN
# if HAVE_MALLOC_H && HAVE_DECL_MEMALIGN
#  define posix_memalign(res,align,size) ((*(res)) = memalign((align), (size)), 0)
# else
#  define posix_memalign(res,align,size) ((*(res)) = malloc(size), 0)
# endif
#endif

#if !HAVE_DECL_LSEEK64
# define lseek64 lseek
#endif

#if HAVE_DECL_RANDOM
# define RAND_TYPE long int
#else
# define RAND_TYPE int
# define random rand
#endif


// use -lrt -lm for linking.
//
// example: gcc -Wall -O2 blkreplay.c -lrt -lm -o blkreplay

#define QUEUES               4096
#define MAX_THREADS         32768
#define FILL_FACTOR             8
#define DEFAULT_START_GRACE    15
#define DEFAULT_THREADS      1024
#define DEFAULT_FAN_OUT         4
#define DEFAULT_SPEEDUP       1.0

#ifndef TMP_DIR
# define TMP_DIR "/tmp"
#endif
#define VERIFY_TABLE     "verify_table" // default filename for verify table
#define COMPLETION_TABLE "completion_table" // default filename for completed requests

#define RQ_HASH_MAX (16 * QUEUES)
#define FLOAT long double

//#define DEBUG_TIMING

FLOAT time_factor = DEFAULT_SPEEDUP;
FLOAT time_stretch = 1.0 / DEFAULT_SPEEDUP;
int replay_start = 0;
int replay_end = 0;
int replay_duration = 0;
int replay_out = 0;
int already_forked = 0;
int overflow = 0;
int verify_fd = -1;
int complete_fd = -1;
int main_fd = -1;
char *mmap_ptr = NULL;
char *main_name = NULL;
long long main_size = 0;
long long max_size = 0;
int dry_run = 0;
int fake_io = 0;
int fork_dispatcher = 1;
int use_o_direct = 1;
int use_o_sync = 0;

int fill_random = 0;
int mmap_mode = 0;
int conflict_mode = 2; 
/* 0 = allow arbitrary permutations in ordering
 * 1 = drop conflicting requests
 * 2 = partial order by pushing back conflicting requests
 * 3 = enforce ordering by waiting for conflicts
 */
int strong_mode = 1;
/* 0 = conflicts are only counting between write/write
 * 1 = conflicts are between read/write and write/write
 * 2 = treat any damaged IO as conflicting
 */
int verify_mode = 0; 
/* 0 = no verify
 * 1 = verify data during execution
 * 2 = additionally verify data after execution
 * 3 = additionally re-read and verify data immediatley after each write
 */
int final_verify_mode = 0; 

int table_max = 0;
int total_max = DEFAULT_THREADS; // parallelism
int sub_max = 0;
int fan_out = DEFAULT_FAN_OUT;
int verbose = 0;
int bottleneck = 0;

int count_submitted = 0;   // number of requests on the fly
int count_catchup = 0;     // number of requests catching up
int count_pushback = 0;    // number of requests on pushback list

int max_submitted = 0;
int max_pushback = 0;

int statist_lines = 0;     // total number of input lines
int statist_total = 0;     // total number of requests
int statist_writes = 0;    // total number of write requests
int statist_completed = 0; // number of completed requests
int statist_dropped = 0;   // number of dropped requests (verify_mode == 1)
int statist_pushback = 0;  // number of pushed back requests (verify_mode == 2)
int statist_ordered = 0;   // number of waits (verify_mode == 3)
long long verify_errors = 0;
long long verify_errors_after = 0;
long long verify_mismatches = 0;

struct timespec start_grace = {
	.tv_sec = DEFAULT_START_GRACE,
};
struct timespec start_stamp = {};
struct timespec first_stamp = {};
struct timespec timeshift = {};
struct timespec meta_delays = {};
long long meta_delay_count;
struct timespec simulate_io = {};
struct timespec ahead_limit = {};

#define _STRINGIFY(x) #x
#define STRINGIFY(x) _STRINGIFY(x)

///////////////////////////////////////////////////////////////////////

#define NANO 1000000000

static
void timespec_diff(struct timespec *res, struct timespec *start, struct timespec *time)
{
	res->tv_sec = time->tv_sec - start->tv_sec;
	res->tv_nsec = time->tv_nsec - start->tv_nsec;
	//if (time->tv_nsec < start->tv_nsec) {
	if (res->tv_nsec < 0) {
		res->tv_nsec += NANO;
		res->tv_sec--;
	}
}

static
void timespec_add(struct timespec *res, struct timespec *time)
{
	res->tv_sec += time->tv_sec;
	res->tv_nsec += time->tv_nsec;
	if (res->tv_nsec >= NANO) {
		res->tv_nsec -= NANO;
		res->tv_sec++;
	}
}

static
void timespec_multiply(struct timespec *res, FLOAT factor)
{
	FLOAT val = res->tv_nsec * (1.0/(FLOAT)NANO) + res->tv_sec;
	val *= factor;
	res->tv_sec = floor(val);
	res->tv_nsec = floor((val - res->tv_sec) * NANO);
}

///////////////////////////////////////////////////////////////////////

// output flushing

static struct timespec flush_total = {};
char *my_role = "undefined";

static
void flush_stdout(void)
{
	if (verbose > 0) {
		struct timespec before;
		struct timespec after;
		struct timespec diff;
		
		clock_gettime(CLOCK_REALTIME, &before);
		fflush(stdout);
		clock_gettime(CLOCK_REALTIME, &after);
		
		timespec_diff(&diff, &before, &after);
		timespec_add(&flush_total, &diff);
	} else { // save processor time and syscalls
		fflush(stdout);
	}
}

static
void set_role(char *txt)
{
	my_role = txt;
	if (verbose > 0) {
		printf("INFO: process pid=%d role='%s'\n",
		       getpid(),
		       my_role);
		flush_stdout();
	}
}

static
void do_exit(int status)
{
	if (verbose > 0) {
		printf("INFO: exit pid=%d role='%s' status=%d flush_total=%lu.%09lu\n",
		       getpid(),
		       my_role,
		       status,
		       flush_total.tv_sec, flush_total.tv_nsec);
	}
	exit(status);
}

///////////////////////////////////////////////////////////////////////

// internal data structures

struct verify_tag {
	long long tag_start;
	unsigned int tag_seqnr;
	unsigned int tag_write_seqnr;
	long long tag_len;
	long long tag_index;
};

struct request {
	struct verify_tag tag;
	struct timespec orig_stamp;
	struct timespec orig_factor_stamp;
	struct timespec replay_stamp;
	struct timespec replay_duration;
	long long seqnr;
	long long sector;
	int length;
	int verify_errors;
	short q_nr;
	short q_index;
	char rwbs;
	char has_version;
	// starting from here, the rest is _not_ transferred over the pipelines
	struct request *next;
	unsigned int *old_version;
};

// the following reduces a potential space bottleneck on the answer pipe
#define RQ_SIZE (sizeof(struct request) - 2*sizeof(void*))
#ifdef PIPE_BUF
# define FILL_MAX  (PIPE_BUF / RQ_SIZE)
#else
# define FILL_MAX  (1024 / RQ_SIZE)
#endif

static struct request *rq_hash[RQ_HASH_MAX] = {};

#define rq_hash_fn(sector) (sector % RQ_HASH_MAX)

static
struct request *find_request_seq(long long sector, long long seqnr)
{
	struct request *rq = rq_hash[rq_hash_fn(sector)];
	while (rq && rq->seqnr != seqnr)
		rq = rq->next;
	return rq;
}

static
void add_request(struct request *rq)
{
	struct request **ptr = &rq_hash[rq_hash_fn(rq->sector)];
	rq->next = *ptr;
	*ptr = rq;
}

static
void del_request(long long sector, long long seqnr)
{
	struct request **ptr = &rq_hash[rq_hash_fn(sector)];
	struct request *found = NULL;
	while (*ptr && (*ptr)->seqnr != seqnr) {
		ptr = &(*ptr)->next;
	}
	if (*ptr && (*ptr)->sector == sector) {
		found = *ptr;
		*ptr = found->next;
		if (found->old_version) {
			free(found->old_version);
		}
		free(found);
	}
}

///////////////////////////////////////////////////////////////////////

/* Fast in-memory determination of conflicts.
 * Used in place of temporary files (where possible).
 */
#define FLY_HASH      2048
#define FLY_ZONE        64
#define FLY_HASH_FN(sector) (((sector) / FLY_ZONE) % FLY_HASH)
#define FLY_NEXT_ZONE(sector) (((sector / FLY_ZONE) + 1) * FLY_ZONE)

struct fly {
	struct fly *fl_next;
	long long   fl_sector;
	int         fl_len;
	char        fl_rwbs;
};

struct fly_hash {
	struct fly *fly_hash_table[FLY_HASH];
	int         fly_count;
};

static
void fly_add(struct fly_hash *hash, long long sector, int len, char rwbs)
{
	while (len > 0) {
		struct fly *new = malloc(sizeof(struct fly));
		int index = FLY_HASH_FN(sector);
		int max_len = FLY_NEXT_ZONE(sector) - sector;
		int this_len = len;
		if (this_len > max_len)
			this_len = max_len;

		if (!new) {
			printf("FATAL ERROR: out of memory for hashing\n");
			flush_stdout();
			do_exit(-1);
		}
		new->fl_sector = sector;
		new->fl_len = this_len;
		new->fl_rwbs = rwbs;
		new->fl_next = hash->fly_hash_table[index];
		hash->fly_hash_table[index] = new;
		hash->fly_count++;

		sector += this_len;
		len -= this_len;
	}
}

static
int fly_check(struct fly_hash *hash, long long sector, int len, char rwbs)
{
	while (len > 0) {
		struct fly *tmp = hash->fly_hash_table[FLY_HASH_FN(sector)];
		int max_len = FLY_NEXT_ZONE(sector) - sector;
		int this_len = len;
		if (this_len > max_len)
			this_len = max_len;

		for (; tmp != NULL; tmp = tmp->fl_next) {
			if (sector + this_len > tmp->fl_sector && sector < tmp->fl_sector + tmp->fl_len) {
				if (strong_mode >= 2)
					return 1;
				if (rwbs == 'R' && toupper(tmp->fl_rwbs) == 'R')
					continue;
				if (strong_mode ||
				    (rwbs == 'W' &&
				     toupper(tmp->fl_rwbs) == 'W'))
					return 1;
			}
		}

		sector += this_len;
		len -= this_len;
	}
	return 0;
}

static
void fly_delete(struct fly_hash *hash, long long sector, int len, char rwbs)
{
	while (len > 0) {
		struct fly **res = &hash->fly_hash_table[FLY_HASH_FN(sector)];
		struct fly *tmp;
		int max_len = FLY_NEXT_ZONE(sector) - sector;
		int this_len = len;
		if (this_len > max_len)
			this_len = max_len;
		
		for (tmp = *res; tmp != NULL; res = &tmp->fl_next, tmp = *res) {
			if (tmp->fl_sector == sector && tmp->fl_len == this_len && toupper(tmp->fl_rwbs) == rwbs) {
				*res = tmp->fl_next;
				hash->fly_count--;
				free(tmp);
				break;
			}
		}
		
		sector += this_len;
		len -= this_len;
	}
}

static struct fly_hash fly_hash = {};

///////////////////////////////////////////////////////////////////////

// abstracting read() and write()

static
int do_read(void *buffer, int len)
{
#ifdef HAVE_DECL_NANOSLEEP
	if (simulate_io.tv_sec || simulate_io.tv_nsec)
		nanosleep(&simulate_io, NULL);
#endif
	if (dry_run)
		return len;
	if (mmap_ptr) {
	}
	return read(main_fd, buffer, len);
}

static
int do_write(void *buffer, int len)
{
#ifdef HAVE_DECL_NANOSLEEP
	if (simulate_io.tv_sec || simulate_io.tv_nsec)
		nanosleep(&simulate_io, NULL);
#endif
	if (dry_run)
		return len;
	if (mmap_ptr) {
	}
	return write(main_fd, buffer, len);
}

///////////////////////////////////////////////////////////////////////

// version number bookkeeping

static
unsigned int *get_blockversion(int fd, long long blocknr, int count)
{
	int status;
	void *data;
	int memlen = count * sizeof(unsigned int);
	struct timespec t0;
	struct timespec t1;
	struct timespec delay;

	if (!verify_mode)
		return NULL;

	clock_gettime(CLOCK_REALTIME, &t0);

	data = malloc(memlen);
	if (!data) {
		printf("FATAL ERROR: out of memory for hashing\n");
		flush_stdout();
		verify_mode = 0;
		return NULL;
	}
	if (lseek64(fd, blocknr * sizeof(unsigned int), SEEK_SET) != blocknr * sizeof(unsigned int)) {
		printf("FATAL ERROR: llseek(%lld) in verify table failed %d (%s)\n", blocknr, errno, strerror(errno));
		flush_stdout();
		verify_mode = 0;
		free(data);
		return NULL;
	}

	status = read(fd, data, memlen);

	clock_gettime(CLOCK_REALTIME, &t1);
	timespec_diff(&delay, &t0, &t1);
	timespec_add(&meta_delays, &delay);
	meta_delay_count++;

	if (status < 0) {
		printf("FATAL ERROR: read(%lld) from verify table failed %d %d (%s)\n", blocknr, status, errno, strerror(errno));
		flush_stdout();
		verify_mode = 0;
		free(data);
		return NULL;
	}
	if (status < memlen) { // this may result from a sparse file
		memset(data+status, 0, memlen - status);
	}
	return data;
}

static
void put_blockversion(int fd, unsigned int *data, long long blocknr, int count)
{
	int status;
	struct timespec t0;
	struct timespec t1;
	struct timespec delay;

	if (!verify_mode)
		return;

	clock_gettime(CLOCK_REALTIME, &t0);

	if (lseek64(fd, blocknr * sizeof(unsigned int), SEEK_SET) != blocknr * sizeof(unsigned int)) {
		printf("FATAL ERROR: llseek(%lld) in verify table failed %d (%s)\n", blocknr, errno, strerror(errno));
		flush_stdout();
		verify_mode = 0;
		return;
	}

	status = write(fd, data, count * sizeof(unsigned int));

	clock_gettime(CLOCK_REALTIME, &t1);
	timespec_diff(&delay, &t0, &t1);
	timespec_add(&meta_delays, &delay);
	meta_delay_count++;

	if (status < 0) {
		printf("FATAL ERROR: write to verify table failed %d (%s)\n", errno, strerror(errno));
		flush_stdout();
		verify_mode = 0;
	}
}

static
void force_blockversion(int fd, unsigned int version, long long blocknr, int count)
{
	if (!verify_mode)
		return;
	{
		unsigned int data[count];
		int i;
		for (i = 0; i < count; i++)
			data[i] = version;
		put_blockversion(fd, data, blocknr, count);
	}
}

static
int compare_blockversion(unsigned int *a, unsigned int *b, int count)
{
	for (; count > 0; a++, b++, count--) {
		if (!*a)
			continue; // ignore
		if (!*b || *a > *b)
			return 1; // mismatch
	}
	return 0;
}

static
void advance_blockversion(unsigned int *data, int count, unsigned int version)
{
	for (; count > 0; data++, count--) {
		if (*data < version)
			*data = version;
	}
}

///////////////////////////////////////////////////////////////////////

// infrastructure for verify mode

static
void check_tags(struct request *rq, void *buffer, int len, int do_write)
{
	int i;
	char *mode = do_write ? "write" : "read";
	if (!verify_mode || !rq->old_version)
		return;
	for (i = 0; i < len; i += 512) {
		struct verify_tag *tag = buffer+i;
		if (!rq->old_version[i/512]) { // version not yet valid
			continue;
		}
		if (dry_run)
			continue;
		if (tag->tag_start != start_stamp.tv_sec) {
			printf("VERIFY ERROR (%s): bad start tag at sector %lld+%d (tag %lld != [expected] %ld)\n", mode, rq->sector, i/512, tag->tag_start, start_stamp.tv_sec);
			flush_stdout();
			rq->verify_errors++;
			continue;
		}
		/* Only writes are fully protected from overlaps with other writes.
		 * Reads may overlap with concurrent writes to some degree.
		 * Therefore we check reads only for backslips in time
		 * (which never should happen).
		 */
		if ((do_write && tag->tag_write_seqnr != rq->old_version[i/512]) ||
		   tag->tag_write_seqnr < rq->old_version[i/512]) {
			printf("VERIFY ERROR (%s): data version mismatch at sector %lld+%d (seqnr %u != [expected] %u)\n", mode, rq->sector, i/512, tag->tag_write_seqnr, rq->old_version[i/512]);
			flush_stdout();
			rq->verify_errors++;
			continue;
		}
	}
}

static
void make_tags(struct request *rq, void *buffer, int len)
{
	int random_border;
	int random_rest;
	int i;

	// check old tag before overwriting
	if (verify_mode >= 1) {
		int status;
		status = do_read(buffer, len);
		if (status == len) {
			check_tags(rq, buffer, len, 1);
		}
		if (lseek64(main_fd, rq->sector * 512, SEEK_SET) != rq->sector * 512) {
			printf("ERROR: bad lseek at make_tags()\n");
			flush_stdout();
		}
	}

	if (fill_random < 0)
		fill_random = 0;
	random_border = fill_random * 512 / (100 * sizeof(RAND_TYPE));
	if (random_border > 512 / sizeof(RAND_TYPE))
		random_border = 512 / sizeof(RAND_TYPE);
	random_rest = 512 / sizeof(RAND_TYPE) - random_border;

	for (i = 0; i < len; i += 512) {
		struct verify_tag *tag = buffer+i;
		if (random_rest > 0)
			memset(tag, 0, random_rest * sizeof(RAND_TYPE));
		if (random_border > 0) {
			int j;
			for (j = random_rest; j < 512 / sizeof(RAND_TYPE); j++) {
				RAND_TYPE *ptr = (void*)&((char*)buffer+i)[j * sizeof(RAND_TYPE)];
				*ptr = random();
			}
		}
		memcpy(tag, &rq->tag, sizeof(*tag));
		tag->tag_len = len;
		tag->tag_index = i;
	}
}

#define TAG_CHUNK (4096 * 1024 / sizeof(unsigned int))

static
void check_all_tags()
{
	long long blocknr = 0;
	int status;
	long long checked = 0;
	int i;
	unsigned int *table = NULL;
	unsigned int *table2 = NULL;
	void *buffer = NULL;
	struct verify_tag *tag;

	printf("checking all tags..........\n");
	flush_stdout();

	if (posix_memalign((void**)&table, 4096, TAG_CHUNK) ||
	    posix_memalign((void**)&table2, 4096, TAG_CHUNK) ||
	    posix_memalign(&buffer, 512, 512)) {
		printf("FATAL ERROR: cannot allocate memory\n");
		flush_stdout();
		do_exit(-1);
	}
	tag = buffer;

	lseek64(verify_fd, 0, SEEK_SET);
	lseek64(complete_fd, 0, SEEK_SET);

	for (;;) {
		status = read(verify_fd, table, TAG_CHUNK);
		if (!status)
			break;
		if (status < 0) {
			printf("ERROR: cannot read version table for block %lld: %d %s\n", blocknr, errno, strerror(errno));
			flush_stdout();
			break;
		}
		status = read(complete_fd, table2, TAG_CHUNK);
		if (status <= 0) {
			printf("ERROR: cannot read completion table for block %lld: %d %s\n", blocknr, errno, strerror(errno));
			flush_stdout();
			break;
		}

		for (i = 0; i < status / sizeof(unsigned int); i++, blocknr++) {
			unsigned int version = table[i];
			unsigned int version2;

			if (!version)
				continue;

			if (lseek64(main_fd, blocknr * 512, SEEK_SET) != blocknr * 512) {
				printf("ERROR: bad lseek in check_all_tags()\n");
				flush_stdout();
			}
			if (do_read(buffer, 512) != 512) {
				printf("ERROR: bad read in check_all_tags(): %d %s\n", errno, strerror(errno));
				flush_stdout();
			}
			checked++;
			version2 = table2[i];
			if (dry_run)
				continue;
			if (tag->tag_write_seqnr != version && tag->tag_write_seqnr != version2) {
				if (version != version2) {
					printf("VERIFY MISMATCH: at block %lld (%u != %u != %u)\n", blocknr, tag->tag_write_seqnr, version, version2);
					flush_stdout();
					verify_mismatches++;
				} else {
					printf("VERIFY ERROR:    at block %lld (%u != [expected] %u)\n", blocknr, tag->tag_write_seqnr, version);
					flush_stdout();
					verify_errors_after++;
				}
			}
		}
	}
	printf("SUMMARY: checked %lld / %lld blocks (%1.3f%%), found %lld errors, %lld mismatches\n", checked, max_size, 100.0 * (double)checked / (double)max_size, verify_errors_after, verify_mismatches);
	flush_stdout();
	free(table);
	free(table2);
	free(buffer);
}

static
void paranoia_check(struct request *rq, void *buffer)
{
	int len = rq->length * 512;
	long long newpos = (long long)rq->sector * 512;
	int status2;
	void *buffer2 = NULL;
	struct verify_tag *tag = buffer;
	struct verify_tag *tag2;
	long long s_status;

	if (posix_memalign(&buffer2, 4096, len)) {
		printf("VERIFY ERROR: cannot allocate memory\n");
		flush_stdout();
		rq->verify_errors++;
		goto done;
	}
	tag2 = buffer2;
	s_status = lseek64(main_fd, newpos, SEEK_SET);
	if (s_status != newpos) {
		printf("VERIFY ERROR: bad lseek64() %lld on main_fd=%d at pos %lld (%s) pid=%d\n", s_status, main_fd, rq->sector, strerror(errno), getpid());
		flush_stdout();
		rq->verify_errors++;
		goto done;
	}
	status2 = do_read(buffer2, len);
	if (status2 != len) {
		printf("VERIFY ERROR: bad %cIO %d / %d on %d at pos %lld (%s)\n", rq->rwbs, status2, errno, main_fd, rq->sector, strerror(errno));
		flush_stdout();
		rq->verify_errors++;
		goto done;
	}
	if (dry_run)
		goto done;
	if (memcmp(buffer, buffer2, len)) {
		printf("VERIFY ERROR: memcmp(): bad storage semantics at sector = %lld len = %d tag_start = %lld tag2_start = %lld\n", rq->sector, len, tag->tag_start, tag2->tag_start);
		flush_stdout();
		rq->verify_errors++;
		goto done;
	}
done:
	free(buffer2);
}

///////////////////////////////////////////////////////////////////////

void grace_diff(struct timespec *diff, struct timespec *now)
{
	struct timespec grace;
	clock_gettime(CLOCK_REALTIME, now);
	timespec_diff(&grace, &start_stamp, now);
	timespec_diff(diff, &start_grace, &grace);
}

void verbose_status(struct request *rq, char *info)
{
	struct timespec now;
	struct timespec diff;

	grace_diff(&diff, &now);

	printf("INFO:"
	       " action='%s'"
	       " pid=%d"
	       " count_submitted=%d"
	       " count_catchup=%d"
	       " count_pushback=%d"
	       " real_time=%ld.%09ld",
	       info,
	       getpid(),
	       count_submitted,
	       count_catchup,
	       count_pushback,
	       diff.tv_sec, diff.tv_nsec);

	if (rq) {
		printf(" rq_time=%ld.%09ld"
		       " seqnr=%lld"
		       " q_nr=%d"
		       " block=%lld"
		       " len=%d"
		       " mode='%c'",
		       rq->orig_stamp.tv_sec, rq->orig_stamp.tv_nsec,
		       rq->seqnr,
		       rq->q_nr,
		       rq->sector,
		       rq->length,
		       rq->rwbs);
	}

	printf("\n");
	flush_stdout();
}

///////////////////////////////////////////////////////////////////////

static
void do_wait(struct request *rq, struct timespec *now)
{
#ifdef DEBUG_TIMING
	printf("do_wait\n");
#endif

	for (;;) {
		struct timespec rest_wait = {};

		grace_diff(&rq->replay_stamp, now);
		timespec_diff(&rest_wait, &rq->replay_stamp, &rq->orig_factor_stamp);

#ifdef DEBUG_TIMING
		printf("(%d) %lld.%09ld %lld.%09ld %lld.%09ld\n",
		       getpid(),
		       (long long)now->tv_sec,
		       now->tv_nsec,
		       (long long)rq->replay_stamp.tv_sec,
		       rq->replay_stamp.tv_nsec,
		       (long long)rest_wait.tv_sec,
		       rest_wait.tv_nsec);
		flush_stdout();
#endif

		if ((long long)rest_wait.tv_sec < 0) {
			break;
		}

		if (verbose > 3) {
			verbose_status(rq, "nanosleep");
		}

		nanosleep(&rest_wait, NULL);
	}
}

///////////////////////////////////////////////////////////////////////

static
int do_action(struct request *rq)
{
	struct timespec t0 = {};
	struct timespec t1 = {};
	int len = rq->length * 512;
	long long newpos;
	long long s_status;

	if (main_fd < 0) {
		return -1;
	}
	if (rq->sector + rq->length >= main_size) {
		printf("ERROR: trying to position at %lld (main_size=%lld)\n", rq->sector, main_size);
		flush_stdout();
	}

	newpos = (long long)rq->sector * 512;
	s_status = lseek64(main_fd, newpos, SEEK_SET);
	if (s_status != newpos) {
		printf("ERROR: bad lseek64() %lld on main_fd=%d at pos %lld (%s) pid=%d\n", s_status, main_fd, rq->sector, strerror(errno), getpid());
		flush_stdout();
		return -1;
	}
	{
		int status = -1;
		void *buffer = NULL;
		if (posix_memalign(&buffer, 4096, len)) {
			printf("ERROR: cannot allocate memory\n");
			flush_stdout();
			return -1;
		}
		if (toupper(rq->rwbs) == 'R') {
			do_wait(rq, &t0);
			
			if (!fake_io)
				status = do_read(buffer, len);

			clock_gettime(CLOCK_REALTIME, &t1);
			timespec_diff(&rq->replay_duration, &t0, &t1);

			check_tags(rq, buffer, len, 0);
		} else {
			make_tags(rq, buffer, len);

			do_wait(rq, &t0);

			if (!fake_io)
				status = do_write(buffer, len);

			clock_gettime(CLOCK_REALTIME, &t1);
			timespec_diff(&rq->replay_duration, &t0, &t1);

			if (verify_mode >= 3 && status == len) { // additional re-read and verify
				paranoia_check(rq, buffer);
			}
		}
		free(buffer);
		if (!fake_io && status != len) {
			printf("ERROR: bad %cIO %d / %d on %d at pos %lld (%s)\n", rq->rwbs, status, errno, main_fd, rq->sector, strerror(errno));
			flush_stdout();
			//do_exit(-1);
		}
	}
	return 0;
}


///////////////////////////////////////////////////////////////////////

// submitting requests over pipes

static
void pipe_write(int fd, void *data, int len)
{
	while (len > 0) {
		int status = write(fd, data, len);
		if (status > 0) {
			data += status;
			len -= status;
		} else {
			printf("FATAL ERROR: bad pipe write fd=%d status=%d (%d %s)\n", fd, status, errno, strerror(errno));
			flush_stdout();
			do_exit(-1);
		}
	}
}

static
int pipe_read(int fd, void *data, int len)
{
	int status = read(fd, data, len);
	if (status < 0) {
		printf("FATAL ERROR: bad pipe read fd=%d status=%d (%d %s)\n", fd, status, errno, strerror(errno));
		flush_stdout();
		do_exit(-1);
	}
	return status;
}

static
void submit_request(int fd, struct request *rq)
{
	pipe_write(fd, rq, RQ_SIZE);
	if (rq->has_version) {
		int size = rq->length * sizeof(unsigned int);
		pipe_write(fd, rq->old_version, size);
	}
}

static
int get_request(int fd, struct request *rq)
{
	int status = pipe_read(fd, rq, RQ_SIZE);
	if (status == RQ_SIZE && rq->has_version) {
		int size = rq->length * sizeof(unsigned int);
		rq->old_version = malloc(size);
		if (!rq->old_version) {
			printf("FATAL ERROR: out of memory\n");
			flush_stdout();
			do_exit(-1);
		}
		pipe_read(fd, rq->old_version, size);
	}
	return status;
}

///////////////////////////////////////////////////////////////////////

// the pipes

static int queue[QUEUES][2];
static int answer[2];

static
void make_all_queues(int q[][2], int max)
{
	int i;
	for(i = 0; i < max; i++) {
		int status = pipe(q[i]);
		if (status < 0) {
			printf("FATAL ERROR: cannot create pipe %d\n", i);
			do_exit(-1);
		}
	}
}

static
void close_all_queues(int q[][2], int max, int i2, int except)
{
	int i;
	for(i = 0; i < max; i++) {
		if (i == except)
			continue;
		close(q[i][i2]);
	}
}

///////////////////////////////////////////////////////////////////////

// positions: which queue to take?

static short *pos_table = NULL;

static
int pos_get(int offset, int max_filled)
{
	static int pos_last[2] = {};
	int start;
	int max;
	int best;
	int besti;
	int i;

	if (!pos_table) {
		pos_table = malloc(table_max * sizeof(short));
		if (!pos_table) {
			printf("FATAL ERROR: out of memory for pos_table\n");
			flush_stdout();
			do_exit(-1);
		}
		memset(pos_table, 0, table_max * sizeof(short));
	}

	start = (pos_last[offset / total_max] + 1) % total_max;

	for (max = total_max, i = start; --max >= 0; i = (i + 1) % total_max) {
		if (pos_table[i + offset] < max_filled)
			goto ok;
	}

	printf("WARN: cannot allocate pipe slot!\n"
	       "WARN: table_max=%d total_max=%d max_filled=%d\n"
	       "WARN: count_submitted=%d count_catchup=%d count_pushback=%d\n"
	       "WARN: offset=%d i=%d pos_table[i+offset]=%d\n"
	       "WARN: pos_last[0]=%d pos_last[1]=%d\n"
	       "WARN: this is no real harm, but your measurements might be DISTORTED\n"
	       "WARN: by some unnecessary artificial delays.\n"
	       "HINT: increase the --threads= parameter.\n",
	       table_max,
	       total_max,
	       max_filled,
	       count_submitted,
	       count_catchup,
	       count_pushback,
	       offset,
	       i,
	       pos_table[i + offset],
	       pos_last[0],
	       pos_last[1]);

	/* Try to make the best of the mess...
	 */
	best = INT_MAX;
	besti = -1;
	for (max = total_max, i = start; --max >= 0; i = (i + 1) % total_max) {
		if (pos_table[i + offset] < best) {
			best = pos_table[i + offset];
			besti = i;
		}
	}
	if (besti >= 0) {
		i = besti;
		printf("WARN: remapped to i=%d pos_table[i+offset]=%d\n",
		       i,
		       pos_table[i + offset]);
	}
	flush_stdout();

 ok:
	count_submitted++;
	if (count_submitted > max_submitted)
		max_submitted = count_submitted;

	if (offset > 0)
		count_catchup++;

	pos_last[offset / total_max] = i;
	pos_table[i + offset]++;
	return i + offset;
}

static
void pos_put(int pos)
{
	if (pos >= total_max)
		count_catchup--;
	count_submitted--;
	pos_table[pos]--;
	if (pos_table[pos] < 0) {
		printf("ERROR: imbalanced pos_table at %d (%d), count_submitted=%d count_catchup=%d\n",
		       pos, 
		       pos_table[pos],
		       count_submitted,
		       count_catchup);
		flush_stdout();
	}
}

///////////////////////////////////////////////////////////////////////

// submission to the right queue

static
void submit_to_queues(struct request *rq, int is_pushback)
{
	static long long seqnr = 0;
	static long long write_seqnr = 0;
	int index;

	if (is_pushback) {
		rq->q_nr = pos_get(total_max, 1);
	} else {
		rq->q_nr = pos_get(0, FILL_MAX);
	}

	index = rq->q_nr % sub_max;
	rq->q_index = rq->q_nr / sub_max;

	// generate write tag
	rq->tag.tag_start = start_stamp.tv_sec;
	rq->tag.tag_seqnr = ++seqnr;
	if (conflict_mode &&
	    (strong_mode || toupper(rq->rwbs) == 'W'))
		fly_add(&fly_hash, rq->sector, rq->length, toupper(rq->rwbs));

	if (toupper(rq->rwbs) != 'R') {
		write_seqnr++;
		force_blockversion(verify_fd, write_seqnr, rq->sector, rq->length);
	}
	rq->tag.tag_write_seqnr = write_seqnr;
	rq->has_version = !!rq->old_version;

	submit_request(queue[index][1], rq);

	if (verbose) {
		verbose_status(rq, "submit");
	}
}

///////////////////////////////////////////////////////////////////////

// pushback handling (used for partial ordering)

static struct request *pushback_head = NULL;
static struct request *pushback_tail = NULL;

static
void add_pushback(struct request *rq)
{
	if (pushback_tail) {
		pushback_tail->next = rq;
	} else {
		pushback_head = rq;
	}
	pushback_tail = rq;
	rq->next = NULL;

	rq->rwbs = tolower(rq->rwbs);

	statist_pushback++;
	count_pushback++;
	if (count_pushback > max_pushback)
		max_pushback = count_pushback;
	
	if (verbose) {
		verbose_status(rq, "pushback");
	}
}

static
void check_pushback(void)
{
	struct request **ptr = &pushback_head;
	struct request *tmp = *ptr;
	struct request *prev = NULL;
	
	while (tmp) {
		int has_conflict;

		if (count_catchup >= total_max) {
			printf("INFO: stopping pushback scan at"
			       " count_submitted=%d"
			       " count_catchup=%d"
			       " count_pushback=%d\n"
			       "HINT: this means that catch-up is limited by available threads\n",
			       count_submitted,
			       count_catchup,
			       count_pushback);
			flush_stdout();
			break;
		}

		has_conflict = fly_check(&fly_hash, tmp->sector, tmp->length, toupper(tmp->rwbs));
		if (has_conflict) {
			prev = tmp;
			ptr = &tmp->next;
			tmp = *ptr;
			continue;
		}

		// remove from list
		*ptr = tmp->next;
		tmp->next = NULL;
		count_pushback--;

		// inform...
		if (verbose) {
			verbose_status(tmp, "revival");
		}

		// delayed submit
		submit_to_queues(tmp, 1);
		add_request(tmp);

		// special conditions
		if (tmp == pushback_tail) {
			pushback_tail = prev;
			if (!pushback_tail)
				pushback_head = NULL;
		} else if (!pushback_head)
			pushback_tail = NULL;
		tmp = *ptr;
	}
}

///////////////////////////////////////////////////////////////////////

static
void dump_request(struct request *rq)
{
	struct timespec delay;
	static int did_head = 0;
	if (!did_head) {
		printf("orig_start ; sector; length ; op ;  replay_delay; replay_duration\n");
		did_head++;
	}

	timespec_diff(&delay, &rq->orig_factor_stamp, &rq->replay_stamp);

	printf("%4lu.%09lu ; %10lld ; %3d ; %c ; %3lu.%09lu ; %3lu.%09lu\n",
	       rq->orig_factor_stamp.tv_sec,
	       rq->orig_factor_stamp.tv_nsec,
	       rq->sector,
	       rq->length,
	       rq->rwbs,

	       delay.tv_sec + replay_out,
	       delay.tv_nsec,

	       rq->replay_duration.tv_sec,
	       rq->replay_duration.tv_nsec);

	flush_stdout();
	statist_completed++;
}

static
int get_answer(void)
{
	struct request rq = {};
	int status;
	int res = 0;

#ifdef DEBUG_TIMING
	printf("get_answer\n");
#endif
	if (!already_forked)
		goto done;

	if (verbose > 3) {
		verbose_status(NULL, "wait_for_answer");
	}

	status = get_request(answer[0], &rq);
	if (status == RQ_SIZE) {
		struct request *old;

		pos_put(rq.q_nr);
		verify_errors += rq.verify_errors;
		res = 1;

		if (verbose) {
			verbose_status(&rq, "got_answer");
		}

		old = find_request_seq(rq.sector, rq.seqnr);
		if (old) {
			memcpy(&old->replay_stamp, &rq.replay_stamp, sizeof(old->replay_stamp));
			memcpy(&old->replay_duration, &rq.replay_duration, sizeof(old->replay_duration));
			memcpy(&old->orig_factor_stamp, &rq.orig_factor_stamp, sizeof(old->orig_factor_stamp));
			dump_request(old);
			del_request(old->sector, old->seqnr);
		} else {
			printf("ERROR: request %lld vanished\n", rq.sector);
		}

		if (conflict_mode &&
		    (strong_mode || toupper(rq.rwbs) == 'W'))
			fly_delete(&fly_hash, rq.sector, rq.length, toupper(rq.rwbs));
		if (toupper(rq.rwbs) != 'R') {
			if (verify_mode) {
				unsigned int *data = get_blockversion(complete_fd, rq.sector, rq.length);
				advance_blockversion(data, rq.length, rq.tag.tag_write_seqnr);
      
				put_blockversion(complete_fd, data, rq.sector, rq.length);
			}
		}
		if (conflict_mode == 2) {
			/* Handle pushback requests.
			 */
			check_pushback();
		}
	}

done:
#ifdef DEBUG_TIMING
	printf("aw=%d\n", res);
	flush_stdout();
#endif
	return res;
}

///////////////////////////////////////////////////////////////////////

static
void main_open(int again)
{
	long long size;
	int flags = O_RDWR;
	if (again || dry_run) {
		flags = O_RDONLY;
	}
#ifdef O_LARGEFILE
	flags |= O_LARGEFILE;
#endif
	if (use_o_direct)
		flags |= O_DIRECT;
	if (use_o_sync)
		flags |= O_SYNC;

	main_fd = open(main_name, flags);
	if (main_fd < 0) {
		printf("ERROR: cannot open file '%s', errno = %d (%s)\n", main_name, errno, strerror(errno));
		do_exit(-1);
	}
	size = lseek64(main_fd, 0, SEEK_END);
	if (size <= 0) {
		printf("ERROR: cannot determine size of device (%d %s)\n", errno, strerror(errno));
		do_exit(-1);
	}
	if (!main_size) {
		main_size = size / 512;
		printf("INFO: device=%s has blocks=%lld (%lld kB).\n", main_name, main_size, main_size/2);
	}
}

static
char *mk_temp(const char *basename)
{
	char *tmpdir = getenv("TMPDIR");
	char *res = malloc(1024);
	int len;
	if (!res) {
		printf("FATAL ERROR: out of memory for tmp pathname\n");
		do_exit(-1);
	}
	if (!tmpdir)
		tmpdir = TMP_DIR;
	len = snprintf(res, 1024, "%s/blkreplay.%d/", tmpdir, getpid());
	(void)mkdir(res, 0700);
	snprintf(res + len, 1024 - len, "%s", basename);
	return res;
}

static
void verify_open(int again)
{
	int flags;

	if (!verify_mode)
		return;

	flags = O_RDWR | O_CREAT | O_TRUNC;
	if (again) {
		flags = O_RDONLY;
	}
#ifdef O_LARGEFILE
	flags |= O_LARGEFILE;
#endif
	if (verify_fd < 0) {
		char *file = getenv("VERIFY_TABLE");
		if (!file) {
			file = mk_temp(VERIFY_TABLE);
		}
		verify_fd = open(file, flags, S_IRUSR | S_IWUSR);
		if (verify_fd < 0) {
			printf("ERROR: cannot open '%s' (%d %s)\n", VERIFY_TABLE, errno, strerror(errno));
			flush_stdout();
			verify_mode = 0;
		}
	}
	if (complete_fd < 0) {
		char *file = getenv("COMPLETION_TABLE");
		if (!file) {
			file = mk_temp(COMPLETION_TABLE);
		}
		complete_fd = open(file, flags, S_IRUSR | S_IWUSR);
		if (complete_fd < 0) {
			printf("ERROR: cannot open '%s' (%d %s)\n", COMPLETION_TABLE, errno, strerror(errno));
			flush_stdout();
			verify_mode = 0;
		}
	}
}

///////////////////////////////////////////////////////////////////////

static
void do_worker(int in_fd, int back_fd)
{
	int count = 0;
	/* Each worker needs his own filehandle instance to avoid races
	 * between seek() and read()/write().
	 * So open it again.
	 */
	if (main_name && main_name[0]) {
		main_open(0);
	}
	for (;;) {
		struct request rq = {};
		int status = get_request(in_fd, &rq);
		if (!status)
			break;

		if (verbose > 1) {
			verbose_status(&rq, "worker_got_rq");
		}

		(void)do_action(&rq);
		count++;

		if (rq.old_version) {
			free(rq.old_version);
			rq.old_version = NULL;
		}
		rq.has_version = 0;

		if (verbose > 1) {
			verbose_status(&rq, "worker_send_answer");
		}

		submit_request(back_fd, &rq);
	}
	close(in_fd);
	close(back_fd);
	if (verbose > 2) {
		printf("worker %d count = %d\n", getpid(), count);
		flush_stdout();
	}
}

/* Intermediate copy thread.
 * When too many threads are writing into the same pipe,
 * lock contention may put a serious limit onto overall
 * throughput.
 * In order to limit the fan-in / fan-out, we create intermediate
 * "distributor threads" limiting the maximum competition.
 */
#define CP_SIZE    (RQ_SIZE * FILL_MAX)

static
void do_dispatcher_copy(int in_fd, int out_fd)
{
	int packet_len = CP_SIZE;
#if 0 // is this necessary in general? Linux pipe semantics seems to deliver short reads whenever possible, but do other kernels also?
	if (conflict_mode >= 2) {
		/* Ensure that replies are delivered as fast as possible.
		 */
		packet_len = RQ_SIZE;
	}
#endif

	if (verbose > 2) {
		printf("dispatcher %d fan_out = %d fill_max = %lu packet_len = %d\n", getpid(), fan_out, (unsigned long)FILL_MAX, packet_len);
		flush_stdout();
	}

	for (;;) {
		char buf[CP_SIZE];
		int status;

		status = pipe_read(in_fd, buf, packet_len);
		if (status <= 0)
			break;
		if ((status % RQ_SIZE) != 0) {
			printf("FATAL ERROR: bad record len %d from pipe fd=%d status=%d\n", status % (int)RQ_SIZE, in_fd, status);
			flush_stdout();
			do_exit(-1);
		}

		pipe_write(out_fd, buf, status);
	}
}

static
void _fork_answer_dispatcher(int close_fd)
{
	int old_answer_1 = answer[1];
	int status;
	pid_t pid;
				
	status = pipe(answer);
	if (status < 0) {
		printf("FATAL ERROR: cannot create sub-answer pipe\n");
		do_exit(-1);
	}
	
	flush_stdout();
	pid = fork();
	if (pid < 0) {
		printf("FATAL ERROR: cannot fork answer dispatcher\n");
		do_exit(-1);
	}
	if (!pid) { // son
		set_role("answer_dispatcher");
		close(close_fd);
		close(answer[1]);
		
		do_dispatcher_copy(answer[0], old_answer_1);
		
		do_exit(0);
	}
	close(old_answer_1);
}

/* Intermediate submit thread.
 * Also useful to reduce contention.
 * Necessary to distribute the requests to more than 1000 workers,
 * since the max number of filehandles / pipes is limited.
 */
static
void _fork_childs(int in_fd, int this_max)
{
	static int next_max[QUEUES] = {};
	int rest;
	int i;

	if (this_max <= 1 && in_fd >= 0) {
		set_role("worker");
		do_worker(in_fd, answer[1]);
		return;
	}

	sub_max = fan_out;
	if (this_max < sub_max)
		sub_max = this_max;

	rest = this_max - ((this_max / sub_max) * sub_max);
	for (i = 0; i < sub_max; i++) {
		next_max[i] = this_max / sub_max;
		if (rest-- > 0)
			next_max[i]++;
	}

	make_all_queues(queue, sub_max);
	flush_stdout();

	for (i = 0; i < sub_max; i++) {
		pid_t pid;

		pid = fork();
		if (pid < 0) {
			printf("FATAL ERROR: cannot fork child\n");
			do_exit(-1);
		}
		if (!pid) { // son
			if (next_max[i] > 1)
				set_role("submit_dispatcher");

			if (in_fd < 0)
				fclose(stdin);
			else
				close(answer[0]);
			close_all_queues(queue, sub_max, 0, i);
			close_all_queues(queue, sub_max, 1, -1);

			if (fork_dispatcher &&
			    next_max[i] > 1) {
				_fork_answer_dispatcher(queue[i][0]);
			}

			_fork_childs(queue[i][0], next_max[i]);

			do_exit(0);
		}
	}

	close_all_queues(queue, sub_max, 0, -1);
	close(answer[1]);

	if (in_fd  < 0)
		return;

	for (;;) {
		struct request rq = {};
		int index;
		int status = get_request(in_fd, &rq);
		if (!status)
			break;

		index = rq.q_index % sub_max;
		rq.q_index /= sub_max;
		submit_request(queue[index][1], &rq);

		if (rq.old_version) {
			free(rq.old_version);
			rq.old_version = NULL;
		}
	}
}

static
void fork_childs()
{
	int status;

	if (already_forked)
		return;
	already_forked++;

	set_role("main");

	// setup pipes and fork()

	status = pipe(answer);
	if (status < 0) {
		printf("FATAL ERROR: cannot create answer pipe\n");
		do_exit(-1);
	}

	if (verbose > 2) {
		printf("forking %d child processes in total for parallelism=%d\n", table_max, total_max);
		flush_stdout();
	}

	_fork_childs(-1, table_max);

	if (verbose > 2) {
		printf("done forking (fan_out=%d)\n\n", sub_max);
		flush_stdout();
	}
}

///////////////////////////////////////////////////////////////////////

static
void execute(struct request *rq)
{
	static int is_called = 0;
	int first_time;

	if (!is_called) { // only upon first call
		is_called++;
		fork_childs();
	}

	if (rq->old_version) {
		free(rq->old_version);
		rq->old_version = NULL;
	}

	if (verify_mode) {
		rq->old_version = get_blockversion(verify_fd, rq->sector, rq->length);
	}

	first_time = 0;
	for (;;) {
		int status = 0;

		if (verbose > 3) {
			verbose_status(rq, "check_for_conflicts");
		}

		if (conflict_mode) {
			status = fly_check(&fly_hash, rq->sector, rq->length, toupper(rq->rwbs));
		} else if (verify_mode) {
			unsigned int *now_version = get_blockversion(complete_fd, rq->sector, rq->length);
			status = compare_blockversion(rq->old_version, now_version, rq->length);
			free(now_version);
		}
		if (!status) { // no conflict
			break;
		}
		// well, we have a conflict.
		if (conflict_mode == 1) { // drop request
			printf("INFO: dropping block=%lld len=%d mode=%c\n", rq->sector, rq->length, rq->rwbs);
			flush_stdout();
			statist_dropped++;
			return;
		}
		if (conflict_mode == 2) { // pushback request
			add_pushback(rq);
			return;
		}
		// wait until conflict has gone...
		if (count_submitted > 0) {
			get_answer();
			if (!first_time++)
				statist_ordered++;
			continue;
		}
		printf("FATAL ERROR: block %lld waiting for up-to-date version\n", rq->sector);
		do_exit(-1);
	}

	// submit request to worker threads
	submit_to_queues(rq, 0);
	add_request(rq);
}

///////////////////////////////////////////////////////////////////////

// predicates for strategic decisions

/* Ensure that the submit queues cannot be filled too much,
 * measured in real time.
 * Too much filling could result in unnecessary drops, for example.
 * Too much filling will not result in any advantage.
 */
static
int delay_distance(struct timespec *check)
{
	struct timespec now;
	struct timespec elapsed;
	struct timespec diff;
	struct timespec delta;

	if (!check->tv_sec)
		return 0;

	grace_diff(&elapsed, &now);
	timespec_diff(&diff, &elapsed, check);
	timespec_diff(&delta, &diff, &ahead_limit);
	if ((long)delta.tv_sec < 0)
		return 1;

	return 0;
}

///////////////////////////////////////////////////////////////////////

/* Main dispatcher routine.
*/
static
void parse(FILE *inp)
{
	static int first_call = 0;
	char buffer[4096];
	struct timespec old_stamp = {};
	struct request *rq = NULL;

	if (main_name && main_name[0]) { // just determine main_size
		main_open(0);
		close(main_fd);
		main_fd = -1;
	}
	verify_open(0);

	// get start time, but only after opening everything, since open() may produce delays
	clock_gettime(CLOCK_REALTIME, &start_stamp);
	
	if (verbose) {
		printf("INFO: tag_start=%ld\n", start_stamp.tv_sec);
		flush_stdout();
	}

	for (;;) {
		int count;

		if (verbose > 3) {
			verbose_status(NULL, "wait_for_input");
		}

		if (!fgets(buffer, sizeof(buffer), inp))
			break;
		
		statist_lines++;

		if (!rq)
			rq = malloc(sizeof(struct request));
		if (!rq) {
			printf("FATAL ERROR: out of memory for requests\n");
			flush_stdout();
			do_exit(-1);
		}
		memset(rq, 0, sizeof(struct request));

		count = sscanf(buffer, "%ld.%ld ; %lld ; %d ; %c", &rq->orig_stamp.tv_sec, &rq->orig_stamp.tv_nsec, &rq->sector, &rq->length, &rq->rwbs);
		if (count != 5) {
			printf("ERROR: bad input count=%d, line='%s'\n", count, buffer);
			flush_stdout();
			continue;
		}

		// treat backshifts in time (caused by repeated input files)
		timespec_add(&rq->orig_stamp, &timeshift);
		if (rq->orig_stamp.tv_sec < old_stamp.tv_sec) {
			struct timespec delta = {};
			timespec_diff(&delta, &rq->orig_stamp, &old_stamp);
			timespec_add(&timeshift, &delta);
			timespec_add(&rq->orig_stamp, &delta);
			printf("INFO: backshift at time=%ld.%09ld s detected (delta=%ld.%09ld s, total timeshift=%ld.%09ld s)\n",
			       rq->orig_stamp.tv_sec, rq->orig_stamp.tv_nsec,
			       delta.tv_sec, delta.tv_nsec,
			       timeshift.tv_sec, timeshift.tv_nsec);
			flush_stdout();
		}
		memcpy(&old_stamp, &rq->orig_stamp, sizeof(old_stamp));

		if (verbose) {
			verbose_status(rq, "got_input");
		}

		// check replay time window
		if (rq->orig_stamp.tv_sec < replay_start) {
			continue;
		}
		if (replay_end && rq->orig_stamp.tv_sec >= replay_end) {
			break;
		}
		rq->rwbs = toupper(rq->rwbs);
		if (rq->rwbs != 'R' && rq->rwbs != 'W') {
			printf("ERROR: bad character '%c'\n", rq->rwbs);
			continue;
		}

		// compute virtual start time
		if (!first_call) {
			first_call++;
			memcpy(&first_stamp, &rq->orig_stamp, sizeof(first_stamp));
		}
		timespec_diff(&rq->orig_factor_stamp, &first_stamp, &rq->orig_stamp);
		timespec_multiply(&rq->orig_factor_stamp, time_stretch);

		// avoid flooding the pipelines too much
		while (count_submitted > bottleneck ||
		       ((conflict_mode == 1 || count_pushback > 0) &&
			count_submitted > 1 &&
			delay_distance(&rq->orig_factor_stamp))) {
			get_answer();
		}

		rq->seqnr = ++statist_total;
		if (rq->rwbs != 'R')
			statist_writes++;

		if (rq->sector + rq->length > max_size)
			max_size = rq->sector + rq->length;

		if (rq->sector + rq->length > main_size && main_size) {
			if (!overflow) {
				printf("INFO: sector %lld+%d exceeds %lld, turning on wraparound.\n", rq->sector, rq->length, main_size);
				flush_stdout();
				overflow++;
			}
			rq->sector %= main_size;
			if (rq->sector + rq->length > main_size) { // damn, it spans the border
				printf("WARN: cannot process sector %lld+%d at border %lld, please use a larger device\n", rq->sector, rq->length, main_size);
				flush_stdout();
				continue;
			}
		}

		// add new element
		execute(rq);
		rq = NULL;
	}

	printf("--------------------------------------\n");
	flush_stdout();

	while (count_submitted > 0 && get_answer()) {
	}

	printf("--------------------------------------\n");
	flush_stdout();

	// close all pipes => leads to EOF at childs
	close_all_queues(queue, sub_max, 1, -1);

	printf("=======================================\n\n");
	printf("meta_ops=%lld, total meta_delay=%lu.%09ld, avg=%lf\n",
	       meta_delay_count,
	       meta_delays.tv_sec,
	       meta_delays.tv_nsec,
	       meta_delay_count ? (meta_delays.tv_nsec * ((double)1.0/NANO) + (double)meta_delays.tv_sec) / meta_delay_count : 0);
	printf("# total input lines           : %6d\n", statist_lines);
	printf("# total     requests          : %6d\n", statist_total);
	printf("# completed requests          : %6d\n", statist_completed);
	printf("# write     requests          : %6d\n", statist_writes);
	printf("# dropped   requests          : %6d\n", statist_dropped);
	printf("# pushback  requests          : %6d\n", statist_pushback);
	printf("# ordered   requests (waits)  : %6d\n", statist_ordered);
	printf("# verify errors during replay : %6lld\n", verify_errors);
	printf("conflict_mode                 : %6d\n", conflict_mode);
	printf("strong_mode                   : %6d\n", strong_mode);
	printf("verify_mode                   : %6d\n", verify_mode);
	if (fly_hash.fly_count)
		printf("fly_count                     : %6d\n", fly_hash.fly_count);
	if (count_submitted)
		printf("count_submitted               : %6d\n", count_submitted);
	if (count_catchup)
		printf("count_catchup                 : %6d\n", count_catchup);
	if (count_pushback)
		printf("count_pushback                : %6d\n", count_pushback);

	printf("max_submitted                 : %6d\n", max_submitted);
	printf("max_pushback                  : %6d\n", max_pushback);

	printf("size of device:      %12lld blocks (%lld kB)\n", main_size, main_size/2);
	printf("max block# occurred: %12lld blocks (%lld kB)\n", max_size, max_size/2);
	printf("wraparound factor:   %6.3f\n", (double)max_size / (double)main_size);
	flush_stdout();
}

///////////////////////////////////////////////////////////////////////

// arg parsing
// don't use GNU getopt, it's not available everywhere

#define ARG_INT		-1
#define ARG_FLOAT	-2
#define ARG_TIMESPEC	-3

#define ARG_ADD		-10
#define ARG_SUB		-11

struct arg {
	char *arg_name;
	char *arg_descr;
	int   arg_const;
	void *arg_val;
};

static
const struct arg arg_table[] = {
	{
		.arg_name  = "|",
		.arg_descr = "Influence replay duration:",
	},
	{
		.arg_name  = "replay-start",
		.arg_descr = "start offset (in seconds, 0=from_start)",
		.arg_const = ARG_INT,
		.arg_val   = &replay_start,
	},
	{
		.arg_name  = "replay-end",
		.arg_descr = "end offset (in seconds, 0=unlimited)",
		.arg_const = ARG_INT,
		.arg_val   = &replay_end,
	},
	{
		.arg_name  = "replay-duration",
		.arg_descr = "alternatively specify the end offset as delta",
		.arg_const = ARG_INT,
		.arg_val   = &replay_duration,
	},
	{
		.arg_name  = "replay-out",
		.arg_descr = "start offset, used for output (in seconds)",
		.arg_const = ARG_INT,
		.arg_val   = &replay_out,
	},
	{
		.arg_name  = "start-grace",
		.arg_descr = "start after grace period for filling the pipes (in seconds)",
		.arg_const = ARG_INT,
		.arg_val   = &start_grace.tv_sec,
	},


	{
		.arg_name  = "|",
		.arg_descr = "Handling of conflicting IO requests:",
	},
	{
		.arg_name  = "with-conflicts",
		.arg_descr = "conflicting writes are ALLOWED (damaged IO)",
		.arg_const = 0,
		.arg_val   = &conflict_mode,
	},
	{
		.arg_name  = "with-drop",
		.arg_descr = "conflicting writes are simply dropped",
		.arg_const = 1,
		.arg_val   = &conflict_mode,
	},
	{
		.arg_name  = "with-partial",
		.arg_descr = "partial ordering by pushing back conflicts (default)",
		.arg_const = 2,
		.arg_val   = &conflict_mode,
	},
	{
		.arg_name  = "with-ordering",
		.arg_descr = "enforce total order in case of conflicts",
		.arg_const = 3,
		.arg_val   = &conflict_mode,
	},
	{
		.arg_name  = "strong",
		.arg_descr = "mode between 0 and 2, see docs (default=1)",
		.arg_const = ARG_INT,
		.arg_val   = &strong_mode,
	},



	{
		.arg_name  = "|",
		.arg_descr = "Replay parameters:",
	},
	{
		.arg_name  = "threads",
		.arg_descr = "parallelism (default=" STRINGIFY(DEFAULT_THREADS) ")",
		.arg_const = ARG_INT,
		.arg_val   = &total_max,
	},
	{
		.arg_name  = "fill-random",
		.arg_descr = "fill data blocks with random bytes (%, default=0)",
		.arg_const = ARG_INT,
		.arg_val   = &fill_random,
	},



	{
		.arg_name  = "|",
		.arg_descr = "Verification modes:",
	},
	{
		.arg_name  = "no-overhead",
		.arg_descr = "verify is OFF (default)",
		.arg_const = 0,
		.arg_val   = &verify_mode,
	},
	{
		.arg_name  = "with-verify",
		.arg_descr = "verify on reads",
		.arg_const = 1,
		.arg_val   = &verify_mode,
	},
	{
		.arg_name  = "with-final-verify",
		.arg_descr = "additional verify pass at the end",
		.arg_const = 2,
		.arg_val   = &verify_mode,
	},
	{
		.arg_name  = "with-paranoia",
		.arg_descr = "re-read after each write (destroys performance)",
		.arg_const = 3,
		.arg_val   = &verify_mode,
	},



	{
		.arg_name  = "|",
		.arg_descr = "Convenience:",
	},
	{
		.arg_name  = "verbose",
		.arg_descr = "increase verbosity, show additional INFO: output",
		.arg_const = ARG_ADD,
		.arg_val   = &verbose,
	},

	{
		.arg_name  = "|",
		.arg_descr = "Expert options (DANGEROUS):",
	},
	{
		.arg_name  = "o-direct",
		.arg_descr = "use O_DIRECT (default)",
		.arg_const = 1,
		.arg_val   = &use_o_direct,
	},
	{
		.arg_name  = "no-o-direct",
		.arg_descr = "don't use O_DIRECT, deliver FAKE results",
		.arg_const = 0,
		.arg_val   = &use_o_direct,
	},
	{
		.arg_name  = "o-sync",
		.arg_descr = "use O_SYNC",
		.arg_const = 1,
		.arg_val   = &use_o_sync,
	},
	{
		.arg_name  = "no-o-sync",
		.arg_descr = "don't use O_SYNC (default)",
		.arg_const = 0,
		.arg_val   = &use_o_sync,
	},
	{
		.arg_name  = "dry-run",
		.arg_descr = "don't actually do IO, measure internal overhead",
		.arg_const = 1,
		.arg_val   = &dry_run,
	},
	{
		.arg_name  = "fake-io",
		.arg_descr = "omit lseek() and tags, even less internal overhead",
		.arg_const = 1,
		.arg_val   = &fake_io,
	},
#ifdef HAVE_DECL_NANOSLEEP
	{
		.arg_name  = "simulate-io",
		.arg_descr = "delay value for IO simulation (timespec <sec>.<nsec>)",
		.arg_const = ARG_TIMESPEC,
		.arg_val   = &simulate_io,
	},
#endif
	{
		.arg_name  = "ahead-limit",
		.arg_descr = "limit pipe fillahead (realtime <sec>.<nsec>)",
		.arg_const = ARG_TIMESPEC,
		.arg_val   = &ahead_limit,
	},
	{
		.arg_name  = "fan-out",
		.arg_descr = "only for kernel hackers (default=" STRINGIFY(DEFAULT_FAN_OUT) ")",
		.arg_const = ARG_INT,
		.arg_val   = &fan_out,
	},
	{
		.arg_name  = "no-dispatcher",
		.arg_descr = "only for kernel hackers",
		.arg_const = 0,
		.arg_val   = &fork_dispatcher,
	},
	{
		.arg_name  = "bottleneck",
		.arg_descr = "max #requests on dispatch",
		.arg_const = ARG_INT,
		.arg_val   = &bottleneck,
	},
	{
		.arg_name  = "speedup",
		.arg_descr = "speedup / slowdown by REAL factor (default=" STRINGIFY(DEFAULT_SPEEDUP) ")",
		.arg_const = ARG_FLOAT,
		.arg_val   = &time_factor,
	},
	{
		.arg_name  = "mmap-mode",
		.arg_descr = "use mmap() instead of read() / write() [NYI]",
		.arg_const = 1,
		.arg_val = &mmap_mode,
	},
	{}
};

static
void usage(void)
{
	const struct arg *tmp;

	printf("usage: blkreplay {--<option>[=<value>]} <device>\n");

	for (tmp = arg_table; tmp->arg_name; tmp++) {
		int len;

		if (tmp->arg_name[0] == '|') {
			printf("\n%s\n", tmp->arg_descr);
			continue;
		}

		len = strlen(tmp->arg_name);
		printf("  --%s", tmp->arg_name);
		if (tmp->arg_const < 0) {
			printf("=<val>");
			len += 6;
		}
		while (len++ < 21) {
			printf(" ");
		}
		printf(" %s\n", tmp->arg_descr);
	}
	do_exit(-1);
}

static
void parse_args(int argc, char *argv[])
{
	int i;

	for (i = 1; i < argc; i++) {
		char *this = argv[i];
		const struct arg *tmp;
		int count = 1;

		if (this[0] != '-') {
			if (main_name) {
				printf("<device> must not be set twice\n");
				usage();
			}
			main_name = this;
			continue;
		}
		this++;
		if (this[0] != '-') {
			printf("only double options starting with -- are allowed\n");
			usage();
		}
		this++;
		for (tmp = arg_table; tmp->arg_name; tmp++) {
			int len = strlen(tmp->arg_name);
			if (!strncmp(tmp->arg_name, this, len)) {
				this += len;
				break;
			}
		}

		if (!tmp->arg_name) {
			printf("unknown option '%s'\n", this);
			usage();
		}

		if (tmp->arg_const >= 0) {
			*(int*)tmp->arg_val = tmp->arg_const;
			continue;
		}

		while (*this == ' ')
			this++;
		if (*this == '=')
			this++;

		switch (tmp->arg_const) {
		case ARG_INT:
			count = sscanf(this, "%d", (int*)tmp->arg_val);
			break;
		case ARG_FLOAT:
			*(FLOAT*)tmp->arg_val = atof(this);
			break;
		case ARG_TIMESPEC:
			count = sscanf(this, "%lu.%lu",
				       &((struct timespec*)tmp->arg_val)->tv_sec,
				       &((struct timespec*)tmp->arg_val)->tv_nsec);
			if (count == 2)
				count = 1;
			break;
		case ARG_ADD:
		case ARG_SUB:
			count = sscanf(this, "%d", (int*)tmp->arg_val);
			if (!count) {
				if (tmp->arg_const == ARG_ADD)
					(*(int*)tmp->arg_val)++;
				else
					(*(int*)tmp->arg_val)--;
			}
			count = 1;
			break;
		default: ;
		}
		if (count != 1) {
			printf("cannot parse <val> '%s'\n", this);
			usage();
		}
	}

	if (!main_name) {
		printf("you forgot to provide a <device>\n");
		usage();
	}
}

void print_fake(void)
{
	printf("INFO: use_o_direct=%d\n", use_o_direct);
	printf("INFO: use_o_sync=%d\n", use_o_sync);
	printf("INFO: fill_random=%d\n", fill_random);
	printf("INFO: ahead_limit=%lu.%09lu\n", ahead_limit.tv_sec, ahead_limit.tv_nsec);
	printf("INFO: simulate_io=%lu.%09lu\n", simulate_io.tv_sec, simulate_io.tv_nsec);
	printf("INFO: dry_run=%d\n", dry_run);

	if (dry_run || !use_o_direct) {
		printf("\n"
		       "INFO: measurement results are thus FAKE results!!!\n"
		       "\n"
		       );
	}
	flush_stdout();
}

int main(int argc, char *argv[])
{
	int max_threads;
	time_t now = time(NULL);
	/* avoid zombies */
	signal(SIGCHLD, SIG_IGN);

	/* argument parsing */
	parse_args(argc, argv);

	if (verify_mode >= 2)
		final_verify_mode++;

	max_threads = MAX_THREADS;
	if (conflict_mode == 2)
		max_threads /= 2;
	if (total_max > max_threads)
		total_max = max_threads;
	if (total_max < 1)
		total_max = 1;
	table_max = total_max;
	if (conflict_mode == 2)
		table_max *= 2;

	if (fan_out < 2)
		fan_out = 2;
	if (fan_out > QUEUES)
		fan_out = QUEUES;

	if (bottleneck <= 0)
		bottleneck = total_max * FILL_FACTOR;

	if (replay_duration > 0)
		replay_end = replay_start + replay_duration;

	if (fake_io)
		dry_run = 1;
	if (simulate_io.tv_sec || simulate_io.tv_nsec)
		dry_run = 1;

	if (ahead_limit.tv_sec <= 0 && ahead_limit.tv_nsec <= 0)
		ahead_limit.tv_sec = 1;

	if (time_factor != 0.0) {
		time_stretch = 1.0 / time_factor;

		/* Notice: the following GNU all-permissive license applies
		 * to the generated DATA file only, and does not change
		 * the GPL of this program.
		 */

		printf(
			"Copyright Thomas Schoebel-Theuer /  1&1 Internet AG\n"
			"\n"
			"This file was automatically generated by blkreplay\n"
			"\n"
			"Copying and distribution of this file, with or without modification,\n"
			"are permitted in any medium without royalty provided the copyright\n"
			"notice and this notice are preserved.  This file is offered as-is,\n"
			"without any warranty.\n"
			"\n"
			"\n"
			"blkreplay on %s started at %s\n"
			"\n",
			main_name,
			ctime(&now));
		flush_stdout();

		print_fake();

		parse(stdin);

		printf("blkreplay on %s ended at %s\n",
		       main_name,
		       ctime(&now));
		flush_stdout();
	}

	// verify the end result
	if (final_verify_mode) {
		printf("-------------------------------\n");
		printf("verifying the end result.......\n");
		flush_stdout();
		verify_open(1);
		main_open(1);
		sleep(3);
		check_all_tags();
	}

	print_fake();

	do_exit(0);
	return 0;
}
