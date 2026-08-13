/*
 * diskviz.c — minimal libfdisk-based partition table visualiser + creator
 * Version 36
 *
 * A blank length at the new-partition prompt now means "fill to the end
 * of this segment", matching how a blank start already means "start of
 * the segment" — previously it was parsed as 0 and rejected as "doesn't
 * fit". format_size()'s MiB/GiB output now says "MiB"/"GiB" rather than
 * a bare "M"/"G", matching the column headers and the labels
 * format_bytes_short() already uses elsewhere.
 *
 * parse_sectors()'s locale fix reworked to be portable: was using
 * newlocale()/strtod_l() (glibc-only, needed _GNU_SOURCE), now saves
 * and restores LC_NUMERIC around a plain strtod() call instead — works
 * on musl and other libcs too, and there's no locale_t left to leak.
 *
 * SIGINT/SIGHUP/SIGTERM now run the same device-deassign cleanup every
 * other quit path already goes through — previously Ctrl-C (or a closed
 * terminal, or `kill`) at any prompt killed the process immediately and
 * skipped it. draw_bar() now guards against last_lba < first_lba on an
 * empty/degenerate table instead of wrapping into a huge uint64_t.
 * parse_sectors() parses against a fixed "C" locale rather than
 * whatever LC_NUMERIC happens to be active, so "1.5G" can't silently
 * become "1" under a comma-decimal locale. The two generic
 * "Failed to add partition"/"Write failed" messages now include
 * libfdisk's own error code via strerror().
 *
 * Every "q" quit path — main menu, every sub-prompt, every y/N warning
 * — now prints the same message and does the same cleanup, rather than
 * the main menu's [q] and quit_if_requested() (used everywhere else)
 * disagreeing on both. --help documents [d]/[r]'s device-path
 * confirmation. Full sweep of every user-facing message: split the
 * few that were still a single long line (the incremental-reread
 * fallback warning, the "partitions created" intro, the backup
 * restore-command line, the startup Tab tip, the misalignment legend).
 *
 * Full pass over --help: [u]'s line now uses the same parenthetical
 * style as the rest of the command list, and the NO_COLOR-under-sudo
 * sentence is split into two plainer clauses instead of one relative
 * clause doing double duty.
 *
 * Reworded the --help colour paragraph: it previously implied the main
 * table itself shows red for a staged delete, which it can't — a
 * staged delete vanishes from the table into free space (by design),
 * so red only ever appears in the separate staged-operations list.
 *
 * --help now includes the correct way to set NO_COLOR under sudo
 * (`sudo env NO_COLOR=1 ./diskviz /dev/DEVICE`), since sudo's usual
 * environment reset silently drops a plain "NO_COLOR=1 sudo ...".
 *
 * British English throughout comments and user-facing messages
 * ("visualiser" was the one word that had slipped through as American).
 *
 * --help brought up to date with the feature set: [w] returning to the
 * menu instead of exiting, GPT auto-init on a blank disk, colour and
 * NO_COLOR, and the "nothing staged" requirement for [b]/[r].
 *
 * The longer prompts and status/warning messages (partition creation,
 * delete confirmation, in-use warnings, alignment warnings, backup and
 * restore) are split across two or three short lines rather than one
 * long one, so they read cleanly and don't wrap awkwardly on a normal-
 * width terminal.
 *
 * Minimal ANSI colour, only ever emitted on a real terminal with
 * NO_COLOR unset: a partition that only exists because of a staged,
 * not-yet-written [n] shows green right in the table (rather than
 * looking identical to one genuinely on disk), a misaligned start
 * shows yellow, free space is dimmed, and the staged-operations list
 * below colours CREATE/DELETE/RESTORE to match.
 *
 * Shows a proportional bar of the whole disk (partitions + every gap of
 * free space, however many there are), then lets you pick a free-space
 * segment and create a new partition at a chosen start offset within it.
 * Uses GNU readline for input, so arrow keys/backspace/history work
 * properly instead of raw escape sequences landing in the input.
 *
 * [d]'s in-use check now also follows a crypttab match to its mapped
 * /dev/mapper/<name> device (-> /dev/dm-N) before deciding a partition
 * looks safe to delete — plain dm-crypt swap (no on-disk header, no
 * blkid signature) is mounted/active there, not on the raw partition
 * device, so checking only the partition itself used to miss it.
 *
 * The filesystem description shown in the table and the one shown at
 * [d]'s confirmation prompt now come from one shared function, so they
 * can't disagree — they previously could for a partition with no
 * confirmed on-disk fstype but a crypttab match (e.g. plain dm-crypt
 * swap), which is exactly the case where showing the wrong thing right
 * before a delete confirmation would matter most.
 *
 * [d] checks /proc/mounts and /proc/swaps for the partition you've
 * picked and warns if it's currently mounted or active as swap, right
 * where that matters — at the point you're about to confirm deleting
 * it — rather than as a blanket status dump for every partition up
 * front. It's a courtesy warning, not an enforced block.
 *
 * Run with -h/--help for a full command summary without opening a disk.
 *
 * Alignment is advisory, not enforced: creating a partition whose start
 * or end isn't a multiple of the alignment target (1 MiB, or wider if
 * the kernel reports a larger optimal_io_size for this disk) prints a
 * warning and asks to proceed — never a hard block, since a misaligned
 * partition still works fine, it's a performance/wear concern on SSDs,
 * not a correctness one. Existing partitions on the table are checked
 * the same way and flagged with a "!" next to their index.
 *
 * The [b]ackup and [r]estore file-path prompts are the one place Tab
 * completes real filesystem paths rather than a fixed word list —
 * everywhere else, Tab still only offers the sensible values relevant
 * to that specific prompt.
 *
 * [b] backs up the partition table currently on disk to an sfdisk-style
 * dump file (restorable with `sfdisk /dev/DEVICE < backupfile`), using
 * libfdisk's own script API — no external tool required. [r] restores
 * one back, via the same API (fdisk_apply_script) sfdisk itself uses —
 * staged like everything else, so it only replaces the real table on
 * [w]rite, and [u] can undo it back to the genuine on-disk state.
 * Both only work while nothing else is staged, so the table being
 * backed up from or restored onto is always what's really on disk.
 *
 * On a disk with no partition table at all yet, diskviz offers to stage
 * a fresh GPT label before doing anything else — like every other
 * change here, that's only committed on [w]rite.
 *
 * All staged, not-yet-written creates and deletes live in one ordered
 * queue and are printed before every prompt, so it's always visible
 * exactly what the next [w] is about to do. [u] undoes the most recent
 * entry regardless of kind: a staged create is dropped again; a staged
 * delete is restored at its original start/size with its original GPT
 * type. [d] stages deletion of any existing partition (not just ones
 * created this session) — it requires typing that partition's own
 * device path back as confirmation, since unlike everything else here
 * it can destroy real data once [w]ritten.
 *
 * The partition table lists each partition's actual device node (e.g.
 * /dev/nvme0n1p11) in a Device column, resolved via fdisk_partname() —
 * the same path you'd hand to mkfs/mount.
 *
 * [w] writes whatever's currently staged and returns to the menu — it
 * does not end the session, so you can stage and write several
 * partitions in one run. [q] is the only way to exit, and is a safety
 * net: it discards anything staged but not yet written.
 *
 * diskviz only ever writes the partition table — it never formats
 * anything. After each [w]rite, it uses fdisk_reread_changes() to add
 * device nodes for the newly-written partition(s) incrementally (via
 * BLKPG), which works even while other partitions on the same disk are
 * mounted, and prints each new partition's device path plus a suggested
 * mkfs/mkswap command for you to run by hand.
 *
 * This is a skeleton to build from, not a finished tool: error handling
 * is minimal, and it commits partition table changes to the real disk on
 * request. Test against a loop device or spare disk before trusting it on
 * real data.
 *
 * Build:
 *   gcc -O2 -Wall $(pkg-config --cflags --libs fdisk blkid) -lreadline -o diskviz diskviz.c
 *
 * Packages needed (readline is already present on Arch — it's a bash
 * dependency; fdisk/blkid come from util-linux-libs, also already present):
 *   util-linux-libs, readline
 *
 * Run:
 *   sudo ./diskviz /dev/nvme0n1
 */

#define DISKVIZ_VERSION "36"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <locale.h>
#include <readline/readline.h>
#include <readline/history.h>
#include <libfdisk/libfdisk.h>
#include <blkid/blkid.h>

#define BAR_WIDTH 100   /* characters wide the proportional bar will be */
#define MAX_SEGMENTS 256
#define FSTYPE_LEN 24
#define GPTTYPE_LEN 40

typedef enum { SEG_PARTITION, SEG_FREE } seg_type_t;
typedef enum { UNIT_SECTORS, UNIT_MIB, UNIT_GIB } unit_t;

typedef struct {
	seg_type_t type;
	uint64_t start;   /* sector */
	uint64_t end;     /* sector, inclusive */
	int partno;       /* only meaningful for SEG_PARTITION */
	char devpath[PATH_MAX];   /* e.g. "/dev/nvme0n1p11"; only meaningful for SEG_PARTITION */
	char fstype[FSTYPE_LEN];   /* filesystem actually detected on-disk by blkid, e.g. "ext4"; empty if unknown/free */
	char gpttype[GPTTYPE_LEN]; /* GPT/MBR partition TYPE label from fdisk, e.g. "Linux swap"; shown when blkid finds nothing */
	char crypto_detail[96];   /* if fstype is crypto_LUKS: what's actually inside, resolved via /etc/crypttab */
	char crypt_mapper_name[128]; /* crypttab <name> field if this partition matched an entry, e.g. "swap"; empty otherwise. The kernel sees the active mount/swap on /dev/mapper/<name> (-> /dev/dm-N), not this partition's own device path, so anything checking "is this in use" needs this too. */
} segment_t;

/* ---- staged queue: every create/delete this session that hasn't been
 * written yet, in the order it was staged. libfdisk itself only keeps
 * one in-memory table (fdisk_write_disklabel is the one call that
 * commits anything), so this queue is diskviz's own bookkeeping on top
 * of that — it's what lets [u] undo either kind of operation, and what
 * gets printed so a person can see exactly what [w] is about to do
 * before they confirm it. Declared here (rather than right above
 * list_staged_queue()) so list_segments() can also check it, to colour
 * a still-staged CREATE differently from a genuinely on-disk partition. */
typedef enum { OP_CREATE, OP_DELETE, OP_RESTORE } staged_op_type_t;

typedef struct {
	staged_op_type_t type;
	size_t partno;                  /* fdisk 0-indexed partno affected; unused for OP_RESTORE */
	char devpath[PATH_MAX];          /* CREATE/DELETE: device path. RESTORE: backup file path (reused field) */
	char fstype[GPTTYPE_LEN];        /* CREATE: filesystem chosen. DELETE: what was there before. RESTORE: label type before restore (reused field) */
	uint64_t start, size;            /* sectors — also needed to restore an undone DELETE; unused for OP_RESTORE */
	struct fdisk_parttype *saved_type; /* DELETE only: original GPT type, ref'd so undo can restore it exactly; NULL otherwise */
} staged_op_t;

