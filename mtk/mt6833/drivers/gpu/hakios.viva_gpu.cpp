/*
 * VivaGPUAccelerator — the vgpu UAPI service for the prebuilt VivaMTLDriver.bundle.
 *
 * The Metal driver talks to a service of class "VivaGPUAccelerator" with 24 selectors
 * (viva_gpu_uapi.h). This service implements the first increment: version, params,
 * buffer objects and mapping — enough for libdrm-xnu/Mesa to open the device and allocate
 * buffers. Job submission and fences come in the next increment.
 *
 * The low-level GPU (power, MMU, job submit) is the same one HakiosPanfrostGPU uses, via the
 * extern "C" hooks in hakios.mt6833_mali.c. We keep our own BO table here (handles are per-client).
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <mach/vm_param.h>
#include <kern/task.h>
#include <sys/proc.h>
#include <sys/systm.h>
#include "MiniIOSurface.h"

/* vgpu UAPI (inlined from viva_gpu_uapi.h so this driver is self-contained in the kernel tree) */
#define VGPU_UAPI_MAJOR      1
#define VGPU_UAPI_MINOR      2
#define VGPU_SEL_GET_VERSION 0
#define VGPU_SEL_GET_PARAM 1
#define VGPU_SEL_CREATE_BO 2
#define VGPU_SEL_MMAP_BO 3
#define VGPU_SEL_GET_BO_OFFSET 4
#define VGPU_SEL_GEM_CLOSE 5
#define VGPU_SEL_SUBMIT 6
#define VGPU_SEL_WAIT_BO 7
#define VGPU_SEL_MADVISE 8
#define VGPU_SEL_SYNCOBJ_CREATE 9
#define VGPU_SEL_SYNCOBJ_DESTROY 10
#define VGPU_SEL_SYNCOBJ_WAIT 11
#define VGPU_SEL_SYNCOBJ_RESET 12
#define VGPU_SEL_SYNCOBJ_SIGNAL 13
#define VGPU_SEL_IMPORT_IOSURFACE 14
#define VGPU_SEL_SYNC_BO 15
#define VGPU_SEL_QUERY_BO_INFO 16
#define VGPU_SEL_JM_CTX_CREATE 17
#define VGPU_SEL_JM_CTX_DESTROY 18
#define VGPU_SEL_SYNCOBJ_TRANSFER 19
#define VGPU_SEL_IMPORT_USERPTR 20
#define VGPU_SEL_SUBMIT_BATCH 21
#define VGPU_SEL_SURFACE_PROMISE 22
#define VGPU_SEL_COUNT 23
#define VGPU_BO_NOEXEC   (1u << 0)
#define VGPU_BO_HEAP     (1u << 1)
#define VGPU_BO_WB_MMAP  (1u << 2)
#define VGPU_JD_REQ_FS   (1u << 0)
#define VGPU_SYNCOBJ_CREATE_SIGNALED (1u << 0)
#define VGPU_SYNCOBJ_WAIT_ALL (1u << 0)
#define VGPU_MADV_WILLNEED 0
#define VGPU_MADV_DONTNEED 1
struct vgpu_version { uint32_t major, minor, gpu_id, page_size; char name[32]; };
struct vgpu_get_param { uint32_t param, pad; uint64_t value; };
struct vgpu_create_bo { uint32_t size, flags, handle, pad; uint64_t offset; };
struct vgpu_mmap_bo { uint32_t handle, flags; uint64_t offset; };
struct vgpu_get_bo_offset { uint32_t handle, pad; uint64_t offset; };
struct vgpu_gem_close { uint32_t handle, pad; };
struct vgpu_submit {
   uint64_t jc, in_syncs; uint32_t in_sync_count, out_sync;
   uint64_t bo_handles; uint32_t bo_handle_count, requirements;
   uint32_t jm_ctx_handle, seq;
};
struct vgpu_wait_bo { uint32_t handle, pad; int64_t timeout_ns; };
struct vgpu_madvise { uint32_t handle, madv, retained; };
struct vgpu_syncobj_create { uint32_t handle, flags; };
struct vgpu_syncobj_destroy { uint32_t handle, pad; };
struct vgpu_syncobj_wait {
   uint64_t handles; int64_t timeout_nsec;
   uint32_t count_handles, flags, first_signaled, pad;
   uint64_t deadline_nsec;
};
struct vgpu_syncobj_array { uint64_t handles; uint32_t count_handles, pad; };
struct vgpu_syncobj_transfer { uint32_t src, dst; };
struct vgpu_import_iosurface {
   uint32_t surface_id, plane, handle, pad;
   uint64_t offset, size;
   uint32_t bytes_per_row, pixel_format, width, height;
};
struct vgpu_bo_sync_op { uint32_t handle, type, offset, size; };
struct vgpu_sync_bo { uint64_t ops; uint32_t op_count, pad; };
struct vgpu_query_bo_info { uint32_t handle, extra_flags, create_flags, pad; };
struct vgpu_jm_ctx { uint32_t handle, priority; };
/* exact selector struct sizes in enum order (23 selectors: 0..22) */
static const uint32_t viva_selector_size[VGPU_SEL_COUNT] = {
   sizeof(struct vgpu_version), sizeof(struct vgpu_get_param), sizeof(struct vgpu_create_bo),
   sizeof(struct vgpu_mmap_bo), sizeof(struct vgpu_get_bo_offset), sizeof(struct vgpu_gem_close),
   sizeof(struct vgpu_submit), sizeof(struct vgpu_wait_bo), sizeof(struct vgpu_madvise),
   sizeof(struct vgpu_syncobj_create), sizeof(struct vgpu_syncobj_destroy), sizeof(struct vgpu_syncobj_wait),
   sizeof(struct vgpu_syncobj_array), sizeof(struct vgpu_syncobj_array), sizeof(struct vgpu_import_iosurface),
   sizeof(struct vgpu_sync_bo), sizeof(struct vgpu_query_bo_info), sizeof(struct vgpu_jm_ctx), sizeof(struct vgpu_jm_ctx),
   sizeof(struct vgpu_syncobj_transfer), 24, 24, 24,  /* userptr, submit_batch, surface_promise (unused) */
};

