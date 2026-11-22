/*
 * The libdrm of xf86drm.h (see there): Linux's DRM requests to the kernel's GPU driver as calls
 * of its IOKit user client. A request's number (the low byte of the ioctl code) is the call's
 * selector, its structure goes in and comes back as it is; a buffer's memory is the client
 * memory of the type that is the buffer's handle.
 *
 * freedreno knows its device by a file descriptor. It gets one that means nothing (/dev/null):
 * there is one GPU and one connection to it in a process.
 */
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <IOKit/IOKitLib.h>

#include "xf86drm.h"
#include "drm-uapi/panfrost_drm.h"

#define HAKIOS_GPU_SERVICE     "HakiosGPU"     /* the Adreno (iokit/Kernel/hakios.gpu.cpp) */
#define HAKIOS_MALI_SERVICE    "HakiosPanfrostGPU"   /* the Mali (iokit/Kernel/hakios.mali.cpp) */
#define HAKIOS_SURFACE         0x7e            /* the request that is not Linux's */
#define HAKIOS_DESCRIBE        0xffffffffu     /* ... asked only what the buffer would be like */
#define HAKIOS_WAIT            0x7d            /* a job's number, waited for */
#define HAKIOS_SUBMIT_PAIR     0x7c            /* vertex/tiler + fragment chains submitted together */

static int is_panfrost;

static io_connect_t connection;
static int connection_fd = -1;

int
drmOpenWithType(const char *name, const char *busid, int type)
{
	io_service_t service;

	(void)busid;
	(void)type;
	if (strcmp(name, "msm") == 0) {
		is_panfrost = 0;
	} else if (strcmp(name, "panfrost") == 0) {
		is_panfrost = 1;
	} else {
		return -1;
	}
	if (connection_fd >= 0) {
		return dup(connection_fd);
	}
	service = IOServiceGetMatchingService(0,
	    IOServiceMatching(is_panfrost ? HAKIOS_MALI_SERVICE : HAKIOS_GPU_SERVICE));
	if (service == 0) {
		return -1;
	}
	if (IOServiceOpen(service, mach_task_self(), 0, &connection) != 0) {
		IOObjectRelease(service);
		return -1;
	}
	IOObjectRelease(service);
	connection_fd = open("/dev/null", O_RDWR);
	return dup(connection_fd);
}

static int
call(unsigned number, void *data, size_t size)
{
	size_t returned = size;
	kern_return_t result = IOConnectCallStructMethod(connection, number, data, size, data, &returned);

	switch (result) {
	case 0:
		return 0;
	case kIOReturnNoMemory:
		return ENOMEM;
	case kIOReturnBadArgument:
		return EINVAL;
	case kIOReturnTimeout:
		return ETIMEDOUT;
	case kIOReturnBusy:
		return EBUSY;
	case kIOReturnNotFound:
		return ENOENT;
	case kIOReturnUnsupported:
		return ENOSYS;
	default:
		return EIO;
	}
}

struct hakios_submit_pair {
	uint64_t jc;
	uint64_t frag_jc;
	uint64_t bo_handles;
	uint32_t bo_handle_count;
	uint32_t requirements;
};

struct hakios_wait {
	uint64_t seq;
	int64_t timeout_ns;
};

#define MAX_SYNCOBJS    1024
#define MAX_BO_CACHE    4097

static pthread_mutex_t drm_sync_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t syncobj_seq[MAX_SYNCOBJS];
static bool syncobj_used[MAX_SYNCOBJS];
static uint64_t syncfd_seq[MAX_SYNCOBJS];
static uint32_t syncfd_next = 1;
static uint64_t bo_seq[MAX_BO_CACHE];
static uint8_t bo_imported[MAX_BO_CACHE];
static uint64_t cached_done_seq;

