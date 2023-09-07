#include "services/ldr_ro.hpp"
#include "ipc.hpp"

#include <cstdio>
#include <string>

namespace LDRCommands {
	enum : u32 {
		Initialize = 0x000100C2,
		LoadCRR = 0x00020082,
		LoadCRONew = 0x000902C2,
	};
}

namespace CROHeader {
	enum : u32 {
		ID = 0x080,
		NameOffset = 0x084,
		NextCRO = 0x088,
		PrevCRO = 0x08C,
		CodeOffset = 0x0B0,
		DataOffset = 0x0B8,
		ModuleNameOffset = 0x0C0,
		SegmentTableOffset = 0x0C8,
		SegmentTableSize = 0x0CC,
		NamedExportTableOffset = 0x0D0,
		NamedExportTableSize = 0x0D4,
		IndexedExportTableOffset = 0x0D8,
		IndexedExportTableSize = 0x0DC,
		ExportStringTableOffset = 0x0E0,
		ExportStringSize = 0x0E4,
		ExportTreeOffset = 0x0E8,
		ImportModuleTableOffset = 0x0F0,
		ImportModuleTableSize = 0x0F4,
		ImportPatchTableOffset = 0x0F8,
		ImportPatchTableSize = 0x0FC,
		NamedImportTableOffset = 0x100,
		NamedImportTableSize = 0x104,
		IndexedImportTableOffset = 0x108,
		IndexedImportTableSize = 0x10C,
		AnonymousImportTableOffset = 0x110,
		AnonymousImportTableSize = 0x114,
		ImportStringTableOffset = 0x118,
		ImportStringSize = 0x11C,
		StaticAnonymousSymbolTableOffset = 0x120,
		StaticAnonymousSymbolTableSize = 0x124,
		RelocationPatchTableOffset = 0x128,
		RelocationPatchTableSize = 0x12C,
		StaticAnonymousPatchTableOffset = 0x130,
		StaticAnonymousPatchTableSize = 0x134,
	};
}

namespace SegmentTable {
	enum : u32 {
		Offset = 0,
		Size = 4,
		ID = 8,
	};

	namespace SegmentID {
		enum : u32 {
			TEXT, RODATA, DATA, BSS,
		};
	}
}

namespace NamedExportTable {
	enum : u32 {
		NameOffset = 0,
		SegmentOffset = 4,
	};
};

namespace NamedImportTable {
	enum : u32 {
		NameOffset = 0,
		RelocationOffset = 4,
	};
};

namespace IndexedImportTable {
	enum : u32 {
		Index = 0,
		RelocationOffset = 4,
	};
};

namespace AnonymousImportTable {
	enum : u32 {
		SegmentOffset = 0,
		RelocationOffset = 4,
	};
};

namespace ImportModuleTable {
	enum : u32 {
		NameOffset = 0,
		IndexedOffset = 8,
		AnonymousOffset = 16,
	};
};

namespace RelocationPatch {
	enum : u32 {
		SegmentOffset = 0,
		PatchType = 4,
		IsLastEntry = 5,  // For import patches
		SegmentIndex = 5, // For relocation patches
		IsResolved = 6,
		Addend = 8,
	};

	namespace RelocationPatchType {
		enum : u32 {
			AbsoluteAddress = 2,
		};
	};
};

struct CROHeaderEntry {
	u32 offset, size;
};

static constexpr u32 CRO_HEADER_SIZE = 0x138;

class CRO {
	Memory &mem;

	u32 croPointer; // Origin address of CRO in RAM

	bool isCRO; // False if CRS

public:
	CRO(Memory &mem, u32 croPointer, bool isCRO) : mem(mem), croPointer(croPointer), isCRO(isCRO) {}
	~CRO() = default;

	u32 getNextCRO() {
		return mem.read32(croPointer + CROHeader::NextCRO);
	}
	
	u32 getPrevCRO() {
		return mem.read32(croPointer + CROHeader::PrevCRO);
	}

	void setNextCRO(u32 nextCRO) {
		mem.write32(croPointer + CROHeader::NextCRO, nextCRO);
	}