extern "C" {
bool hakios_mali_ready(void);
bool hakios_mali_parameter(uint32_t which, uint64_t * value);
uint64_t hakios_mali_address(uint64_t size, uint64_t align = 0);
bool hakios_mali_map(uint64_t gpu, uint64_t phys);
void hakios_mali_mapped(void);
void hakios_mali_unmap(uint64_t gpu, uint64_t size);
uint32_t hakios_mali_submit(uint64_t chain, bool fragment);
void HakiosVivaGPUStart(void);
extern uint32_t gMaliClients;
}

#define VIVA_TAG "VivaGPU: "

/* ---- the accelerator service ---- */

/* IOAcceleratorES is defined in MiniIOMobileFramebuffer.cpp (same kernel link). Metal finds
 * GPU plug-ins by matching that class, so VivaGPUAccelerator must derive from it. */
class IOAcceleratorES : public IOService
{
	OSDeclareDefaultStructors(IOAcceleratorES);
};

class VivaGPUAccelerator : public IOAcceleratorES
{
	OSDeclareDefaultStructors(VivaGPUAccelerator);
public:
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	uint32_t gpuId(void) const { return fGpuId; }
private:
	uint32_t fGpuId;
};

OSDefineMetaClassAndStructors(VivaGPUAccelerator, IOAcceleratorES);

bool
VivaGPUAccelerator::start(IOService * provider)
{
	if (!hakios_mali_ready() || !IOService::start(provider)) {
		return false;
	}
	uint64_t id = 0, rev = 0;
	hakios_mali_parameter(0, &id);           /* GPU_PROD_ID  = 0x9093 */
	hakios_mali_parameter(1, &rev);          /* GPU_REVISION = 0x0010 */
	fGpuId = (uint32_t)((id << 16) | (rev & 0xffff));   /* 0x90930010, not 0x90930000 */
	setProperty(kIOUserClientClassKey, "VivaGPUUserClient");
	setName("VivaGPUAccelerator");
	/* Metal matches IOAcceleratorES and reads these two properties to load the plug-in bundle
	 * from /System/Library/Extensions/<MetalPluginName>.bundle and take MetalPluginClassName.
	 * The viva bundle is installed at that path (symlink into /redwood/lib). */
	/* Reuse the existing /redwood/lib/HakiosMetal.bundle symlink in system.apfs (no 2.5GB
	 * reflash): the viva bundle is installed at that path, but its class is VMDevice. */
	setProperty("MetalPluginName", "HakiosMetal");
	setProperty("MetalPluginClassName", "VMDevice");
	registerService();
	IOLog(VIVA_TAG "accelerator published, gpu_id=%#x\n", fGpuId);
	return true;
}

