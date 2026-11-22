/*
 * HakiosPanfrostGPU: the Mali-G57 MC2 of the Tecno Pova 5 Pro (LH8n, MT6833) for programs to draw
 * with.
 *
 * bsd/dev/hakios.mali.c brings the GPU up (boot-arg mali=N) and, once it is up, starts this
 * service. A program that opens it speaks the interface of Linux's panfrost DRM driver, the
 * one Mesa's panfrost knows: every request is a call of the user client with the request's
 * number as the selector and its structure passed in and back -- as HakiosGPU does it for the
 * other phone's Adreno and freedreno. What Linux does with mmap() on the device is the client
 * memory of the type that is the buffer's handle.
 *
 * What there is: parameters, buffers (made, entered into the GPU's address space, told their
 * address, mapped, given back), and job chains. A buffer is wired kernel memory, zero when
 * made and write-combined, so what the program writes goes to memory where the GPU reads it. A
 * job chain is run to its end before the call returns, so waiting for a buffer has nothing left
 * to wait for.
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOSubMemoryDescriptor.h>
#include <mach/vm_param.h>
#include <kern/clock.h>
#include <sys/systm.h>

#include "MiniIOSurface.h"

extern "C" {
void FlushPoC_DcacheRegion(vm_offset_t va, size_t length);
bool hakios_mali_ready(void);
bool hakios_mali_parameter(uint32_t which, uint64_t * value);
uint64_t hakios_mali_address(uint64_t size, uint64_t align = 0);
bool hakios_mali_map(uint64_t gpu, uint64_t phys);
void hakios_mali_mapped(void);
void hakios_mali_unmap(uint64_t gpu, uint64_t size);
uint32_t hakios_mali_submit(uint64_t chain, bool fragment);
uint64_t hakios_mali_enqueue(uint64_t chain, bool fragment);
uint64_t hakios_mali_enqueue_pair(uint64_t chain, uint64_t chain2, uint32_t heap_tag);
bool hakios_mali_wait(uint64_t seq, uint64_t timeout_ns);
uint64_t hakios_mali_done_seq(void);

void HakiosMaliStart(void);
void MiniIOMobileFramebufferShow(const uint8_t * pixels, uint32_t width, uint32_t height, uint32_t rowBytes);

uint32_t gMaliClients, gMaliBuffers, gMaliSubmits, gMaliFailed;
uint64_t gMaliGpuUs, gMaliJs0Us, gMaliJs1Us, gMaliJs0Count, gMaliJs1Count, gMaliPipelined;
struct hakios_mali_made { uint64_t address, size; uint32_t asked, flags; };
struct hakios_mali_made gMaliMade[32];
}

/* Display/IOSurface wait for asynchronous Mali jobs targeting this surface. */
bool
hakios_mali_surface_drawn(MiniIOSurface * surface, uint64_t milliseconds)
{
	if (surface == NULL || surface->maliSeq == 0) {
		return true;
	}
	if (surface->maliSeq == ~0ull) {
		return false;
	}
	return hakios_mali_wait(surface->maliSeq, milliseconds * NSEC_PER_MSEC);
}

/* Linux's include/uapi/drm/panfrost_drm.h and drm.h: the requests' numbers and structures. */
enum {
	kDrmGemClose            = 0x09,
	kPanfrostSubmit         = 0x40,
	kPanfrostWaitBO         = 0x41,
	kPanfrostCreateBO       = 0x42,
	kPanfrostMmapBO         = 0x43,
	kPanfrostGetParam       = 0x44,
	kPanfrostGetBOOffset    = 0x45,
	kPanfrostMadvise        = 0x48,
	kHakiosSubmitPair       = 0x7c,
	kHakiosWait             = 0x7d,
	kHakiosSurface          = 0x7e,
	kHakiosShow             = 0x7f,
};

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

struct hakios_show {
	uint64_t pixels;
	uint32_t width, height, rowBytes, pad;
};

#define kHakiosDescribe        0xffffffffu