	void setPrevCRO(u32 prevCRO) {
		mem.write32(croPointer + CROHeader::PrevCRO, prevCRO);
	}

	// Returns CRO header offset-size pair
	CROHeaderEntry getHeaderEntry(u32 entry) {
		return CROHeaderEntry{.offset = mem.read32(croPointer + entry), .size = mem.read32(croPointer + entry + 4)};
	}

	u32 getSegmentAddr(u32 segmentOffset) {
		// "Decoded" segment tag
		const u32 segmentIndex = segmentOffset & 0xF;
		const u32 offset = segmentOffset >> 4;

		const CROHeaderEntry segmentTable = getHeaderEntry(CROHeader::SegmentTableOffset);

		// Safeguard
		if (segmentIndex >= segmentTable.size) {
			Helpers::panic("Invalid segment index = %u (table size = %u)", segmentIndex, segmentTable.size);
		}

		// Get segment table entry
		const u32 entryOffset = mem.read32(segmentTable.offset + 12 * segmentIndex + SegmentTable::Offset);
		const u32 entrySize = mem.read32(segmentTable.offset + 12 * segmentIndex + SegmentTable::Size);

		// Another safeguard
		if (offset >= entrySize) {
			Helpers::panic("Invalid segment entry offset = %u (entry size = %u)", offset, entrySize);
		}

		return entryOffset + offset;
	}

	u32 getNamedExportSymbolAddr(const std::string& symbolName) {
		// Note: The CRO contains a trie for fast symbol lookup. For simplicity,
		// we won't use it and instead look up the symbol in the named export symbol table

		const u32 exportStringSize = mem.read32(croPointer + CROHeader::ExportStringSize);

		const CROHeaderEntry namedExportTable = getHeaderEntry(CROHeader::NamedExportTableOffset);

		for (u32 namedExport = 0; namedExport < namedExportTable.size; namedExport++) {
			const u32 nameOffset = mem.read32(namedExportTable.offset + 8 * namedExport + NamedExportTable::NameOffset);
				
			const std::string exportSymbolName = mem.readString(nameOffset, exportStringSize);

			if (symbolName.compare(exportSymbolName) == 0) {
				return getSegmentAddr(mem.read32(namedExportTable.offset + 8 * namedExport + NamedExportTable::SegmentOffset));
			}
		}

		return 0;
	}

	// Patches one symbol
	bool patchSymbol(u32 relocationTarget, u8 patchType, u32 addend, u32 symbolOffset) {
		switch (patchType) {
			case RelocationPatch::RelocationPatchType::AbsoluteAddress: mem.write32(relocationTarget, symbolOffset + addend); break;
			default: Helpers::panic("Unhandled relocation type = %X\n", patchType);
		}

		return true;
	}

	// Patches symbol batches
	bool patchBatch(u32 batchAddr, u32 symbolAddr) {
		u32 relocationPatch = batchAddr;

		while (true) {
			const u32 segmentOffset = mem.read32(relocationPatch + RelocationPatch::SegmentOffset);
			const u8 patchType = mem.read8(relocationPatch + RelocationPatch::PatchType);
			const u8 isLastBatch = mem.read8(relocationPatch + RelocationPatch::IsLastEntry);
			const u32 addend = mem.read32(relocationPatch + RelocationPatch::Addend);

			const u32 relocationTarget = getSegmentAddr(segmentOffset);

			if (relocationTarget == 0) {
				Helpers::panic("Relocation target is NULL");
			}

			patchSymbol(relocationTarget, patchType, addend, symbolAddr);

			if (isLastBatch != 0) {
				break;
			}

			relocationPatch += 12;
		}

		mem.write8(relocationPatch + RelocationPatch::IsResolved, 1);

		return true;
	}

	bool load() {
		// TODO: verify SHA hashes?

		// Verify CRO magic
		const std::string magic = mem.readString(croPointer + CROHeader::ID, 4);
		if (magic.compare(std::string("CRO0")) != 0) {
			return false;
		}

		// These fields are initially 0, the RO service sets them on load. If non-0,
		// this CRO has already been loaded
		if ((getNextCRO() != 0) || (getPrevCRO() != 0)) {
			return false;
		}

		return true;
	}