void
HakiosVivaGPUStart(void)
{
	IOService * provider = IOService::getPlatform();
	VivaGPUAccelerator * gpu;

	gpu = OSTypeAlloc(VivaGPUAccelerator);
	if (gpu == NULL || provider == NULL || !gpu->init() || !gpu->attach(provider)) {
		OSSafeReleaseNULL(gpu);
		IOLog(VIVA_TAG "cannot create the service\n");
		return;
	}
	if (!gpu->start(provider)) {
		gpu->detach(provider);
		IOLog(VIVA_TAG "service did not start\n");
	}
	gpu->release();
}

/* ---- per-client BO ---- */

struct VivaBO {
	IOBufferMemoryDescriptor * memory;
	uint64_t va;            /* GPU virtual address */
	uint64_t size;
	uint32_t flags;
	bool imported;          /* true for IOSurface BOs: memory owned by the surface, don't release */
};

#define VIVA_MAX_BOS 4096

/* A syncobj is just a fence. Our low-level submit is synchronous (the job finishes before it
 * returns), so a fence is signalled the moment we create it for a completed job. */
struct VivaSyncobj {
	bool used;
	bool signalled;
};

#define VIVA_MAX_SYNCOBJS 4096

/* ---- the user client: the 24-selector vgpu UAPI ---- */

class VivaGPUUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(VivaGPUUserClient);
public:
	virtual bool initWithTask(task_t owningTask, void * securityToken, UInt32 type,
	    OSDictionary * properties) APPLE_KEXT_OVERRIDE;
	virtual bool initWithTask(task_t owningTask, void * securityToken, UInt32 type) APPLE_KEXT_OVERRIDE;
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientMemoryForType(UInt32 type, IOOptionBits * options, IOMemoryDescriptor ** memory) APPLE_KEXT_OVERRIDE;
	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments * args,
	    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference) APPLE_KEXT_OVERRIDE;
private:
	VivaGPUAccelerator * fGPU;
	task_t fTask;                  /* owning task, for the client-name log */
	VivaBO * fBOs;                 /* handle n is fBOs[n-1] */
	UInt32   fNextBO;
	VivaSyncobj * fSyncobjs;       /* handle n is fSyncobjs[n-1] */
	UInt32   fNextSync;
	IOReturn createBO(struct vgpu_create_bo * a);
	IOReturn flushCachedBOs(struct vgpu_submit * a);
	IOReturn submitJob(struct vgpu_submit * a);
	IOReturn importIOSurface(struct vgpu_import_iosurface * a);
	IOReturn dispatch(uint32_t selector, void * arg);
	VivaBO * bo(uint32_t handle) { return (handle > 0 && handle <= VIVA_MAX_BOS) ? &fBOs[handle-1] : NULL; }
	bool syncValid(uint32_t h) { return h > 0 && h <= VIVA_MAX_SYNCOBJS && fSyncobjs[h-1].used; }
	IOLock * fLock;
};

OSDefineMetaClassAndStructors(VivaGPUUserClient, IOUserClient);

bool
VivaGPUUserClient::initWithTask(task_t owningTask, void * securityToken, UInt32 type,
    OSDictionary * properties)
{
	if (!IOUserClient::initWithTask(owningTask, securityToken, type, properties)) {
		return false;
	}
	fTask = owningTask;
	return true;
}

bool
VivaGPUUserClient::initWithTask(task_t owningTask, void * securityToken, UInt32 type)
{
	if (!IOUserClient::initWithTask(owningTask, securityToken, type)) {
		return false;
	}
	fTask = owningTask;
	return true;
}