#define MAX_STAGED 64
static staged_op_t staged_ops[MAX_STAGED];
static int n_staged = 0;

static segment_t segments[MAX_SEGMENTS];
static int nsegments = 0;
static unsigned long sector_size = 512; /* logical sector size, read from the disk at startup */
static unit_t display_unit = UNIT_SECTORS;
static uint64_t alignment_sectors = 2048; /* advisory alignment target in sectors, recomputed once real sector_size is known — see get_alignment_sectors() */
static int use_color = 0; /* set once at startup: real terminal on stdout, and NO_COLOR not set — see main() */

/* Set once fdisk_assign_device() succeeds and cleared again at every
 * point the device is deassigned, purely so handle_termination_signal()
 * below always knows whether there's a live device to release — see
 * that function for why it needs this at all. */
static struct fdisk_context *g_cxt = NULL;

/* ---- minimal ANSI colour support. ----
 * Used to make the table self-explanatory without cross-referencing the
 * staged-operations list below it: a partition that only exists because
 * of a staged, not-yet-written [n] shows in green right in the table,
 * rather than looking identical to a partition that's genuinely on
 * disk. Never emitted unless stdout is a real terminal and NO_COLOR
 * isn't set — never assume colour is safe. */
#define COLOR_RESET  "\x1b[0m"
#define COLOR_GREEN  "\x1b[32m"
#define COLOR_YELLOW "\x1b[33m"
#define COLOR_DIM    "\x1b[2m"
#define COLOR_RED    "\x1b[31m"
#define COLOR_CYAN   "\x1b[36m"

static const char *c_reset(void)  { return use_color ? COLOR_RESET  : ""; }
static const char *c_green(void)  { return use_color ? COLOR_GREEN  : ""; }
static const char *c_yellow(void) { return use_color ? COLOR_YELLOW : ""; }
static const char *c_dim(void)    { return use_color ? COLOR_DIM    : ""; }
static const char *c_red(void)    { return use_color ? COLOR_RED    : ""; }
static const char *c_cyan(void)   { return use_color ? COLOR_CYAN   : ""; }


/* Render a sector count as text in whatever unit is currently selected.
 * MiB/GiB use binary (1024-based) units, matching what parted/gparted show. */
static void format_size(uint64_t sectors, char *buf, size_t buflen) {
	double bytes = (double)sectors * (double)sector_size;
	switch (display_unit) {
	case UNIT_MIB:
		snprintf(buf, buflen, "%.2f MiB", bytes / (1024.0 * 1024.0));
		break;
	case UNIT_GIB:
		snprintf(buf, buflen, "%.2f GiB", bytes / (1024.0 * 1024.0 * 1024.0));
		break;
	case UNIT_SECTORS:
	default:
		snprintf(buf, buflen, "%llu", (unsigned long long)sectors);
		break;
	}
}

/* Parse user input that may be a raw sector count ("204800") or a size with
 * a unit suffix ("500M", "20G", "1.5G", case-insensitive). Returns sectors.
 *
 * Temporarily forces LC_NUMERIC to "C" around the strtod() call rather
 * than using whatever locale is active. Under a locale that uses a
 * comma as the decimal separator, plain strtod("1.5G") stops at the
 * '.' and silently returns 1 instead of 1.5 — pinning the locale here
 * means "1.5G" parses the same regardless of the environment it's run
 * in. setlocale()/strtod() (rather than the GNU-only strtod_l()) keeps
 * this portable to non-glibc libcs too; safe here since diskviz is
 * single-threaded, so there's no concurrent caller to race with the
 * global locale change. */
static uint64_t parse_sectors(const char *str) {
	char *saved_locale = setlocale(LC_NUMERIC, NULL);
	char saved_locale_buf[64];
	if (saved_locale) snprintf(saved_locale_buf, sizeof(saved_locale_buf), "%s", saved_locale);
	else saved_locale_buf[0] = '\0';

	setlocale(LC_NUMERIC, "C");

	char *end = NULL;
	double val = strtod(str, &end);

	if (saved_locale_buf[0]) setlocale(LC_NUMERIC, saved_locale_buf);

	if (end == str) return 0; /* nothing numeric found */

	while (*end == ' ') end++;

	if (*end == 'M' || *end == 'm')
		return (uint64_t)((val * 1024.0 * 1024.0) / (double)sector_size);
	if (*end == 'G' || *end == 'g')
		return (uint64_t)((val * 1024.0 * 1024.0 * 1024.0) / (double)sector_size);
	if (*end == 'K' || *end == 'k')
		return (uint64_t)((val * 1024.0) / (double)sector_size);

	/* no recognised suffix: treat the number as a raw sector count */
	return (uint64_t)val;
}

/* Render a byte count as a short human string for warning messages —
 * always KiB or MiB, independent of the display_unit the main table is
 * currently using, since alignment offsets are always small. */
static void format_bytes_short(uint64_t bytes, char *buf, size_t buflen) {
	if (bytes >= 1024ULL * 1024ULL)
		snprintf(buf, buflen, "%.1f MiB", (double)bytes / (1024.0 * 1024.0));
	else
		snprintf(buf, buflen, "%.1f KiB", (double)bytes / 1024.0);
}

/* ---- best-effort SSD/RAID-friendly alignment target for this disk, in
 * sectors. Defaults to the modern standard of 1 MiB (what parted and
 * gparted also default to), and widens to the kernel's reported optimal
 * I/O size when that's larger and actually exposed — some SSDs and RAID
 * arrays report a natural stripe/erase-block size bigger than 1 MiB.
 * This is advisory only — diskviz warns on misalignment but never
 * blocks on it, so a missing/unreadable sysfs file just falls back to
 * the 1 MiB baseline rather than being treated as an error. ---- */
static uint64_t get_alignment_sectors(const char *disk_path) {
	uint64_t alignment_bytes = 1024ULL * 1024ULL; /* 1 MiB baseline */
	const char *base = strrchr(disk_path, '/');
	base = base ? base + 1 : disk_path;

	char path[PATH_MAX];
	snprintf(path, sizeof(path), "/sys/block/%s/queue/optimal_io_size", base);

	FILE *f = fopen(path, "r");
	if (f) {
		unsigned long long v = 0;
		if (fscanf(f, "%llu", &v) == 1 && v > alignment_bytes)
			alignment_bytes = v;
		fclose(f);
	}

	uint64_t sectors = alignment_bytes / (sector_size ? sector_size : 512);
	return sectors ? sectors : 1;
}

/* ---- detect the actual filesystem on a partition, via libblkid ----
 * This is different from the GPT "partition type" (e.g. "Linux filesystem")
 * that fdisk shows — blkid probes the partition's own superblock, so it
 * reports what's actually formatted there (ext4, xfs, ntfs, vfat, swap...),
 * the same way `blkid` and gparted's File System column do. */
static void detect_fstype(const char *disk_path, int partno, char *out, size_t outlen,
                           char *uuid_out, size_t uuid_outlen) {
	char devpath[PATH_MAX];
	blkid_probe pr;
	const char *value = NULL;
	size_t len = 0;

	out[0] = '\0';
	if (uuid_out) uuid_out[0] = '\0';

	{
		char *built = fdisk_partname(disk_path, partno);
		if (!built) return;
		snprintf(devpath, sizeof(devpath), "%s", built);
		free(built);
	}

	pr = blkid_new_probe_from_filename(devpath);
	if (!pr) return;

	blkid_probe_enable_superblocks(pr, 1);
	blkid_probe_set_superblocks_flags(pr, BLKID_SUBLKS_TYPE | BLKID_SUBLKS_UUID);

	if (blkid_do_safeprobe(pr) == 0) {
		if (blkid_probe_lookup_value(pr, "TYPE", &value, &len) == 0 && value)
			snprintf(out, outlen, "%s", value);
		if (uuid_out && blkid_probe_lookup_value(pr, "UUID", &value, &len) == 0 && value)
			snprintf(uuid_out, uuid_outlen, "%s", value);
	} else {
		/* safeprobe returns non-zero both when nothing was found and when
		 * the result was ambiguous (multiple signatures on the device).
		 * A fresh probe + fullprobe takes the first match instead of
		 * refusing, which recovers cases safeprobe gives up on. */
		blkid_reset_probe(pr);
		blkid_probe_enable_superblocks(pr, 1);
		blkid_probe_set_superblocks_flags(pr, BLKID_SUBLKS_TYPE | BLKID_SUBLKS_UUID);
		if (blkid_do_fullprobe(pr) == 0) {
			if (blkid_probe_lookup_value(pr, "TYPE", &value, &len) == 0 && value)
				snprintf(out, outlen, "%s", value);
			if (uuid_out && blkid_probe_lookup_value(pr, "UUID", &value, &len) == 0 && value)
				snprintf(uuid_out, uuid_outlen, "%s", value);
		}
	}
	blkid_free_probe(pr);
}

/* ---- if a partition is a LUKS container, try to resolve what it actually
 * holds by matching it against /etc/crypttab and, if currently unlocked,
 * probing the mapped /dev/mapper/<name> device for its real filesystem.
 * This only works when run on the live target system (crypttab must exist
 * and, for the mapper probe, the container must currently be open) — not
 * from an install ISO, which is fine since that's how this gets used here.
 *
 * Matching order: filesystem UUID (blkid, only present for LUKS or an
 * already-formatted plain device) -> GPT PARTUUID (always present,
 * independent of encryption, since it's part of the partition table
 * itself, not the content) -> a plain device-path spec in crypttab.
 * The PARTUUID path matters specifically for "plain" dm-crypt mode
 * (common for swap, keyed from /dev/urandom each boot): it writes no
 * on-disk header at all, so blkid can never see it — PARTUUID is the
 * only stable identifier available for that case. */