	// Modifies CRO offsets to point at virtual addresses
	bool rebase(u32 loadedCRS, u32 mapVaddr, u32 dataVaddr, u32 bssVaddr) {
		rebaseHeader(mapVaddr);

		u32 oldDataVaddr = 0;

		// Note: Citra rebases the segment table only if the file is not a CRS.
		// Presumably because CRS files don't contain segments?
		if (isCRO) {
			rebaseSegmentTable(mapVaddr, dataVaddr, bssVaddr, &oldDataVaddr);
		}

		rebaseNamedExportTable(mapVaddr);
		rebaseImportModuleTable(mapVaddr);
		rebaseNamedImportTable(mapVaddr);
		rebaseIndexedImportTable(mapVaddr);
		rebaseAnonymousImportTable(mapVaddr);

		relocateInternalSymbols(oldDataVaddr);

		// Note: Citra relocates static anonymous symbols and exit symbols only if the file is not a CRS
		if (isCRO) {
			relocateStaticAnonymousSymbols();
			relocateExitSymbols(loadedCRS);
		}

		return true;
	}

	bool rebaseHeader(u32 mapVaddr) {
		constexpr u32 headerOffsets[] = {
			CROHeader::NameOffset,
			CROHeader::CodeOffset,
			CROHeader::DataOffset,
			CROHeader::ModuleNameOffset,
			CROHeader::SegmentTableOffset,
			CROHeader::NamedExportTableOffset,
			CROHeader::IndexedExportTableOffset,
			CROHeader::ExportStringTableOffset,
			CROHeader::ExportTreeOffset,
			CROHeader::ImportModuleTableOffset,
			CROHeader::ImportPatchTableOffset,
			CROHeader::NamedImportTableOffset,
			CROHeader::IndexedImportTableOffset,
			CROHeader::AnonymousImportTableOffset,
			CROHeader::ImportStringTableOffset,
			CROHeader::StaticAnonymousSymbolTableOffset,
			CROHeader::RelocationPatchTableOffset,
			CROHeader::StaticAnonymousPatchTableOffset,
		};

		for (u32 offset : headerOffsets) {
			mem.write32(croPointer + offset, mem.read32(croPointer + offset) + mapVaddr);
		}

		return true;
	}

	bool rebaseSegmentTable(u32 mapVaddr, u32 dataVaddr, u32 bssVaddr, u32 *oldDataVaddr) {
		const CROHeaderEntry segmentTable = getHeaderEntry(CROHeader::SegmentTableOffset);

		for (u32 segment = 0; segment < segmentTable.size; segment++) {
			u32 segmentOffset = mem.read32(segmentTable.offset + 12 * segment + SegmentTable::Offset);

			const u32 segmentID = mem.read32(segmentTable.offset + 12 * segment + SegmentTable::ID);
			switch (segmentID) {
				case SegmentTable::SegmentID::DATA:
					*oldDataVaddr = segmentOffset + dataVaddr; segmentOffset = dataVaddr; break;
				case SegmentTable::SegmentID::BSS: segmentOffset = bssVaddr; break;
				case SegmentTable::SegmentID::TEXT:
				case SegmentTable::SegmentID::RODATA:
					segmentOffset += mapVaddr; break;
				default:
					Helpers::panic("Unknown segment ID = %u", segmentID);
			}

			mem.write32(segmentTable.offset + 12 * segment + SegmentTable::Offset, segmentOffset);
		}

		return true;
	}

	bool rebaseNamedExportTable(u32 mapVaddr) {
		const CROHeaderEntry namedExportTable = getHeaderEntry(CROHeader::NamedExportTableOffset);

		for (u32 namedExport = 0; namedExport < namedExportTable.size; namedExport++) {
			u32 nameOffset = mem.read32(namedExportTable.offset + 8 * namedExport);

			if (nameOffset != 0) {
				mem.write32(namedExportTable.offset + 8 * namedExport, nameOffset + mapVaddr);
			}
		}

		return true;
	}

