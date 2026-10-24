// SPDX-License-Identifier: GPL-2.0-or-later
/* Fault injection around the real IMSM writer; all I/O uses temporary files. */
#include "../super-intel.c"

const char Name[] = "imsm-write-test";

enum fault_type { no_fault, write_error, short_write, seek_error, size_error };
static enum fault_type fault;
static int fault_fd = -1;
static off_t fault_offset;
static unsigned int test_sector_size;
static int write_calls;

ssize_t __real_write(int fd, const void *buf, size_t count);
off64_t __real_lseek64(int fd, off64_t offset, int whence);

ssize_t __wrap_write(int fd, const void *buf, size_t count)
{
	write_calls++;
	if (fd == fault_fd && __real_lseek64(fd, 0, SEEK_CUR) == fault_offset) {
		if (fault == write_error) {
			errno = EIO;
			return -1;
		}
		if (fault == short_write)
			return __real_write(fd, buf, count / 2);
	}
	return __real_write(fd, buf, count);
}

off64_t __wrap_lseek64(int fd, off64_t offset, int whence)
{
	if (fd == fault_fd && fault == seek_error && offset == fault_offset &&
	    whence == SEEK_SET) {
		errno = EIO;
		return -1;
	}
	return __real_lseek64(fd, offset, whence);
}

int __wrap_get_dev_size(int fd, char *name, unsigned long long *size)
{
	struct stat st;

	if (fd == fault_fd && fault == size_error) {
		errno = EIO;
		return 0;
	}
	if (fstat(fd, &st))
		return 0;
	*size = st.st_size;
	return 1;
}

int __wrap_get_dev_sector_size(int fd, char *name, unsigned int *size)
{
	*size = test_sector_size;
	return 1;
}

#define DEVICE_SIZE (1024 * 1024)

struct fixture {
	struct supertype st;
	struct intel_super super;
	struct intel_dev id;
	struct dl disks[4];
	FILE *files[4];
};

static void setup(struct fixture *f, unsigned int sector_size)
{
	struct imsm_map *map;
	int i;

	memset(f, 0, sizeof(*f));
	fault = no_fault;
	fault_fd = -1;
	test_sector_size = sector_size;
	f->super.anchor = calloc(1, 16384);
	f->super.migr_rec_buf = calloc(1, MIGR_REC_BUF_SECTORS * MAX_SECTOR_SIZE);
	f->super.bbm_log = calloc(1, sizeof(*f->super.bbm_log));
	f->id.dev = calloc(1, 1024);
	assert(f->super.anchor && f->super.migr_rec_buf && f->super.bbm_log && f->id.dev);
	/* Force an extended MPB on 512-byte devices. */
	f->super.bbm_log->signature = BBM_LOG_SIGNATURE;
	f->super.bbm_log->entry_count = 16;
	f->super.len = 16384;
	f->super.sector_size = sector_size;
	f->super.anchor->num_disks = 2;
	f->super.anchor->num_raid_devs = 1;
	f->super.anchor->family_num = __cpu_to_le32(1);
	f->super.devlist = &f->id;
	f->super.disks = f->disks;
	f->st.sb = &f->super;
	map = get_imsm_map(f->id.dev, MAP_0);
	map->num_members = 2;
	map->num_domains = 2;
	map->raid_level = IMSM_T_RAID1;
	map->map_state = IMSM_T_STATE_DEGRADED;
	map->failed_disk_num = 0;
	set_blocks_per_member(map, 1024);
	set_imsm_ord_tbl_ent(map, 0, IMSM_ORD_REBUILD);
	set_imsm_ord_tbl_ent(map, 1, 1);
	for (i = 0; i < 4; i++) {
		f->files[i] = tmpfile();
		assert(f->files[i]);
		f->disks[i].fd = fileno(f->files[i]);
		assert(ftruncate(f->disks[i].fd, DEVICE_SIZE) == 0);
		f->disks[i].index = i < 2 ? i : -1;
		f->disks[i].disk.status = i < 2 ? CONFIGURED_DISK : SPARE_DISK;
		f->disks[i].minor = i;
		set_total_blocks(&f->disks[i].disk, DEVICE_SIZE / 512);
		if (i < 3)
			f->disks[i].next = &f->disks[i + 1];
	}
	assert(write_super_imsm(&f->st, 0) == 0);
	map->map_state = IMSM_T_STATE_NORMAL;
	map->failed_disk_num = 0xff;
	set_imsm_ord_tbl_ent(map, 0, 0);
	f->super.updates_pending = 1;
}