bool
VivaGPUUserClient::start(IOService * provider)
{
	char name[32] = "?";
	int pid = -1;

	if (!IOUserClient::start(provider)) {
		return false;
	}
	fGPU = OSDynamicCast(VivaGPUAccelerator, provider);
	if (fGPU == NULL) {
		return false;
	}
	fBOs = IONewZero(VivaBO, VIVA_MAX_BOS);
	fSyncobjs = IONewZero(VivaSyncobj, VIVA_MAX_SYNCOBJS);
	fLock = IOLockAlloc();
	fNextBO = 1;
	fNextSync = 1;
	if (fTask != TASK_NULL) {
		pid = task_pid(fTask);
		proc_name(pid, name, sizeof(name));
	}
	IOLog(VIVA_TAG "client opened by %s (pid %d)\n", name, pid);
	if (fBOs == NULL || fSyncobjs == NULL || fLock == NULL) {
		return false;
	}
	return true;
}

IOReturn
VivaGPUUserClient::clientClose(void)
{
	if (fBOs != NULL) {
		for (uint32_t i = 0; i < VIVA_MAX_BOS; i++) {
			if (fBOs[i].memory != NULL) {
				hakios_mali_unmap(fBOs[i].va, fBOs[i].size);
				if (!fBOs[i].imported) {
					fBOs[i].memory->release();
				}
				fBOs[i].memory = NULL;
			}
		}
	}
	if (fSyncobjs != NULL) {
		IODelete(fSyncobjs, VivaSyncobj, VIVA_MAX_SYNCOBJS);
		fSyncobjs = NULL;
	}
	return IOUserClient::clientClose();
}

IOReturn
VivaGPUUserClient::clientMemoryForType(UInt32 type, IOOptionBits * options, IOMemoryDescriptor ** memory)
{
	IOReturn ret = kIOReturnBadArgument;
	IOLockLock(fLock);
	VivaBO * b = bo(type);
	if (b != NULL && b->memory != NULL) {
		b->memory->retain();
		*memory = b->memory;
		/* Match the allocation's kernel cache attribute (see createBO): WB_MMAP
		 * BOs are cached on both sides, all others are write-combining on both. */
		*options = (b->flags & VGPU_BO_WB_MMAP) ? 0 : kIOMapWriteCombineCache;
		ret = kIOReturnSuccess;
	}
	IOLockUnlock(fLock);
	return ret;
}

IOReturn
VivaGPUUserClient::createBO(struct vgpu_create_bo * a)
{
	uint64_t size = round_page_64((uint64_t)a->size);
	if (size == 0) {
		return kIOReturnBadArgument;
	}
	uint64_t align = (a->flags & VGPU_BO_HEAP) ? (2ull << 20) : 0;

	IOLockLock(fLock);
	/* find a free handle */
	uint32_t handle = 0;
	for (uint32_t n = 0; n < VIVA_MAX_BOS; n++) {
		uint32_t i = (fNextBO + n) % VIVA_MAX_BOS;
		if (fBOs[i].memory == NULL) {
			handle = i + 1;
			fNextBO = i + 2;
			break;
		}
	}
	if (handle == 0) {
		IOLockUnlock(fLock);
		return kIOReturnNoMemory;
	}

	/* Keep the kernel and user cache attributes identical: a WB user mapping of a
	 * WC kernel allocation (or vice versa) is an illegal alias on arm64 and a
	 * prime suspect for the GPU reading stale descriptor bytes (DATA_INVALID_FAULT). */
	IOOptionBits cache = (a->flags & VGPU_BO_WB_MMAP) ? kIOMapDefaultCache : kIOMapWriteCombineCache;
	IOBufferMemoryDescriptor * mem = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
	    kIODirectionInOut | kIOMemoryKernelUserShared | cache, (vm_size_t)size, PAGE_SIZE);
	if (mem == NULL) {
		IOLockUnlock(fLock);
		return kIOReturnNoMemory;
	}
	uint64_t va = hakios_mali_address(size, align);
	bool ok = va != 0;
	if (ok) {
		/* map each 4K GPU page to the physical segment at that offset */
		for (uint64_t off = 0; off < size && ok; off += 4096) {
			IOByteCount len = 0;
			addr64_t phys = mem->getPhysicalSegment(off, &len, kIOMemoryMapperNone);
			ok = phys != 0 && hakios_mali_map(va + off, phys);
		}
	}
	if (!ok) {
		if (va != 0) {
			hakios_mali_unmap(va, size);
		}
		mem->release();
		IOLockUnlock(fLock);
		return kIOReturnNoMemory;
	}
	hakios_mali_mapped();

	VivaBO * b = &fBOs[handle - 1];
	b->memory = mem;
	b->va = va;
	b->size = size;
	b->flags = a->flags;
	a->handle = handle;
	a->offset = va;                 /* GPU VA, within VGPU_VA_* */
	IOLockUnlock(fLock);
	return kIOReturnSuccess;
}