struct hakios_surface {
	uint32_t surface;                       /* the IOSurface's ID */
	uint32_t handle;                        /* kHakiosDescribe: no buffer is wanted; else, back: the buffer, */
	uint64_t size;                          /* how big it is, */
	uint32_t offset, rowBytes;              /* where the pixels start in it and how long a row of them is, */
	uint32_t width, height, pixelFormat, planes;    /* and what they are */
};

struct drm_gem_close {
	uint32_t handle, pad;
};

struct drm_panfrost_submit {
	uint64_t jc;
	uint64_t in_syncs;
	uint32_t in_sync_count;
	uint32_t out_sync;
	uint64_t bo_handles;
	uint32_t bo_handle_count;
	uint32_t requirements;
};
#define PANFROST_JD_REQ_FS      (1u << 0)

struct drm_panfrost_wait_bo {
	uint32_t handle, pad;
	int64_t timeout_ns;
};

struct drm_panfrost_create_bo {
	uint32_t size, flags, handle, pad;
	uint64_t offset;
};

struct drm_panfrost_mmap_bo {
	uint32_t handle, flags;
	uint64_t offset;
};

struct drm_panfrost_get_param {
	uint32_t param, pad;
	uint64_t value;
};

struct drm_panfrost_get_bo_offset {
	uint32_t handle, pad;
	uint64_t offset;
};

struct drm_panfrost_madvise {
	uint32_t handle, madv, retained;
};

#define MALI_BUFFERS            4096
#define MALI_BUFFER_MAX         (256ull << 20)

struct mali_buffer {
	IOMemoryDescriptor * memory;
	MiniIOSurface * surface;
	uint64_t address, size, last_seq;
	uint32_t flags;                  /* drm_panfrost_create_bo flags (value 2 = BO_HEAP) */
};

static IOLock * gMaliLock;

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