static void resolve_crypttab_mapping(const char *devpath, const char *fs_uuid,
                                      const char *partuuid, char *out, size_t outlen,
                                      char *mapper_name_out, size_t mapper_name_outlen) {
	FILE *f;
	char line[512];
	char real_devpath[PATH_MAX];

	out[0] = '\0';
	if (mapper_name_out) mapper_name_out[0] = '\0';

	f = fopen("/etc/crypttab", "r");
	if (!f) return;

	if (!realpath(devpath, real_devpath))
		snprintf(real_devpath, sizeof(real_devpath), "%s", devpath);

	while (fgets(line, sizeof(line), f)) {
		char *p = line;
		while (isspace((unsigned char)*p)) p++;
		if (*p == '#' || *p == '\0' || *p == '\n') continue;

		char name[128] = "", spec[256] = "";
		if (sscanf(p, "%127s %255s", name, spec) != 2) continue;

		int matched = 0;
		if (strncasecmp(spec, "UUID=", 5) == 0) {
			if (fs_uuid[0] && strcasecmp(spec + 5, fs_uuid) == 0) matched = 1;
		} else if (strncasecmp(spec, "PARTUUID=", 9) == 0) {
			if (partuuid[0] && strcasecmp(spec + 9, partuuid) == 0) matched = 1;
		} else if (strstr(spec, "by-partuuid/")) {
			const char *p2 = strstr(spec, "by-partuuid/") + strlen("by-partuuid/");
			if (partuuid[0] && strcasecmp(p2, partuuid) == 0) matched = 1;
		} else {
			char real_spec[PATH_MAX];
			if (realpath(spec, real_spec) && strcmp(real_spec, real_devpath) == 0)
				matched = 1;
		}

		if (matched) {
			char mapper_path[PATH_MAX];
			snprintf(mapper_path, sizeof(mapper_path), "/dev/mapper/%s", name);
			if (mapper_name_out) snprintf(mapper_name_out, mapper_name_outlen, "%s", name);

			if (access(mapper_path, F_OK) == 0) {
				blkid_probe pr = blkid_new_probe_from_filename(mapper_path);
				if (pr) {
					const char *value = NULL;
					size_t len = 0;
					blkid_probe_enable_superblocks(pr, 1);
					blkid_probe_set_superblocks_flags(pr, BLKID_SUBLKS_TYPE);
					if (blkid_do_safeprobe(pr) == 0 &&
					    blkid_probe_lookup_value(pr, "TYPE", &value, &len) == 0 && value) {
						snprintf(out, outlen, "%.60s", value);
					} else {
						snprintf(out, outlen, "unlocked, unknown fs");
					}
					blkid_free_probe(pr);
				}
			} else {
				snprintf(out, outlen, "crypttab: %.40s, not currently unlocked", name);
			}
			break;
		}
	}
	fclose(f);
}

/* GPT/MBR partition TYPE label as fdisk itself reports it (e.g. "Linux
 * swap", "EFI System") — shown as a fallback when blkid can't read an
 * actual on-disk signature, since that at least reflects declared intent. */
static void get_gpttype_name(struct fdisk_partition *pa, char *out, size_t outlen) {
	struct fdisk_parttype *pt;
	out[0] = '\0';
	pt = fdisk_partition_get_type(pa);
	if (pt) {
		const char *name = fdisk_parttype_get_name(pt);
		if (name) snprintf(out, outlen, "%s", name);
	}
}

/* ---- build the segment list: partitions + every gap between them ---- */

static int cmp_by_start(const void *a, const void *b) {
	const segment_t *sa = a, *sb = b;
	if (sa->start < sb->start) return -1;
	if (sa->start > sb->start) return 1;
	return 0;
}

static void build_segments(struct fdisk_context *cxt, const char *disk_path) {
	struct fdisk_table *tb = NULL;
	struct fdisk_partition *pa = NULL;
	struct fdisk_iter *itr = NULL;
	uint64_t first_lba = fdisk_get_first_lba(cxt);
	uint64_t last_lba  = fdisk_get_last_lba(cxt);
	uint64_t cursor = first_lba;

	nsegments = 0;

	if (fdisk_get_partitions(cxt, &tb) != 0 || !tb) {
		fprintf(stderr, "Could not read partition table.\n");
		return;
	}
	fdisk_table_sort_partitions(tb, fdisk_partition_cmp_partno);

	/* First pass: collect real partitions, sorted by start sector.
	 * fdisk_table_sort_partitions sorts by partno, not start, so we
	 * re-sort our own array by start below. */
	itr = fdisk_new_iter(FDISK_ITER_FORWARD);
	while (fdisk_table_next_partition(tb, itr, &pa) == 0) {
		if (!fdisk_partition_has_start(pa) || !fdisk_partition_has_size(pa))
			continue;
		if (nsegments >= MAX_SEGMENTS) break;

		segments[nsegments].fstype[0] = '\0';
		segments[nsegments].crypto_detail[0] = '\0';
		segments[nsegments].crypt_mapper_name[0] = '\0';
		segments[nsegments].devpath[0] = '\0';
		{
			char uuid[64] = "";
			int partno = fdisk_partition_get_partno(pa) + 1;
			const char *partuuid = fdisk_partition_get_uuid(pa); /* GPT's own PARTUUID; NULL on MBR disks */

			detect_fstype(disk_path, partno,
				segments[nsegments].fstype, sizeof(segments[nsegments].fstype),
				uuid, sizeof(uuid));

			/* Always try crypttab matching, not just when blkid already
			 * found crypto_LUKS: "plain" dm-crypt mode (the common choice
			 * for swap) writes no on-disk header at all, so blkid finds
			 * nothing there — PARTUUID matching is the only way to
			 * recognise that case. */
			{
				char *devpath = fdisk_partname(disk_path, partno);
				if (devpath) {
					snprintf(segments[nsegments].devpath, sizeof(segments[nsegments].devpath), "%s", devpath);
					resolve_crypttab_mapping(devpath, uuid, partuuid ? partuuid : "",
						segments[nsegments].crypto_detail,
						sizeof(segments[nsegments].crypto_detail),
						segments[nsegments].crypt_mapper_name,
						sizeof(segments[nsegments].crypt_mapper_name));
					free(devpath);
				}
			}
		}
		get_gpttype_name(pa, segments[nsegments].gpttype, sizeof(segments[nsegments].gpttype));

		segments[nsegments].type   = SEG_PARTITION;
		segments[nsegments].start  = fdisk_partition_get_start(pa);
		segments[nsegments].end    = fdisk_partition_get_end(pa);
		segments[nsegments].partno = fdisk_partition_get_partno(pa);
		nsegments++;
	}
	fdisk_free_iter(itr);
	fdisk_unref_table(tb);

	qsort(segments, nsegments, sizeof(segment_t), cmp_by_start);

	/* Second pass: walk the sorted partitions and insert a SEG_FREE
	 * segment into any gap — before the first partition, between any
	 * two partitions, and after the last one. This is the same logic
	 * cfdisk itself uses internally to compute free space, since
	 * libfdisk has no single call that hands you "all the gaps". */
	{
		segment_t merged[MAX_SEGMENTS];
		int nmerged = 0;

		for (int i = 0; i < nsegments; i++) {
			if (segments[i].start > cursor) {
				if (nmerged < MAX_SEGMENTS) {
					merged[nmerged].type  = SEG_FREE;
					merged[nmerged].start = cursor;
					merged[nmerged].end   = segments[i].start - 1;
					merged[nmerged].partno = -1;
					merged[nmerged].devpath[0] = '\0';
					merged[nmerged].fstype[0] = '\0';
					merged[nmerged].gpttype[0] = '\0';
					merged[nmerged].crypto_detail[0] = '\0';
					merged[nmerged].crypt_mapper_name[0] = '\0';
					nmerged++;
				}
			}
			if (nmerged < MAX_SEGMENTS)
				merged[nmerged++] = segments[i];
			cursor = segments[i].end + 1;
		}
		if (cursor <= last_lba && nmerged < MAX_SEGMENTS) {
			merged[nmerged].type  = SEG_FREE;
			merged[nmerged].start = cursor;
			merged[nmerged].end   = last_lba;
			merged[nmerged].partno = -1;
			merged[nmerged].devpath[0] = '\0';
			merged[nmerged].fstype[0] = '\0';
			merged[nmerged].gpttype[0] = '\0';
			merged[nmerged].crypto_detail[0] = '\0';
			merged[nmerged].crypt_mapper_name[0] = '\0';
			nmerged++;
		}

		memcpy(segments, merged, sizeof(segment_t) * nmerged);
		nsegments = nmerged;
	}
}

/* ---- render a proportional bar across the whole disk ---- */

static void draw_bar(struct fdisk_context *cxt) {
	uint64_t first_lba = fdisk_get_first_lba(cxt);
	uint64_t last_lba  = fdisk_get_last_lba(cxt);

	/* On an empty/degenerate table (no label yet, or one libfdisk reports
	 * zero-sized usable space for) last_lba can come back equal to or
	 * below first_lba. The subtraction below would then wrap around to
	 * a huge uint64_t rather than going negative, silently corrupting
	 * every width calculation in the loop below — so bail with an empty
	 * bar instead of drawing garbage. */
	if (last_lba < first_lba) {
		printf("[]\n");
		printf("  (no usable space to show yet)\n\n");
		return;
	}
	uint64_t total = last_lba - first_lba + 1;

	printf("[");
	for (int i = 0; i < nsegments; i++) {
		uint64_t seg_size = segments[i].end - segments[i].start + 1;
		int width = (int)((double)seg_size / (double)total * BAR_WIDTH);
		if (width < 1) width = 1; /* every segment gets at least 1 char, even tiny ones */

		const char *glyph = (segments[i].type == SEG_PARTITION) ? "#" : ".";
		for (int c = 0; c < width; c++) fputs(glyph, stdout);
	}
	printf("]\n");
	printf("  # = partition   . = free space\n\n");
}

/* ---- render a sector range as text, including the currently selected
 * display unit alongside raw sectors so prompts stay consistent with
 * whatever unit the table above is shown in. ---- */
static void format_range(uint64_t start, uint64_t end, char *buf, size_t buflen) {
	uint64_t size = end - start + 1;
	if (display_unit == UNIT_SECTORS) {
		snprintf(buf, buflen, "sectors %llu-%llu (%llu sectors)",
			(unsigned long long)start, (unsigned long long)end, (unsigned long long)size);
	} else {
		char sb[32], eb[32], zb[32];
		format_size(start, sb, sizeof(sb));
		format_size(end, eb, sizeof(eb));
		format_size(size, zb, sizeof(zb));
		snprintf(buf, buflen, "sectors %llu-%llu (%s to %s, %s)",
			(unsigned long long)start, (unsigned long long)end, sb, eb, zb);
	}
}

/* Build the filesystem description for a partition segment, exactly as
 * shown in the table's Filesystem column: confirmed on-disk type first
 * (with the encryption target appended if it's a container), then
 * crypto_detail alone if blkid found nothing but crypttab matched
 * anyway — "plain" dm-crypt mode, common for swap, writes no on-disk
 * signature for blkid to see in the first place — then the declared GPT
 * type in parentheses as a last resort, "unknown" if none of that
 * resolved anything.
 *
 * This is the single source of truth for that description, used both
 * by the table itself and by [d]'s confirmation prompt — previously
 * delete had its own shorter, two-case version that skipped the
 * crypto_detail-only case, so a partition like an encrypted swap with
 * no confirmed fstype showed as generic "(Linux filesystem)" at the
 * confirmation prompt right when accuracy mattered most, while the
 * table two lines above correctly showed "encrypted -> swap". Sharing
 * one function means the two can't drift apart like that again. */
static void describe_filesystem(const segment_t *seg, char *buf, size_t buflen) {
	if (seg->fstype[0]) {
		if (seg->crypto_detail[0])
			snprintf(buf, buflen, "%s -> %s", seg->fstype, seg->crypto_detail);
		else
			snprintf(buf, buflen, "%s", seg->fstype);
	} else if (seg->crypto_detail[0]) {
		snprintf(buf, buflen, "encrypted -> %s", seg->crypto_detail);
	} else if (seg->gpttype[0]) {
		snprintf(buf, buflen, "(%s)", seg->gpttype);
	} else {
		snprintf(buf, buflen, "-");
	}
}