static void
advance_done_seq(uint64_t seq)
{
	uint64_t cur = __atomic_load_n(&cached_done_seq, __ATOMIC_RELAXED);

	while (cur < seq) {
		if (__atomic_compare_exchange_n(&cached_done_seq, &cur, seq, false,
		    __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
			break;
		}
	}
}

static int
wait_seq(uint64_t seq, int64_t rel_timeout_ns)
{
	struct hakios_wait w;
	int error;

	if (seq == 0 || seq <= __atomic_load_n(&cached_done_seq, __ATOMIC_ACQUIRE)) {
		return 0;
	}
	w.seq = seq;
	w.timeout_ns = rel_timeout_ns;
	error = call(HAKIOS_WAIT, &w, sizeof(w));
	if (error != 0) {
		return error;
	}
	advance_done_seq(seq);
	return 0;
}

static int64_t
abs_to_rel_ns(int64_t timeout_nsec)
{
	struct timespec ts;
	int64_t now_ns;

	if (timeout_nsec <= 0) {
		return 0;
	}
	if (timeout_nsec == INT64_MAX) {
		return 10000000000LL;
	}
#ifdef CLOCK_UPTIME_RAW
	clock_gettime(CLOCK_UPTIME_RAW, &ts);
#else
	clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
	now_ns = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
	return timeout_nsec > now_ns ? timeout_nsec - now_ns : 0;
}

static void
record_submitted_seq(uint64_t seq, uint64_t done_seq, uint32_t out_sync, const uint32_t *handles, uint32_t count)
{
	if (done_seq != 0) {
		advance_done_seq(done_seq);
	}
	if (seq != 0) {
		pthread_mutex_lock(&drm_sync_lock);
		if (out_sync > 0 && out_sync < MAX_SYNCOBJS) {
			syncobj_seq[out_sync] = seq;
		}
		for (uint32_t i = 0; handles != NULL && i < count; i++) {
			uint32_t h = handles[i];
			if (h > 0 && h < MAX_BO_CACHE) {
				bo_seq[h] = seq;
			}
		}
		pthread_mutex_unlock(&drm_sync_lock);
	}
}

int
hakios_drm_submit_pair(int fd, uint64_t vtc_jc, uint64_t frag_jc, uint32_t requirements,
    uint32_t out_sync, const uint32_t *bo_handles, uint32_t bo_handle_count)
{
	struct hakios_submit_pair p = {
		.jc = vtc_jc,
		.frag_jc = frag_jc,
		.bo_handles = (uint64_t)(uintptr_t)bo_handles,
		.bo_handle_count = bo_handle_count,
		.requirements = requirements,
	};
	int error;

	(void)fd;
	error = call(HAKIOS_SUBMIT_PAIR, &p, sizeof(p));
	if (error != 0) {
		errno = error;
		return -1;
	}
	record_submitted_seq(p.jc, p.frag_jc, out_sync, bo_handles, bo_handle_count);
	return 0;
}

int
drmIoctl(int fd, unsigned long request, void *arg)
{
	unsigned number = (unsigned)(request & 0xff);
	uint64_t orig_jc = 0, orig_in_syncs = 0;
	int error;

	(void)fd;
	if (is_panfrost && number == (DRM_IOCTL_PANFROST_MADVISE & 0xff) && arg != NULL) {
		struct drm_panfrost_madvise *m = arg;

		m->retained = 1;
		return 0;
	}
	if (is_panfrost && number == (DRM_IOCTL_PANFROST_WAIT_BO & 0xff) && arg != NULL) {
		struct drm_panfrost_wait_bo *w = arg;
		int64_t orig_timeout = w->timeout_ns;
		uint64_t seq = 0, done_seq;
		uint8_t imported = 1;

		if (w->handle > 0 && w->handle < MAX_BO_CACHE) {
			pthread_mutex_lock(&drm_sync_lock);
			seq = bo_seq[w->handle];
			imported = bo_imported[w->handle];
			pthread_mutex_unlock(&drm_sync_lock);
			done_seq = __atomic_load_n(&cached_done_seq, __ATOMIC_ACQUIRE);
			if ((!imported && seq == 0) || (seq != 0 && seq <= done_seq)) {
				return 0;
			}
		}
		error = call(number, arg, IOCPARM_LEN(request));
		if (error == 0) {
			done_seq = (uint64_t)w->timeout_ns;
			w->timeout_ns = orig_timeout;
			if (done_seq > seq) {
				seq = done_seq;
			}
			if (seq != 0) {
				advance_done_seq(seq);
			}
		} else {
			w->timeout_ns = orig_timeout;
		}
		if (error != 0) {
			errno = error;
			return -1;
		}
		return 0;
	}

	if (is_panfrost && number == (DRM_IOCTL_PANFROST_SUBMIT & 0xff) && arg != NULL) {
		struct drm_panfrost_submit *s = arg;
		orig_jc = s->jc;
		orig_in_syncs = s->in_syncs;
	}

	error = call(number, arg, IOCPARM_LEN(request));
	if (error != 0) {
		errno = error;
		return -1;
	}

	if (is_panfrost && number == (DRM_IOCTL_PANFROST_CREATE_BO & 0xff) && arg != NULL) {
		uint32_t h = ((struct drm_panfrost_create_bo *)arg)->handle;

		if (h > 0 && h < MAX_BO_CACHE) {
			pthread_mutex_lock(&drm_sync_lock);
			bo_seq[h] = 0;
			bo_imported[h] = 0;
			pthread_mutex_unlock(&drm_sync_lock);
		}
	} else if (is_panfrost && number == (DRM_IOCTL_GEM_CLOSE & 0xff) && arg != NULL) {
		uint32_t h = ((struct drm_gem_close *)arg)->handle;

		if (h > 0 && h < MAX_BO_CACHE) {
			pthread_mutex_lock(&drm_sync_lock);
			bo_seq[h] = 0;
			bo_imported[h] = 0;
			pthread_mutex_unlock(&drm_sync_lock);
		}
	} else if (is_panfrost && number == (DRM_IOCTL_PANFROST_SUBMIT & 0xff) && arg != NULL) {
		struct drm_panfrost_submit *s = arg;
		uint64_t seq = s->jc;
		uint64_t done_seq = s->in_syncs;
		const uint32_t *handles = (const uint32_t *)(uintptr_t)s->bo_handles;

		s->jc = orig_jc;
		s->in_syncs = orig_in_syncs;
		record_submitted_seq(seq, done_seq, s->out_sync, handles, s->bo_handle_count);
	}
	return 0;
}

int
drmCommandWrite(int fd, unsigned long index, const void *data, unsigned long size)
{
	(void)fd;
	return -call((unsigned)(DRM_COMMAND_BASE + index), (void *)(uintptr_t)data, size);
}

int
drmCommandWriteRead(int fd, unsigned long index, void *data, unsigned long size)
{
	(void)fd;
	return -call((unsigned)(DRM_COMMAND_BASE + index), data, size);
}

/*
 * The driver calls itself what Linux's is called, and is of the version whose interface it has:
 * 1.7, buffers at addresses the kernel chooses and tells ("softpin"), nothing newer.
 */
drmVersionPtr
drmGetVersion(int fd)
{
	drmVersionPtr version = calloc(1, sizeof(*version));

	(void)fd;
	if (version != NULL) {
		version->version_major = 1;
		version->version_minor = is_panfrost ? 1 : 7;
		version->name = strdup(is_panfrost ? "panfrost" : "msm");
		version->name_len = (int)strlen(version->name);
		version->date = strdup("");
		version->desc = strdup(is_panfrost ? "Mali on Darwin" : "Adreno on Darwin");
		version->desc_len = (int)strlen(version->desc);
	}
	return version;
}

void
drmFreeVersion(drmVersionPtr version)
{
	if (version != NULL) {
		free(version->name);
		free(version->date);
		free(version->desc);
		free(version);
	}
}

int
drmCloseBufferHandle(int fd, uint32_t handle)
{
	struct drm_gem_close close = { .handle = handle };

	return drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &close);
}