extern "C" void FlushPoC_DcacheRegion(vm_offset_t va, size_t length);

/* Flush every cached BO the job names, to PoC (dc civac + dsb sy). Called before the
 * submit so the GPU reads Mesa's writes, and after it so the CPU sees GPU output.
 * The low-level submit() only flushes the job's own descriptor pool, not these BOs. */
IOReturn
VivaGPUUserClient::flushCachedBOs(struct vgpu_submit * a)
{
	if (a->bo_handle_count == 0 || a->bo_handles == 0) {
		return kIOReturnSuccess;
	}
	uint32_t done = 0;
	while (done < a->bo_handle_count) {
		uint32_t n = a->bo_handle_count - done;
		uint32_t handles[512];

		if (n > 512) n = 512;
		if (copyin((user_addr_t)(a->bo_handles + done), handles, n * sizeof(uint32_t)) != 0) {
			return kIOReturnBadArgument;
		}
		for (uint32_t i = 0; i < n; i++) {
			VivaBO * b = bo(handles[i]);
			if (b == NULL) {
				return kIOReturnNotFound;
			}
			if ((b->flags & VGPU_BO_WB_MMAP) || b->imported) {
				if (b->memory != NULL && b->memory->getBytesNoCopy() != NULL) {
					FlushPoC_DcacheRegion((vm_offset_t)b->memory->getBytesNoCopy(), (size_t)b->size);
				}
			}
		}
		done += n;
	}
	return kIOReturnSuccess;
}

/* submit: copy in the BO handles, run the job synchronously (our low-level driver completes it
 * before returning), then signal the out_sync fence. Fences are trivial under a synchronous
 * submit: a job's fence is signalled as soon as submit returns. */
IOReturn
VivaGPUUserClient::submitJob(struct vgpu_submit * a)
{
	if (a->jc == 0 || a->bo_handle_count > VIVA_MAX_BOS) {
		return kIOReturnBadArgument;
	}
	IOReturn fr = flushCachedBOs(a);
	if (fr != kIOReturnSuccess) {
		return fr;
	}
	/* Run the job to completion. Low-level driver handles cache flush to PoC and the MMU. */
	uint32_t status = hakios_mali_submit(a->jc, (a->requirements & VGPU_JD_REQ_FS) != 0);
	/* GPU may have written WB cached BOs: FlushPoC_DcacheRegion is dc civac, so
	 * the CPU sees the GPU's output rather than stale cache lines. Never a bare
	 * dc ivac here (it would discard dirty CPU lines before they reach memory). */
	(void)flushCachedBOs(a);
	if (status != 0) {
		IOLog(VIVA_TAG "submit jc=%#llx failed status=%#x\n", (unsigned long long)a->jc, status);
		return kIOReturnError;
	}
	/* Signal the out_sync fence (job is done). */
	if (a->out_sync != 0) {
		IOLockLock(fLock);
		if (syncValid(a->out_sync)) {
			fSyncobjs[a->out_sync - 1].signalled = true;
		}
		IOLockUnlock(fLock);
	}
	a->seq = 0;   /* completion page not implemented; libdrm-xnu falls back to WAIT_BO */
	return kIOReturnSuccess;
}