static void teardown(struct fixture *f)
{
	int i;

	for (i = 0; i < 4; i++)
		fclose(f->files[i]);
	free(f->super.anchor);
	free(f->super.migr_rec_buf);
	free(f->super.bbm_log);
	free(f->id.dev);
}

static void check_written(struct fixture *f)
{
	unsigned char buf[8192];
	struct imsm_super *mpb = (void *)buf;
	unsigned int sector = test_sector_size;
	int i;

	for (i = 0; i < 2; i++) {
		struct imsm_map *map;
		unsigned int extra;

		assert(pread(f->disks[i].fd, buf, sector,
			     DEVICE_SIZE - 2 * sector) == sector);
		extra = (mpb_sectors(mpb, sector) - 1) * sector;
		if (extra)
			assert(pread(f->disks[i].fd, buf + sector, extra,
				     DEVICE_SIZE - 2 * sector - extra) == extra);
		assert(__gen_imsm_checksum(mpb) == __le32_to_cpu(mpb->check_sum));
		map = get_imsm_map(__get_imsm_dev(mpb, 0), MAP_0);
		assert(map->map_state == IMSM_T_STATE_NORMAL);
		assert(map->failed_disk_num == 0xff);
		assert(!(map->disk_ord_tbl[0] & __cpu_to_le32(IMSM_ORD_REBUILD)));
		assert(blocks_per_member(map) == 1024 / (sector / 512));
	}
}

static void retry_fault(unsigned int sector, enum fault_type type, int member,
			off_t offset)
{
	struct fixture f;
	int calls;

	setup(&f, sector);
	fault = type;
	fault_fd = f.disks[member].fd;
	fault_offset = offset;
	f.super.clean_migration_record_by_mdmon = 1;
	imsm_sync_metadata(&f.st);
	assert(f.super.updates_pending);
	assert(f.super.clean_migration_record_by_mdmon);
	assert(f.st.retry_soon);
	/* Retry must remain pending even when the error persists. */
	imsm_sync_metadata(&f.st);
	assert(f.super.updates_pending);
	fault = no_fault;
	f.st.retry_soon = 0; /* monitor clears this after its timed wait */
	imsm_sync_metadata(&f.st);
	assert(!f.super.updates_pending);
	assert(!f.super.clean_migration_record_by_mdmon);
	assert(!f.st.retry_soon);
	check_written(&f);
	calls = write_calls;
	imsm_sync_metadata(&f.st);
	assert(write_calls == calls); /* no pending update means no I/O */
	teardown(&f);
}

static void writer_reports_partial_failure(void)
{
	struct fixture f;
	unsigned char buf[512];

	setup(&f, 512);
	fault = write_error;
	fault_fd = f.disks[0].fd;
	fault_offset = DEVICE_SIZE - 1024;
	/* Successful spare writes must not hide a failed member write. */
	assert(write_super_imsm(&f.st, 0) != 0);
	/* The healthy member must still receive the new generation. */
	assert(pread(f.disks[1].fd, buf, sizeof(buf), fault_offset) == sizeof(buf));
	assert(((struct imsm_super *)buf)->generation_num == f.super.anchor->generation_num);
	teardown(&f);
}

static void failed_members_are_skipped(void)
{
	struct fixture f;

	setup(&f, 512);
	f.disks[0].disk.status |= FAILED_DISK;
	fault = size_error;
	fault_fd = f.disks[0].fd;
	imsm_sync_metadata(&f.st);
	assert(!f.super.updates_pending);
	assert(!f.st.retry_soon);
	teardown(&f);
}

static void close_all_after_error(void)
{
	struct fixture f;
	int i;

	setup(&f, 512);
	fault = write_error;
	fault_fd = f.disks[2].fd;
	fault_offset = DEVICE_SIZE - 1024;
	assert(write_super_imsm(&f.st, 1) != 0);
	for (i = 0; i < 4; i++)
		assert(f.disks[i].fd == -1);
	teardown(&f);
}

int main(void)
{
	unsigned int sector;
	int member;
	enum fault_type type;

	writer_reports_partial_failure();
	failed_members_are_skipped();
	close_all_after_error();
	for (sector = 512; sector <= 4096; sector *= 8) {
		for (type = write_error; type <= size_error; type++) {
			for (member = 0; member < 4; member++)
				retry_fault(sector, type, member, DEVICE_SIZE - 2 * sector);
			/* Migration-record failure independently of the MPB. */
			retry_fault(sector, type, 0, DEVICE_SIZE - sector);
			if (sector == 512)
				/* Extended MPB write/seek, preceding the anchor. */
				retry_fault(sector, type, 0, DEVICE_SIZE - 3 * sector);
		}
	}
	puts("IMSM metadata write-error tests passed");
	return 0;
}