/* ---- list segments in a table, numbered for selection ---- */

static void list_segments(void) {
	const char *unit_label = (display_unit == UNIT_SECTORS) ? "Sectors" :
	                          (display_unit == UNIT_MIB) ? "Size (MiB)" : "Size (GiB)";
	int any_misaligned = 0;

	printf("%-4s %-6s %14s %14s %14s %-24s %s\n", "Idx", "Type", "Start", "End", unit_label, "Filesystem", "Device");
	for (int i = 0; i < nsegments; i++) {
		uint64_t size = segments[i].end - segments[i].start + 1;
		char start_buf[32], end_buf[32], size_buf[32], fs_buf[GPTTYPE_LEN + 100], idx_buf[8];
		int misaligned = (segments[i].type == SEG_PARTITION) && (segments[i].start % alignment_sectors != 0);
		int staged_create = 0;
		const char *row_color = "";

		if (misaligned) any_misaligned = 1;

		/* A partition that only exists because of a staged, not-yet-
		 * written [n] looks identical to a genuinely on-disk one
		 * otherwise — colour it so the table alone tells the story,
		 * without needing to cross-reference the queue list below. */
		if (segments[i].type == SEG_PARTITION)
			for (int j = 0; j < n_staged; j++)
				if (staged_ops[j].type == OP_CREATE && staged_ops[j].partno == (size_t)segments[i].partno) {
					staged_create = 1;
					break;
				}

		if (misaligned) row_color = c_yellow();
		else if (staged_create) row_color = c_green();
		else if (segments[i].type == SEG_FREE) row_color = c_dim();

		describe_filesystem(&segments[i], fs_buf, sizeof(fs_buf));

		if (display_unit == UNIT_SECTORS) {
			snprintf(start_buf, sizeof(start_buf), "%llu", (unsigned long long)segments[i].start);
			snprintf(end_buf, sizeof(end_buf), "%llu", (unsigned long long)segments[i].end);
		} else {
			format_size(segments[i].start, start_buf, sizeof(start_buf));
			format_size(segments[i].end, end_buf, sizeof(end_buf));
		}
		format_size(size, size_buf, sizeof(size_buf));
		snprintf(idx_buf, sizeof(idx_buf), "%d%s", i, misaligned ? "!" : "");

		if (segments[i].type == SEG_PARTITION)
			printf("%s%-4s %-6s %14s %14s %14s %-24s %s%s\n",
				row_color, idx_buf, "PART", start_buf, end_buf, size_buf, fs_buf, segments[i].devpath, c_reset());
		else
			printf("%s%-4s %-6s %14s %14s %14s %-24s%s\n",
				row_color, idx_buf, "FREE", start_buf, end_buf, size_buf, fs_buf, c_reset());
	}
	if (any_misaligned) {
		char targetbuf[32];
		format_bytes_short(alignment_sectors * sector_size, targetbuf, sizeof(targetbuf));
		printf("  %s!%s = start not aligned to a %s boundary — can hurt SSD\n"
			"      performance and wear, but is otherwise harmless\n",
			c_yellow(), c_reset(), targetbuf);
	}
	printf("\n");
}

/* ---- ask the user which unit to display sizes in ---- */

/* ---- Tab-completion support. ----
 * By default, readline falls back to filename completion when no custom
 * completer is registered — which is what caused the directory listing
 * to appear when Tab was pressed with nothing else set up. Registering
 * our own attempted_completion_function fixes that everywhere, and lets
 * specific prompts offer sensible numeric suggestions (e.g. the maximum
 * available length) instead of falling through to filenames.
 *
 * A couple of prompts (the backup/restore file paths) genuinely want
 * real filesystem-path completion instead of a fixed word list — for
 * those, set completion_want_filenames before the read_line() call and
 * clear it right after; diskviz_attempted_completion then hands off to
 * readline's own filename completer for that one prompt only. */
#define MAX_COMPLETIONS 8
static char completion_words[MAX_COMPLETIONS][32];
static int n_completion_words = 0;
static int completion_want_filenames = 0;

static void clear_completions(void) {
	n_completion_words = 0;
}

static void add_completion(const char *word) {
	if (n_completion_words < MAX_COMPLETIONS)
		snprintf(completion_words[n_completion_words++], sizeof(completion_words[0]), "%s", word);
}

static char *diskviz_completion_generator(const char *text, int state) {
	static int idx, len;
	if (!state) { idx = 0; len = (int)strlen(text); }
	while (idx < n_completion_words) {
		const char *candidate = completion_words[idx++];
		if (strncmp(candidate, text, len) == 0)
			return strdup(candidate);
	}
	return NULL;
}

static char **diskviz_attempted_completion(const char *text, int start, int end) {
	(void)start; (void)end;
	if (completion_want_filenames) {
		rl_completion_append_character = '\0'; /* single-path prompt — no trailing space after a full match */
		return rl_completion_matches(text, rl_filename_completion_function);
	}
	rl_attempted_completion_over = 1; /* never fall back to filename completion */
	rl_completion_append_character = '\0'; /* don't tack a trailing space onto a completed number */
	return rl_completion_matches(text, diskviz_completion_generator);
}

/* Reads one line of input via GNU readline — gives proper arrow-key
 * cursor movement, backspace/delete mid-line, and up/down history,
 * instead of raw escape sequences landing in the buffer as text.
 * The prompt is passed to readline() itself (not printed separately)
 * so it redraws correctly if the line gets edited or the terminal
 * resizes. Returns 0 on EOF (Ctrl-D). */
static int read_line(const char *prompt, char *buf, size_t sz) {
	char *line = readline(prompt);
	if (!line) {
		printf("\n");
		return 0;
	}
	if (line[0] != '\0') add_history(line);
	snprintf(buf, sz, "%s", line);
	free(line);
	return 1;
}

/* Drop the whole queue, releasing any GPT type references DELETE entries
 * were holding onto for a possible undo. Called once a write commits the
 * queue for real (nothing left to undo), and on exit. */
static void clear_staged_queue(void) {
	for (int i = 0; i < n_staged; i++)
		if (staged_ops[i].type == OP_DELETE && staged_ops[i].saved_type)
			fdisk_unref_parttype(staged_ops[i].saved_type);
	n_staged = 0;
}

/* If the trimmed input is exactly "q"/"Q", quit — same message and
 * cleanup as the main menu's own [q], regardless of which prompt this
 * was typed at, so quitting behaves identically everywhere rather than
 * depending on which corner of the program you happened to type it in.
 * original_tb may be NULL if the initial partition-table read failed. */
static void quit_if_requested(const char *s, struct fdisk_context *cxt, struct fdisk_table *original_tb) {
	if (s[0] && (s[0] == 'q' || s[0] == 'Q') && s[1] == '\0') {
		printf("Quitting — any staged, unwritten changes are discarded.\n");
		clear_staged_queue();
		if (original_tb) fdisk_unref_table(original_tb);
		fdisk_deassign_device(cxt, 1);
		g_cxt = NULL;
		fdisk_unref_context(cxt);
		exit(0);
	}
}

/* Simple y/N confirmation, defaulting to "no" on blank/EOF/anything that
 * doesn't start with y or Y — used for advisory warnings (alignment)
 * where declining just means "let me pick a different value" rather
 * than aborting the whole partition creation. */
static int ask_yes_no(const char *prompt, struct fdisk_context *cxt, struct fdisk_table *original_tb) {
	char line[16];
	clear_completions();
	add_completion("y"); add_completion("n");
	if (!read_line(prompt, line, sizeof(line))) return 0;
	quit_if_requested(line, cxt, original_tb);
	return (line[0] == 'y' || line[0] == 'Y');
}

static void choose_display_unit(struct fdisk_context *cxt, struct fdisk_table *original_tb) {
	char line[16];
	clear_completions();
	add_completion("1"); add_completion("2"); add_completion("3");
	if (!read_line("Display sizes in: 1) Sectors  2) MiB  3) GiB  (q to quit)  [1]: ",
	               line, sizeof(line)) || line[0] == '\0') {
		display_unit = UNIT_SECTORS;
		return;
	}
	quit_if_requested(line, cxt, original_tb);
	switch (atoi(line)) {
	case 2: display_unit = UNIT_MIB; break;
	case 3: display_unit = UNIT_GIB; break;
	default: display_unit = UNIT_SECTORS; break;
	}
}

/* Print the queue of staged, not-yet-written operations in order. */
static void list_staged_queue(void) {
	if (n_staged == 0) return;
	printf("Staged operations this session (not yet written), in order:\n");
	for (int i = 0; i < n_staged; i++) {
		if (staged_ops[i].type == OP_RESTORE) {
			printf("  %s%d. RESTORE table from %s (was: %s label) — replaces everything below%s\n",
				c_cyan(), i + 1, staged_ops[i].devpath, staged_ops[i].fstype, c_reset());
			continue;
		}
		{
			char rangebuf[160];
			format_range(staged_ops[i].start, staged_ops[i].start + staged_ops[i].size - 1,
				rangebuf, sizeof(rangebuf));
			if (staged_ops[i].type == OP_CREATE)
				printf("  %s%d. CREATE  %-20s %s (%s)%s\n",
					c_green(), i + 1, staged_ops[i].devpath, rangebuf, staged_ops[i].fstype, c_reset());
			else
				printf("  %s%d. DELETE  %-20s %s (was %s)%s\n",
					c_red(), i + 1, staged_ops[i].devpath, rangebuf, staged_ops[i].fstype, c_reset());
		}
	}
	printf("\n");
}

/* ---- create a new partition at a chosen start within a chosen free
 * segment. Stages it via fdisk_add_partition only — nothing is written
 * to disk here, so the caller can preview, undo, and retry freely. ---- */