/* Import an IOSurface plane as a BO, reusing our MiniIOSurface lookup. */
IOReturn
VivaGPUUserClient::importIOSurface(struct vgpu_import_iosurface * a)
{
	MiniIOSurface * s = MiniIOSurfaceCopy(a->surface_id);
	if (s == NULL) {
		return kIOReturnNotFound;
	}
	if (a->plane >= s->info.planeCount) {
		MiniIOSurfaceDrop(s);
		return kIOReturnBadArgument;
	}
	struct minisurf_plane * p = &s->planes[a->plane];
	uint64_t planeOff = p->offset;
	uint64_t size = round_page_64((uint64_t)p->size + planeOff);

	IOLockLock(fLock);
	uint32_t handle = 0;
	for (uint32_t n = 0; n < VIVA_MAX_BOS; n++) {
		uint32_t i = (fNextBO + n) % VIVA_MAX_BOS;
		if (fBOs[i].memory == NULL) { handle = i + 1; fNextBO = i + 2; break; }
	}
	if (handle == 0) { IOLockUnlock(fLock); MiniIOSurfaceDrop(s); return kIOReturnNoMemory; }

	/* MiniIOSurface's descriptor has a PAGE_SIZE header (kbase) before the pixels
	 * (kpixels = kbase + PAGE_SIZE), and plane offsets are relative to kpixels.
	 * The old loop asked getPhysicalSegment() for the SAME first page on every
	 * iteration (no "+ off"), so all GPU pages pointed at one physical page. */
	IOBufferMemoryDescriptor * mem = s->memory;   /* held by the MiniIOSurface */
	uint64_t base = PAGE_SIZE + (planeOff & ~0xfffull);
	uint64_t va = hakios_mali_address(size, 0);
	bool ok = va != 0;
	if (ok) {
		for (uint64_t off = 0; off < size && ok; off += 4096) {
			IOByteCount len = 0;
			addr64_t phys = mem->getPhysicalSegment(base + off, &len, kIOMemoryMapperNone);
			ok = phys != 0 && hakios_mali_map(va + off, phys);
		}
	}
	IOLog(VIVA_TAG "importIOSurface: surf=%u plane=%u off=%#llx base=%#llx size=%llu va=%#llx ok=%d\n",
	    a->surface_id, a->plane, (unsigned long long)planeOff, (unsigned long long)base,
	    (unsigned long long)size, (unsigned long long)va, ok);
	if (!ok) { if (va) hakios_mali_unmap(va, size); IOLockUnlock(fLock); MiniIOSurfaceDrop(s); return kIOReturnNoMemory; }
	hakios_mali_mapped();

	VivaBO * b = &fBOs[handle - 1];
	b->memory = mem;          /* we keep the MiniIOSurface alive via the ref (s->memory owned by it) */
	b->va = va;
	b->size = size;
	b->flags = VGPU_BO_WB_MMAP;
	b->imported = true;           /* memory owned by the MiniIOSurface */
	a->handle = handle;
	a->offset = va + planeOff;   /* GPU VA of plane's first byte */
	a->size = p->size;
	a->bytes_per_row = p->bytesPerRow;
	a->pixel_format = s->info.pixelFormat;
	a->width = p->width;
	a->height = p->height;
	IOLockUnlock(fLock);
	/* Note: we intentionally leak the MiniIOSurface ref (kept alive for the BO's lifetime), same
	 * as the existing HakiosPanfrostGPU::surfaceBuffer does. */
	return kIOReturnSuccess;
}