	bool rebaseImportModuleTable(u32 mapVaddr) {
		const CROHeaderEntry importModuleTable = getHeaderEntry(CROHeader::ImportModuleTableOffset);

		for (u32 importModule = 0; importModule < importModuleTable.size; importModule++) {
			u32 nameOffset = mem.read32(importModuleTable.offset + 20 * importModule + ImportModuleTable::NameOffset);

			if (nameOffset != 0) {
				mem.write32(importModuleTable.offset + 20 * importModule + ImportModuleTable::NameOffset, nameOffset + mapVaddr);
			}

			u32 indexedOffset = mem.read32(importModuleTable.offset + 20 * importModule + ImportModuleTable::IndexedOffset);

			if (indexedOffset != 0) {
				mem.write32(importModuleTable.offset + 20 * importModule + ImportModuleTable::IndexedOffset, indexedOffset + mapVaddr);
			}

			u32 anonymousOffset = mem.read32(importModuleTable.offset + 20 * importModule + ImportModuleTable::AnonymousOffset);

			if (anonymousOffset != 0) {
				mem.write32(importModuleTable.offset + 20 * importModule + ImportModuleTable::AnonymousOffset, anonymousOffset + mapVaddr);
			}
		}

		return true;
	}

	bool rebaseNamedImportTable(u32 mapVaddr) {
		const CROHeaderEntry namedImportTable = getHeaderEntry(CROHeader::NamedImportTableOffset);

		for (u32 namedImport = 0; namedImport < namedImportTable.size; namedImport++) {
			u32 nameOffset = mem.read32(namedImportTable.offset + 8 * namedImport + NamedImportTable::NameOffset);

			if (nameOffset != 0) {
				mem.write32(namedImportTable.offset + 8 * namedImport + NamedImportTable::NameOffset, nameOffset + mapVaddr);
			}

			u32 relocationOffset = mem.read32(namedImportTable.offset + 8 * namedImport + NamedImportTable::RelocationOffset);

			if (relocationOffset != 0) {
				mem.write32(namedImportTable.offset + 8 * namedImport + NamedImportTable::RelocationOffset, relocationOffset + mapVaddr);
			}
		}

		return true;
	}

	bool rebaseIndexedImportTable(u32 mapVaddr) {
		const CROHeaderEntry indexedImportTable = getHeaderEntry(CROHeader::IndexedImportTableOffset);

		for (u32 indexedImport = 0; indexedImport < indexedImportTable.size; indexedImport++) {
			u32 relocationOffset = mem.read32(indexedImportTable.offset + 8 * indexedImport + IndexedImportTable::RelocationOffset);

			if (relocationOffset != 0) {
				mem.write32(indexedImportTable.offset + 8 * indexedImport + IndexedImportTable::RelocationOffset, relocationOffset + mapVaddr);
			}
		}

		return true;
	}

	bool rebaseAnonymousImportTable(u32 mapVaddr) {
		const CROHeaderEntry anonymousImportTable = getHeaderEntry(CROHeader::AnonymousImportTableOffset);

		for (u32 anonymousImport = 0; anonymousImport < anonymousImportTable.size; anonymousImport++) {
			u32 relocationOffset = mem.read32(anonymousImportTable.offset + 8 * anonymousImport + AnonymousImportTable::RelocationOffset);

			if (relocationOffset != 0) {
				mem.write32(anonymousImportTable.offset + 8 * anonymousImport + AnonymousImportTable::RelocationOffset, relocationOffset + mapVaddr);
			}
		}

		return true;
	}