static void create_partition_once(struct fdisk_context *cxt, const char *disk_path, struct fdisk_table *original_tb) {
	int idx;
	uint64_t start, size, end;
	struct fdisk_partition *pa;
	size_t new_partno;
	char line[64], rangebuf[160], valbuf[32];

	clear_completions();
	for (int i = 0; i < nsegments; i++)
		if (segments[i].type == SEG_FREE) {
			snprintf(valbuf, sizeof(valbuf), "%d", i);
			add_completion(valbuf);
		}
	if (!read_line("Select a FREE segment index to place a new partition in (q to quit): ",
	               line, sizeof(line))) return;
	quit_if_requested(line, cxt, original_tb);
	idx = atoi(line);

	if (idx < 0 || idx >= nsegments || segments[idx].type != SEG_FREE) {
		fprintf(stderr, "That's not a valid free-space segment.\n");
		return;
	}

	format_range(segments[idx].start, segments[idx].end, rangebuf, sizeof(rangebuf));
	printf("Segment spans %s.\n", rangebuf);

	/* Tab-complete to the segment's own start or end — the two obvious
	 * reference points someone would want when aligning a new partition
	 * exactly to an existing boundary, in whatever unit is on screen. */
	{
		char startval[32], endval[32], promptbuf[256];
		clear_completions();
		format_size(segments[idx].start, startval, sizeof(startval));
		add_completion(startval);
		format_size(segments[idx].end, endval, sizeof(endval));
		add_completion(endval);

		snprintf(promptbuf, sizeof(promptbuf),
			"Start of new partition — absolute sectors, or e.g. 500M/20G\n"
			"  (blank = segment start, Tab = %s/%s, q to quit)\n> ",
			startval, endval);
		if (!read_line(promptbuf, line, sizeof(line))) return;
	}
	quit_if_requested(line, cxt, original_tb);
	start = (line[0] == '\0') ? segments[idx].start : parse_sectors(line);

	/* Validate the start immediately — before it ever reaches the size
	 * prompt's arithmetic. Letting an out-of-range start through would
	 * make (segment.end - start) underflow (unsigned), producing a
	 * garbage multi-exabyte "available" figure instead of a clean error. */
	if (start < segments[idx].start || start > segments[idx].end) {
		fprintf(stderr, "That start position falls outside the selected free segment (%llu-%llu).\n",
			(unsigned long long)segments[idx].start, (unsigned long long)segments[idx].end);
		return;
	}

	/* Advisory alignment check on the start. Misaligned partitions
	 * still work fine — this is a performance/wear concern on SSDs,
	 * not a correctness one — so it's a warning with a way to proceed,
	 * never a hard block. */
	if (start % alignment_sectors != 0) {
		char offbuf[32], targetbuf[32];
		format_bytes_short((start % alignment_sectors) * sector_size, offbuf, sizeof(offbuf));
		format_bytes_short(alignment_sectors * sector_size, targetbuf, sizeof(targetbuf));
		fprintf(stderr, "Warning: that start isn't aligned to a %s boundary (off by %s) —\n"
			"  misaligned partitions can hurt SSD performance and wear.\n", targetbuf, offbuf);
		if (!ask_yes_no("Proceed anyway? [y/N]: ", cxt, original_tb)) {
			printf("Not creating partition — pick a different start.\n\n");
			return;
		}
	}

	format_range(start, segments[idx].end, rangebuf, sizeof(rangebuf));
	printf("From this start, up to %s is available.\n", rangebuf);

	/* Tab-complete straight to the maximum available length — this is
	 * the exact value someone typing an absolute end position (a common
	 * mix-up) actually needs, without doing the subtraction by hand. */
	{
		char maxval[32], promptbuf[256];
		clear_completions();
		format_size(segments[idx].end - start + 1, maxval, sizeof(maxval));
		add_completion(maxval);

		snprintf(promptbuf, sizeof(promptbuf),
			"Length of new partition — sectors, or e.g. 500M/20G\n"
			"  (blank = fill to end, Tab = %s max, NOT an absolute end, q to quit)\n> ",
			maxval);
		if (!read_line(promptbuf, line, sizeof(line))) return;
	}
	quit_if_requested(line, cxt, original_tb);

	/* Blank means "use everything left in this segment", matching how
	 * a blank start already means "start of the segment" above — rather
	 * than parse_sectors("") returning 0 and that 0 then being rejected
	 * as "doesn't fit". */
	size = (line[0] == '\0') ? (segments[idx].end - start + 1) : parse_sectors(line);

	if (size == 0 || size > segments[idx].end - start + 1) {
		fprintf(stderr, "That length doesn't fit in the remaining space from this start.\n");
		return;
	}

	end = start + size - 1;

	/* Advisory alignment check on the end too — an odd end here becomes
	 * the start of whatever free space follows, so this is really
	 * flagging "the next thing created after this will inherit a bad
	 * boundary" as much as anything about this partition itself. */
	if ((end + 1) % alignment_sectors != 0) {
		char offbuf[32], targetbuf[32], endbuf[32];
		format_size(end + 1, endbuf, sizeof(endbuf));
		format_bytes_short(((end + 1) % alignment_sectors) * sector_size, offbuf, sizeof(offbuf));
		format_bytes_short(alignment_sectors * sector_size, targetbuf, sizeof(targetbuf));
		fprintf(stderr, "Warning: this partition would end at %s, not aligned to a %s boundary (off by %s) —\n"
			"  the free space after it would inherit that misalignment.\n",
			endbuf, targetbuf, offbuf);
		if (!ask_yes_no("Proceed anyway? [y/N]: ", cxt, original_tb)) {
			printf("Not creating partition — pick a different length.\n\n");
			return;
		}
	}

	/* Intended filesystem, used only to (a) set the GPT partition TYPE
	 * metadata below, and (b) print a suggested mkfs command after write.
	 * diskviz never runs mkfs itself — formatting is left to the person,
	 * on purpose, since it's destructive and separate from partitioning. */
	clear_completions();
	add_completion("ext4"); add_completion("xfs"); add_completion("btrfs");
	add_completion("swap"); add_completion("vfat"); add_completion("none");
	if (!read_line("Intended filesystem — sets partition type only, not formatted automatically\n"
	               "  ext4/xfs/btrfs/swap/vfat/none (q to quit) [none]\n> ",
	               line, sizeof(line))) return;
	quit_if_requested(line, cxt, original_tb);
	{
		char fs[16];
		if (line[0] == '\0')
			snprintf(fs, sizeof(fs), "none");
		else
			snprintf(fs, sizeof(fs), "%.15s", line);
		for (char *p = fs; *p; p++) *p = (char)tolower((unsigned char)*p);

		if (strcmp(fs, "ext4") && strcmp(fs, "xfs") && strcmp(fs, "btrfs") &&
		    strcmp(fs, "swap") && strcmp(fs, "vfat") && strcmp(fs, "none")) {
			fprintf(stderr, "Unrecognised filesystem '%s' — staging as 'none' (unformatted).\n", fs);
			snprintf(fs, sizeof(fs), "none");
		}

		pa = fdisk_new_partition();
		fdisk_partition_set_start(pa, start);
		fdisk_partition_set_size(pa, size);
		fdisk_partition_partno_follow_default(pa, 1);

		/* Set the GPT partition type to match, purely as metadata (OS
		 * installers and disk tools read this to guess intent) — it does
		 * NOT format anything by itself. All filesystem types share the
		 * same generic "Linux filesystem" GPT type; only swap is distinct. */
		if (strcmp(fs, "none") != 0) {
			struct fdisk_label *lb = fdisk_get_label(cxt, NULL);
			struct fdisk_parttype *pt = fdisk_label_advparse_parttype(
				lb, strcmp(fs, "swap") == 0 ? "swap" : "linux", FDISK_PARTTYPE_PARSE_DEFAULT);
			if (pt) {
				fdisk_partition_set_type(pa, pt);
				fdisk_unref_parttype(pt);
			}
		}

		int add_rc = fdisk_add_partition(cxt, pa, &new_partno);
		if (add_rc != 0) {
			/* libfdisk functions return a negative errno-style code on
			 * failure (e.g. -EINVAL for a bad start/size), so this is
			 * the same information strerror(errno) would give elsewhere
			 * in this program — just sourced from the return value
			 * instead of errno, since libfdisk doesn't set errno itself. */
			fprintf(stderr, "Failed to add partition (start=%llu size=%llu): %s\n",
			        (unsigned long long)start, (unsigned long long)size,
			        strerror(add_rc < 0 ? -add_rc : add_rc));
			fdisk_unref_partition(pa);
			return;
		}
		fdisk_unref_partition(pa);

		if (n_staged < MAX_STAGED) {
			staged_op_t *op = &staged_ops[n_staged];
			op->type = OP_CREATE;
			op->partno = new_partno;
			op->start = start;
			op->size = size;
			op->saved_type = NULL;
			snprintf(op->fstype, sizeof(op->fstype), "%s", fs);
			{
				char *dp = fdisk_partname(disk_path, (int)new_partno + 1);
				if (dp) { snprintf(op->devpath, sizeof(op->devpath), "%s", dp); free(dp); }
				else op->devpath[0] = '\0';
			}
			n_staged++;
		}
	}

	format_range(start, end, rangebuf, sizeof(rangebuf));
	printf("Staged partition #%zu at %s — not written yet.\n\n", new_partno, rangebuf);
}

/* Check whether a device path is currently in active use — mounted as
 * a filesystem (/proc/mounts) or active as swap (/proc/swaps). Checks
 * the path itself and its realpath() (so a symlink like /dev/mapper/x
 * matches even when the kernel reports the resolved /dev/dm-N in those
 * files, which is exactly what happens for dm-crypt). This is a
 * courtesy warning, not something enforced here: diskviz still lets
 * the deletion proceed either way, since the write itself will simply
 * fail (or the kernel will refuse the incremental rescan for that
 * node) if it's genuinely still in use at that point. */
static int device_in_use(const char *devpath, char *where, size_t wherelen) {
	FILE *f;
	char line[512], real_devpath[PATH_MAX];

	if (!devpath || !devpath[0]) return 0;
	if (!realpath(devpath, real_devpath))
		snprintf(real_devpath, sizeof(real_devpath), "%s", devpath);

	f = fopen("/proc/mounts", "r");
	if (f) {
		while (fgets(line, sizeof(line), f)) {
			char dev[256], mnt[256];
			if (sscanf(line, "%255s %255s", dev, mnt) == 2 &&
			    (strcmp(dev, devpath) == 0 || strcmp(dev, real_devpath) == 0)) {
				snprintf(where, wherelen, "mounted at %s", mnt);
				fclose(f);
				return 1;
			}
		}
		fclose(f);
	}

	f = fopen("/proc/swaps", "r");
	if (f) {
		if (fgets(line, sizeof(line), f)) { /* discard header row */ }
		while (fgets(line, sizeof(line), f)) {
			char dev[256];
			if (sscanf(line, "%255s", dev) == 1 &&
			    (strcmp(dev, devpath) == 0 || strcmp(dev, real_devpath) == 0)) {
				snprintf(where, wherelen, "active as swap");
				fclose(f);
				return 1;
			}
		}
		fclose(f);
	}

	return 0;
}

/* Check whether a partition segment is in use, either directly (a
 * plain filesystem or swap on the partition itself) or, for a
 * crypttab-matched partition, via its mapped /dev/mapper/<name> device
 * — which is what the kernel actually sees as mounted/active, not the
 * raw partition. Plain dm-crypt swap (no LUKS header, no on-disk
 * signature at all) is exactly the case this exists for: the table
 * already shows it as "encrypted -> ...", but until this check also
 * looks at the mapper device, [d] had no way to notice it was live. */
static int partition_in_use(const segment_t *seg, char *where, size_t wherelen) {
	if (device_in_use(seg->devpath, where, wherelen))
		return 1;

	if (seg->crypt_mapper_name[0]) {
		char mapper_path[PATH_MAX];
		char sub_where[280];
		snprintf(mapper_path, sizeof(mapper_path), "/dev/mapper/%s", seg->crypt_mapper_name);
		if (device_in_use(mapper_path, sub_where, sizeof(sub_where))) {
			snprintf(where, wherelen, "%s (via /dev/mapper/%s)", sub_where, seg->crypt_mapper_name);
			return 1;
		}
	}

	return 0;
}