int
drmGetCap(int fd, uint64_t capability, uint64_t *value)
{
	(void)fd;
	(void)capability;
	*value = 0;
	return 0;
}

void *
hakios_drm_map(int fd, uint32_t handle, uint64_t size)
{
	mach_vm_address_t address = 0;
	mach_vm_size_t mapped = 0;

	(void)fd;
	(void)size;
	/*
	 * Linux gives freedreno its buffers write-combined, with no cache of this processor's between
	 * the program and the memory the GPU sees. So does the kernel's driver here: the memory is
	 * made that way, and this mapping is as the memory is.
	 */
	if (IOConnectMapMemory64(connection, handle, mach_task_self(), &address, &mapped, kIOMapAnywhere) != 0) {
		return (void *)-1;              /* mmap()'s MAP_FAILED */
	}
	return (void *)(uintptr_t)address;
}

void
hakios_drm_unmap(int fd, uint32_t handle, void *address)
{
	(void)fd;
	IOConnectUnmapMemory64(connection, handle, mach_task_self(), (mach_vm_address_t)(uintptr_t)address);
}

int
hakios_drm_surface(int fd, struct hakios_drm_surface *surface)
{
	int error;

	(void)fd;
	surface->handle = HAKIOS_DESCRIBE;
	error = call(HAKIOS_SURFACE, surface, sizeof(*surface));
	if (error != 0) {
		errno = error;
		return -1;
	}
	return 0;
}

