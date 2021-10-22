// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <android/sharedmem.h>
#include <asm/unistd.h>
#include <unistd.h>
#include "KPrivateMemory.h"
#include "KProcess.h"

namespace skyline::kernel::type {
    KPrivateMemory::KPrivateMemory(const DeviceState &state, u8 *ptr, size_t size, memory::Permission permission, memory::MemoryState memState) : ptr(ptr), size(size), permission(permission), memoryState(memState), KMemory(state, KType::KPrivateMemory) {
        if (!state.process->memory.base.IsInside(ptr) || !state.process->memory.base.IsInside(ptr + size))
            throw exception("KPrivateMemory allocation isn't inside guest address space: 0x{:X} - 0x{:X}", ptr, ptr + size);
        if (!util::IsPageAligned(ptr) || !util::IsPageAligned(size))
            throw exception("KPrivateMemory mapping isn't page-aligned: 0x{:X} - 0x{:X} (0x{:X})", ptr, ptr + size, size);

        if (mprotect(ptr, size, PROT_READ | PROT_WRITE | PROT_EXEC) < 0) // We only need to reprotect as the allocation has already been reserved by the MemoryManager
            throw exception("An occurred while mapping private memory: {} with 0x{:X} @ 0x{:X}", strerror(errno), ptr, size);

        state.process->memory.InsertChunk(ChunkDescriptor{
            .ptr = ptr,
            .size = size,
            .permission = permission,
            .state = memState,
        });
    }

    void KPrivateMemory::Resize(size_t nSize) {
        if (mprotect(ptr, nSize, PROT_READ | PROT_WRITE | PROT_EXEC) < 0)
            throw exception("An occurred while resizing private memory: {}", strerror(errno));

        if (nSize < size) {
            state.process->memory.InsertChunk(ChunkDescriptor{
                .ptr = ptr + nSize,
                .size = size - nSize,
                .state = memory::states::Unmapped,
            });
        } else if (size < nSize) {
            state.process->memory.InsertChunk(ChunkDescriptor{
                .ptr = ptr + size,
                .size = nSize - size,
                .permission = permission,
                .state = memoryState,
            });
        }

        size = nSize;
    }

    void KPrivateMemory::Remap(u8 *nPtr, size_t nSize) {
        if (!state.process->memory.base.IsInside(nPtr) || !state.process->memory.base.IsInside(nPtr + nSize))
            throw exception("KPrivateMemory remapping isn't inside guest address space: 0x{:X} - 0x{:X}", nPtr, nPtr + nSize);
        if (!util::IsPageAligned(nPtr) || !util::IsPageAligned(nSize))
            throw exception("KPrivateMemory remapping isn't page-aligned: 0x{:X} - 0x{:X} (0x{:X})", nPtr, nPtr + nSize, nSize);

        if (mprotect(ptr, size, PROT_NONE) < 0)
            throw exception("An occurred while remapping private memory: {}", strerror(errno));

        if (mprotect(nPtr, nSize, PROT_NONE) < 0)
            throw exception("An occurred while remapping private memory: {}", strerror(errno));
    }

    void KPrivateMemory::UpdatePermission(u8 *pPtr, size_t pSize, memory::Permission pPermission) {
        pPtr = std::clamp(pPtr, ptr, ptr + size);
        pSize = std::min(pSize, static_cast<size_t>((ptr + size) - pPtr));

        if (pPtr && !util::IsPageAligned(pPtr))
            throw exception("KPrivateMemory permission updated with a non-page-aligned address: 0x{:X}", pPtr);

        // If a static code region has been mapped as writable it needs to be changed to mutable
        if (memoryState == memory::states::CodeStatic && pPermission.w)
            memoryState = memory::states::CodeMutable;

        state.process->memory.InsertChunk(ChunkDescriptor{
            .ptr = pPtr,
            .size = pSize,
            .permission = pPermission,
            .state = memoryState,
        });
    }

    KPrivateMemory::~KPrivateMemory() {
        mprotect(ptr, size, PROT_NONE);
        state.process->memory.InsertChunk(ChunkDescriptor{
            .ptr = ptr,
            .size = size,
            .state = memory::states::Unmapped,
        });
    }
}