/* ---- delete an existing partition (whether it was already on disk
 * before this session, or created and written earlier in this same
 * session). Like everything else here, this only edits libfdisk's
 * in-memory table — nothing is actually removed from disk until the
 * next [w]rite — but deleting a real, already-written partition can
 * destroy real data the moment that write happens, so this asks for
 * the partition's own device path to be typed back rather than a bare
 * YES: harder to do by reflex, on the wrong index, at 2am. ---- */
static void delete_partition(struct fdisk_context *cxt, struct fdisk_table *original_tb) {
	int idx;
	char line[PATH_MAX];

	clear_completions();
	for (int i = 0; i < nsegments; i++)
		if (segments[i].type == SEG_PARTITION) {
			char valbuf[32];
			snprintf(valbuf, sizeof(valbuf), "%d", i);
			add_completion(valbuf);
		}
	if (!read_line("Select a PART segment index to delete (q to quit): ", line, sizeof(line))) return;
	quit_if_requested(line, cxt, original_tb);
	idx = atoi(line);

	if (idx < 0 || idx >= nsegments || segments[idx].type != SEG_PARTITION) {
		fprintf(stderr, "That's not a valid partition segment.\n\n");
		return;
	}

	{
		char fsdesc[GPTTYPE_LEN + 100];
		char promptbuf[PATH_MAX + 80];
		char whereused[320];

		describe_filesystem(&segments[idx], fsdesc, sizeof(fsdesc));

		printf("About to stage deletion of partition %d: %s, %s\n"
			"  — this only takes effect at [w]rite.\n",
			segments[idx].partno + 1, segments[idx].devpath, fsdesc);

		if (partition_in_use(&segments[idx], whereused, sizeof(whereused))) {
			fprintf(stderr, "Warning: this partition is currently %s —\n"
				"  writing this deletion will make that inaccessible through this partition.\n", whereused);
		}

		snprintf(promptbuf, sizeof(promptbuf),
			"Type the device path exactly to confirm — %s (q to quit)\n> ", segments[idx].devpath);
		if (!read_line(promptbuf, line, sizeof(line))) return;
	}
	quit_if_requested(line, cxt, original_tb);

	if (strcmp(line, segments[idx].devpath) != 0) {
		printf("Confirmation didn't match — not deleting.\n\n");
		return;
	}

	/* Grab a live handle on the partition being deleted, purely so its
	 * exact GPT type can be pinned (ref'd) before the delete — that's
	 * what lets [u] restore it faithfully afterwards, rather than
	 * falling back to a generic "linux"/"swap" guess. start/size are
	 * already known from segments[idx], but re-reading them here too
	 * keeps this self-contained if that ever changes. */
	struct fdisk_partition *pa = NULL;
	struct fdisk_parttype *saved_type = NULL;
	uint64_t saved_start = segments[idx].start;
	uint64_t saved_size = segments[idx].end - segments[idx].start + 1;
	if (fdisk_get_partition(cxt, (size_t)segments[idx].partno, &pa) == 0 && pa) {
		struct fdisk_parttype *t = fdisk_partition_get_type(pa);
		if (t) {
			fdisk_ref_parttype(t);
			saved_type = t;
		}
		saved_start = fdisk_partition_get_start(pa);
		saved_size = fdisk_partition_get_size(pa);
		fdisk_unref_partition(pa);
	}

	if (fdisk_delete_partition(cxt, (size_t)segments[idx].partno) != 0) {
		if (saved_type) fdisk_unref_parttype(saved_type);
		fprintf(stderr, "Failed to delete partition %d.\n\n", segments[idx].partno + 1);
		return;
	}

	/* If this partition was itself a still-staged, not-yet-written
	 * CREATE from earlier in this session, cancel that queue entry
	 * instead of adding a new DELETE — net effect is "never happened",
	 * and there's nothing on disk to restore later anyway. */
	for (int i = 0; i < n_staged; i++) {
		if (staged_ops[i].type == OP_CREATE && staged_ops[i].partno == (size_t)segments[idx].partno) {
			for (int j = i; j < n_staged - 1; j++)
				staged_ops[j] = staged_ops[j + 1];
			n_staged--;
			if (saved_type) fdisk_unref_parttype(saved_type); /* not stored, just release */
			printf("Cancelled staged creation of partition %d — not written yet.\n\n", segments[idx].partno + 1);
			return;
		}
	}

	if (n_staged < MAX_STAGED) {
		staged_op_t *op = &staged_ops[n_staged];
		op->type = OP_DELETE;
		op->partno = segments[idx].partno;
		op->start = saved_start;
		op->size = saved_size;
		op->saved_type = saved_type;
		snprintf(op->fstype, sizeof(op->fstype), "%s",
			segments[idx].fstype[0] ? segments[idx].fstype :
			(segments[idx].gpttype[0] ? segments[idx].gpttype : "unknown"));
		snprintf(op->devpath, sizeof(op->devpath), "%s", segments[idx].devpath);
		n_staged++;
	} else if (saved_type) {
		fdisk_unref_parttype(saved_type); /* queue full, couldn't record it — release the ref */
	}
	printf("Staged deletion of partition %d — not written yet.\n\n", segments[idx].partno + 1);
}

/* Undo the most recently staged operation, whichever kind it was — pops
 * the queue LIFO. A staged CREATE is simply deleted again; a staged
 * DELETE is restored using the exact start/size/GPT type captured when
 * it was staged. Either way, nothing on the real disk is touched, since
 * neither kind has been written yet. */
/* Wipe cxt's current in-memory partitions and rebuild them exactly as
 * found in `tb` (start/size/GPT type, at the same partno), under the
 * given label type. This is the same in-memory-only rebuild [r]estore
 * itself performs from a file, just sourced from an in-memory table
 * snapshot instead — which is exactly what's needed to undo a staged
 * [r]estore: rebuild from `original_tb`, the session's own record of
 * what's genuinely still on disk. */
static int replace_table_from_snapshot(struct fdisk_context *cxt, struct fdisk_table *tb, const char *label_name) {
	struct fdisk_partition *pa = NULL;
	struct fdisk_iter *itr;
	struct fdisk_label *lb;
	int rc = 0;

	fdisk_delete_all_partitions(cxt);

	lb = fdisk_get_label(cxt, NULL);
	if (label_name && (!lb || strcmp(fdisk_label_get_name(lb), label_name) != 0)) {
		if (fdisk_create_disklabel(cxt, label_name) != 0)
			return -1;
	}

	itr = fdisk_new_iter(FDISK_ITER_FORWARD);
	while (fdisk_table_next_partition(tb, itr, &pa) == 0) {
		struct fdisk_partition *newpa = fdisk_new_partition();
		struct fdisk_parttype *t = fdisk_partition_get_type(pa);
		size_t partno_out;

		fdisk_partition_set_start(newpa, fdisk_partition_get_start(pa));
		fdisk_partition_set_size(newpa, fdisk_partition_get_size(pa));
		fdisk_partition_set_partno(newpa, fdisk_partition_get_partno(pa));
		if (t) fdisk_partition_set_type(newpa, t);

		if (fdisk_add_partition(cxt, newpa, &partno_out) != 0)
			rc = -1;
		fdisk_unref_partition(newpa);
	}
	fdisk_free_iter(itr);
	return rc;
}

/* Undo the most recently staged operation, whichever kind it was — pops
 * the queue LIFO. A staged CREATE is simply deleted again; a staged
 * DELETE is restored using the exact start/size/GPT type captured when
 * it was staged; a staged RESTORE is reverted by rebuilding from
 * `original_tb`, the genuine on-disk state from before this session's
 * edits. Either way, nothing on the real disk is touched, since none of
 * it has been written yet. */
static void undo_last_staged(struct fdisk_context *cxt, struct fdisk_table *original_tb) {
	if (n_staged == 0) {
		printf("Nothing staged to undo.\n\n");
		return;
	}
	staged_op_t *op = &staged_ops[n_staged - 1];

	if (op->type == OP_CREATE) {
		if (fdisk_delete_partition(cxt, op->partno) == 0) {
			n_staged--;
			printf("Removed staged creation of partition %zu.\n\n", op->partno + 1);
		} else {
			fprintf(stderr, "Failed to remove staged partition %zu.\n\n", op->partno + 1);
		}
		return;
	}

	if (op->type == OP_RESTORE) {
		if (!original_tb) {
			fprintf(stderr, "No baseline table available to revert to.\n\n");
			return;
		}
		if (replace_table_from_snapshot(cxt, original_tb, op->fstype) == 0) {
			printf("Reverted staged table restore — back to what's actually on disk.\n\n");
			n_staged--;
		} else {
			fprintf(stderr, "Failed to fully revert the restore — check the table carefully before writing.\n\n");
		}
		return;
	}

	/* OP_DELETE: restore it at the same slot with the same start/size
	 * and original GPT type. */
	{
		struct fdisk_partition *pa = fdisk_new_partition();
		size_t restored_partno;

		fdisk_partition_set_start(pa, op->start);
		fdisk_partition_set_size(pa, op->size);
		fdisk_partition_set_partno(pa, op->partno);
		if (op->saved_type)
			fdisk_partition_set_type(pa, op->saved_type);

		if (fdisk_add_partition(cxt, pa, &restored_partno) == 0) {
			printf("Restored staged deletion of partition %zu.\n\n", restored_partno + 1);
			if (op->saved_type) fdisk_unref_parttype(op->saved_type);
			n_staged--;
		} else {
			fprintf(stderr, "Failed to restore partition %zu — it's still staged as deleted.\n\n", op->partno + 1);
		}
		fdisk_unref_partition(pa);
	}
}

/* ---- read a single-line sysfs attribute for the disk, e.g. model/serial.
 * These live under /sys/block/<basename>/device/, not in libfdisk at all —
 * they're not partition-table data, they're the physical drive's own
 * identity, exposed by the kernel driver (nvme/scsi/ata). Missing files
 * (e.g. inside a VM, or a controller that doesn't expose one) are handled
 * gracefully rather than treated as an error. */
static void read_sysfs_attr(const char *disk_path, const char *attr, char *out, size_t outlen) {
	const char *base = strrchr(disk_path, '/');
	base = base ? base + 1 : disk_path;

	char path[PATH_MAX];
	snprintf(path, sizeof(path), "/sys/block/%s/device/%s", base, attr);

	FILE *f = fopen(path, "r");
	if (!f) {
		snprintf(out, outlen, "unknown");
		return;
	}
	if (!fgets(out, outlen, f)) {
		snprintf(out, outlen, "unknown");
	} else {
		size_t len = strlen(out);
		while (len > 0 && isspace((unsigned char)out[len - 1]))
			out[--len] = '\0';
		if (out[0] == '\0') snprintf(out, outlen, "unknown");
	}
	fclose(f);
}