int
drmPrimeFDToHandle(int fd, int surface, uint32_t *handle)
{
	struct hakios_drm_surface request = { .surface = (uint32_t)surface };
	int error = call(HAKIOS_SURFACE, &request, sizeof(request));

	(void)fd;
	if (error != 0) {
		errno = error;
		return -1;
	}
	*handle = request.handle;
	if (is_panfrost && request.handle > 0 && request.handle < MAX_BO_CACHE) {
		pthread_mutex_lock(&drm_sync_lock);
		bo_imported[request.handle] = 1;
		pthread_mutex_unlock(&drm_sync_lock);
	}
	return 0;
}

static int
none(void)
{
	errno = ENOSYS;
	return -1;
}

/* Buffers and fences as file descriptors, for handing them to other programs and drivers: none of that here. */
#define NONE(name, ...) int name(__VA_ARGS__) { return none(); }
NONE(drmPrimeHandleToFD, __attribute__((unused)) int fd, __attribute__((unused)) uint32_t handle,
    __attribute__((unused)) uint32_t flags, __attribute__((unused)) int *prime_fd)
/*
 * Userspace syncobjs backed by kernel job sequence numbers (submit.jc -> HAKIOS_WAIT).
 */
static int
alloc_syncobj(uint32_t *handle)
{
	pthread_mutex_lock(&drm_sync_lock);
	for (uint32_t i = 1; i < MAX_SYNCOBJS; i++) {
		if (!syncobj_used[i]) {
			syncobj_used[i] = true;
			syncobj_seq[i] = 0;
			pthread_mutex_unlock(&drm_sync_lock);
			*handle = i;
			return 0;
		}
	}
	pthread_mutex_unlock(&drm_sync_lock);
	errno = ENOMEM;
	return -1;
}

int
drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle)
{
	(void)fd; (void)flags;
	return alloc_syncobj(handle);
}

int
drmSyncobjDestroy(int fd, uint32_t handle)
{
	(void)fd;
	if (handle > 0 && handle < MAX_SYNCOBJS) {
		pthread_mutex_lock(&drm_sync_lock);
		syncobj_used[handle] = false;
		syncobj_seq[handle] = 0;
		pthread_mutex_unlock(&drm_sync_lock);
	}
	return 0;
}

