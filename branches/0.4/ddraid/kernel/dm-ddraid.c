#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/pagemap.h>
#include <linux/file.h>
#include <linux/syscalls.h> // recvmsg
#include <linux/socket.h>
#include <linux/un.h>
#include <net/sock.h>
#include <asm/uaccess.h>
#include <linux/bio.h>
#include "dm.h"
#include "dm-ddraid.h"

#define DM_MSG_PREFIX "ddraid"

#define warn(string, args...) do { printk("%s: " string "\n", __func__, ##args); } while (0)
#define error(string, args...) do { warn(string, ##args); BUG(); } while (0)
#define assert(expr) do { if (!(expr)) error("Assertion " #expr " failed!\n"); } while (0)
#define trace_on(args) args
#define trace_off(args)

#define trace trace_off
#define tracebio trace_off
#define DDRAID
#define NORAID 0
#define NOCALC 1
#define NOSYNC 1

/*
 * To do:
 *  - accept highwater updates
 *  - handle IO failures
 *  - download/upload region dirty list distributions (faster failover)
 *  - some sane approach to read balancing so user space can specify policy
 */

static int transfer(struct file *file, const void *buffer, unsigned int count,
	ssize_t (*op)(struct kiocb *, const char *, size_t, loff_t), int mode)
{
	struct kiocb iocb;
	mm_segment_t oldseg = get_fs();
	int err = 0;

	trace_off(warn("%s %i bytes", mode == FMODE_READ? "read": "write", count);)
	if (!(file->f_mode & mode))
		return -EBADF;
	if (!op)
		return -EINVAL;
	init_sync_kiocb(&iocb, file); // new in 2.5 (hmm)
	iocb.ki_pos = file->f_pos;
	set_fs(get_ds());
	while (count) {
		int chunk = (*op)(&iocb, buffer, count, iocb.ki_pos);
		if (chunk <= 0) {
			err = chunk? chunk: -EPIPE;
			break;
		}
		BUG_ON(chunk > count);
		count -= chunk;
		buffer += chunk;
	}
	set_fs(oldseg);
	file->f_pos = iocb.ki_pos;
	return err;
}

static inline int readpipe(struct file *file, void *buffer, unsigned int count)
{
	return transfer(file, buffer, count, (void *)file->f_op->aio_read, FMODE_READ);
}

static inline int writepipe(struct file *file, void *buffer, unsigned int count)
{
	return transfer(file, buffer, count, file->f_op->aio_write, FMODE_WRITE);
}

#define outbead(SOCK, CODE, STRUCT, VALUES...) ({ \
	struct { struct head head; STRUCT body; } PACKED message = \
		{ { CODE, sizeof(STRUCT) }, { VALUES } }; \
	writepipe(SOCK, &message, sizeof(message)); })

static int recv_fd(int sock, char *bogus, unsigned *len)
{
	char payload[CMSG_SPACE(sizeof(int))];
	struct msghdr msg = {
		.msg_control = payload,
		.msg_controllen = sizeof(payload),
		.msg_iov = &(struct iovec){ .iov_base = bogus, .iov_len = *len },
		.msg_iovlen = 1,
	};
	mm_segment_t oldseg = get_fs();
	struct cmsghdr *cmsg;
	int result;

	set_fs(get_ds());
	result = sys_recvmsg(sock, &msg, 0);
	set_fs(oldseg);

	if (result <= 0)
		return result;
	if (!(cmsg = CMSG_FIRSTHDR(&msg)))
		return -ENODATA;
	if (cmsg->cmsg_len != CMSG_LEN(sizeof(int)) ||
		cmsg->cmsg_level != SOL_SOCKET ||
		cmsg->cmsg_type != SCM_RIGHTS)
		return -EBADMSG;

	*len = result;
	return *((int *)CMSG_DATA(cmsg));
}

static void submit_bdev(struct bio *bio, struct block_device *bdev)
{
	bio->bi_bdev = bdev;
	generic_make_request(bio);
}

static inline long IS_ERRNO(const void *ptr)
{
	return unlikely(IS_ERR(ptr))? (unsigned long)ptr: 0;
}

#if 0
static void kick(struct block_device *dev)
{
	request_queue_t *q = bdev_get_queue(dev);
	if (q->unplug_fn)
		q->unplug_fn(q);
}
#endif

static void pagecopy(struct page *sp, unsigned os, struct page *dp, unsigned od, unsigned n)
{
	void *s = kmap_atomic(sp, KM_USER0);
	void *d = kmap_atomic(dp, KM_USER1);
	memcpy(d + od, s + os, n);
	kunmap_atomic(s, KM_USER0);
	kunmap_atomic(d, KM_USER1);
}

static inline void hexdump(void *data, unsigned length)
{
	while (length ) {
		int row = length < 16? length: 16;
		printk("%p: ", data);
		length -= row;
		while (row--)
			printk("%02hx ", *(unsigned char *)data++);
		printk("\n");
	}
}

/*
 * Bio stacking hack.
 *
 * A block device is essentially a stack of virtualization layers, where
 * each layer is a virtual device, or at the bottom of the stack, a real
 * device.  Each layer has a driver that receives the bio and either
 * relays it to the next layer or handles it in some other way, perhaps
 * by creating one or more new bios, submitting those and arranging to
 * signal completion of the original bio when all the "stacked" bios have
 * completed.  So we have two ways of passing a bio from one layer of the
 * device stack to another: relaying and stacking.  In the relay case, the
 * sector and/or device fields may be rewritten by the underlying driver,
 * and therefore the submitter may not rely on either fields after
 * submitting the bio.  Consequently, if the underlying driver does not
 * relay the bio but services it by other means, such as stacking, the
 * underlying driver owns these two fields until it signals completion.
 * This is convenient, since a stacking driver needs some way to find the
 * original bio when the underlying bios complete, and may need other
 * working storage as well.
 *
 * To provide some semblance of type safety, I provide inline wrappers to
 * alias the two fields as an atomic count and a void * pointer respectively.
 * This assumes that an atomic count will always fit in the bdev field
 * (hashed locking was adopted for atomic fields for one architecture that
 * lacked a native atomic type) and that a pointer will always fit into the
 * sector field.  The driver must take care not to set these aliased fields
 * before it has retrieved the original contents.
 *
 * My original approach to stacking a bio was to hook the private field and
 * restore it on completion, making it unavailable to the true owner while
 * the bio is in flight.  This seemed a little risky.
 */

static inline atomic_t *bio_hackcount(struct bio *bio)
{
	return (atomic_t *)&bio->bi_bdev;
}

static inline int *bio_hacklong(struct bio *bio)
{
	return (int *)&bio->bi_bdev;
}

static inline void **bio_hackhook(struct bio *bio)
{
	return (void **)&bio->bi_sector;
}

typedef u64 chunk_t;