class HakiosPanfrostGPU : public IOService
{
	OSDeclareDefaultStructors(HakiosPanfrostGPU);

public:
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(HakiosPanfrostGPU, IOService);

bool
HakiosPanfrostGPU::start(IOService * provider)
{
	if (!hakios_mali_ready() || !IOService::start(provider)) {
		return false;
	}
	setProperty(kIOUserClientClassKey, "HakiosPanfrostGPUUserClient");
	setName("HakiosPanfrostGPU");
	registerService();
	return true;
}

void
HakiosMaliStart(void)
{
	IOService * provider = IOService::getPlatform();
	HakiosPanfrostGPU * gpu;

	if (gMaliLock == NULL) {
		gMaliLock = IOLockAlloc();
	}
	gpu = OSTypeAlloc(HakiosPanfrostGPU);
	if (gpu == NULL || provider == NULL || !gpu->init() || !gpu->attach(provider)) {
		OSSafeReleaseNULL(gpu);
		IOLog("HakiosPanfrostGPU: cannot create the service\n");
		return;
	}
	if (!gpu->start(provider)) {
		gpu->detach(provider);
		IOLog("HakiosPanfrostGPU: the service did not start\n");
	}
	gpu->release();
	IOLog("HakiosPanfrostGPU: the GPU is there for programs to draw with\n");
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

class HakiosPanfrostGPUUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(HakiosPanfrostGPUUserClient);

	struct mali_buffer *    fBuffers;               /* handle n is fBuffers[n - 1] */
	uint32_t                fNextBuffer;
	uint32_t                fSubmits, fFailed, fRequests;
	uint64_t                fFlushTotal, fGpuTotal;   /* B2.2: summed us for the averages */

	struct mali_buffer * buffer(uint32_t handle);
	void freeBuffer(struct mali_buffer * b);
	void logSubmitDesc(uint64_t jc);
	void markSubmittedBOs(uint64_t boHandles, uint32_t count, uint64_t seq);
	IOReturn createBO(struct drm_panfrost_create_bo * request);
	struct mali_buffer * newBuffer(IOMemoryDescriptor * memory, uint64_t from, uint64_t size, uint32_t * handle, uint64_t align = 0);
	IOReturn surfaceBuffer(struct hakios_surface * request);
	IOReturn show(const struct hakios_show * request);

public:
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual void free(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientMemoryForType(UInt32 type, IOOptionBits * options, IOMemoryDescriptor ** memory) APPLE_KEXT_OVERRIDE;
	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments * args,
	    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(HakiosPanfrostGPUUserClient, IOUserClient);

bool
HakiosPanfrostGPUUserClient::start(IOService * provider)
{
	if (!hakios_mali_ready() || !IOUserClient::start(provider)) {
		IOLog("HakiosPanfrostGPU: a client could not be started\n");
		return false;
	}
	IOLog("HakiosPanfrostGPU: a client opened the GPU\n");
	OSIncrementAtomic((SInt32 *) &gMaliClients);
	fBuffers = IONewZero(struct mali_buffer, MALI_BUFFERS);
	return fBuffers != NULL;
}

struct mali_buffer *
HakiosPanfrostGPUUserClient::buffer(uint32_t handle)
{
	if (handle == 0 || handle > MALI_BUFFERS || fBuffers[handle - 1].memory == NULL) {
		return NULL;
	}
	return &fBuffers[handle - 1];
}

/* With the lock held. Wait for queued/running jobs on this buffer before unmapping. */
void
HakiosPanfrostGPUUserClient::freeBuffer(struct mali_buffer * b)
{
	if (b->last_seq != 0) {
		(void)hakios_mali_wait(b->last_seq, 2ull * NSEC_PER_SEC);
	}
	hakios_mali_unmap(b->address, b->size);
	if (b->surface != NULL) {
		MiniIOSurfaceDrop(b->surface);
	}
	b->memory->release();
	bzero(b, sizeof(*b));
}

/*
 * A handle and a GPU address for `size` bytes of `memory` from byte `from` on, every page entered
 * into the GPU's tables. With the lock held; NULL: no handle, no address, or a page that would
 * not go in. The caller sets `memory` of what comes back.
 */
struct mali_buffer *
HakiosPanfrostGPUUserClient::newBuffer(IOMemoryDescriptor * memory, uint64_t from, uint64_t size, uint32_t * handle, uint64_t align)
{
	struct mali_buffer * b = NULL;
	bool mapped = true;

	for (uint32_t n = 0; n < MALI_BUFFERS; n++) {
		uint32_t i = (fNextBuffer + n) % MALI_BUFFERS;

		if (fBuffers[i].memory == NULL) {
			b = &fBuffers[i];
			*handle = i + 1;
			fNextBuffer = i + 1;
			break;
		}
	}
	if (b == NULL) {
		IOLog("HakiosMali: no handles left\n");
		return NULL;
	}
	b->address = hakios_mali_address(size, align);
	/* every 4 KB of it: the GPU's pages, a quarter of the kernel's 16 KB ones */
	for (uint64_t offset = 0; offset < size && mapped && b->address != 0; offset += 4096) {
		IOByteCount length = 0;
		addr64_t phys = memory->getPhysicalSegment(from + offset, &length, kIOMemoryMapperNone);

		mapped = phys != 0 && hakios_mali_map(b->address + offset, phys);
	}
	if (b->address == 0 || !mapped) {
		if (b->address != 0) {
			hakios_mali_unmap(b->address, size);
		}
		bzero(b, sizeof(*b));
		IOLog("HakiosMali: a buffer of %llu bytes cannot be entered into the GPU's address space\n", size);
		return NULL;
	}
	hakios_mali_mapped();
	if (gMaliBuffers < 32) {
		gMaliMade[gMaliBuffers] = (struct hakios_mali_made) { b->address, size, (uint32_t) size, 0 };
	}
	OSIncrementAtomic((SInt32 *) &gMaliBuffers);
	b->size = size;
	return b;
}

/* With the lock held. */
IOReturn
HakiosPanfrostGPUUserClient::createBO(struct drm_panfrost_create_bo * request)
{
	uint64_t size = round_page_64(request->size);
	IOBufferMemoryDescriptor * memory;
	struct mali_buffer * b;

	uint64_t align = 0x10000;
	if (request->flags & 2 /* PANFROST_BO_HEAP */) {
		/* Match the working X6739 (G77 Valhall) policy: cap the tiler heap at 16 MB with
		 * 2 MB alignment. (The earlier no-cap experiment did not fix the G57 0x58.) */
		if (size > 16 * 1024 * 1024) {
			size = 16 * 1024 * 1024;
		}
		request->size = (uint32_t)size;
		align = 2 * 1024 * 1024;
	}

	if (size == 0 || size > MALI_BUFFER_MAX) {
		return kIOReturnBadArgument;
	}
	/* B2.4: matched write-combine for non-surface BOs. Kernel and userspace
	 * both map WC (no WB/WC alias). CPU writes become visible in order with a
	 * dsb sy at submit, so no per-BO FlushPoC_DcacheRegion is needed (that was
	 * ~676 us/submit). Surface BOs (created elsewhere) stay cached. */
	memory = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut | kIOMemoryKernelUserShared |
	    kIOMapWriteCombineCache, (vm_size_t) size, PAGE_SIZE);
	if (memory == NULL) {
		IOLog("HakiosPanfrostGPU: no memory for a buffer of %llu bytes\n", size);
		return kIOReturnNoMemory;
	}
	b = newBuffer(memory, 0, size, &request->handle, align);
	if (b == NULL) {
		memory->release();
		return kIOReturnNoMemory;
	}
	b->memory = memory;
	b->flags = request->flags;       /* remember HEAP so submit can skip its flush */
	request->offset = b->address;
	if (request->handle <= 32) {
		gMaliMade[request->handle - 1] = (struct hakios_mali_made) { b->address, size, request->size, request->flags };
	}
	if (gMaliBuffers <= 32) {
		IOLog("HakiosPanfrostGPU: BO #%u created: addr %#llx..%#llx (size %llu, asked %u, flags %#x)\n",
		    request->handle, b->address, b->address + size, size, request->size, request->flags);
	}
	return kIOReturnSuccess;
}

IOReturn
HakiosPanfrostGPUUserClient::surfaceBuffer(struct hakios_surface * request)
{
	MiniIOSurface * s = MiniIOSurfaceCopy(request->surface);
	struct mali_buffer * b = NULL;
	IOMemoryDescriptor * memory;
	uint64_t size;

	if (s == NULL) {
		return kIOReturnNotFound;
	}
	if (s->client != NULL) {
		MiniIOSurfaceDrop(s);
		return kIOReturnUnsupported;
	}
	size = round_page_64(s->info.allocSize);
	memory = IOSubMemoryDescriptor::withSubRange(s->memory, PAGE_SIZE, size, kIODirectionInOut);
	if (memory == NULL || size == 0 || size > MALI_BUFFER_MAX) {
		OSSafeReleaseNULL(memory);
		MiniIOSurfaceDrop(s);
		return kIOReturnNoMemory;
	}
	request->size = size;
	request->offset = s->info.offset;
	request->rowBytes = s->info.bytesPerRow;
	request->width = s->info.width;
	request->height = s->info.height;
	request->pixelFormat = s->info.pixelFormat;
	request->planes = s->info.planeCount;
	if (request->handle == kHakiosDescribe) {
		request->handle = 0;
		memory->release();
		MiniIOSurfaceDrop(s);
		return kIOReturnSuccess;
	}
	for (uint32_t i = 0; i < MALI_BUFFERS; i++) {
		if (fBuffers[i].memory != NULL && fBuffers[i].surface == s) {
			b = &fBuffers[i];
			request->handle = i + 1;
			break;
		}
	}
	if (b != NULL) {
		memory->release();
		MiniIOSurfaceDrop(s);
		return kIOReturnSuccess;
	}
	b = newBuffer(s->memory, PAGE_SIZE, size, &request->handle);
	if (b == NULL) {
		memory->release();
		MiniIOSurfaceDrop(s);
		return kIOReturnNoMemory;
	}
	b->memory = memory;
	b->surface = s;
	if (request->handle <= 32) {
		gMaliMade[request->handle - 1] = (struct hakios_mali_made) { b->address, size, (uint32_t)size, 0x7e };
	}
	if (gMaliBuffers <= 32) {
		IOLog("HakiosPanfrostGPU: Surface BO #%u created: addr %#llx..%#llx (size %llu)\n",
		    request->handle, b->address, b->address + size, size);
	}
	return kIOReturnSuccess;
}

IOReturn
HakiosPanfrostGPUUserClient::show(const struct hakios_show * request)
{
	uint8_t * pixels;
	size_t size = (size_t) request->height * (size_t) request->rowBytes;
	IOReturn ret = kIOReturnSuccess;

	if (request->width == 0 || request->width > 4096 || request->height == 0 || request->height > 4096 ||
	    request->rowBytes < 4 * request->width) {
		return kIOReturnBadArgument;
	}
	if (size > (16 * 1024 * 1024)) {
		return kIOReturnBadArgument;
	}
	pixels = (uint8_t *) IOMallocData(size);
	if (pixels == NULL) {
		return kIOReturnNoMemory;
	}
	if (copyin((user_addr_t) request->pixels, pixels, size) != 0) {
		ret = kIOReturnVMError;
	} else {
		MiniIOMobileFramebufferShow(pixels, request->width, request->height, request->rowBytes);
	}
	IOFreeData(pixels, size);
	return ret;
}

IOReturn
HakiosPanfrostGPUUserClient::clientClose(void)
{
	IOLockLock(gMaliLock);
	for (uint32_t i = 0; fBuffers != NULL && i < MALI_BUFFERS; i++) {
		if (fBuffers[i].memory != NULL) {
			freeBuffer(&fBuffers[i]);
		}
	}
	IOLockUnlock(gMaliLock);
	if (fSubmits != 0) {
		IOLog("HakiosPanfrostGPU: a client goes after %u job chains, %u of them unfinished\n", fSubmits, fFailed);
	}
	terminate();
	return kIOReturnSuccess;
}

void
HakiosPanfrostGPUUserClient::free(void)
{
	if (fBuffers != NULL) {
		IODelete(fBuffers, struct mali_buffer, MALI_BUFFERS);
		fBuffers = NULL;
	}
	IOUserClient::free();
}

IOReturn
HakiosPanfrostGPUUserClient::clientMemoryForType(UInt32 type, IOOptionBits * options, IOMemoryDescriptor ** memory)
{
	struct mali_buffer * b;
	IOReturn ret = kIOReturnBadArgument;

	IOLockLock(gMaliLock);
	b = buffer(type);
	if (b != NULL) {
		b->memory->retain();
		*memory = b->memory;
		/* B2.4: match the createBO allocation — non-surface BOs are WC on both
		 * sides, surfaces stay cached (0). No WB/WC alias. */
		*options = b->surface != NULL ? 0 : kIOMapWriteCombineCache;
		ret = kIOReturnSuccess;
	}
	IOLockUnlock(gMaliLock);
	return ret;
}

void
HakiosPanfrostGPUUserClient::logSubmitDesc(uint64_t jc)
{
	struct mali_buffer * jc_b = NULL;

	for (uint32_t i = 0; i < MALI_BUFFERS; i++) {
		if (fBuffers[i].memory != NULL &&
		    jc >= fBuffers[i].address &&
		    jc < fBuffers[i].address + fBuffers[i].size) {
			jc_b = &fBuffers[i];
			break;
		}
	}
	if (jc_b != NULL && jc_b->surface == NULL) {
		uint32_t * w = (uint32_t *)((uint8_t *)((IOBufferMemoryDescriptor *)jc_b->memory)->getBytesNoCopy() +
		                            (jc - jc_b->address));
		uint64_t avgGpu = gMaliSubmits ? (__atomic_load_n(&gMaliGpuUs, __ATOMIC_RELAXED) / gMaliSubmits) : 0;
		uint64_t c0 = __atomic_load_n(&gMaliJs0Count, __ATOMIC_RELAXED);
		uint64_t c1 = __atomic_load_n(&gMaliJs1Count, __ATOMIC_RELAXED);
		uint64_t avg0 = c0 ? (__atomic_load_n(&gMaliJs0Us, __ATOMIC_RELAXED) / c0) : 0;
		uint64_t avg1 = c1 ? (__atomic_load_n(&gMaliJs1Us, __ATOMIC_RELAXED) / c1) : 0;
		uint64_t piped = __atomic_load_n(&gMaliPipelined, __ATOMIC_RELAXED);
		IOLog("HakiosPanfrostGPU: submit #%u (total=%u failed=%u avg_gpu=%llu us js1=%llu us js0=%llu us pipe=%llu) jc=%#llx desc=[%#x, %#x, %#x, %#x, %#x, %#x, %#x, %#x]\n",
		    fSubmits, gMaliSubmits, gMaliFailed, avgGpu, avg1, avg0, piped,
		    jc, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
	}
}

void
HakiosPanfrostGPUUserClient::markSubmittedBOs(uint64_t boHandles, uint32_t count, uint64_t seq)
{
	for (uint32_t i = 0; seq != 0 && i < count && i < 4096; i += 64) {
		uint32_t handles[64];
		uint32_t n = count - i < 64 ? count - i : 64;

		if (copyin((user_addr_t) (boHandles + i * sizeof(uint32_t)), handles, n * sizeof(uint32_t)) != 0) {
			break;
		}
		for (uint32_t k = 0; k < n; k++) {
			struct mali_buffer * b = buffer(handles[k]);
			if (b != NULL) {
				b->last_seq = seq;
				if (b->surface != NULL) {
					b->surface->maliSeq = seq;
				}
			}
		}
	}
}

IOReturn
HakiosPanfrostGPUUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments * args,
    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference)
{
	union {
		struct drm_gem_close close;
		struct drm_panfrost_submit submit;
		struct hakios_submit_pair pair;
		struct drm_panfrost_wait_bo wait;
		struct drm_panfrost_create_bo create;
		struct drm_panfrost_mmap_bo mmap;
		struct drm_panfrost_get_param param;
		struct drm_panfrost_get_bo_offset offset;
		struct drm_panfrost_madvise madvise;
		struct hakios_surface surface;
		struct hakios_show show;
		struct hakios_wait waitseq;
	} r;
	size_t size = args->structureInputSize;
	struct mali_buffer * b;
	IOReturn ret = kIOReturnSuccess;

	if (args->structureInput == NULL || size > sizeof(r) || fBuffers == NULL) {
		return kIOReturnBadArgument;
	}
	if (!hakios_mali_ready()) {
		return kIOReturnNotReady;
	}
	bzero(&r, sizeof(r));
	memcpy(&r, args->structureInput, size);

	if (selector == kHakiosWait || selector == kPanfrostWaitBO) {
		uint64_t seq = r.waitseq.seq;
		int64_t timeout = r.waitseq.timeout_ns;

		if (selector == kPanfrostWaitBO) {
			IOLockLock(gMaliLock);
			b = buffer(r.wait.handle);
			seq = b != NULL ? b->last_seq : 0;
			IOLockUnlock(gMaliLock);
			if (b == NULL) {
				return kIOReturnNotFound;
			}
			timeout = r.wait.timeout_ns;
		}
		if (seq != 0 && !hakios_mali_wait(seq, timeout > 0 ? (uint64_t)timeout : 0)) {
			ret = timeout <= 0 ? kIOReturnBusy : kIOReturnTimeout;
		}
		if (selector == kPanfrostWaitBO) {
			r.wait.timeout_ns = (int64_t)hakios_mali_done_seq();
		}
		if (args->structureOutput != NULL && args->structureOutputSize >= size) {
			memcpy(args->structureOutput, &r, size);
			args->structureOutputSize = (uint32_t) size;
		}
		return ret;
	}

	IOLockLock(gMaliLock);
	if (fRequests++ < 32) {                 /* the first ones a client makes, for following it from the log */
		IOLog("HakiosPanfrostGPU: request %#x, %zu bytes\n", selector, size);
	}
	switch (selector) {
	case kPanfrostGetParam:
		if (!hakios_mali_parameter(r.param.param, &r.param.value)) {
			ret = kIOReturnBadArgument;
		}
		/* G57 0x58 diagnosis: log the values Mesa actually sees for GPU_PROD_ID and
		 * TILER_FEATURES (param 10 was historically mis-routed -> hierarchy_mask 0). */
		if (r.param.param == 0 || r.param.param == 10) {
			IOLog("HakiosPanfrostGPU: GET_PARAM %u = %#llx\n", r.param.param,
			    (unsigned long long)r.param.value);
		}
		break;
	case kPanfrostCreateBO:
		ret = createBO(&r.create);
		break;
	case kPanfrostMmapBO:                   /* the offset is the handle: the program maps client memory of that type */
		ret = buffer(r.mmap.handle) != NULL ? kIOReturnSuccess : kIOReturnNotFound;
		r.mmap.offset = (uint64_t) r.mmap.handle << 12;
		break;
	case kPanfrostGetBOOffset:
		b = buffer(r.offset.handle);
		ret = b != NULL ? kIOReturnSuccess : kIOReturnNotFound;
		r.offset.offset = b != NULL ? b->address : 0;
		break;
	case kPanfrostSubmit: {
		uint64_t seq = 0;

		fSubmits++;
		gMaliSubmits++;
		if (r.submit.jc != 0) {
			if (fSubmits <= 5 || (fSubmits % 20000) == 0) {
				logSubmitDesc(r.submit.jc);
			}
			asm volatile("dsb sy\n" ::: "memory");
			seq = hakios_mali_enqueue(r.submit.jc, (r.submit.requirements & PANFROST_JD_REQ_FS) != 0);
		}
		markSubmittedBOs(r.submit.bo_handles, r.submit.bo_handle_count, seq);
		r.submit.jc = seq;
		r.submit.in_syncs = hakios_mali_done_seq();
		break;
	}
	case kHakiosSubmitPair: {
		uint64_t seq = 0;

		fSubmits++;
		gMaliSubmits += 2;
		if (r.pair.jc != 0 && r.pair.frag_jc != 0) {
			if (fSubmits <= 5 || (fSubmits % 20000) == 0) {
				logSubmitDesc(r.pair.jc);
			}
			asm volatile("dsb sy\n" ::: "memory");
			seq = hakios_mali_enqueue_pair(r.pair.jc, r.pair.frag_jc, (r.pair.requirements >> 8) & 0xffu);
		}
		markSubmittedBOs(r.pair.bo_handles, r.pair.bo_handle_count, seq);
		r.pair.jc = seq;
		r.pair.frag_jc = hakios_mali_done_seq();
		break;
	}
	case kPanfrostWaitBO:                   /* every job has finished before its submission returned */
		ret = buffer(r.wait.handle) != NULL ? kIOReturnSuccess : kIOReturnNotFound;
		break;
	case kPanfrostMadvise:
		r.madvise.retained = 1;
		break;
	case kDrmGemClose:
		b = buffer(r.close.handle);
		if (b != NULL) {
			freeBuffer(b);
		} else {
			ret = kIOReturnNotFound;
		}
		break;
	case kHakiosSurface:
		ret = surfaceBuffer(&r.surface);
		break;
	case kHakiosShow:
		ret = show(&r.show);
		break;
	default:
		IOLog("HakiosPanfrostGPU: request %#x (%zu bytes) is not known here\n", selector, size);
		ret = kIOReturnUnsupported;
		break;
	}
	IOLockUnlock(gMaliLock);

	if (args->structureOutput != NULL && args->structureOutputSize >= size) {
		memcpy(args->structureOutput, &r, size);
		args->structureOutputSize = (uint32_t) size;
	}
	return ret;
}