	bool relocateInternalSymbols(u32 oldDataVaddr) {
		const u8* header = (u8*)mem.getReadPointer(croPointer);

		const CROHeaderEntry relocationPatchTable = getHeaderEntry(CROHeader::RelocationPatchTableOffset);
		const CROHeaderEntry segmentTable = getHeaderEntry(CROHeader::SegmentTableOffset);

		for (u32 relocationPatch = 0; relocationPatch < relocationPatchTable.size; relocationPatch++) {
			const u32 segmentOffset = mem.read32(relocationPatchTable.offset + 12 * relocationPatch + RelocationPatch::SegmentOffset);
			const u8 patchType = mem.read8(relocationPatchTable.offset + 12 * relocationPatch + RelocationPatch::PatchType);
			const u8 segmentIndex = mem.read8(relocationPatchTable.offset + 12 * relocationPatch + RelocationPatch::SegmentIndex);
			const u32 addend = mem.read32(relocationPatchTable.offset + 12 * relocationPatch + RelocationPatch::Addend);

			const u32 segmentAddr = getSegmentAddr(segmentOffset);

			const u32 entryID = mem.read32(segmentTable.offset + 12 * (segmentOffset & 0xF) + SegmentTable::ID);

			u32 relocationTarget = segmentAddr;
			if (entryID == SegmentTable::SegmentID::DATA) {
				// Recompute relocation target for .data
				relocationTarget = oldDataVaddr + (segmentOffset >> 4);
			}

			if (relocationTarget == 0) {
				Helpers::panic("Relocation target is NULL");
			}

			const u32 symbolOffset = mem.read32(segmentTable.offset + 12 * segmentIndex + SegmentTable::Offset);

			patchSymbol(relocationTarget, patchType, addend, symbolOffset);
		}

		return true;
	}

	bool relocateStaticAnonymousSymbols() {
		const CROHeaderEntry staticAnonymousSymbolTable = getHeaderEntry(CROHeader::StaticAnonymousSymbolTableOffset);

		for (u32 symbol = 0; symbol < staticAnonymousSymbolTable.size; symbol++) {
			Helpers::panic("TODO: relocate static anonymous symbols");
		}

		return true;
	}

	// Patches "__aeabi_atexit" symbol to "nnroAeabiAtexit_"
	bool relocateExitSymbols(u32 loadedCRS) {
		if (loadedCRS == 0) {
			Helpers::panic("CRS not loaded");
		}

		const u32 importStringSize = mem.read32(croPointer + CROHeader::ImportStringSize);

		const CROHeaderEntry namedImportTable = getHeaderEntry(CROHeader::NamedImportTableOffset);

		for (u32 namedImport = 0; namedImport < namedImportTable.size; namedImport++) {
			const u32 nameOffset = mem.read32(namedImportTable.offset + 8 * namedImport + NamedImportTable::NameOffset);
			const u32 relocationOffset = mem.read32(namedImportTable.offset + 8 * namedImport + NamedImportTable::RelocationOffset);
				
			const std::string symbolName = mem.readString(nameOffset, importStringSize);

			if (symbolName.compare(std::string("__aeabi_atexit")) == 0) {
				// Find exit symbol in other CROs
				u32 currentCROPointer = loadedCRS;
				while (currentCROPointer != 0) {
					CRO cro(mem, currentCROPointer, true);

					const u32 exportSymbolAddr = cro.getNamedExportSymbolAddr(std::string("nnroAeabiAtexit_"));
					if (exportSymbolAddr != 0) {
						patchBatch(relocationOffset, exportSymbolAddr);
						
						return true;
					}

					currentCROPointer = cro.getNextCRO();
				}
			}
		}

		Helpers::warn("Failed to relocate exit symbols");

		return false;
	}

	bool importNamedSymbols(u32 loadedCRS) {
		if (loadedCRS == 0) {
			Helpers::panic("CRS not loaded");
		}

		const u32 importStringSize = mem.read32(croPointer + CROHeader::ImportStringSize);

		const CROHeaderEntry namedImportTable = getHeaderEntry(CROHeader::NamedImportTableOffset);

		for (u32 namedImport = 0; namedImport < namedImportTable.size; namedImport++) {
			const u32 relocationOffset = mem.read32(namedImportTable.offset + 8 * namedImport + NamedImportTable::RelocationOffset);

			u8 isResolved = mem.read8(relocationOffset + RelocationPatch::IsResolved);

			if (isResolved == 0) {
				const u32 nameOffset = mem.read32(namedImportTable.offset + 8 * namedImport + NamedImportTable::NameOffset);
				
				const std::string symbolName = mem.readString(nameOffset, importStringSize);

				// Check every loaded CRO for the symbol (the pain)
				u32 currentCROPointer = loadedCRS;
				while (currentCROPointer != 0) {
					CRO cro(mem, currentCROPointer, true);

					const u32 exportSymbolAddr = cro.getNamedExportSymbolAddr(symbolName);
					if (exportSymbolAddr != 0) {
						patchBatch(relocationOffset, exportSymbolAddr);

						isResolved = 1;
						break;
					}

					currentCROPointer = cro.getNextCRO();
				}

				if (isResolved == 0) {
					Helpers::panic("Failed to resolve symbol %s", symbolName.c_str());
				}

				mem.write8(relocationOffset + RelocationPatch::IsResolved, 1);
			}
		}

		return true;
	}