int
drmSyncobjReset(int fd, const uint32_t *handles, uint32_t count)
{
	(void)fd;
	pthread_mutex_lock(&drm_sync_lock);
	for (uint32_t i = 0; handles != NULL && i < count; i++) {
		if (handles[i] > 0 && handles[i] < MAX_SYNCOBJS) {
			syncobj_seq[handles[i]] = 0;
		}
	}
	pthread_mutex_unlock(&drm_sync_lock);
	return 0;
}

int
drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle)
{
	int ret = alloc_syncobj(handle);

	(void)fd;
	if (ret == 0 && obj_fd >= 0) {
		pthread_mutex_lock(&drm_sync_lock);
		syncobj_seq[*handle] = syncfd_seq[obj_fd & (MAX_SYNCOBJS - 1)];
		pthread_mutex_unlock(&drm_sync_lock);
	}
	return ret;
}

int
drmSyncobjHandleToFD(int fd, uint32_t handle, int *obj_fd)
{
	int out;

	(void)fd;
	pthread_mutex_lock(&drm_sync_lock);
	out = 0x40000000 | (int)(syncfd_next++ & (MAX_SYNCOBJS - 1));
	syncfd_seq[out & (MAX_SYNCOBJS - 1)] = (handle > 0 && handle < MAX_SYNCOBJS) ? syncobj_seq[handle] : 0;
	pthread_mutex_unlock(&drm_sync_lock);
	*obj_fd = out;
	return 0;
}

int
drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd)
{
	(void)fd;
	if (handle > 0 && handle < MAX_SYNCOBJS && sync_file_fd >= 0) {
		pthread_mutex_lock(&drm_sync_lock);
		syncobj_seq[handle] = syncfd_seq[sync_file_fd & (MAX_SYNCOBJS - 1)];
		pthread_mutex_unlock(&drm_sync_lock);
	}
	return 0;
}

int
drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd)
{
	return drmSyncobjHandleToFD(fd, handle, sync_file_fd);
}

int
drmSyncobjWait(int fd, const uint32_t *handles, uint32_t count, int64_t timeout_nsec,
    uint32_t flags, uint32_t *first_signaled)
{
	uint64_t max_seq = 0;
	int err;

	(void)fd; (void)flags;
	pthread_mutex_lock(&drm_sync_lock);
	for (uint32_t i = 0; handles != NULL && i < count; i++) {
		uint32_t h = handles[i];
		if (h > 0 && h < MAX_SYNCOBJS && syncobj_seq[h] > max_seq) {
			max_seq = syncobj_seq[h];
		}
	}
	pthread_mutex_unlock(&drm_sync_lock);
	if (is_panfrost && max_seq != 0) {
		err = wait_seq(max_seq, abs_to_rel_ns(timeout_nsec));
		if (err != 0) {
			errno = err == EBUSY ? ETIMEDOUT : err;
			return -1;
		}
	}
	if (first_signaled != NULL && count > 0) {
		*first_signaled = handles[0];
	}
	return 0;
}

int
drmSyncobjTransfer(int fd, uint32_t dst_handle, uint64_t dst_point, uint32_t src_handle,
    uint64_t src_point, uint32_t flags)
{
	(void)fd; (void)dst_point; (void)src_point; (void)flags;
	if (dst_handle > 0 && dst_handle < MAX_SYNCOBJS) {
		pthread_mutex_lock(&drm_sync_lock);
		syncobj_seq[dst_handle] = (src_handle > 0 && src_handle < MAX_SYNCOBJS) ? syncobj_seq[src_handle] : 0;
		pthread_mutex_unlock(&drm_sync_lock);
	}
	return 0;
}

int
drmSyncobjTimelineWait(int fd, const uint32_t *handles, const uint64_t *points,
    uint32_t count, int64_t timeout_nsec, uint32_t flags, uint32_t *first_signaled)
{
	(void)points;
	return drmSyncobjWait(fd, handles, count, timeout_nsec, flags, first_signaled);
}