#define SECTOR_SHIFT 9
#define FINISH_FLAG 4
#define HASH_BUCKETS 64
#define MASK_BUCKETS (HASH_BUCKETS - 1)
#define MAX_MEMBERS 10

#ifdef DDRAID
#  define is_ddraid 1
#else
#  define is_ddraid 0
#endif

struct devinfo {
	unsigned flags;
	unsigned region_size_bits;
#ifdef DDRAID
	int blocksize_bits, fragsize_bits;
#endif
	struct dm_dev *member[MAX_MEMBERS];
	unsigned members;
	struct file *sock;
	struct file *control_socket;
	struct semaphore server_in_sem;
	struct semaphore server_out_sem;
	struct semaphore more_work_sem;
	struct semaphore destroy_sem;
	struct semaphore exit1_sem;
	struct semaphore exit2_sem;
	struct semaphore exit3_sem;
	struct list_head hash[HASH_BUCKETS];
	struct list_head requests;
	struct list_head releases;
	struct list_head bogus;
	struct region *spare_region;
	spinlock_t region_lock;
	spinlock_t endio_lock;
	atomic_t destroy_hold;
	region_t highwater;
	unsigned balance_acc, balance_num, balance_den, balance;
	int dead;
};

static inline int running(struct devinfo *info)
{
	return !(info->flags & FINISH_FLAG);
}

static inline int frags_per_block_bits(struct devinfo *info)
{
	return info->blocksize_bits - info->fragsize_bits;
}

static inline int blocksize(struct devinfo *info)
{
	return 1 << info->blocksize_bits;
}

/*
 * SMP Locking notes:
 *
 * endio_lock protects:
 *   - only the retire queue
 *
 * region_lock protects:
 *   - region hash list
 *   - region desync and drain bits
 *   - incrementing region count
 *
 * Decrementing region->count is not protected by region_lock so that region_lock
 * does not have to disable irqs.  This is safe because only the zero state is
 * meaningful outside interrupt context, and once zero is reached there will be
 * no more racy decrements.
 *
 * These locks are never nested.
 */

/* Region hash records both dirty and desynced regions */
struct region {
	atomic_t count;
	unsigned flags;
	region_t regnum;
	struct list_head hash;
	struct list_head wait;
};

/* Gizmo union eliminates a few nasty allocations */
struct defer { struct list_head list; struct bio *bio; };
struct query { struct list_head list; region_t regnum; };

struct hook {
	sector_t sector; // debug trace
	unsigned length; // debug trace
	struct devinfo *info;
	struct region *region;
	struct bio *parity; };

struct retire {
	struct list_head list;
	struct devinfo *info;
	struct region *region;
	struct timer_list *timer; };

union gizmo {
	struct defer defer;
	struct query query;
	struct retire retire;
	struct hook hook; };

static kmem_cache_t *gizmo_cache;

static void *alloc_gizmo(void)
{
	return kmem_cache_alloc(gizmo_cache, GFP_NOIO|__GFP_NOFAIL);
}

#ifdef DDRAID
typedef unsigned long long xor_t;
#define S4K2 (4096 / (2*sizeof(xor_t)))
#define S4K4 (4096 / (4*sizeof(xor_t)))
#define S4K8 (4096 / (8*sizeof(xor_t)))
#define S4K16 (4096 / (16*sizeof(xor_t)))

static void compute_parity(struct devinfo *info, xor_t *v, xor_t *p)
{
	int fragsize = 1 << info->fragsize_bits;
	int frags = info->members - 1;
	int stride = fragsize / sizeof(xor_t);
	xor_t *limit = p + stride;
#if 1 /* doesn't seem to help much */
	switch (blocksize(info) == 4096? frags: 0) {
#if 0
	case 1:
		warn(">>>optimize for mirror");
		memcpy(p, v, fragsize);
#endif
	case 2:
		for (; p < limit; p += 4, v += 4) {
			*(p + 0) = *(v + 0) ^ *(v + 0 + S4K2);
			*(p + 1) = *(v + 1) ^ *(v + 1 + S4K2);
			*(p + 2) = *(v + 2) ^ *(v + 2 + S4K2);
			*(p + 3) = *(v + 3) ^ *(v + 3 + S4K2);
		}
		return;
	case 4:
		for (; p < limit; v++)
			*p++ =	*(v + 0*S4K4) ^ *(v + 1*S4K4) ^ *(v + 2*S4K4) ^ *(v + 3*S4K4);
		return;
	case 8:
		for (; p < limit; v++)
			*p++ =	*(v + 0*S4K8) ^ *(v + 1*S4K8) ^ *(v + 2*S4K8) ^ *(v + 3*S4K8) ^
				*(v + 4*S4K8) ^ *(v + 5*S4K8) ^ *(v + 6*S4K8) ^ *(v + 7*S4K8);
		return;
	case 16:
		for (; p < limit; v++)
			*p++ =	*(v + 0*S4K16) ^ *(v + 1*S4K16) ^ *(v + 2*S4K16) ^ *(v + 3*S4K16) ^
				*(v + 4*S4K16) ^ *(v + 5*S4K16) ^ *(v + 6*S4K16) ^ *(v + 7*S4K16) ^
				*(v + 8*S4K16) ^ *(v + 9*S4K16) ^ *(v + 10*S4K16) ^ *(v + 11*S4K16) ^
				*(v + 12*S4K16) ^ *(v + 13*S4K16) ^ *(v + 14*S4K16) ^ *(v + 15*S4K16);
		return;
	}
#endif
	while (p < limit) {
		int n = frags - 1;
		xor_t x = *v, *q = v;
	
		while (n--)
			x ^= *(q += stride);
		*p++ = x;
		v++;
	}
}

static int verify_parity(struct devinfo *info, xor_t *v, xor_t *p)
{
	unsigned frags = info->members - 1;
	unsigned stride = (1 << info->fragsize_bits) / sizeof(xor_t);
	xor_t *limit = p + stride;

	while (p < limit) {
		int n = frags - 1;
		xor_t x = *v, *q = v;

		while (n--)
			x ^= *(q += stride);
		if (*p++ ^ x)
			return -1;
		v++;
	}
	return 0;
}
#endif

/*
 * Life cycle of a raid write request:
 *
 * A write request arrives in _map, then if it can't be handled immediately,
 * it goes to the work daemon, hooked onto a struct region by a struct defer,
 * which emits the write request message.  The incoming daemon receives the
 * response, finds the region with the defer list in the hash, and submits
 * any defered bio requests.  The bio completion has to be hooked in order to
 * keep track of writes in progress, by linking a struct hook into the bio's
 * private field to store the old completion and private fields so they can
 * be restored after our own completion handler runs.  The completion
 * handler runs in interrupt context, so when the final active write on a
 * region completes, this has to be communicated to a daemon that can send
 * the release message by linking a struct retire onto the raid releases
 * list.  The work daemon picks up the retires, checks the region status
 * under a lock to be sure no new io came along in the meantime, and if
 * not, emits the release message and removes the region from the hash,
 * unless it's an unsynced region below the sync highwater mark, in which
 * case it stays, so that readers can find out about unsynced regions by
 * looking in the region hash.
 */