IOReturn
VivaGPUUserClient::dispatch(uint32_t selector, void * arg)
{
	switch (selector) {
	case VGPU_SEL_GET_VERSION: {
		struct vgpu_version * v = (struct vgpu_version *)arg;
		v->major = VGPU_UAPI_MAJOR;
		/* vgpu_iokit.c switches EVERY ioctl to IOConnectTrap2 when minor >= 1,
		 * but this user client has no getTargetAndTrapForIndex: Metal then
		 * aborts silently. Stay at 0 until trap/batch selectors are implemented. */
		v->minor = 0;
		v->gpu_id = fGPU->gpuId();
		v->page_size = (uint32_t)PAGE_SIZE;
		snprintf(v->name, sizeof(v->name), "Mali-G57 MC2");
		return kIOReturnSuccess;
	}
	case VGPU_SEL_GET_PARAM: {
		struct vgpu_get_param * a = (struct vgpu_get_param *)arg;
		if (!hakios_mali_parameter(a->param, &a->value)) {
			return kIOReturnBadArgument;
		}
		return kIOReturnSuccess;
	}
	case VGPU_SEL_CREATE_BO:
		return createBO((struct vgpu_create_bo *)arg);
	case VGPU_SEL_MMAP_BO: {
		struct vgpu_mmap_bo * a = (struct vgpu_mmap_bo *)arg;
		IOLockLock(fLock);
		VivaBO * b = bo(a->handle);
		if (b == NULL) { IOLockUnlock(fLock); return kIOReturnNotFound; }
		a->offset = a->handle;      /* IOConnectMapMemory64 memoryType = BO handle */
		IOLockUnlock(fLock);
		return kIOReturnSuccess;
	}
	case VGPU_SEL_GET_BO_OFFSET: {
		struct vgpu_get_bo_offset * a = (struct vgpu_get_bo_offset *)arg;
		IOLockLock(fLock);
		VivaBO * b = bo(a->handle);
		if (b == NULL) { IOLockUnlock(fLock); return kIOReturnNotFound; }
		a->offset = b->va;
		IOLockUnlock(fLock);
		return kIOReturnSuccess;
	}
	case VGPU_SEL_GEM_CLOSE: {
		struct vgpu_gem_close * a = (struct vgpu_gem_close *)arg;
		IOLockLock(fLock);
		VivaBO * b = bo(a->handle);
		if (b == NULL) { IOLockUnlock(fLock); return kIOReturnNotFound; }
		hakios_mali_unmap(b->va, b->size);
		if (!b->imported) {
			b->memory->release();
		}
		bzero(b, sizeof(*b));
		IOLockUnlock(fLock);
		return kIOReturnSuccess;
	}
	case VGPU_SEL_SUBMIT:
		return submitJob((struct vgpu_submit *)arg);
	case VGPU_SEL_WAIT_BO:
		/* Our submit is synchronous: the job is done before it returns, so a BO is always idle
		 * from the kernel's point of view. Return "free" immediately. */
		return kIOReturnSuccess;
	case VGPU_SEL_MADVISE: {
		struct vgpu_madvise * a = (struct vgpu_madvise *)arg;
		a->retained = 1;
		return kIOReturnSuccess;
	}
	case VGPU_SEL_SYNCOBJ_CREATE: {
		struct vgpu_syncobj_create * a = (struct vgpu_syncobj_create *)arg;
		IOLockLock(fLock);
		uint32_t h = 0;
		for (uint32_t i = 0; i < VIVA_MAX_SYNCOBJS; i++) {
			uint32_t idx = (fNextSync + i) % VIVA_MAX_SYNCOBJS;
			if (!fSyncobjs[idx].used) { h = idx + 1; fNextSync = idx + 2; break; }
		}
		if (h != 0) {
			fSyncobjs[h-1].used = true;
			fSyncobjs[h-1].signalled = (a->flags & VGPU_SYNCOBJ_CREATE_SIGNALED) != 0;
			a->handle = h;
		}
		IOLockUnlock(fLock);
		return h != 0 ? kIOReturnSuccess : kIOReturnNoMemory;
	}
	case VGPU_SEL_SYNCOBJ_DESTROY: {
		struct vgpu_syncobj_destroy * a = (struct vgpu_syncobj_destroy *)arg;
		IOLockLock(fLock);
		if (!syncValid(a->handle)) { IOLockUnlock(fLock); return kIOReturnBadArgument; }
		fSyncobjs[a->handle-1].used = false;
		fSyncobjs[a->handle-1].signalled = false;
		IOLockUnlock(fLock);
		return kIOReturnSuccess;
	}
	case VGPU_SEL_SYNCOBJ_WAIT: {
		struct vgpu_syncobj_wait * a = (struct vgpu_syncobj_wait *)arg;
		if (a->count_handles == 0 || a->count_handles > 512) return kIOReturnBadArgument;
		uint32_t handles[512];
		if (copyin((user_addr_t)a->handles, handles, a->count_handles * sizeof(uint32_t)) != 0) {
			return kIOReturnBadArgument;
		}
		IOLockLock(fLock);
		uint32_t signalled = 0, first = 0xffffffff;
		for (uint32_t i = 0; i < a->count_handles; i++) {
			if (syncValid(handles[i]) && fSyncobjs[handles[i]-1].signalled) {
				signalled++;
				if (i < first) first = i;
			}
		}
		IOLockUnlock(fLock);
		bool ok = (a->flags & VGPU_SYNCOBJ_WAIT_ALL) ? (signalled == a->count_handles) : (signalled > 0);
		if (ok) {
			a->first_signaled = (first == 0xffffffff) ? 0 : first;
			return kIOReturnSuccess;
		}
		return kIOReturnTimeout;
	}
	case VGPU_SEL_SYNCOBJ_RESET:
	case VGPU_SEL_SYNCOBJ_SIGNAL: {
		struct vgpu_syncobj_array * a = (struct vgpu_syncobj_array *)arg;
		if (a->count_handles == 0 || a->count_handles > 512) return kIOReturnBadArgument;
		uint32_t handles[512];
		if (copyin((user_addr_t)a->handles, handles, a->count_handles * sizeof(uint32_t)) != 0) {
			return kIOReturnBadArgument;
		}
		IOLockLock(fLock);
		for (uint32_t i = 0; i < a->count_handles; i++) {
			if (syncValid(handles[i])) {
				fSyncobjs[handles[i]-1].signalled = (selector == VGPU_SEL_SYNCOBJ_SIGNAL);
			}
		}
		IOLockUnlock(fLock);
		return kIOReturnSuccess;
	}
	case VGPU_SEL_SYNCOBJ_TRANSFER: {
		struct vgpu_syncobj_transfer * a = (struct vgpu_syncobj_transfer *)arg;
		IOLockLock(fLock);
		if (!syncValid(a->src) || !syncValid(a->dst)) { IOLockUnlock(fLock); return kIOReturnBadArgument; }
		fSyncobjs[a->dst-1].signalled = fSyncobjs[a->src-1].signalled;
		IOLockUnlock(fLock);
		return kIOReturnSuccess;
	}
	case VGPU_SEL_IMPORT_IOSURFACE:
		return importIOSurface((struct vgpu_import_iosurface *)arg);
	case VGPU_SEL_SYNC_BO:
		return kIOReturnSuccess;   /* our submit flushes to PoC already */
	case VGPU_SEL_QUERY_BO_INFO: {
		struct vgpu_query_bo_info * a = (struct vgpu_query_bo_info *)arg;
		VivaBO * b = bo(a->handle);
		if (b == NULL) return kIOReturnNotFound;
		a->create_flags = b->flags;
		a->extra_flags = 0;
		return kIOReturnSuccess;
	}
	case VGPU_SEL_JM_CTX_CREATE: {
		struct vgpu_jm_ctx * a = (struct vgpu_jm_ctx *)arg;
		a->handle = 1;
		return kIOReturnSuccess;
	}
	case VGPU_SEL_JM_CTX_DESTROY:
		return kIOReturnSuccess;
	default:
		IOLog(VIVA_TAG "unimplemented selector %u\n", selector);
		return kIOReturnUnsupported;
	}
}