static void print_disk_info(struct fdisk_context *cxt, const char *disk_path) {
	char model[65], serial[65];
	uint64_t nsectors = fdisk_get_nsectors(cxt);
	uint64_t total_bytes = nsectors * (uint64_t)sector_size;
	double total_gib = (double)total_bytes / (1024.0 * 1024.0 * 1024.0);

	read_sysfs_attr(disk_path, "model", model, sizeof(model));
	read_sysfs_attr(disk_path, "serial", serial, sizeof(serial));

	printf("Disk: %s   Model: %s   Serial: %s\n",
		fdisk_get_devname(cxt), model, serial);
	printf("Size: %.2f GiB (%llu bytes, %llu sectors)   Sector size: %lu bytes\n\n",
		total_gib, (unsigned long long)total_bytes,
		(unsigned long long)nsectors, sector_size);
}

/* Build the mkfs-family command someone would run by hand to format a
 * staged partition with the filesystem they picked at creation time.
 * diskviz itself never runs this — see note on format_staged_partitions(). */
static void suggest_mkfs_cmd(const char *fs, const char *devpath, char *buf, size_t buflen) {
	if (strcmp(fs, "swap") == 0)
		snprintf(buf, buflen, "sudo mkswap %s", devpath);
	else if (strcmp(fs, "none") == 0)
		snprintf(buf, buflen, "no filesystem requested for this one");
	else
		snprintf(buf, buflen, "sudo mkfs.%s %s", fs, devpath);
}

/* After a successful write, make the kernel create device nodes for the
 * partitions added this session, then report their paths.
 *
 * diskviz does NOT format anything itself — it only ever writes the
 * partition table. Formatting is destructive and separate from
 * partitioning, so it's left to the person to run mkfs themselves once
 * they've double-checked the device path below.
 *
 * fdisk_reread_partition_table() (BLKRRPART) rereads the WHOLE table and
 * refuses (silently, as far as new nodes are concerned) if ANY partition
 * on the disk is currently mounted/busy — which is normal for a live
 * system disk. fdisk_reread_changes() instead diffs `original_tb` (the
 * table as it was before this session's edits) against the current one
 * and adds/removes only the partitions that actually changed, via BLKPG —
 * the same incremental approach fdisk/cfdisk use, and it works fine
 * alongside other mounted partitions on the same disk. */
static void format_staged_filesystems(struct fdisk_context *cxt, const char *disk_path,
                                       struct fdisk_table *original_tb) {
	if (n_staged == 0) return;

	printf("Applying partition table changes to the running kernel...\n");
	if (!original_tb || fdisk_reread_changes(cxt, original_tb) != 0) {
		fprintf(stderr, "Incremental update unavailable or failed — falling back to a full reread\n"
			"  (this can fail to add new nodes if other partitions on the disk are mounted).\n");
		fdisk_reread_partition_table(cxt);
	}

	int any_create = 0;
	for (int i = 0; i < n_staged; i++)
		if (staged_ops[i].type == OP_CREATE) any_create = 1;
	if (!any_create) { printf("\n"); return; }

	printf("\nPartition(s) created this round — unformatted, so run the suggested\n"
	       "command yourself if you want a filesystem on any of them:\n");
	for (int i = 0; i < n_staged; i++) {
		if (staged_ops[i].type != OP_CREATE) continue;

		char *devpath = fdisk_partname(disk_path, (int)staged_ops[i].partno + 1);
		if (!devpath) continue;

		int tries = 0;
		while (access(devpath, F_OK) != 0 && tries < 20) {
			usleep(250000);
			tries++;
		}
		if (access(devpath, F_OK) != 0) {
			fprintf(stderr, "  %s — never appeared; a reboot will pick it up.\n", devpath);
		} else {
			char suggestion[160];
			suggest_mkfs_cmd(staged_ops[i].fstype, devpath, suggestion, sizeof(suggestion));
			printf("  %-20s %s\n", devpath, suggestion);
		}
		free(devpath);
	}
	printf("\n");
}

/* ---- back up the partition table currently on disk to an sfdisk-style
 * dump file, using libfdisk's own script API — the same mechanism
 * `sfdisk --dump` uses, so the result is restorable with a plain
 * `sfdisk /dev/DEVICE < backupfile`, no extra tools required.
 *
 * Only allowed while nothing is staged: cxt's in-memory table always
 * reflects staged-but-unwritten edits too, so a backup taken mid-stage
 * would capture the changes about to be written rather than what's
 * actually still on disk right now — which defeats the point of a
 * pre-write safety net. Right after opening the disk, or right after a
 * [w]rite (when the queue is empty again), cxt matches the real disk
 * exactly, which is the only time this can give a trustworthy backup. */
static void backup_partition_table(struct fdisk_context *cxt, const char *disk_path, struct fdisk_table *original_tb) {
	char line[PATH_MAX], defaultpath[PATH_MAX];
	const char *base;
	struct fdisk_script *dp;
	FILE *f;

	if (n_staged > 0) {
		fprintf(stderr, "You have staged, unwritten changes — write or undo them first, "
			"so the backup reflects what's actually on disk.\n\n");
		return;
	}

	base = strrchr(disk_path, '/');
	base = base ? base + 1 : disk_path;
	{
		time_t now = time(NULL);
		struct tm tmv;
		char stamp[32];
		localtime_r(&now, &tmv);
		strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tmv);
		snprintf(defaultpath, sizeof(defaultpath), "/root/%s-partition-backup-%s.sfdisk", base, stamp);
	}

	clear_completions();
	completion_want_filenames = 1;
	{
		char promptbuf[PATH_MAX + 64];
		snprintf(promptbuf, sizeof(promptbuf),
			"Backup file path — Tab to complete\n"
			"  (blank = %s, q to quit)\n> ", defaultpath);
		int ok = read_line(promptbuf, line, sizeof(line));
		completion_want_filenames = 0;
		if (!ok) return;
	}
	quit_if_requested(line, cxt, original_tb);

	const char *outpath = line[0] ? line : defaultpath;

	dp = fdisk_new_script(cxt);
	if (!dp) {
		fprintf(stderr, "Failed to create backup script object.\n\n");
		return;
	}
	if (fdisk_script_read_context(dp, cxt) != 0) {
		fprintf(stderr, "Failed to read the current partition table for backup.\n\n");
		fdisk_unref_script(dp);
		return;
	}
	f = fopen(outpath, "w");
	if (!f) {
		fprintf(stderr, "Failed to open %s for writing: %s\n\n", outpath, strerror(errno));
		fdisk_unref_script(dp);
		return;
	}
	if (fdisk_script_write_file(dp, f) != 0) {
		fprintf(stderr, "Failed to write backup to %s.\n\n", outpath);
		fclose(f);
		fdisk_unref_script(dp);
		return;
	}
	fclose(f);
	fdisk_unref_script(dp);

	printf("Partition table backed up to %s\n", outpath);
	printf("To restore it later if a write goes wrong:\n"
	       "  sudo sfdisk %s < %s\n\n", disk_path, outpath);
}

/* ---- restore a previously-backed-up partition table (as produced by
 * [b], or any sfdisk-compatible text dump) into the in-memory context.
 * Uses libfdisk's own script-apply path (fdisk_new_script_from_file +
 * fdisk_apply_script) — the exact same mechanism `sfdisk /dev/X < file`
 * uses internally, so anything sfdisk can restore, this can too.
 *
 * Like everything else here, applying the script only edits cxt's
 * in-memory table; nothing touches the real disk until [w]rite. Unlike
 * a normal create/delete this can replace the WHOLE table in one go, so
 * it's staged as its own queue entry (OP_RESTORE) rather than expanded
 * into individual CREATE/DELETE entries — [u] undoing it reverts cxt
 * back to exactly the state in `original_tb`, the genuine on-disk table
 * from before this session's edits.
 *
 * Only allowed while nothing else is staged, for the same reason as
 * [b]: it needs cxt to still match the real disk, both so the "back to
 * disk" undo is meaningful and so a restore doesn't clobber other
 * pending, unwritten work. ---- */
static void restore_partition_table(struct fdisk_context *cxt, struct fdisk_table *original_tb) {
	char path[PATH_MAX], confirm[PATH_MAX];
	struct fdisk_script *dp;
	struct fdisk_label *lb;
	const char *prior_label_name;
	const char *disk_path = fdisk_get_devname(cxt);

	if (n_staged > 0) {
		fprintf(stderr, "You have staged, unwritten changes — write or undo them first, "
			"so restoring starts from a table that actually matches disk.\n\n");
		return;
	}

	clear_completions();
	completion_want_filenames = 1;
	{
		int ok = read_line("Backup file to restore from (Tab to complete, q to quit): ", path, sizeof(path));
		completion_want_filenames = 0;
		if (!ok) return;
	}
	quit_if_requested(path, cxt, original_tb);
	if (path[0] == '\0') {
		printf("No file given.\n\n");
		return;
	}

	dp = fdisk_new_script_from_file(cxt, path);
	if (!dp) {
		fprintf(stderr, "Failed to read %s: %s\n\n", path, strerror(errno));
		return;
	}

	lb = fdisk_get_label(cxt, NULL);
	prior_label_name = lb ? fdisk_label_get_name(lb) : "gpt";

	printf("This will REPLACE the entire partition table with the contents of %s.\n", path);
	printf("Like everything else here it only takes effect at [w]rite —\n"
	       "  [u] can undo it back to what's currently really on disk.\n");
	{
		char promptbuf[PATH_MAX + 80];
		snprintf(promptbuf, sizeof(promptbuf),
			"Type the device path to confirm — %s (q to quit)\n> ", disk_path);
		if (!read_line(promptbuf, confirm, sizeof(confirm))) {
			fdisk_unref_script(dp);
			return;
		}
	}
	quit_if_requested(confirm, cxt, original_tb);
	if (strcmp(confirm, disk_path) != 0) {
		printf("Confirmation didn't match — not restoring.\n\n");
		fdisk_unref_script(dp);
		return;
	}

	if (fdisk_apply_script(cxt, dp) != 0) {
		fprintf(stderr, "Failed to apply %s — table left unchanged.\n\n", path);
		fdisk_unref_script(dp);
		return;
	}
	fdisk_unref_script(dp);

	if (n_staged < MAX_STAGED) {
		staged_op_t *op = &staged_ops[n_staged];
		op->type = OP_RESTORE;
		op->saved_type = NULL;
		snprintf(op->devpath, sizeof(op->devpath), "%s", path);              /* reused: backup file path */
		snprintf(op->fstype, sizeof(op->fstype), "%s", prior_label_name);    /* reused: label before restore */
		n_staged++;
	}
	printf("Staged: partition table replaced from %s — not written yet.\n\n", path);
}