#define DESYNC_FLAG 1
#define DRAIN_FLAG 2
#define PAUSE_FLAG 4

static inline unsigned hash_region(region_t value)
{
	return value & MASK_BUCKETS;
}

static inline void get_region(struct region *region)
{
	atomic_inc(&region->count);
}

static inline int put_region_test_zero(struct region *region)
{
	return atomic_dec_and_test(&region->count);
}

static inline int region_count(struct region *region)
{
	return atomic_read(&region->count);
}

static inline void set_region_count(struct region *region, int value)
{
	atomic_set(&region->count, value);
}

static inline int is_desynced(struct region *region)
{
	return region->flags & DESYNC_FLAG;
}

static inline int drain_region(struct region *region)
{
	return (region->flags & DRAIN_FLAG);
}

static inline void _show_regions(struct devinfo *info)
{
	unsigned i, regions = 0, defered = 0;

	spin_lock(&info->region_lock);
	for (i = 0; i < HASH_BUCKETS; i++) {
		struct list_head *list;
		list_for_each(list, info->hash + i) {
			struct region *region = list_entry(list, struct region, hash);
			struct list_head *wait;
			printk(is_desynced(region)? "~": "");
			printk("%Lx/%i ", (long long)region->regnum, region_count(region));
			list_for_each(wait, &region->wait) {
				struct defer *defer = list_entry(wait, struct defer, list);
				printk("<%Lx> ", (long long)(defer->bio? defer->bio->bi_sector: -1));
				defered++;
			}
			regions++;
		}
	}
	printk("(%u/%u)\n", regions, defered);
	spin_unlock(&info->region_lock);
}

#define show_regions(info) do { warn("regions:"); _show_regions(info); } while (0)

static struct region *find_region(struct devinfo *info, region_t regnum)
{
	struct list_head *list, *bucket = info->hash + hash_region(regnum);
	struct region *region;

	list_for_each(list, bucket)
		if ((region = list_entry(list, struct region, hash))->regnum == regnum)
			goto found;
	trace(warn("No cached region %Lx", (long long)regnum);)
	return NULL;
found:
	trace(warn("Found region %Lx", (long long)regnum);)
	return region;
}

static void insert_region(struct devinfo *info, struct region *region)
{
	INIT_LIST_HEAD(&region->wait);
	list_add(&region->hash, info->hash + hash_region(region->regnum));
}

static kmem_cache_t *region_cache;

static struct region *alloc_region(void)
{
	return kmem_cache_alloc(region_cache, GFP_NOIO|__GFP_NOFAIL);
}

static void free_region_unlock(struct devinfo *info, struct region *region)
{
	list_del(&region->hash);
	spin_unlock(&info->region_lock);
	kmem_cache_free(region_cache, region);
}

static void queue_request_lock(struct devinfo *info, region_t regnum)
{
	struct query *query = alloc_gizmo();
	*query = (struct query){ .regnum = regnum };
	spin_lock(&info->region_lock);
	list_add_tail(&query->list, &info->requests);
	up(&info->more_work_sem);
}

static void queue_request(struct devinfo *info, region_t regnum)
{
	queue_request_lock(info, regnum);
	spin_unlock(&info->region_lock);
}

static void send_release(struct devinfo *info, region_t regnum)
{
	down(&info->server_out_sem);
	outbead(info->sock, RELEASE_WRITE, struct region_message, .regnum = regnum);
	up(&info->server_out_sem);
}

static void release_region_unlock(struct devinfo *info, struct region *region)
{
	region_t regnum = region->regnum;
	trace(warn("release region %Lx", (long long)regnum);)

	if (!list_empty(&region->wait)) {
		if (!drain_region(region))
			warn("requests leaked!");
		region->flags &= ~DRAIN_FLAG;
		atomic_set(&region->count, -1);
		spin_unlock(&info->region_lock);
		send_release(info, regnum);
		queue_request(info, region->regnum);
		return;
	}

	/* keep desynced regions for reader cache */
	if (is_desynced(region) && region->regnum < info->highwater) {
		atomic_set(&region->count, -2);
		spin_unlock(&info->region_lock);
		return;
	}

	free_region_unlock(info, region);
	send_release(info, regnum);
}

static inline char *strio(int is_read)
{
	return is_read? "read": "write";
}

/* interrupt context */

static void queue_release(struct retire *retire)
{
	struct devinfo *info = retire->info;
	trace(warn("queue region %Lx for release", (long long)retire->region->regnum);)
	spin_lock(&info->endio_lock);
	list_add_tail(&retire->list, &info->releases);
	spin_unlock(&info->endio_lock);
	up(&info->more_work_sem);
}

static void free_bio_pages(struct bio *bio, int stride)
{
	int vec;
	for (vec = 0; vec < bio->bi_vcnt; vec += stride)
		__free_page(bio->bi_io_vec[vec].bv_page);
}

/*
 * Delayed release.
 *
 * When there are no more in-flight writes to a given region, we release
 * the region so that the server can mark it clean in the persistent dirty
 * log.  However, if we do this immediately then back-to-back writes will
 * suffer horribly.  So we need to delay the release a little.  A timer
 * struct is allocated and freed each time a region looks like it may be
 * released, and the actual decision to release is made later in the worker
 * thread.  So there tends to be an annoying extra allocate/release on every
 * back to back write.  This can probably be changed to a single timer
 * embedded in the region struct, since only one delayed release can be in
 * flight per region.  Probably.
 */
static void timer_release(unsigned long data)
{
	queue_release((struct retire *)data);
}

static int clone_endio(struct bio *bio, unsigned int done, int error)
{
	struct bio *parent = bio->bi_private;
	tracebio(warn("%p, parent count = %i", bio, atomic_read(bio_hackcount(parent)));)
	if (atomic_dec_and_test(bio_hackcount(parent))) {
		struct hook *hook = *bio_hackhook(parent);
		if (hook) {
			struct devinfo *info = hook->info;
			struct bio *parity = hook->parity;
			if (parity) {
				tracebio(warn("free parity");)
				free_bio_pages(parity, 1 << frags_per_block_bits(info));
				bio_put(parity);
			}
			tracebio(warn("free hook");)
			kmem_cache_free(gizmo_cache, hook);
		}
		tracebio(warn("release parent");)
		bio_endio(parent, parent->bi_size, error);
	}
	bio_put(bio);
	return 0;
}