IOReturn
VivaGPUUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments * args,
    __unused IOExternalMethodDispatch * dispatchTable, __unused OSObject * target, __unused void * reference)
{
	if (selector >= VGPU_SEL_COUNT) {
		IOLog(VIVA_TAG "selector %u out of range\n", selector);
		return kIOReturnBadArgument;
	}
	uint32_t size = viva_selector_size[selector];
	if (size > 512 || args->structureInput == NULL || args->structureInputSize < size) {
		IOLog(VIVA_TAG "selector %u bad input (size=%u have=%u)\n", selector, size,
		    args->structureInputSize);
		return kIOReturnBadArgument;
	}
	uint8_t buf[512];
	bcopy(args->structureInput, buf, size);
	IOReturn ret = dispatch(selector, buf);
	{
		static uint32_t ncall;
		if (ncall < 32) {
			ncall++;
			IOLog(VIVA_TAG "dispatch #%u sel=%u ret=%#x\n", ncall, selector, ret);
		}
	}
	if (ret != kIOReturnSuccess) {
		IOLog(VIVA_TAG "dispatch sel=%u FAILED ret=%#x\n", selector, ret);
	}
	if (args->structureOutput != NULL && args->structureOutputSize >= size) {
		bcopy(buf, args->structureOutput, size);
		args->structureOutputSize = size;
	}
	return ret;
}