	bool importModules(u32 loadedCRS) {
		if (loadedCRS == 0) {
			Helpers::panic("CRS not loaded");
		}

		const u32 importStringSize = mem.read32(croPointer + CROHeader::ImportStringSize);

		const CROHeaderEntry importModuleTable = getHeaderEntry(CROHeader::ImportModuleTableOffset);

		for (u32 importModule = 0; importModule < importModuleTable.size; importModule++) {
			Helpers::panic("TODO: import modules");
		}

		return true;
	}

	// Links CROs. Heavily based on Citra's CRO linker
	bool link(u32 loadedCRS) {
		if (loadedCRS == 0) {
			Helpers::panic("CRS not loaded");
		}

		const CROHeaderEntry segmentTable = getHeaderEntry(CROHeader::SegmentTableOffset);

		// Fix data segment offset (LoadCRO_New)
		// Note: the old LoadCRO does *not* fix .data
		u32 dataVaddr;
		if (segmentTable.size > 1) {
			// Note: ldr:ro assumes that segment index 2 is .data
			dataVaddr = mem.read32(segmentTable.offset + 24 + SegmentTable::Offset);

			mem.write32(segmentTable.offset + 24 + SegmentTable::Offset, mem.read32(croPointer + CROHeader::DataOffset));
		}

		importNamedSymbols(loadedCRS);
		importModules(loadedCRS);

		// TODO: export symbols to other CROs

		// Restore .data segment offset (LoadCRO_New)
		if (segmentTable.size > 1) {
			mem.write32(segmentTable.offset + 24 + SegmentTable::Offset, dataVaddr);
		}

		return true;
	}

	// Adds CRO to the linked list of loaded CROs
	void registerCRO(u32 loadedCRS, bool autoLink) {
		if (loadedCRS == 0) {
			Helpers::panic("CRS not loaded");
		}

		CRO crs(mem, loadedCRS, false);
		
		u32 headAddr = crs.getPrevCRO();
		if (autoLink) {
			headAddr = crs.getNextCRO();
		}

		if (headAddr == 0) {
			// Register first CRO
			setPrevCRO(croPointer);

			if (autoLink) {
				crs.setNextCRO(croPointer);
			} else {
				crs.setPrevCRO(croPointer);
			}
		} else {
			// Register new CRO
			CRO head(mem, headAddr, true);

			if (head.getPrevCRO() == 0) {
				Helpers::panic("No tail CRO found");
			}

			CRO tail(mem, head.getPrevCRO(), true);

			setPrevCRO(tail.croPointer);

			tail.setNextCRO(croPointer);
			head.setPrevCRO(croPointer);
		}
	}
};

void LDRService::reset() {
	loadedCRS = 0;
}

void LDRService::handleSyncRequest(u32 messagePointer) {
	const u32 command = mem.read32(messagePointer);
	switch (command) {
		case LDRCommands::Initialize: initialize(messagePointer); break;
		case LDRCommands::LoadCRR: loadCRR(messagePointer); break;
		case LDRCommands::LoadCRONew: loadCRONew(messagePointer); break;
		default: Helpers::panic("LDR::RO service requested. Command: %08X\n", command);
	}
}