static int bounce_read_endio(struct bio *bounce, unsigned int done, int error)
{
	struct bio *parent = bounce->bi_private;
	void *bp = bounce->bi_io_vec[0].bv_page;
	void *pp = parent->bi_io_vec[0].bv_page;
	unsigned o = *bio_hacklong(parent);

	tracebio(warn("copy to bounce %p+%x", bounce, o);) 
	pagecopy(bp, o, pp, parent->bi_io_vec[0].bv_offset, parent->bi_size); // !!! what about error
	flush_dcache_page(pp);
	__free_page(bp);
	bio_endio(parent, parent->bi_size, error);
	bio_put(bounce);
	return 0;
}

static int clone_write_endio(struct bio *bio, unsigned int done, int error)
{
	struct bio *parent = bio->bi_private;

	tracebio(warn("%p, parent count = %i", bio, atomic_read(bio_hackcount(parent)));)
	if (atomic_dec_and_test(bio_hackcount(parent))) {
		struct hook *hook = *bio_hackhook(parent);
		struct devinfo *info = hook->info;
		struct region *region = hook->region;
		struct bio *parity = hook->parity;

		trace(warn("parent end io");)
		if (put_region_test_zero(region)) {
			*(struct retire *)hook = (struct retire){ .info = info, .region = region };
			if (1) {
				struct timer_list *timer = kmalloc(sizeof(struct timer_list), GFP_ATOMIC);
				get_region(region);
				trace(warn("delay region %Lx release, count = %i", (long long)region->regnum, region_count(region));)
				init_timer(timer);
				timer->function = timer_release;
				timer->expires = jiffies + HZ;
				timer->data = (unsigned long)hook;
				((struct retire *)hook)->timer = timer;
				add_timer(timer);
				if (atomic_add_return(1, &info->destroy_hold) == 1)
					down(&info->destroy_sem);
			} else
				queue_release((struct retire *)hook);
		} else
			kmem_cache_free(gizmo_cache, hook);
		bio_endio(parent, parent->bi_size, error); /* after destroy_hold inc */

		if (parity) {
			tracebio(warn("put bio, count = %i", atomic_read(&parity->bi_cnt));)
			free_bio_pages(parity, 1 << frags_per_block_bits(info));
			bio_put(parity);
		}
	}
	tracebio(warn("put bio, count = %i", atomic_read(&bio->bi_cnt));)
	bio_put(bio);
	return 0;
}

/*
 * Reconstruction: Let's do a nasty trick.  Copy the parity to the
 * missing fragment, then compute_parity with the same fragment as
 * destination, overwriting the parity with the reconstructed data.
 */
static int clone_read_endio(struct bio *bio, unsigned int done, int error)
{
	struct bio *parent = bio->bi_private;

	tracebio(warn("%p, parent count = %i", bio, atomic_read(bio_hackcount(parent)));)
	if (atomic_dec_and_test(bio_hackcount(parent))) {
		struct hook *hook = *bio_hackhook(parent);
		struct bio *parity = hook->parity;
		trace(warn("parent end io");)

		if (parity) {
			struct devinfo *info = hook->info;

			if (!NOCALC) {
				int vec;
				for (vec = 0; vec < bio->bi_vcnt; vec++) {
					struct page *spage = parent->bi_io_vec[vec].bv_page;
					struct page *ppage = parity->bi_io_vec[vec].bv_page;
					void *s = kmap_atomic(spage, KM_USER0);
					void *p = kmap_atomic(ppage, KM_USER1);
					int mask = ~PAGE_CACHE_MASK;
					int offset = (vec << info->fragsize_bits) & mask;
					int dead = info->dead;

					if (dead >= 0) {
						void *d = s + (dead << info->fragsize_bits);
						memcpy(d, p + offset, 1 << info->fragsize_bits);
						compute_parity(info, s, d);
						flush_dcache_page(ppage);
					} else {
						if (verify_parity(info, s, p + offset))
							warn("Parity check failed, bio=%Lx/%x", (long long)hook->sector, hook->length);
					}
					kunmap_atomic(s, KM_USER0);
					kunmap_atomic(p, KM_USER1);
				}
			}
			free_bio_pages(parity, 1 << frags_per_block_bits(info));
			trace_off(warn("put parity bio, count = %i", atomic_read(&parity->bi_cnt));)
			bio_put(parity);
		}
		bio_endio(parent, parent->bi_size, error);
		kmem_cache_free(gizmo_cache, hook);
	}
	trace_off(warn("put bio, count = %i", atomic_read(&bio->bi_cnt));)
	bio_put(bio);
	return 0;
}

/*
 * Degraded mode:
 * Lost parity disk: don't submit/check parity bio
 * Lost data disk, write: don't submit bio for missing disk
 * Lost data disk, read: reconstruct missing frag as xor of others
 */
static int submit_rw(struct devinfo *info, struct bio *bio, int synced, struct hook *hook, bio_end_io_t endio)
{
#ifdef DDRAID
	int vec, vecs = bio->bi_vcnt;
	int disk, disks = info->members, dead = info->dead;
	int is_read = bio_data_dir(bio) == READ;
	int need_hook = 1; // !!! don't need hook if parity dead
	int fragsize = 1 << info->fragsize_bits;
	int mask = ~PAGE_CACHE_MASK; // !!! assume blocksize = pagesize for now
	int err = 0;
	sector_t sector = bio->bi_sector; // hackhook trashes bi_sector

	tracebio(warn("submit %i clones, size = %x, vecs = %i", disks, fragsize, vecs);)
	atomic_set(bio_hackcount(bio), disks - (dead >= 0));

	if (need_hook) {
		if (!hook) {
			hook = alloc_gizmo();
			*hook = (struct hook){ .info = info };
		}
		hook->sector = sector; // debug only
		hook->length = bio->bi_size; // debug only
		*bio_hackhook(bio) = hook;
	}

	for (disk = 0; disk < disks; disk++) {
		int is_parity = (disk == disks - 1);
		struct page *parity_page = NULL;
		struct bio *clone;

		if (disk == dead)
			continue;

		clone = bio_alloc(GFP_NOIO, vecs);
		clone->bi_rw = bio->bi_rw;
		clone->bi_bdev = (info->member[disk])->bdev;
		clone->bi_sector = sector >> frags_per_block_bits(info);
		clone->bi_vcnt = vecs;
		clone->bi_size = vecs << info->fragsize_bits;
		clone->bi_private = bio;
		clone->bi_end_io = endio;

		if (is_parity) {
			hook->parity = clone;
			bio_get(clone);
		}

		for (vec = 0; vec < vecs; vec++) {
			struct page *spage = bio->bi_io_vec[vec].bv_page;
			unsigned offset;

			if (!is_parity) {
				clone->bi_io_vec[vec] = (struct bio_vec){
					.bv_page = spage,
					.bv_offset = disk << info->fragsize_bits,
					.bv_len = fragsize };
				continue;
			}

			if (!(offset = (vec << info->fragsize_bits) & mask))
				parity_page = alloc_page(GFP_NOIO);

			clone->bi_io_vec[vec] = (struct bio_vec){
				.bv_page = parity_page,
				.bv_offset = offset,
				.bv_len = fragsize };

			if (!NOCALC && !is_read) {
				// should do this only once per page
				void *s = kmap_atomic(spage, KM_USER0);
				void *p = kmap_atomic(parity_page, KM_USER1);
				compute_parity(info, s, p + offset);
				flush_dcache_page(parity_page);
				kunmap_atomic(s, KM_USER0);
				kunmap_atomic(p, KM_USER1);
			}
		}
		trace_off(warn("clone %i, size = %x, vecs = %i", disk, clone->bi_size, clone->bi_vcnt);)
		generic_make_request(clone);
	}
#else
	if (!synced) {
		trace(warn("submit degraded write"));
		submit_bdev(bio, info->member[0]->bdev);
		return;
	}

	atomic_set(bio_hackcount(bio), disks);

	for (i = 0; i < m; i++) {
		struct bio *clone = bio_clone(bio, GFP_NOIO);
		clone->bi_private = bio;
		clone->bi_end_io = clone_endio;
		submit_bdev(bio, (info->member[i])->bdev);
	}
#endif
	return err;
}

