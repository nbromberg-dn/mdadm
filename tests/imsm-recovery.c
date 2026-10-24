// SPDX-License-Identifier: GPL-2.0-or-later
/* Exercise the actual IMSM callbacks without requiring MD devices. */
#include "../super-intel.c"

const char Name[] = "imsm-recovery-test";

struct fixture {
	struct supertype st;
	struct intel_super super;
	struct intel_dev id;
	struct active_array array;
	struct dl disks[2];
	struct mdinfo members[2];
};

static void setup(struct fixture *f)
{
	struct imsm_dev *dev;
	struct imsm_map *map, *prev;
	int i;

	memset(f, 0, sizeof(*f));
	f->super.anchor = calloc(1, 8192);
	f->id.dev = calloc(1, 1024);
	assert(f->super.anchor && f->id.dev);
	f->super.anchor->num_disks = 2;
	f->super.anchor->num_raid_devs = 1;
	f->super.devlist = &f->id;
	f->super.disks = f->disks;
	f->st.sb = &f->super;
	f->st.arrays = &f->array;
	f->array.container = &f->st;
	f->array.info.devs = f->members;
	f->array.info.array.raid_disks = 2;
	dev = f->id.dev;
	dev->vol.migr_state = MIGR_STATE_MIGRATING;
	dev->vol.migr_type = MIGR_REBUILD;
	map = get_imsm_map(dev, MAP_0);
	map->raid_level = IMSM_T_RAID1;
	map->num_members = 2;
	map->num_domains = 2;
	map->map_state = IMSM_T_STATE_NORMAL;
	map->failed_disk_num = 0;
	prev = get_imsm_map(dev, MAP_1);
	memcpy(prev, map, sizeof(*map) + sizeof(__u32));
	prev->map_state = IMSM_T_STATE_DEGRADED;
	for (i = 0; i < 2; i++) {
		f->disks[i].index = i;
		f->disks[i].disk.status = CONFIGURED_DISK;
		f->members[i].disk.raid_disk = i;
		f->members[i].curr_state = DS_INSYNC;
		f->members[i].recovery_start = MaxSector;
		set_imsm_ord_tbl_ent(map, i, i);
		set_imsm_ord_tbl_ent(prev, i, i | (i ? 0 : IMSM_ORD_REBUILD));
	}
	f->disks[0].next = &f->disks[1];
	f->members[0].next = &f->members[1];
}

static void teardown(struct fixture *f)
{
	free(f->id.dev);
	free(f->super.anchor);
}

static void completed_rebuild(int first)
{
	struct fixture f;
	struct imsm_map *map;

	setup(&f);
	imsm_set_disk(&f.array, first, DS_INSYNC);
	imsm_set_disk(&f.array, 1 - first, DS_INSYNC);
	map = get_imsm_map(f.id.dev, MAP_0);
	assert(!f.id.dev->vol.migr_state);
	assert(map->map_state == IMSM_T_STATE_NORMAL);
	assert(map->failed_disk_num == 0xff);
	assert(!(get_imsm_ord_tbl_ent(f.id.dev, 0, MAP_0) & IMSM_ORD_REBUILD));
	assert(f.super.updates_pending);
	teardown(&f);
}

static void interrupted_rebuild(void)
{
	struct fixture f;

	setup(&f);
	f.members[0].recovery_start = 128;
	f.members[0].curr_state = 0;
	imsm_set_disk(&f.array, 0, 0);
	imsm_set_disk(&f.array, 1, DS_INSYNC);
	assert(is_rebuilding(f.id.dev));
	assert(get_imsm_ord_tbl_ent(f.id.dev, 0, MAP_1) & IMSM_ORD_REBUILD);
	teardown(&f);
}

static void failed_member_is_not_forgiven(void)
{
	struct fixture f;

	setup(&f);
	f.disks[0].disk.status |= FAILED_DISK;
	imsm_set_disk(&f.array, 0, DS_INSYNC);
	assert(is_failed(&f.disks[0].disk));
	assert(get_imsm_map(f.id.dev, MAP_0)->map_state == IMSM_T_STATE_DEGRADED);
	teardown(&f);
}

int main(void)
{
	completed_rebuild(0);
	completed_rebuild(1);
	interrupted_rebuild();
	failed_member_is_not_forgiven();
	puts("IMSM recovery callback tests passed");
	return 0;
}