void LDRService::initialize(u32 messagePointer) {
	const u32 crsPointer = mem.read32(messagePointer + 4);
	const u32 size = mem.read32(messagePointer + 8);
	const u32 mapVaddr = mem.read32(messagePointer + 12);
	const Handle process = mem.read32(messagePointer + 20);

	log("LDR_RO::Initialize (buffer = %08X, size = %08X, vaddr = %08X, process = %X)\n", crsPointer, size, mapVaddr, process);

	// Sanity checks
	if (loadedCRS != 0) {
		Helpers::panic("CRS already loaded\n");
	}

	if (size < CRO_HEADER_SIZE) {
		Helpers::panic("CRS too small\n");
	}

	if ((size & mem.pageMask) != 0) {
		Helpers::panic("Unaligned CRS size\n");
	}

	if ((crsPointer & mem.pageMask) != 0) {
		Helpers::panic("Unaligned CRS pointer\n");
	}

	if ((mapVaddr & mem.pageMask) != 0) {
		Helpers::panic("Unaligned CRS output vaddr\n");
	}

	// Map CRO to output address
	mem.mirrorMapping(mapVaddr, crsPointer, size);

	CRO crs(mem, crsPointer, false);

	if (!crs.load()) {
		Helpers::panic("Failed to load CRS");
	}

	if (!crs.rebase(0, mapVaddr, 0, 0)) {
		Helpers::panic("Failed to rebase CRS");
	}

	loadedCRS = crsPointer;

	mem.write32(messagePointer, IPC::responseHeader(0x1, 1, 0));
	mem.write32(messagePointer + 4, Result::Success);
}

void LDRService::loadCRR(u32 messagePointer) {
	const u32 crrPointer = mem.read32(messagePointer + 4);
	const u32 size = mem.read32(messagePointer + 8);
	const Handle process = mem.read32(messagePointer + 20);

	log("LDR_RO::LoadCRR (buffer = %08X, size = %08X, process = %X)\n", crrPointer, size, process);
	mem.write32(messagePointer, IPC::responseHeader(0x2, 1, 0));
	mem.write32(messagePointer + 4, Result::Success);
}

void LDRService::loadCRONew(u32 messagePointer) {
	const u32 croPointer = mem.read32(messagePointer + 4);
	const u32 mapVaddr = mem.read32(messagePointer + 8);
	const u32 size = mem.read32(messagePointer + 12);
	const u32 dataVaddr = mem.read32(messagePointer + 16);
	const u32 dataSize = mem.read32(messagePointer + 24);
	const u32 bssVaddr = mem.read32(messagePointer + 28);
	const u32 bssSize = mem.read32(messagePointer + 32);
	const bool autoLink = mem.read32(messagePointer + 36) != 0;
	const u32 fixLevel = mem.read32(messagePointer + 40);
	const Handle process = mem.read32(messagePointer + 52);

	log("LDR_RO::LoadCRONew (buffer = %08X, vaddr = %08X, size = %08X, .data vaddr = %08X, .data size = %08X, .bss vaddr = %08X, .bss size = %08X, auto link = %d, fix level = %X, process = %X)\n", croPointer, mapVaddr, size, dataVaddr, dataSize, bssVaddr, bssSize, autoLink, fixLevel, process);

	// Sanity checks
	if (size < CRO_HEADER_SIZE) {
		Helpers::panic("CRO too small\n");
	}

	if ((size & mem.pageMask) != 0) {
		Helpers::panic("Unaligned CRO size\n");
	}

	if ((croPointer & mem.pageMask) != 0) {
		Helpers::panic("Unaligned CRO pointer\n");
	}

	if ((mapVaddr & mem.pageMask) != 0) {
		Helpers::panic("Unaligned CRO output vaddr\n");
	}

	// Map CRO to output address
	mem.mirrorMapping(mapVaddr, croPointer, size);

	CRO cro(mem, croPointer, true);

	if (!cro.load()) {
		Helpers::panic("Failed to load CRO");
	}

	if (!cro.rebase(loadedCRS, mapVaddr, dataVaddr, bssVaddr)) {
		Helpers::panic("Failed to rebase CRO");
	}

	if (!cro.link(loadedCRS)) {
		Helpers::panic("Failed to link CRO");
	}

	cro.registerCRO(loadedCRS, autoLink);

	// TODO: add fixing

	mem.write32(messagePointer, IPC::responseHeader(0x9, 2, 0));
	mem.write32(messagePointer + 4, Result::Success);
	mem.write32(messagePointer + 8, size);
}