static void submit_write(struct devinfo *info, struct bio *bio, struct region *region, struct hook *hook)
{
	*hook = (struct hook){ .info = info, .region = region };
	submit_rw(info, bio,
1 ||
!is_desynced(region), hook, clone_write_endio);
}

/* Drops and retakes region lock */
static void restore_spare_region(struct devinfo *info)
{
	struct region *region;
	spin_unlock(&info->region_lock);
	trace(warn(""));
	region = alloc_region();
	spin_lock(&info->region_lock);
	if (info->spare_region)
		kmem_cache_free(region_cache, region);
	else
		info->spare_region = region;
}

static int ddraid_map(struct dm_target *target, struct bio *bio)
{
	struct devinfo *info = target->private;
	unsigned sectors_per_block = info->blocksize_bits - SECTOR_SHIFT;
	unsigned secmask = ((1 << sectors_per_block) - 1);
	unsigned blockmask = blocksize(info) - 1;
	unsigned sector = bio->bi_sector, is_read = bio_data_dir(bio) == READ;
	unsigned size = bio->bi_size;
	region_t regnum = sector >> (info->region_size_bits - SECTOR_SHIFT);
	struct region *region;
	struct defer *defer;

	trace(warn("%s %Lx/%x, region %Lx", strio(is_read), (long long)sector, size, (long long)regnum);)
	assert(size <= 1 << info->region_size_bits);

	if (NORAID) {
		submit_bdev(bio, info->member[0]->bdev);
		return 0;
	}

	if ((sector & secmask) || (size & blockmask)) {
		unsigned o = (sector << SECTOR_SHIFT) & blockmask;
		struct bio_vec *bvec = bio->bi_io_vec;
		struct bio *bounce;
		struct page *pp;
		if (((sector & secmask) + (size >> SECTOR_SHIFT)) > 1 << sectors_per_block || !is_read) {
			warn("Long odd block %s failed", strio(is_read));
			return -EIO;
		}
		warn("%s odd block, %Lx/%x", strio(is_read), (long long)sector, size);
		pp = alloc_page(GFP_NOIO);
		bounce = bio_alloc(GFP_NOIO, 1);
		bounce->bi_rw = bio->bi_rw;
		bounce->bi_sector = sector & ~secmask;
		bounce->bi_size = blocksize(info);
		bounce->bi_vcnt = 1;
		bounce->bi_io_vec[0] = (struct bio_vec){ .bv_page = pp, .bv_len = PAGE_CACHE_SIZE }; // !!! PAGE_SIZE
		bounce->bi_private = bio;
		bounce->bi_end_io = bounce_read_endio;
		*bio_hacklong(bio) = o;
		if (!is_read) {
			pagecopy(bvec->bv_page, bvec->bv_offset, pp, o, size);
			flush_dcache_page(pp);
		}
		return submit_rw(info, bounce, 1, NULL, clone_read_endio);
	}

	if (NOSYNC) {
if (is_read) {
	if ((info->balance_acc += size) >= info->balance_den) {
		info->balance_acc -= info->balance_den;
		if (++info->balance == info->members)
			info->balance = 0;
	}
	if (info->members == 2) {
		submit_bdev(bio, info->member[info->balance]->bdev);
		return 0;
	}
}
		submit_rw(info, bio, 1, NULL, is_read? clone_read_endio: clone_endio);
		return 0;
	}

	if (is_read) {
		int synced = 0;

		if (regnum < info->highwater) {
			spin_lock(&info->region_lock);
			region = find_region(info, regnum);
			synced = !region || !is_desynced(region);
			spin_unlock(&info->region_lock);
		}

		if ((info->balance_acc += size) >= info->balance_den) {
			info->balance_acc -= info->balance_den;
			if (++info->balance == info->members)
				info->balance = 0;
		}
#ifdef DDRAID
		if (info->members == 2) {
			submit_bdev(bio, info->member[info->balance]->bdev);
			return 0;
		}
		submit_rw(info, bio, 1, NULL, clone_read_endio);
#else
		submit_bdev(bio, info->member[synced? info->balance: 0]->bdev);
#endif
		return 0;
	}

	defer = alloc_gizmo();
	*defer = (struct defer){ .bio = bio };

	/*
	 * This would all be a lot easier if we didn't have to worry about
	 * holding the region lock over all the changes to the region hash
	 * while trying to allocate new structs.
	 *
	 * The easy way is to allocate a region before taking the spinlock and
	 * give it back if we find one is already there, but for most writes
	 * this is just extra work, so instead we keep a spare region around,
	 * and restore it later if it gets used.  Versus a mempool, this
	 * strategy spends much less time under the spinlock.
	 */
	spin_lock(&info->region_lock);
try_again:
	if (!(region = find_region(info, regnum))) {
		if (!info->spare_region) {
			restore_spare_region(info);
			goto try_again;
		}
		region = info->spare_region;
		info->spare_region = NULL;
		*region = (struct region){ .regnum = regnum };
		insert_region(info, region);
		goto queue_query;
	}

	/* Already have write grant?  Region will stay synced or unsynced */
	if (region_count(region) >= 0 && !drain_region(region)) {
		trace(warn("rewrite region %Lx, count = %i", (long long)region->regnum, region_count(region));)
		get_region(region);
		spin_unlock(&info->region_lock);
		submit_write(info, bio, region, (struct hook *)defer);
		return 0;
	}

	if (region_count(region) == -2) {
queue_query:	set_region_count(region, -1); /* now we own it */
		spin_unlock(&info->region_lock);
		queue_request_lock(info, region->regnum);
	}

	list_add_tail(&defer->list, &region->wait);
	if (!info->spare_region)
		restore_spare_region(info);
	spin_unlock(&info->region_lock);
	trace(show_regions(info);)
	return 0;
}