/* ---- print usage/help and exit; doesn't touch any device. ---- */
static void print_help(const char *progname) {
	printf(
"diskviz version %s — minimal libfdisk-based partition table visualiser + creator\n\n"
"Usage:\n"
"  %s /dev/DEVICE      Open a disk for viewing and editing (needs root)\n"
"  %s -h | --help      Show this help and exit\n\n"
"If the device has no partition table at all yet, diskviz offers to\n"
"stage a fresh GPT label before anything else — like everything below,\n"
"that's only committed to disk on [w]rite.\n\n"
"Interactive commands, once a disk is open:\n"
"  n   create a partition in a chosen free-space segment (staged, not written)\n"
"  u   undo the most recently staged operation — create, delete, or restore\n"
"  d   delete an existing partition (staged, not written)\n"
"  b   back up the current on-disk partition table to a file\n"
"  r   restore a partition table from a backup file (staged, not written)\n"
"  w   write all staged changes to disk now — returns to this menu, doesn't exit\n"
"  q   quit — discards staged, unwritten changes (the only command that exits)\n\n"
"[b] and [r] only work while nothing else is staged, so what they read\n"
"from or write to always matches what's genuinely on disk.\n\n"
"[d] and [r] also make you type the target device's own path back\n"
"exactly, rather than a bare YES — either can affect real, already-\n"
"written data once [w]ritten, so a name typed twice is the safeguard.\n\n"
"Nothing touches the real disk until you type YES at [w]. diskviz never\n"
"formats anything itself — it only edits the partition table; after a\n"
"write, run the mkfs/mkswap command it suggests yourself if you want a\n"
"filesystem on a new partition.\n\n"
"A staged, not-yet-written create shows green right in the table, and a\n"
"misaligned partition start shows yellow. The staged-operations list\n"
"below the table uses the same green for a create and adds red for a\n"
"delete, so the two views' colours line up. Colour only appears on a\n"
"real terminal, and never if the NO_COLOR environment variable is set.\n"
"Under sudo, set it like this — sudo's usual environment reset strips a\n"
"plain \"NO_COLOR=1 sudo ...\" before diskviz ever sees it:\n"
"  sudo env NO_COLOR=1 %s /dev/DEVICE\n\n"
"At most prompts, press Tab to autocomplete a suggested value, or type\n"
"q to back out without changing anything.\n",
		DISKVIZ_VERSION, progname, progname, progname);
}

/* ---- SIGINT/SIGHUP/SIGTERM handling ----
 * Every other quit path in this program — [q], EOF, the blank-disk
 * "no" answers — runs fdisk_deassign_device() before exiting. Without
 * this, Ctrl-C (or a closed terminal, or a plain `kill`) at any prompt
 * killed the process immediately and skipped that step entirely, which
 * can leave the device's exclusive open/lock held until something else
 * comes along and releases it.
 *
 * This is safe to do abruptly: fdisk_write_disklabel() is the only call
 * that ever commits anything, so every staged-but-unwritten change here
 * only ever existed in memory — an interrupted session is exactly as
 * safe as a deliberate [q]uit, nothing on the real disk changes either
 * way.
 *
 * rl_catch_signals is turned off in main() so readline doesn't install
 * its own handlers first and race with these. rl_free_line_state() and
 * rl_cleanup_after_signal() are the readline-provided calls for undoing
 * readline's terminal state (echo, line discipline, partial input) from
 * inside a handler like this one — normally readline's own signal
 * handling would do this, so we do it ourselves instead.
 *
 * fdisk_deassign_device() itself isn't documented as async-signal-safe,
 * but it's a small, bounded close()+ioctl() and this is the same
 * pattern common CLI tools use for exactly this situation. Exiting via
 * _exit() rather than exit() avoids running atexit handlers or flushing
 * stdio from inside a signal handler. The re-entry guard just covers a
 * second signal arriving while this one is still unwinding. */
static volatile sig_atomic_t g_terminating = 0;

static void handle_termination_signal(int signum) {
	if (g_terminating) return;
	g_terminating = 1;

	rl_free_line_state();
	rl_cleanup_after_signal();

	if (g_cxt) {
		fdisk_deassign_device(g_cxt, 1);
		g_cxt = NULL;
	}

	static const char msg[] =
		"\nInterrupted — device released, staged changes were never written.\n";
	ssize_t ignored = write(STDOUT_FILENO, msg, sizeof(msg) - 1);
	(void)ignored;

	_exit(128 + signum);
}

/* Installs handle_termination_signal() for SIGINT, SIGHUP and SIGTERM.
 * Called once at the top of main(), before readline ever runs. */
static void install_termination_handlers(void) {
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = handle_termination_signal;
	sigfillset(&sa.sa_mask); /* don't let another of these three interrupt the handler itself */
	sa.sa_flags = 0;

	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
}

int main(int argc, char **argv) {
	struct fdisk_context *cxt;

	if (argc == 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
		print_help(argv[0]);
		return 0;
	}

	use_color = isatty(STDOUT_FILENO) && !getenv("NO_COLOR");

	install_termination_handlers();

	rl_attempted_completion_function = diskviz_attempted_completion;
	rl_completion_append_character = '\0'; /* don't add a trailing space after completing a number */
	rl_catch_signals = 0; /* we install our own SIGINT/SIGHUP/SIGTERM handlers above instead */

	printf("diskviz version %s\n\n", DISKVIZ_VERSION);

	if (argc != 2) {
		fprintf(stderr, "Usage: %s /dev/DEVICE   (or: %s --help)\n", argv[0], argv[0]);
		return 1;
	}

	cxt = fdisk_new_context();
	if (!cxt) {
		fprintf(stderr, "Could not create fdisk context.\n");
		return 1;
	}

	if (fdisk_assign_device(cxt, argv[1], 0) != 0) {
		fprintf(stderr, "Could not open %s (are you root?).\n", argv[1]);
		fdisk_unref_context(cxt);
		return 1;
	}
	g_cxt = cxt; /* device is now live — see handle_termination_signal() */

	sector_size = fdisk_get_sector_size(cxt); /* actual logical sector size, not assumed 512 */
	alignment_sectors = get_alignment_sectors(argv[1]); /* advisory SSD/RAID-friendly alignment target */

	print_disk_info(cxt, argv[1]);

	/* A brand-new/blank disk has no partition table at all yet — nothing
	 * else in this program works without one (build_segments has
	 * nowhere to even report free space). Offer to stage a fresh GPT
	 * label, same as everything else here: fdisk_create_disklabel()
	 * only edits the in-memory context, so it's still not committed to
	 * the real disk until [w]rite. */
	if (!fdisk_has_label(cxt)) {
		char line[16];
		printf("No partition table found on %s.\n", argv[1]);
		clear_completions();
		add_completion("gpt");
		if (!read_line("Create a new GPT partition table? Type gpt to confirm (anything else to exit): ",
		               line, sizeof(line))) {
			fdisk_deassign_device(cxt, 1);
			g_cxt = NULL;
			fdisk_unref_context(cxt);
			return 0;
		}
		for (char *p = line; *p; p++) *p = (char)tolower((unsigned char)*p);
		if (strcmp(line, "gpt") != 0) {
			printf("Not creating a partition table — exiting.\n");
			fdisk_deassign_device(cxt, 1);
			g_cxt = NULL;
			fdisk_unref_context(cxt);
			return 0;
		}
		if (fdisk_create_disklabel(cxt, "gpt") != 0) {
			fprintf(stderr, "Failed to create a GPT label.\n");
			fdisk_deassign_device(cxt, 1);
			g_cxt = NULL;
			fdisk_unref_context(cxt);
			return 1;
		}
		printf("Staged a new GPT partition table — nothing is written to disk until [w]rite.\n\n");
	}

	/* Snapshot the partition table as it stood before this session's edits,
	 * so a later fdisk_reread_changes() has something to diff against. */
	struct fdisk_table *original_tb = NULL;
	if (fdisk_get_partitions(cxt, &original_tb) != 0)
		original_tb = NULL;

	printf("Tip: press Tab at any prompt to autocomplete a suggested value\n"
	       "  (e.g. segment boundaries, the maximum available length).\n\n");
	choose_display_unit(cxt, original_tb);

	for (;;) {
		char cmd[16];

		build_segments(cxt, argv[1]); /* reflects any staged, not-yet-written changes too */
		draw_bar(cxt);
		list_segments();

		if (n_staged > 0)
			list_staged_queue();

		clear_completions();
		add_completion("n"); add_completion("u"); add_completion("d"); add_completion("b"); add_completion("r"); add_completion("w"); add_completion("q");
		if (!read_line("[n]ew  [u]ndo  [d]elete  [b]ackup  [r]estore  [w]rite  [q]uit: ",
		               cmd, sizeof(cmd))) break;

		if (cmd[0] == '\0') continue;

		switch (cmd[0]) {
		case 'n': case 'N':
			create_partition_once(cxt, argv[1], original_tb);
			break;
		case 'u': case 'U':
			undo_last_staged(cxt, original_tb);
			break;
		case 'b': case 'B':
			backup_partition_table(cxt, argv[1], original_tb);
			break;
		case 'r': case 'R':
			restore_partition_table(cxt, original_tb);
			break;
		case 'd': case 'D':
			delete_partition(cxt, original_tb);
			break;
		case 'w': case 'W': {
			char confirm[16];
			clear_completions();
			add_completion("YES");
			if (!read_line("Write all changes to disk now? Type YES to confirm: ",
			               confirm, sizeof(confirm))) break;
			if (strcmp(confirm, "YES") == 0) {
				int write_rc = fdisk_write_disklabel(cxt);
				if (write_rc == 0) {
					printf("Partition table written.\n");
					format_staged_filesystems(cxt, argv[1], original_tb);

					/* Session carries on: the operations staged this
					 * round are now committed, so clear the queue
					 * (releasing any pinned GPT types held for a
					 * possible undo), and re-snapshot "before" as the
					 * table now stands on disk so a later write in
					 * this same session diffs against the right
					 * baseline. */
					clear_staged_queue();
					if (original_tb) {
						fdisk_unref_table(original_tb);
						original_tb = NULL;
					}
					if (fdisk_get_partitions(cxt, &original_tb) != 0)
						original_tb = NULL;
				} else {
					/* Same reasoning as the "Failed to add partition"
					 * message above: libfdisk returns a negative
					 * errno-style code rather than setting errno. */
					fprintf(stderr, "Write failed: %s\n",
					        strerror(write_rc < 0 ? -write_rc : write_rc));
				}
			} else {
				printf("Not written.\n");
			}
			printf("\n");
			break;
		}
		case 'q': case 'Q':
			printf("Quitting — any staged, unwritten changes are discarded.\n");
			clear_staged_queue();
			if (original_tb) fdisk_unref_table(original_tb);
			fdisk_deassign_device(cxt, 1);
			g_cxt = NULL;
			fdisk_unref_context(cxt);
			return 0;
		default:
			printf("Unrecognised option.\n\n");
		}
	}

	clear_staged_queue();
	if (original_tb) fdisk_unref_table(original_tb);
	fdisk_deassign_device(cxt, 1 /* no_write_on_deassign: leave as-is, we already wrote explicitly */);
	g_cxt = NULL;
	fdisk_unref_context(cxt);
	return 0;
}
