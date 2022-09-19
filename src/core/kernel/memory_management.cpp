#include "kernel.hpp"

namespace Operation {
	enum : u32 {
		Free = 1,
		Reserve = 2,
		Commit = 3,
		Map = 4,
		Unmap = 5,
		Protect = 6,
		AppRegion = 0x100,
		SysRegion = 0x200,
		BaseRegion = 0x300,
		Linear = 0x10000
	};
}

// Returns whether "value" is aligned to a page boundary (Ie a boundary of 4096 bytes)
static constexpr bool isAligned(u32 value) {
	return (value & 0xFFF) == 0;
}

// Result ControlMemory(u32* outaddr, u32 addr0, u32 addr1, u32 size, 
//						MemoryOperation operation, MemoryPermission permissions)
// This has a weird ABI documented here https://www.3dbrew.org/wiki/Kernel_ABI
// TODO: Does this need to write to outaddr?
void Kernel::controlMemory() {
	u32 operation = regs[0]; // The base address is written here
	u32 addr0 = regs[1];
	u32 addr1 = regs[2];
	u32 size = regs[3];
	u32 perms = regs[4];

	if (perms == 0x10000000) {
		perms = 3; // We make "don't care" equivalent to read-write
		Helpers::panic("Unimplemented allocation permission: DONTCARE");
	}

	// Naturally the bits are in reverse order
	bool r = perms & 0b001;
	bool w = perms & 0b010;
	bool x = perms & 0b100;
	bool linear = operation & Operation::Linear;

	if (x)
		Helpers::panic("ControlMemory: attempted to allocate executable memory");

	if (!isAligned(addr0) || !isAligned(addr1) || !isAligned(size)) {
		Helpers::panic("ControlMemory: Unaligned parameters\nAddr0: %08X\nAddr1: %08X\nSize: %08X", addr0, addr1, size);
	}
	
	printf("ControlMemory(addr0 = %08X, addr1 = %08X, size = %08X, operation = %X (%c%c%c)%s\n",
			addr0, addr1, size, operation, r ? 'r' : '-', w ? 'w' : '-', x ? 'x' : '-', linear ? ", linear" : ""
	);

	switch (operation & 0xFF) {
		case Operation::Commit: {
			std::optional<u32> address = mem.allocateMemory(addr0, 0, size, linear, r, w, x, true);
			if (!address.has_value())
				Helpers::panic("ControlMemory: Failed to allocate memory");

			regs[1] = address.value();
			break;
		}

		default: Helpers::panic("ControlMemory: unknown operation %X\n", operation);
	}

	regs[0] = SVCResult::Success;
}

// Result QueryMemory(MemoryInfo* memInfo, PageInfo* pageInfo, u32 addr)
// TODO: Is this SVC supposed to write to memory or...?
void Kernel::queryMemory() {
	const u32 memInfo = regs[0];
	const u32 pageInfo = regs[1];
	const u32 addr = regs[2];

	if (addr & 0xfff) {
		Helpers::panic("QueryMemory: Address not page aligned\n");
	}

	printf("QueryMemory(mem info pointer = %08X, page info pointer = %08X, addr = %08X)\n", memInfo, pageInfo, addr);

	const auto info = mem.queryMemory(addr);
	regs[0] = SVCResult::Success;
	regs[1] = info.baseAddr;
	regs[2] = info.size;
	regs[3] = info.perms;
	regs[4] = info.state;
	regs[5] = 0; // page flags

	/*
	mem.write32(memInfo, info.baseAddr); // Set memInfo->baseVaddr
	mem.write32(memInfo + 4, info.size); // Set memInfo->size
	mem.write32(memInfo + 8, info.perms); // Set memInfo->perms
	mem.write32(memInfo + 12, info.state); // Set memInfo->state
	mem.write32(pageInfo, 0); // Set pageInfo->flags to 0
	*/
}

// Result MapMemoryBlock(Handle memblock, u32 addr, MemoryPermission myPermissions, MemoryPermission otherPermission)	
void Kernel::mapMemoryBlock() {
	const Handle block = regs[0];
	const u32 addr = regs[1];
	const u32 myPerms = regs[2];
	const u32 otherPerms = regs[3];
	printf("MapMemoryBlock(block = %d, addr = %08X, myPerms = %X, otherPerms = %X\n", block, addr, myPerms, otherPerms);

	if (!isAligned(addr)) [[unlikely]] {
		Helpers::panic("MapMemoryBlock: Unaligned address");
	}

	if (block == KernelHandles::GSPSharedMemHandle) {
		mem.mapGSPSharedMemory(addr, myPerms, otherPerms);
	} else {
		Helpers::panic("MapMemoryBlock where the handle does not refer to GSP memory");
	}

	regs[0] = SVCResult::Success;
}