/*
 * This next bit is bogus because dm already knows how to defer requests but is too
 * messed up to allow a target to start in that state.  This goes away when dm gets
 * a good dunging-out.
 */
static int ddraid_map_bogus(struct dm_target *target, struct bio *bio, union map_info *context)
{
	struct devinfo *info = target->private;
	if (info->region_size_bits == -1) {
		struct defer *defer = alloc_gizmo();

		spin_lock(&info->region_lock);
		if (info->region_size_bits != -1) {
			spin_unlock(&info->region_lock);
			kmem_cache_free(gizmo_cache, defer);
			goto map;
		}

		*defer = (struct defer){ .bio = bio };
		list_add_tail(&defer->list, &info->bogus);
		spin_unlock(&info->region_lock);
		return 0;
	}
map:
	return ddraid_map(target, bio);
}

static void send_next_request_locked(struct devinfo *info)
{
	struct list_head *entry = info->requests.next;
	struct query *query = list_entry(entry, struct query, list);

	list_del(entry);
	spin_unlock(&info->region_lock);
	down(&info->server_out_sem);
	outbead(info->sock, REQUEST_WRITE, struct region_message, .regnum = query->regnum);
	up(&info->server_out_sem);
	kmem_cache_free(gizmo_cache, query);
	spin_lock(&info->region_lock);
}

static int worker(struct dm_target *target)
{
	struct devinfo *info = target->private;

	daemonize("ddraid-worker");
	down(&info->exit1_sem);
	while (running(info)) {
		unsigned long irqsave;
		down(&info->more_work_sem);

		/* Send write request messages */
		spin_lock(&info->region_lock);
		while (!list_empty(&info->requests) && !(info->flags & (FINISH_FLAG|PAUSE_FLAG)))
			send_next_request_locked(info);
		spin_unlock(&info->region_lock);

		/* Send write release messages */
		spin_lock_irqsave(&info->endio_lock, irqsave);
		while (!list_empty(&info->releases) && running(info)) {
			struct list_head *entry = info->releases.next;
			struct retire *retire = list_entry(entry, struct retire, list);
			struct region *region = retire->region;

			list_del(entry);
			spin_unlock_irqrestore(&info->endio_lock, irqsave);
			if (retire->timer)
				kfree(retire->timer); // !!! make it a cache
			kmem_cache_free(gizmo_cache, retire);
			spin_lock(&info->region_lock);
			trace(warn("release region %Lx, count = %i", (long long)region->regnum, region_count(region));)
			if (!put_region_test_zero(region)) {
				/* More submitted before we got here */
				spin_unlock(&info->region_lock);
				spin_lock_irqsave(&info->endio_lock, irqsave);
				continue;
			}
			release_region_unlock(info, region);
			if (atomic_dec_and_test(&info->destroy_hold))
				up(&info->destroy_sem);
			spin_lock_irqsave(&info->endio_lock, irqsave);
		}
		spin_unlock_irqrestore(&info->endio_lock, irqsave);

		trace(show_regions(info);)
		trace(warn("Yowza! More work?");)
	}
	up(&info->exit1_sem); /* !!! crashes if module unloaded before ret executes */
	warn("%s exiting", current->comm);
	return 0;
}

static void do_defered(struct devinfo *info, struct region_message *message, int synced)
{
	region_t regnum = message->regnum;
	struct region *region;

	trace(warn("submit queued writes for region %Lx", (long long)regnum));
	spin_lock(&info->region_lock);
	region = find_region(info, regnum);
	if (!synced && !is_desynced(region) && region->regnum < info->highwater)
		warn("Desynced region not in cache!");

	/*
	 * Submitting a request necessarily drops the region spinlock and
	 * the request just submitted could complete before we get the lock
	 * again, for example, if the submit bails on an error.  To prevent
	 * the region from disappearing, take an extra count and also handle
	 * the possibility that it may need to be released here.
	 */
	set_region_count(region, 1); /* extra count */
	if (is_desynced(region) == synced)
		region->flags ^= DESYNC_FLAG;

	while (!list_empty(&region->wait)) {
		struct list_head *entry = region->wait.next;
		struct defer *defer = list_entry(entry, struct defer, list);
		trace(warn("bio sector %Lx", (long long)defer->bio->bi_sector));
		list_del(entry);
		get_region(region);
		spin_unlock(&info->region_lock);
		submit_write(info, defer->bio, region, (struct hook *)defer);
		trace(show_regions(info);)
		spin_lock(&info->region_lock);
	}
	if (put_region_test_zero(region)) /* drop extra count */
		release_region_unlock(info, region);
}

static int incoming(struct dm_target *target)
{
	struct devinfo *info = target->private;
	struct messagebuf message; // !!! have a buffer in the target->info
	struct file *sock;
	int err, length;

	daemonize("ddraid-client");
	down(&info->exit2_sem);
connect:
	trace(warn("Request socket connection");)
	outbead(info->control_socket, NEED_SERVER, struct { });
	trace(warn("Wait for socket connection");)
	down(&info->server_in_sem);
	trace(warn("got socket %p", info->sock);)
	sock = info->sock;

	while (running(info)) { // stop on module exit
		trace(warn("wait message");)
		if ((err = readpipe(sock, &message.head, sizeof(message.head))))
			goto socket_error;
		length = message.head.length;
		if (length > maxbody)
			goto message_too_long;
		trace(warn("%x/%u", message.head.code, length);)
		if ((err = readpipe(sock, &message.body, length)))
			goto socket_error;
	
		switch (message.head.code) {
		case REPLY_IDENTIFY:
		{
			struct reply_identify *body = (struct reply_identify *)&message.body;
			trace(warn("identify succeeded, region bits = %i", body->region_bits);)
			spin_lock(&info->region_lock);
			info->region_size_bits = body->region_bits;
//			target->split_io = 1 << info->region_size_bits;
			while (!list_empty(&info->bogus)) {
				struct list_head *entry = info->bogus.next;
				struct defer *defer = list_entry(entry, struct defer, list);
				list_del(entry);
				spin_unlock(&info->region_lock);
				ddraid_map(target, defer->bio);
				kmem_cache_free(gizmo_cache, defer);
				spin_lock(&info->region_lock);
			}
			spin_unlock(&info->region_lock);

			up(&info->server_out_sem);
			outbead(info->control_socket, REPLY_CONNECT_SERVER, struct { });
			continue;
		}

		case GRANT_SYNCED:
			trace(warn("granted synced");)
			do_defered(info, (void *)&message.body, 1);
			break;
			
		case GRANT_UNSYNCED:
			trace(warn("granted unsynced");)
			do_defered(info, (void *)&message.body, 0);
			break;

		// On failover, the new server may have found some new unsynced regions
		// (because a client failed to reconnect) or it might have synced some
		// regions before we reconnected (assuming it was able to get hold of a
		// definitive list of which clients held those regions dirty) and we
		// missed the desync delete broadcast.
		
		// If we hold a write grant for a desynced region, the server can't
		// have synced it (because it hasn't seen us yet, a former writer).
		// So we can go ahead and keep writing to it.  If the server sends
		// us a new desync for the region then it's confused and we need to
		// warn.
		
		// If we hold a write grant for a synced region, it's ok to do synced
		// IO even if the region is now unsynced, because the server must not
		// resync the region before all writers go away, so there is no chance
		// for our multi-disk IO to interleave with the server's resync IO.
		// So the region state may suddenly change from synced to desynced,
		// which is fine: further writes will be submitted degraded, and the
		// synced writes in progress won't do any harm.
		
		// As usual, the server always gives us up to date state for any write
		// request.  We upload our list of dirty regions so the server knows
		// we can still write to them and isn't surprised when it sees release
		// messages for those regions.

		// As usual, degraded reads are always safe, just possibly suboptimal.
		// So we only have to worry about balanced reads.  If a client died
		// while writing a synced region, it's up to the cluster filesystem to
		// ensure it disregards reads from those blocks.  But the server must
		// resync the region at some point, so we do need to have some way to
		// drain any balanced reads in the pipeline.  Damn, it means balanced
		// reads have to be hooked, and hooks have to be alloced.  At least
		// reads can still be handled entirely in the submitter's context.

		case ADD_UNSYNCED:
		{
			region_t regnum = ((struct region_message *)&message.body)->regnum;
			struct region *region;

			trace(warn("add unsynced region %Lx", (long long)regnum));
			spin_lock(&info->region_lock);
try_again:		if (!(region = find_region(info, regnum))) {
				if (!info->spare_region) {
					restore_spare_region(info);
					goto try_again;
				}
				region = info->spare_region;
				info->spare_region = NULL;
				*region = (struct region){ .flags = DESYNC_FLAG, .count = { -2 }, .regnum = regnum };
				insert_region(info, region);
			} else
				region->flags |= DESYNC_FLAG;
			spin_unlock(&info->region_lock);
			break;
		}

		case DEL_UNSYNCED:
		{
			region_t regnum = ((struct region_message *)&message.body)->regnum;
			struct region *region;

			trace(warn("del unsynced region %Lx", (long long)regnum));
			spin_lock(&info->region_lock);
			if (!(region = find_region(info, regnum)))
				warn("Deleted uncached unsynced region %Lx", (long long)regnum);
			else {
				region->flags &= ~DESYNC_FLAG;
				if (region_count(region) == -2) {
					free_region_unlock(info, region);
					break;
				}
			}
			spin_unlock(&info->region_lock);
			break;
		}

		case SET_HIGHWATER:
			info->highwater = ((struct region_message *)&message.body)->regnum;
			trace(warn("Set highwater %Lx", (long long)info->highwater));
			break;

		case DRAIN_REGION:
		{
			region_t regnum = ((struct region_message *)&message.body)->regnum;
			struct region *region;

			trace(warn("drain region %Lx", (long long)regnum));
			spin_lock(&info->region_lock);
			if ((region = find_region(info, regnum)) && (region_count(region) >= 0))
				region->flags |= DRAIN_FLAG;
			spin_unlock(&info->region_lock);
			break;
		}

		case PAUSE_REQUESTS:
			info->flags |= PAUSE_FLAG;
			break;

		case RESUME_REQUESTS:
			spin_lock(&info->region_lock);
			info->flags &= ~PAUSE_FLAG;
			while (!list_empty(&info->requests))
				send_next_request_locked(info);
			spin_unlock(&info->region_lock);
			break;

		case BOUNCE_REQUEST:
			queue_request(info, ((struct region_message *)&message.body)->regnum);
			break;

		default: 
			warn("Unknown message %x", message.head.code);
			continue;
		}
	}
out:
	up(&info->exit2_sem); /* !!! will crash if module unloaded before ret executes */
	warn("%s exiting", current->comm);
	return 0;
message_too_long:
	warn("message %x too long (%u bytes)", message.head.code, message.head.length);
	goto out;
socket_error:
	warn("socket error %i", err);
	if (running(info))
		goto connect;
	goto out;
}

static int control(struct dm_target *target)
{
	struct devinfo *info = target->private;
	struct messagebuf message; // !!! have a buffer in the target->info
	struct file *sock;
	int err, length;

	daemonize("ddraid-control");
	sock = info->control_socket;
	trace(warn("got socket %p", sock);)

	down(&info->exit3_sem);
	while (running(info)) {
		trace(warn("wait message");)
		if ((err = readpipe(sock, &message.head, sizeof(message.head))))
			goto socket_error;
		trace(warn("got message header code %x", message.head.code);)
		length = message.head.length;
		if (length > maxbody)
			goto message_too_long;
		trace(warn("%x/%u", message.head.code, length);)
		if ((err = readpipe(sock, &message.body, length)))
			goto socket_error;
	
		switch (message.head.code) {
		case CONNECT_SERVER: {
			unsigned len = 4;
			char bogus[len];
			int sock_fd = get_unused_fd(), fd;

			trace(warn("Received connect server");)
			if (sock_fd < 0) {
				warn("Can't get fd, error %i", sock_fd);
				break;
			}
			fd_install(sock_fd, sock);
			if ((fd = recv_fd(sock_fd, bogus, &len)) < 0) {
				warn("recv_fd failed, error %i", fd);
				put_unused_fd(sock_fd);
				break;
			}
			trace(warn("Received socket %i", fd);)
			info->sock = fget(fd);
			current->files->fd_array[fd] = NULL; /* this is sooo hokey */
			put_unused_fd(sock_fd);
			sys_close(fd);
			up(&info->server_in_sem);
			outbead(info->sock, IDENTIFY, struct identify, .id = 6);
			break;
		}
		default: 
			warn("Unknown message %x", message.head.code);
			continue;
		}
	}
out:
	up(&info->exit3_sem); /* !!! will crash if module unloaded before ret executes */
	warn("%s exiting", current->comm);
	return 0;
message_too_long:
	warn("message %x too long (%u bytes)", message.head.code, message.head.length);
	goto out;
socket_error:
	warn("socket error %i", err);
	goto out;
}

static int get_control_socket(char *sockname)
{
	mm_segment_t oldseg = get_fs();
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	int addr_len = sizeof(addr) - sizeof(addr.sun_path) + strlen(sockname); // !!! check too long
	int sock = sys_socket(AF_UNIX, SOCK_STREAM, 0), err = 0;

	trace(warn("Connect to control socket %s", sockname);)
	if (sock <= 0)
		return sock;
	strncpy(addr.sun_path, sockname, sizeof(addr.sun_path));
	if (sockname[0] == '@')
		addr.sun_path[0] = 0;

	set_fs(get_ds());
	err = sys_connect(sock, (struct sockaddr *)&addr, addr_len);
	set_fs(oldseg);
	return err? err: sock;
}

static int shutdown_socket(struct file *socket)
{
	struct socket *sock = SOCKET_I(socket->f_dentry->d_inode);
	return sock->ops->shutdown(sock, RCV_SHUTDOWN);
}

static int ddraid_status(struct dm_target *target, status_type_t type, char *result, unsigned maxlen)
{
	switch (type) {
	case STATUSTYPE_INFO:
	case STATUSTYPE_TABLE:
		result[0] = '\0';
		break;
	}

	return 0;
}

static void ddraid_destroy(struct dm_target *target)
{
	struct devinfo *info = target->private;
	int i, err;

	trace(warn("%p", target);)
	if (!info)
		return;

	down(&info->destroy_sem);

	/* Unblock helper threads */
	info->flags |= FINISH_FLAG;
	up(&info->server_in_sem); // unblock incoming thread
	up(&info->server_out_sem); // unblock io request threads
	up(&info->more_work_sem);

	if (info->sock && (err = shutdown_socket(info->sock)))
		warn("server socket shutdown error %i", err);
	if (info->sock && (err = shutdown_socket(info->control_socket)))
		warn("control socket shutdown error %i", err);

	// !!! wrong! the thread might be just starting, think about this some more
	// ah, don't let ddraid_destroy run while ddraid_create is spawning threads
	down(&info->exit1_sem);
	warn("thread 1 exited");
	down(&info->exit2_sem);
	warn("thread 2 exited");
	down(&info->exit3_sem);
	warn("thread 3 exited");

	if (info->spare_region)
		kmem_cache_free(region_cache, info->spare_region);
	if (info->sock)
		fput(info->sock);
	for (i = 0; i < info->members; i++)
		if (info->member[i])
			dm_put_device(target, info->member[i]);
	kfree(info);
}

static int ddraid_create(struct dm_target *target, unsigned argc, char **argv)
{
	struct devinfo *info;
	sector_t member_len;
	char *end;
	int err, i, members = simple_strtoul(argv[0], &end, 10);
	char *error;

	err = -ENOMEM;
	error = "Can't get kernel memory";
	if (!(info = kmalloc(sizeof(struct devinfo), GFP_KERNEL)))
		goto eek;

	err = -EINVAL;
	error = "ddraid usage: members device... sockname";
	if (members > MAX_MEMBERS || members > argc - 2)
		goto eek;

	error = "dm-stripe: Target length not divisable by number of members";
	member_len = target->len;
	*info = (struct devinfo){ .members = members, .region_size_bits = -1, .dead = -1 };
#ifdef DDRAID
	{
	int n = members - 1, k = fls(n) - 1;

	info->balance_den = 1 << 21;

	if (sector_div(member_len, members - 1)) /* modifies arg1! */
		goto eek;

//	member_len += n;
//	sector_div(member_len, members - 1); /* modifies arg1! */

	error = "Invalid number of ddraid members (must be 2**k+1)";
	if (members < 2 || (~(-1 << k) & n))
		goto eek;

	error = "Drive out of range";
	if (info->dead >= members)
		goto eek;

	warn("Order %i ddraid", k);
	info->blocksize_bits = PAGE_CACHE_SHIFT; // just for now
	info->fragsize_bits = info->blocksize_bits - k;
	}
#endif
	target->private = info;
	sema_init(&info->destroy_sem, 1);
	sema_init(&info->server_in_sem, 0);
	sema_init(&info->server_out_sem, 0);
	sema_init(&info->exit1_sem, 1);
	sema_init(&info->exit2_sem, 1);
	sema_init(&info->exit3_sem, 1);
	sema_init(&info->more_work_sem, 0);
	spin_lock_init(&info->region_lock);
	spin_lock_init(&info->endio_lock);
	INIT_LIST_HEAD(&info->requests);
	INIT_LIST_HEAD(&info->releases);
	INIT_LIST_HEAD(&info->bogus);
	for (i = 0; i < HASH_BUCKETS; i++)
		INIT_LIST_HEAD(&info->hash[i]);

	error = "Can't connect control socket";
	if ((err = get_control_socket(argv[argc - 1])) < 0)
		goto eek;
	info->control_socket = fget(err);
	sys_close(err);

	error = "Can't open ddraid member";
	for (i = 0; i < members; i++)
		if ((err = dm_get_device(target, argv[i + 1], 0, member_len,
			dm_table_get_mode(target->table), &info->member[i])))
			goto eek;

	error = "Can't start daemon";
	if ((err = kernel_thread((void *)incoming, target, CLONE_KERNEL)) < 0)
		goto eek;
	if ((err = kernel_thread((void *)worker, target, CLONE_KERNEL)) < 0)
		goto eek;
	if ((err = kernel_thread((void *)control, target, CLONE_KERNEL)) < 0)
		goto eek;

	warn("Created cluster raid device");
//	target->split_io = 1 << MIN_REGION_BITS; /* goes away if we can start suspended */
	return 0;

eek:	warn("Device create error %i: %s!", err, error);
	ddraid_destroy(target);
	target->error = error;
	return err;
}

static struct target_type ddraid = {
	.name = "ddraid",
	.version = {0, 0, 0},
	.module = THIS_MODULE,
	.ctr = ddraid_create,
	.dtr = ddraid_destroy,
	.map = ddraid_map_bogus,
	.status = ddraid_status,
};

int __init dm_ddraid_init(void)
{
	int err;
	char *what = "Mirror register";

	if ((err = dm_register_target(&ddraid)))
		goto bad1;
	err = -ENOMEM;
	what = "Cache create";
	if (!(region_cache = kmem_cache_create("ddraid-region",
		sizeof(struct region), __alignof__(struct region), 0, NULL, NULL)))
		goto bad2;
	if (!(gizmo_cache = kmem_cache_create("ddraid-gizmos",
		sizeof(union gizmo), __alignof__(union gizmo), 0, NULL, NULL)))
		goto bad3;
	return 0;
bad3:
	kmem_cache_destroy(region_cache);
bad2:
	dm_unregister_target(&ddraid);
bad1:
	DMERR("%s failed\n", what);
	return err;
}

void dm_ddraid_exit(void)
{
	int err;
	if ((err = dm_unregister_target(&ddraid)))
		DMERR("Unregister failed %d", err);
	if (region_cache)
		kmem_cache_destroy(region_cache);
	if (gizmo_cache)
		kmem_cache_destroy(gizmo_cache);
}

module_init(dm_ddraid_init);
module_exit(dm_ddraid_exit);
