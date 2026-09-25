
#include <iostream>
#include <fstream>
#include <format>
#include <filesystem>

#include "Unreal/ObjectArray.h"
#include "OffsetFinder/Offsets.h"
#include "Settings.h"
#include "Utils.h"

#include "Platform.h"


namespace fs = std::filesystem;

constexpr inline std::array FFixedUObjectArrayLayouts =
{
	FFixedUObjectArrayLayout // Default UE4.11 - UE4.20
	{
		.ObjectsOffset = 0x0,								// 0x00
		.MaxObjectsOffset = sizeof(void*),					// 0x08 (64bit) OR 0x04 (32bit)
		.NumObjectsOffset = sizeof(void*) + sizeof(int)		// 0x0C (64bit) OR 0x08 (32bit)
	}
};

constexpr inline std::array FChunkedFixedUObjectArrayLayouts =
{
	FChunkedFixedUObjectArrayLayout // Default UE4.21 - UE5.7
	{
		.ObjectsOffset = 0x00,
		.MaxElementsOffset = 0x10,
		.NumElementsOffset = 0x14,
		.MaxChunksOffset = 0x18,
		.NumChunksOffset = 0x1C,
	},
	FChunkedFixedUObjectArrayLayout // UE5.8 Developement Build
	{
		.ObjectsOffset = 0x00, 
		.MaxElementsOffset = 0x0C,
		.NumElementsOffset = 0x08,
		.MaxChunksOffset = 0x14,
		.NumChunksOffset = 0x10,
	},
	FChunkedFixedUObjectArrayLayout // Back4Blood
	{
		.ObjectsOffset = 0x10, // last
		.MaxElementsOffset = 0x00,
		.NumElementsOffset = 0x04,
		.MaxChunksOffset = 0x08,
		.NumChunksOffset = 0x0C,
	},
	FChunkedFixedUObjectArrayLayout // Mutliversus
	{
		.ObjectsOffset = 0x18,
		.MaxElementsOffset = 0x10,
		.NumElementsOffset = 0x00, // first
		.MaxChunksOffset = 0x14,
		.NumChunksOffset = 0x20,
	},
	FChunkedFixedUObjectArrayLayout // MindsEye
	{
		.ObjectsOffset = 0x18,
		.MaxElementsOffset = 0x00, // first
		.NumElementsOffset = 0x14,
		.MaxChunksOffset = 0x10,
		.NumChunksOffset = 0x04,
	}
};

namespace
{
	bool ObjectMatchesFastUnsafe(UEObject Object, const std::string& Name, EClassCastFlags RequiredType)
	{
		return Object && Object.IsA(RequiredType) && Object.GetName() == Name;
	}

	bool TryObjectMatchesFast(UEObject Object, const std::string& Name, EClassCastFlags RequiredType)
	{
#if defined(_MSC_VER)
		__try
		{
			return ObjectMatchesFastUnsafe(Object, Name, RequiredType);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
#else
		return ObjectMatchesFastUnsafe(Object, Name, RequiredType);
#endif
	}

	bool IsCurrentDumpObjectUnsafe(UEObject Object, int32 ExpectedIndex)
	{
		const auto* Address = static_cast<const uint8*>(Object.GetAddress());
		if (Address == nullptr)
			return false;

		const int32 Index = *reinterpret_cast<const int32*>(Address + Off::UObject::Index);
		if (Index < 0 || Index >= ObjectArray::Num() || (ExpectedIndex >= 0 && Index != ExpectedIndex))
			return false;

		const auto* Class = *reinterpret_cast<uint8* const*>(Address + Off::UObject::Class);
		if (Class == nullptr)
			return false;

		(void)*reinterpret_cast<const uint64*>(Address + Off::UObject::Name);
		(void)*reinterpret_cast<uint8* const*>(Address + Off::UObject::Outer);
		(void)*reinterpret_cast<const EClassCastFlags*>(Class + Off::UClass::CastFlags);
		return ObjectArray::GetByIndex(Index).GetAddress() == Object.GetAddress();
	}

	bool IsCurrentDumpObject(UEObject Object, int32 ExpectedIndex = -1)
	{
		if (ObjectArray::UsesExternalObjectAccess())
		{
#if defined(_MSC_VER)
			__try
			{
				return IsCurrentDumpObjectUnsafe(Object, ExpectedIndex);
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
#else
			return IsCurrentDumpObjectUnsafe(Object, ExpectedIndex);
#endif
		}

		const auto* Address = static_cast<const uint8*>(Object.GetAddress());
		if (Address == nullptr ||
			Platform::IsBadReadPtr(Address + Off::UObject::Index) ||
			Platform::IsBadReadPtr(Address + Off::UObject::Index + sizeof(int32) - 1) ||
			Platform::IsBadReadPtr(Address + Off::UObject::Class) ||
			Platform::IsBadReadPtr(Address + Off::UObject::Class + sizeof(void*) - 1) ||
			Platform::IsBadReadPtr(Address + Off::UObject::Name) ||
			Platform::IsBadReadPtr(Address + Off::UObject::Name + sizeof(uint64) - 1) ||
			Platform::IsBadReadPtr(Address + Off::UObject::Outer) ||
			Platform::IsBadReadPtr(Address + Off::UObject::Outer + sizeof(void*) - 1))
		{
			return false;
		}

		const int32 Index = *reinterpret_cast<const int32*>(Address + Off::UObject::Index);
		if (Index < 0 || Index >= ObjectArray::Num() || (ExpectedIndex >= 0 && Index != ExpectedIndex))
			return false;

		const auto* Class = *reinterpret_cast<uint8* const*>(Address + Off::UObject::Class);
		if (Class == nullptr ||
			Platform::IsBadReadPtr(Class) ||
			Platform::IsBadReadPtr(Class + Off::UClass::CastFlags) ||
			Platform::IsBadReadPtr(Class + Off::UClass::CastFlags + sizeof(EClassCastFlags) - 1))
		{
			return false;
		}

		return ObjectArray::GetByIndex(Index).GetAddress() == Object.GetAddress();
	}

	bool BuildObjectDumpNameUnsafe(UEObject Object, bool bWithPathname, std::string* OutName)
	{
		if (OutName == nullptr || !IsCurrentDumpObject(Object))
			return false;

		const UEClass Class = Object.GetClass();
		if (!IsCurrentDumpObject(Class))
			return false;

		std::string OuterPath;
		UEObject Outer = Object.GetOuter();
		constexpr int32 MaxOuterDepth = 0x100;
		int32 OuterDepth = 0;

		for (; Outer && OuterDepth < MaxOuterDepth; Outer = Outer.GetOuter(), ++OuterDepth)
		{
			if (!IsCurrentDumpObject(Outer))
				return false;

			OuterPath = (bWithPathname ? Outer.GetNameWithPath() : Outer.GetName()) + "." + OuterPath;
		}

		if (Outer)
			return false;

		std::string Name = bWithPathname ? Class.GetNameWithPath() : Class.GetName();
		Name += " ";
		Name += OuterPath;
		Name += bWithPathname ? Object.GetNameWithPath() : Object.GetName();
		*OutName = std::move(Name);
		return true;
	}

	bool TryBuildObjectDumpName(UEObject Object, bool bWithPathname, std::string* OutName)
	{
#if defined(_MSC_VER)
		__try
		{
			return BuildObjectDumpNameUnsafe(Object, bWithPathname, OutName);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
#else
		return BuildObjectDumpNameUnsafe(Object, bWithPathname, OutName);
#endif
	}

	bool BuildPropertyDumpLineUnsafe(UEProperty Property, std::string* OutLine)
	{
		if (OutLine == nullptr || !Property)
			return false;

		*OutLine = std::format(
			"[{:08X}] {{{}}}     {} {}\n",
			Property.GetOffset(),
			Property.GetAddress(),
			Property.GetPropClassName(),
			Property.GetName());
		return true;
	}

	bool TryBuildPropertyDumpLine(UEProperty Property, std::string* OutLine)
	{
#if defined(_MSC_VER)
		__try
		{
			return BuildPropertyDumpLineUnsafe(Property, OutLine);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
#else
		return BuildPropertyDumpLineUnsafe(Property, OutLine);
#endif
	}
}

bool IsAddressValidGObjects(const uintptr_t Address, const FFixedUObjectArrayLayout& Layout)
{
	/* It is assumed that the FUObjectItem layout is constant amongst all games using FFixedUObjectArray for ObjObjects. */
	struct FUObjectItem
	{
		void* Object;
		uint8_t Pad[sizeof(void*) * 2];
	};

	void* Objects = *reinterpret_cast<void**>(Address + Layout.ObjectsOffset);
	const int32 MaxElements = *reinterpret_cast<const int32*>(Address + Layout.MaxObjectsOffset);
	const int32 NumElements = *reinterpret_cast<const int32*>(Address + Layout.NumObjectsOffset);

	FUObjectItem* ObjectsButDecrypted = reinterpret_cast<FUObjectItem*>(ObjectArray::DecryptPtr(Objects));

	if (NumElements > MaxElements)
		return false;

	if (MaxElements > 0x400000)
		return false;

	if (NumElements < 0x1000)
		return false;

	if (Platform::IsBadReadPtr(ObjectsButDecrypted))
		return false;

	if (Platform::IsBadReadPtr(ObjectsButDecrypted[5].Object))
		return false;

	const uintptr_t FifthObject = reinterpret_cast<uintptr_t>(ObjectsButDecrypted[0x5].Object);
	const int32 IndexOfFithobject = *reinterpret_cast<int32_t*>(FifthObject + sizeof(void*) + sizeof(int32)); // FifthObject -> InternalIndex

	if (IndexOfFithobject != 0x5)
		return false;

	return true;
}

bool IsAddressValidGObjects(const uintptr_t Address, const FChunkedFixedUObjectArrayLayout& Layout)
{
	if (Platform::IsBadReadPtr(Address))
		return false;

	void* Objects = *reinterpret_cast<void**>(Address + Layout.ObjectsOffset);
	const int32 MaxElements = *reinterpret_cast<const int32*>(Address + Layout.MaxElementsOffset);
	const int32 NumElements = *reinterpret_cast<const int32*>(Address + Layout.NumElementsOffset);
	const int32 MaxChunks   = *reinterpret_cast<const int32*>(Address + Layout.MaxChunksOffset);
	const int32 NumChunks   = *reinterpret_cast<const int32*>(Address + Layout.NumChunksOffset);

	void** ObjectsPtrButDecrypted = reinterpret_cast<void**>(ObjectArray::DecryptPtr(Objects));

	if (NumChunks < 0x1 || NumChunks > 0x5FF)
		return false;

	if (MaxChunks < NumChunks || MaxChunks > 0x5FF)
		return false;

	if (NumElements <= 0x800 || MaxElements < 0x10000)
		return false;

	if (NumElements > MaxElements || NumChunks > MaxChunks)
		return false;

	if ((MaxElements % 0x10) != 0)
		return false;

	const int32_t ElementsPerChunk = MaxElements / MaxChunks;

	if ((ElementsPerChunk % 0x10) != 0)
		return false;

	if (ElementsPerChunk < 0x8000 || ElementsPerChunk > 0x80000)
		return false;

	const int32 ExpectedNumChunks = (NumElements + ElementsPerChunk - 1) / ElementsPerChunk;
	const bool bNumChunksFitsNumElements = ExpectedNumChunks == NumChunks;

	if (!bNumChunksFitsNumElements)
		return false;

	const bool bMaxChunksFitsMaxElements = (MaxElements / ElementsPerChunk) == MaxChunks;

	if (!bMaxChunksFitsMaxElements)
		return false;

	if (!ObjectsPtrButDecrypted || Platform::IsBadReadPtr(ObjectsPtrButDecrypted))
		return false;

	for (int i = 0; i < NumChunks; i++)
	{
		if (!ObjectsPtrButDecrypted[i] || Platform::IsBadReadPtr(ObjectsPtrButDecrypted[i]))
			return false;
	}

	return true;
}


bool ObjectArray::InitializeFUObjectItem(uint8_t* FirstItemPtr)
{
	FUObjectItemInitialOffset = 0x0;
	SizeOfFUObjectItem = 0x0;

	if (FirstItemPtr == nullptr || Platform::IsBadReadPtr(FirstItemPtr))
		return false;

	bool bFoundObjectOffset = false;
	for (int i = 0x0; i < 0x20; i += 4)
	{
		if (!Platform::IsBadReadPtr(*reinterpret_cast<uint8_t**>(FirstItemPtr + i)))
		{
			FUObjectItemInitialOffset = i;
			bFoundObjectOffset = true;
			break;
		}
	}

	if (!bFoundObjectOffset)
		return false;

	bool bFoundItemSize = false;
	for (int i = FUObjectItemInitialOffset + sizeof(void*); i <= 0x38; i += 4)
	{
		void* SecondObject = *reinterpret_cast<uint8**>(FirstItemPtr + i);
		void* ThirdObject  = *reinterpret_cast<uint8**>(FirstItemPtr + (i * 2) - FUObjectItemInitialOffset);

		if (!Platform::IsBadReadPtr(SecondObject) && !Platform::IsBadReadPtr(*reinterpret_cast<void**>(SecondObject)) &&
			!Platform::IsBadReadPtr(ThirdObject) && !Platform::IsBadReadPtr(*reinterpret_cast<void**>(ThirdObject)))
		{
			SizeOfFUObjectItem = i - FUObjectItemInitialOffset;
			bFoundItemSize = true;
			break;
		}
	}

	if (!bFoundItemSize)
		return false;

	Off::InSDK::ObjArray::FUObjectItemInitialOffset = FUObjectItemInitialOffset;
	Off::InSDK::ObjArray::FUObjectItemSize = SizeOfFUObjectItem;

	std::cerr << "Off::InSDK::ObjArray::FUObjectItemSize: " << Off::InSDK::ObjArray::FUObjectItemSize << "\n" << std::endl;
	return true;
}

void ObjectArray::InitDecryption(uint8_t* (*DecryptionFunction)(void* ObjPtr), const char* DecryptionLambdaAsStr)
{
	DecryptPtr = DecryptionFunction;
	DecryptionLambdaStr = DecryptionLambdaAsStr;
}


/* We don't speak about this function... */
bool ObjectArray::Init(bool bScanAllMemory, const char* const ModuleName)
{
	GObjects = nullptr;
	ByIndex = nullptr;
	ExternalObjectCount = nullptr;
	ExternalObjectLookup = nullptr;
	bUsesExternalObjectAccess = false;
	bExternalLayoutValidated = false;
	InitializationError.clear();

	if (!bScanAllMemory)
	{
		std::cerr << "\nDumper-7 by me, you & him\n\n\n";
		std::cerr << "Searching for GObjects...\n\n";
	}

	auto MatchesAnyLayout = []<typename ArrayLayoutType, size_t Size>(const std::array<ArrayLayoutType, Size>& ObjectArrayLayouts, uintptr_t Address)
	{
		for (const ArrayLayoutType& Layout : ObjectArrayLayouts)
		{
			if (!IsAddressValidGObjects(Address, Layout))
				continue;

			if constexpr (std::is_same_v<ArrayLayoutType, FFixedUObjectArrayLayout>)
			{
				Off::FUObjectArray::bIsChunked = false;
				Off::FUObjectArray::FixedLayout = Layout;
			}
			else
			{
				Off::FUObjectArray::bIsChunked = true;
				Off::FUObjectArray::ChunkedFixedLayout = Layout;
			}

			return true;
		}
		
		return false;
	};

	bool bIsGObjectsChunked = false;
	auto IsAddressValidGObjects = [MatchesAnyLayout, &bIsGObjectsChunked](const void* CurrentAddress) -> bool
	{
		//std::cerr << "checking addr: " << CurrentAddress << "\n";
		if (MatchesAnyLayout(FFixedUObjectArrayLayouts, reinterpret_cast<uintptr_t>(CurrentAddress)))
		{
			bIsGObjectsChunked = false;
			return true;
		}
		else if (MatchesAnyLayout(FChunkedFixedUObjectArrayLayouts, reinterpret_cast<uintptr_t>(CurrentAddress)))
		{
			bIsGObjectsChunked = true;
			return true;
		}

		return false;
	};

	void* GObjectsAddress = nullptr;

	if (bScanAllMemory)
	{
		GObjectsAddress = Platform::IterateAllSectionsWithCallback(IsAddressValidGObjects, 0x4, 0x50, ModuleName);
	}
	else
	{
		GObjectsAddress = Platform::IterateSectionWithCallback(Platform::GetSectionInfo(".data"), IsAddressValidGObjects, 0x4, 0x50);
	}


	if (GObjectsAddress)
	{
		if (!bIsGObjectsChunked)
		{
			GObjects = static_cast<uint8*>(GObjectsAddress);
			NumElementsPerChunk = -1;

			Off::InSDK::ObjArray::GObjects = Platform::GetOffset(GObjectsAddress);

			std::cerr << "Found FFixedUObjectArray GObjects at offset 0x" << std::hex << Off::InSDK::ObjArray::GObjects << "\n\n";

			ByIndex = [](void* ObjectsArray, int32 Index, uint32 FUObjectItemSize, uint32 FUObjectItemOffset, uint32 PerChunk) -> void*
			{
				if (Index < 0 || Index >= Num())
					return nullptr;

				uint8_t* ChunkPtr = DecryptPtr(*reinterpret_cast<uint8_t**>(ObjectsArray));

				return *reinterpret_cast<void**>(ChunkPtr + FUObjectItemOffset + (Index * FUObjectItemSize));
			};

			uint8_t* FirstItem = DecryptPtr(*reinterpret_cast<uint8_t**>(GObjects + Off::FUObjectArray::GetObjectsOffset()));

			if (!ObjectArray::InitializeFUObjectItem(FirstItem))
			{
				InitializationError = "The selected fixed FUObjectArray layout did not contain valid FUObjectItems";
			}
		}
		else
		{
			GObjects = static_cast<uint8*>(GObjectsAddress);
			
			NumElementsPerChunk = Max() / MaxChunks();
			Off::InSDK::ObjArray::ChunkSize = NumElementsPerChunk;

			SizeOfFUObjectItem = sizeof(void*) + sizeof(int32) + sizeof(int32);
			FUObjectItemInitialOffset = 0x0;

			Off::InSDK::ObjArray::GObjects = Platform::GetOffset(GObjectsAddress);

			std::cerr << "Found FChunkedFixedUObjectArray GObjects at offset 0x" << std::hex << Off::InSDK::ObjArray::GObjects << "\n\n";

			ByIndex = [](void* ObjectsArray, int32 Index, uint32 FUObjectItemSize, uint32 FUObjectItemOffset, uint32 PerChunk) -> void*
			{
				if (Index < 0 || Index >= Num())
					return nullptr;

				const int32 ChunkIndex = Index / PerChunk;
				const int32 InChunkIdx = Index % PerChunk;

				uint8_t* ChunkPtr = DecryptPtr(*reinterpret_cast<uint8_t**>(ObjectsArray));

				uint8_t* Chunk = reinterpret_cast<uint8_t**>(ChunkPtr)[ChunkIndex];
				uint8_t* ItemPtr = Chunk + (InChunkIdx * FUObjectItemSize);

				return *reinterpret_cast<void**>(ItemPtr + FUObjectItemOffset);
			};
			
			uint8_t* ChunksPtr = DecryptPtr(*reinterpret_cast<uint8_t**>(GObjects + Off::FUObjectArray::GetObjectsOffset()));

			if (ChunksPtr == nullptr || Platform::IsBadReadPtr(ChunksPtr) ||
				!ObjectArray::InitializeFUObjectItem(*reinterpret_cast<uint8_t**>(ChunksPtr)))
			{
				InitializationError = "The selected chunked FUObjectArray layout did not contain valid FUObjectItems";
			}
		}

		if (InitializationError.empty())
			return true;

		std::cerr << "Dumper-7: " << InitializationError << "\n" << std::endl;
		GObjects = nullptr;
		ByIndex = nullptr;

		if (!bScanAllMemory)
			return ObjectArray::Init(true, ModuleName);

		return false;
	}

	if (!bScanAllMemory)
	{
		return ObjectArray::Init(true, ModuleName);
	}

	InitializationError = "GObjects could not be found with a validated FUObjectArray layout";
	std::cerr << "\nDumper-7: " << InitializationError << "\n\n";
	return false;
}

bool ObjectArray::InitWithExternalAccess(void* RawGObjects, int32 ExpectedObjectCount, uint32 ItemStride, ExternalObjectCountFn CountFn, ExternalObjectLookupFn LookupFn)
{
	GObjects = nullptr;
	ByIndex = nullptr;
	ExternalObjectCount = nullptr;
	ExternalObjectLookup = nullptr;
	bUsesExternalObjectAccess = false;
	bExternalLayoutValidated = false;
	InitializationError.clear();

	if (CountFn == nullptr || LookupFn == nullptr || ExpectedObjectCount <= 0 || ItemStride < sizeof(void*))
	{
		InitializationError = "UEVR object accessor did not provide a valid object count, lookup function, or item stride";
		return false;
	}

	const int32 ActualObjectCount = CountFn();
	if (ActualObjectCount != ExpectedObjectCount)
	{
		InitializationError = "UEVR object accessor returned an unstable object count";
		return false;
	}

	ExternalObjectCount = CountFn;
	ExternalObjectLookup = LookupFn;
	bUsesExternalObjectAccess = true;
	SizeOfFUObjectItem = ItemStride;
	FUObjectItemInitialOffset = 0x0;
	NumElementsPerChunk = 0x10000;
	Off::InSDK::ObjArray::FUObjectItemSize = ItemStride;
	Off::InSDK::ObjArray::FUObjectItemInitialOffset = 0x0;
	Off::InSDK::ObjArray::ChunkSize = NumElementsPerChunk;

	if (RawGObjects == nullptr || Platform::IsBadReadPtr(RawGObjects))
	{
		InitializationError = "UEVR object accessor did not expose a readable raw FUObjectArray address";
		return true;
	}

	GObjects = static_cast<uint8*>(RawGObjects);
	Off::InSDK::ObjArray::GObjects = Platform::GetOffset(RawGObjects);

	for (const FChunkedFixedUObjectArrayLayout& Layout : FChunkedFixedUObjectArrayLayouts)
	{
		if (!IsAddressValidGObjects(reinterpret_cast<uintptr_t>(RawGObjects), Layout))
			continue;

		const int32 RawObjectCount = *reinterpret_cast<const int32*>(GObjects + Layout.NumElementsOffset);
		if (RawObjectCount != ExpectedObjectCount)
			continue;

		Off::FUObjectArray::bIsChunked = true;
		Off::FUObjectArray::ChunkedFixedLayout = Layout;
		const int32 RawMaxChunks = *reinterpret_cast<const int32*>(GObjects + Layout.MaxChunksOffset);
		const int32 RawMaxElements = *reinterpret_cast<const int32*>(GObjects + Layout.MaxElementsOffset);
		if (RawMaxChunks > 0 && RawMaxElements > 0)
			NumElementsPerChunk = static_cast<uint32>(RawMaxElements / RawMaxChunks);
		Off::InSDK::ObjArray::ChunkSize = NumElementsPerChunk;
		bExternalLayoutValidated = true;
		break;
	}

	return true;
}

void ObjectArray::Init(int32 GObjectsOffset, const FFixedUObjectArrayLayout& ObjectArrayLayout, const char* const ModuleName)
{
	bUsesExternalObjectAccess = false;
	bExternalLayoutValidated = false;
	ExternalObjectCount = nullptr;
	ExternalObjectLookup = nullptr;
	GObjects = reinterpret_cast<uint8_t*>(Platform::GetModuleBase(ModuleName) + GObjectsOffset);
	Off::InSDK::ObjArray::GObjects = GObjectsOffset;

	std::cerr << "GObjects: 0x" << (void*)GObjects << "\n" << std::endl;

	Off::FUObjectArray::bIsChunked = false;
	Off::FUObjectArray::FixedLayout = ObjectArrayLayout.IsValid() ? ObjectArrayLayout : FFixedUObjectArrayLayouts[0];

	ByIndex = [](void* ObjectsArray, int32 Index, uint32 FUObjectItemSize, uint32 FUObjectItemOffset, uint32 PerChunk) -> void*
	{
		if (Index < 0 || Index >= Num())
			return nullptr;

		uint8_t* ItemPtr = *reinterpret_cast<uint8_t**>(ObjectsArray) + (Index * FUObjectItemSize);

		return *reinterpret_cast<void**>(ItemPtr + FUObjectItemOffset);
	};

	uint8_t* ChunksPtr = DecryptPtr(*reinterpret_cast<uint8_t**>(GObjects + Off::FUObjectArray::GetObjectsOffset()));

	std::cerr << "Overwrote FFixedUObjectArray GObjects to offset 0x" << std::hex << Off::InSDK::ObjArray::GObjects << "\n" << std::endl;

	ObjectArray::InitializeFUObjectItem(*reinterpret_cast<uint8_t**>(ChunksPtr));
}

void ObjectArray::Init(int32 GObjectsOffset, int32 ElementsPerChunk, const FChunkedFixedUObjectArrayLayout& ObjectArrayLayout, const char* const ModuleName)
{
	bUsesExternalObjectAccess = false;
	bExternalLayoutValidated = false;
	ExternalObjectCount = nullptr;
	ExternalObjectLookup = nullptr;
	GObjects = reinterpret_cast<uint8_t*>(Platform::GetModuleBase(ModuleName) + GObjectsOffset);
	Off::InSDK::ObjArray::GObjects = GObjectsOffset;

	Off::FUObjectArray::bIsChunked = true;
	Off::FUObjectArray::ChunkedFixedLayout = ObjectArrayLayout.IsValid() ? ObjectArrayLayout : FChunkedFixedUObjectArrayLayouts[0];

	NumElementsPerChunk = ElementsPerChunk;
	Off::InSDK::ObjArray::ChunkSize = ElementsPerChunk;

	ByIndex = [](void* ObjectsArray, int32 Index, uint32 FUObjectItemSize, uint32 FUObjectItemOffset, uint32 PerChunk) -> void*
	{
		if (Index < 0 || Index >= Num())
			return nullptr;

		const int32 ChunkIndex = Index / PerChunk;
		const int32 InChunkIdx = Index % PerChunk;

		uint8_t* Chunk = (*reinterpret_cast<uint8_t***>(ObjectsArray))[ChunkIndex];
		uint8_t* ItemPtr = reinterpret_cast<uint8_t*>(Chunk) + (InChunkIdx * FUObjectItemSize);

		return *reinterpret_cast<void**>(ItemPtr + FUObjectItemOffset);
	};

	uint8_t* ChunksPtr = DecryptPtr(*reinterpret_cast<uint8_t**>(GObjects + Off::FUObjectArray::GetObjectsOffset()));

	std::cerr << "Overwrote FChunkedFixedUObjectArray GObjects to offset 0x" << std::hex << Off::InSDK::ObjArray::GObjects << "\n" << std::endl;

	ObjectArray::InitializeFUObjectItem(*reinterpret_cast<uint8_t**>(ChunksPtr));
}

void ObjectArray::DumpObjects(const fs::path& Path, bool bWithPathname)
{
	std::ofstream DumpStream(Path / "GObjects-Dump.txt");

	DumpStream << "Object dump by Dumper-7\n\n";
	DumpStream << (!Settings::Generator::GameVersion.empty() && !Settings::Generator::GameName.empty() ? (Settings::Generator::GameVersion + '-' + Settings::Generator::GameName) + "\n\n" : "");
	DumpStream << "Count: " << Num() << "\n\n\n";

	const int32 TotalObjects = Num();
	for (int32 Index = 0; Index < TotalObjects; ++Index)
	{
		const UEObject Object = GetByIndex(Index);
		if (!IsCurrentDumpObject(Object, Index))
			continue;

		std::string ObjectName;
		if (TryBuildObjectDumpName(Object, bWithPathname, &ObjectName))
			DumpStream << std::format("[{:08X}] {{{}}} {}\n", Index, Object.GetAddress(), ObjectName);
	}

	DumpStream.close();
}

void ObjectArray::DumpObjectsWithProperties(const fs::path& Path, bool bWithPathname)
{
	std::ofstream DumpStream(Path / "GObjects-Dump-WithProperties.txt");

	DumpStream << "Object dump by Dumper-7\n\n";
	DumpStream << (!Settings::Generator::GameVersion.empty() && !Settings::Generator::GameName.empty() ? (Settings::Generator::GameVersion + '-' + Settings::Generator::GameName) + "\n\n" : "");
	DumpStream << "Count: " << Num() << "\n\n\n";

	const int32 TotalObjects = Num();
	for (int32 Index = 0; Index < TotalObjects; ++Index)
	{
		const UEObject Object = GetByIndex(Index);
		if (!IsCurrentDumpObject(Object, Index))
			continue;

		std::string ObjectName;
		if (!TryBuildObjectDumpName(Object, bWithPathname, &ObjectName))
			continue;

		DumpStream << std::format("[{:08X}] {{{}}} {}\n", Index, Object.GetAddress(), ObjectName);

		if (Object.IsA(EClassCastFlags::Struct))
		{
			for (UEProperty Prop : Object.Cast<UEStruct>().GetProperties())
			{
				std::string PropertyLine;
				if (TryBuildPropertyDumpLine(Prop, &PropertyLine))
					DumpStream << PropertyLine;
			}
		}
	}

	DumpStream.close();
}


int32 ObjectArray::Num()
{
	if (bUsesExternalObjectAccess)
		return ExternalObjectCount != nullptr ? ExternalObjectCount() : 0;

	if (GObjects == nullptr)
		return 0;

	return *reinterpret_cast<int32*>(GObjects + Off::FUObjectArray::GetNumElementsOffset());
}

int32 ObjectArray::Max()
{
	if (bUsesExternalObjectAccess)
		return Num();

	if (GObjects == nullptr)
		return 0;

	return *reinterpret_cast<int32*>(GObjects + Off::FUObjectArray::GetMaxElementsOffset());
}

int32 ObjectArray::NumChunks()
{
	if (bUsesExternalObjectAccess)
		return NumElementsPerChunk == 0 ? 0 : (Num() + static_cast<int32>(NumElementsPerChunk) - 1) / static_cast<int32>(NumElementsPerChunk);

	if (GObjects == nullptr)
		return 0;

	return *reinterpret_cast<int32*>(GObjects + Off::FUObjectArray::GetNumChunksOffset());
}

int32 ObjectArray::MaxChunks()
{
	if (bUsesExternalObjectAccess)
		return NumChunks();

	if (GObjects == nullptr)
		return 0;

	return *reinterpret_cast<int32*>(GObjects + Off::FUObjectArray::GetMaxChunksOffset());
}

std::string ObjectArray::GetInitializationSummary()
{
	if (!IsInitialized())
		return "unresolved";

	if (bUsesExternalObjectAccess)
	{
		return std::format(
			"UEVR-backed GObjects=0x{:X} count={} item_size=0x{:X} chunk_size=0x{:X} raw_layout={}",
			Off::InSDK::ObjArray::GObjects,
			Num(),
			Off::InSDK::ObjArray::FUObjectItemSize,
			Off::InSDK::ObjArray::ChunkSize,
			bExternalLayoutValidated ? "validated" : "unavailable");
	}

	if (!Off::FUObjectArray::bIsChunked)
	{
		return std::format(
			"fixed GObjects=0x{:X} objects=0x{:X} num=0x{:X} item_size=0x{:X} item_object=0x{:X}",
			Off::InSDK::ObjArray::GObjects,
			Off::FUObjectArray::FixedLayout.ObjectsOffset,
			Off::FUObjectArray::FixedLayout.NumObjectsOffset,
			Off::InSDK::ObjArray::FUObjectItemSize,
			Off::InSDK::ObjArray::FUObjectItemInitialOffset);
	}

	return std::format(
		"chunked GObjects=0x{:X} objects=0x{:X} num=0x{:X} max=0x{:X} num_chunks=0x{:X} max_chunks=0x{:X} chunk_size=0x{:X} item_size=0x{:X} item_object=0x{:X}",
		Off::InSDK::ObjArray::GObjects,
		Off::FUObjectArray::ChunkedFixedLayout.ObjectsOffset,
		Off::FUObjectArray::ChunkedFixedLayout.NumElementsOffset,
		Off::FUObjectArray::ChunkedFixedLayout.MaxElementsOffset,
		Off::FUObjectArray::ChunkedFixedLayout.NumChunksOffset,
		Off::FUObjectArray::ChunkedFixedLayout.MaxChunksOffset,
		Off::InSDK::ObjArray::ChunkSize,
		Off::InSDK::ObjArray::FUObjectItemSize,
		Off::InSDK::ObjArray::FUObjectItemInitialOffset);
}

template<typename UEType>
static UEType ObjectArray::GetByIndex(int32 Index)
{
	if (Index < 0 || Index >= Num())
		return UEType();

	if (bUsesExternalObjectAccess)
	{
		if (ExternalObjectLookup == nullptr)
			return UEType();

		return UEType(ExternalObjectLookup(Index));
	}

	if (ByIndex == nullptr || GObjects == nullptr)
		return UEType();

	return UEType(ByIndex(GObjects + Off::FUObjectArray::GetObjectsOffset(), Index, SizeOfFUObjectItem, FUObjectItemInitialOffset, NumElementsPerChunk));
}

template<typename UEType>
UEType ObjectArray::FindObject(const std::string& FullName, EClassCastFlags RequiredType)
{
	for (UEObject Object : ObjectArray())
	{
		if (Object.IsA(RequiredType) && Object.GetFullName() == FullName)
		{
			return Object.Cast<UEType>();
		}
	}

	return UEType();
}

template<typename UEType>
UEType ObjectArray::FindObjectFast(const std::string& Name, EClassCastFlags RequiredType)
{
	auto ObjArray = ObjectArray();
	// UEVR snapshots can retain stale entries while a dump is running. Class
	// lookups are infrequent, so guard them for every externally supplied array
	// instead of allowing one stale class pointer to abort SDK generation.
	const bool bGuardExternalSnapshot = bUsesExternalObjectAccess;

	for (UEObject Object : ObjArray)
	{
		const bool bMatches = bGuardExternalSnapshot
			? TryObjectMatchesFast(Object, Name, RequiredType)
			: (Object.IsA(RequiredType) && Object.GetName() == Name);

		if (bMatches)
		{
			return Object.Cast<UEType>();
		}
	}

	return UEType();
}

template<typename UEType>
static UEType ObjectArray::FindObjectFastInOuter(const std::string& Name, std::string Outer)
{
	auto ObjArray = ObjectArray();

	for (UEObject Object : ObjArray)
	{
		if (Object.GetName() == Name && Object.GetOuter().GetName() == Outer)
		{
			return Object.Cast<UEType>();
		}
	}

	return UEType();
}

UEStruct ObjectArray::FindStruct(const std::string& Name)
{
	return FindObjectFast<UEClass>(Name, EClassCastFlags::Struct);
}

UEStruct ObjectArray::FindStructFast(const std::string& Name)
{
	return FindObjectFast<UEClass>(Name, EClassCastFlags::Struct);
}

UEClass ObjectArray::FindClass(const std::string& FullName)
{
	return FindObject<UEClass>(FullName, EClassCastFlags::Class);
}

UEClass ObjectArray::FindClassFast(const std::string& Name)
{
	return FindObjectFast<UEClass>(Name, EClassCastFlags::Class);
}

ObjectArray::ObjectsIterator ObjectArray::begin()
{
	return ObjectsIterator();
}
ObjectArray::ObjectsIterator ObjectArray::end()
{
	return ObjectsIterator(Num());
}


ObjectArray::ObjectsIterator::ObjectsIterator(int32 StartIndex)
	: CurrentIndex(StartIndex), CurrentObject(ObjectArray::GetByIndex(StartIndex))
{
	// A UEVR-backed array can be sparse. Never yield a null first element to
	// discovery code that immediately dereferences the iterator value.
	while (!CurrentObject && CurrentIndex < ObjectArray::Num())
	{
		++CurrentIndex;
		if (CurrentIndex < ObjectArray::Num())
			CurrentObject = ObjectArray::GetByIndex(CurrentIndex);
	}
}

UEObject ObjectArray::ObjectsIterator::operator*() const
{
	return CurrentObject;
}

ObjectArray::ObjectsIterator& ObjectArray::ObjectsIterator::operator++()
{
	CurrentObject = ObjectArray::GetByIndex(++CurrentIndex);

	while (!CurrentObject && CurrentIndex < (ObjectArray::Num() - 1))
	{
		CurrentObject = ObjectArray::GetByIndex(++CurrentIndex);
	}

	if (!CurrentObject && CurrentIndex == (ObjectArray::Num() - 1)) [[unlikely]]
		CurrentIndex++;

	return *this;
}

bool ObjectArray::ObjectsIterator::operator==(const ObjectsIterator& Other) const
{
	return CurrentIndex == Other.CurrentIndex;
}

bool ObjectArray::ObjectsIterator::operator!=(const ObjectsIterator& Other) const
{
	return CurrentIndex != Other.CurrentIndex;
}

int32 ObjectArray::ObjectsIterator::GetIndex() const
{
	return CurrentIndex;
}

bool AllFieldIterator::operator!=(const AllFieldIterator& Other) const
{
	return CurrentObject != Other.CurrentObject || PropertyIndex != Other.PropertyIndex;
}

AllFieldIterator& AllFieldIterator::operator++()
{
	if (CurrenStructHasMoreMembers())
	{
		PropertyIndex++;

		return *this;
	}

	IterateToNextStructWithMembers();

	return *this;
}

UEProperty AllFieldIterator::operator*() const
{
	return Fields[PropertyIndex];
}


void AllFieldIterator::IterateToNextStruct()
{
	if (IsEndIterator())
		return;

	++CurrentObject;

	while (CurrentObject != ObjectEndIterator && !IsCurrentObjectStruct())
		++CurrentObject;
}
void AllFieldIterator::IterateToNextStructWithMembers()
{
	// Loop, in case we meet a struct wihtout any properties
	while (!CurrenStructHasMoreMembers())
	{
		IterateToNextStruct();
		PropertyIndex = 0;

		if (IsEndIterator())
			return;

		Fields = GetCurrentStruct().GetProperties();
	}
}


/*
* The compiler won't generate functions for a specific template type unless it's used in the .cpp file corresponding to the
* header it was declatred in.
*
* See https://stackoverflow.com/questions/456713/why-do-i-get-unresolved-external-symbol-errors-when-using-templates
*/
template UEObject ObjectArray::FindObject<UEObject>(const std::string& FullName, EClassCastFlags RequiredType);
template UEField ObjectArray::FindObject<UEField>(const std::string& FullName, EClassCastFlags RequiredType);
template UEEnum ObjectArray::FindObject<UEEnum>(const std::string& FullName, EClassCastFlags RequiredType);
template UEStruct ObjectArray::FindObject<UEStruct>(const std::string& FullName, EClassCastFlags RequiredType);
template UEClass ObjectArray::FindObject<UEClass>(const std::string& FullName, EClassCastFlags RequiredType);
template UEFunction ObjectArray::FindObject<UEFunction>(const std::string& FullName, EClassCastFlags RequiredType);
template UEProperty ObjectArray::FindObject<UEProperty>(const std::string& FullName, EClassCastFlags RequiredType);
template UEByteProperty ObjectArray::FindObject<UEByteProperty>(const std::string& FullName, EClassCastFlags RequiredType);
template UEBoolProperty ObjectArray::FindObject<UEBoolProperty>(const std::string& FullName, EClassCastFlags RequiredType);
template UEObjectProperty ObjectArray::FindObject<UEObjectProperty>(const std::string& FullName, EClassCastFlags RequiredType);
template UEClassProperty ObjectArray::FindObject<UEClassProperty>(const std::string& FullName, EClassCastFlags RequiredType);
template UEStructProperty ObjectArray::FindObject<UEStructProperty>(const std::string& FullName, EClassCastFlags RequiredType);
template UEArrayProperty ObjectArray::FindObject<UEArrayProperty>(const std::string& FullName, EClassCastFlags RequiredType);
template UEMapProperty ObjectArray::FindObject<UEMapProperty>(const std::string& FullName, EClassCastFlags RequiredType);
template UESetProperty ObjectArray::FindObject<UESetProperty>(const std::string& FullName, EClassCastFlags RequiredType);
template UEEnumProperty ObjectArray::FindObject<UEEnumProperty>(const std::string& FullName, EClassCastFlags RequiredType);

template UEObject ObjectArray::FindObjectFast<UEObject>(const std::string& FullName, EClassCastFlags RequiredType);
template UEField ObjectArray::FindObjectFast<UEField>(const std::string& FullName, EClassCastFlags RequiredType);
template UEEnum ObjectArray::FindObjectFast<UEEnum>(const std::string& FullName, EClassCastFlags RequiredType);
template UEStruct ObjectArray::FindObjectFast<UEStruct>(const std::string& FullName, EClassCastFlags RequiredType);
template UEClass ObjectArray::FindObjectFast<UEClass>(const std::string& FullName, EClassCastFlags RequiredType);
template UEFunction ObjectArray::FindObjectFast<UEFunction>(const std::string& FullName, EClassCastFlags RequiredType);
template UEProperty ObjectArray::FindObjectFast<UEProperty>(const std::string& FullName, EClassCastFlags RequiredType);
template UEByteProperty ObjectArray::FindObjectFast<UEByteProperty>(const std::string& FullName, EClassCastFlags RequiredType);
template UEBoolProperty ObjectArray::FindObjectFast<UEBoolProperty>(const std::string& FullName, EClassCastFlags RequiredType);
template UEObjectProperty ObjectArray::FindObjectFast<UEObjectProperty>(const std::string& FullName, EClassCastFlags RequiredType);
template UEClassProperty ObjectArray::FindObjectFast<UEClassProperty>(const std::string& FullName, EClassCastFlags RequiredType);
template UEStructProperty ObjectArray::FindObjectFast<UEStructProperty>(const std::string& FullName, EClassCastFlags RequiredType);
template UEArrayProperty ObjectArray::FindObjectFast<UEArrayProperty>(const std::string& FullName, EClassCastFlags RequiredType);
template UEMapProperty ObjectArray::FindObjectFast<UEMapProperty>(const std::string& FullName, EClassCastFlags RequiredType);
template UESetProperty ObjectArray::FindObjectFast<UESetProperty>(const std::string& FullName, EClassCastFlags RequiredType);
template UEEnumProperty ObjectArray::FindObjectFast<UEEnumProperty>(const std::string& FullName, EClassCastFlags RequiredType);

template UEObject ObjectArray::FindObjectFastInOuter<UEObject>(const std::string& FullName, std::string Outer);
template UEField ObjectArray::FindObjectFastInOuter<UEField>(const std::string& FullName, std::string Outer);
template UEEnum ObjectArray::FindObjectFastInOuter<UEEnum>(const std::string& FullName, std::string Outer);
template UEStruct ObjectArray::FindObjectFastInOuter<UEStruct>(const std::string& FullName, std::string Outer);
template UEClass ObjectArray::FindObjectFastInOuter<UEClass>(const std::string& FullName, std::string Outer);
template UEFunction ObjectArray::FindObjectFastInOuter<UEFunction>(const std::string& FullName, std::string Outer);
template UEProperty ObjectArray::FindObjectFastInOuter<UEProperty>(const std::string& FullName, std::string Outer);
template UEByteProperty ObjectArray::FindObjectFastInOuter<UEByteProperty>(const std::string& FullName, std::string Outer);
template UEBoolProperty ObjectArray::FindObjectFastInOuter<UEBoolProperty>(const std::string& FullName, std::string Outer);
template UEObjectProperty ObjectArray::FindObjectFastInOuter<UEObjectProperty>(const std::string& FullName, std::string Outer);
template UEClassProperty ObjectArray::FindObjectFastInOuter<UEClassProperty>(const std::string& FullName, std::string Outer);
template UEStructProperty ObjectArray::FindObjectFastInOuter<UEStructProperty>(const std::string& FullName, std::string Outer);
template UEArrayProperty ObjectArray::FindObjectFastInOuter<UEArrayProperty>(const std::string& FullName, std::string Outer);
template UEMapProperty ObjectArray::FindObjectFastInOuter<UEMapProperty>(const std::string& FullName, std::string Outer);
template UESetProperty ObjectArray::FindObjectFastInOuter<UESetProperty>(const std::string& FullName, std::string Outer);
template UEEnumProperty ObjectArray::FindObjectFastInOuter<UEEnumProperty>(const std::string& FullName, std::string Outer);

template UEObject ObjectArray::GetByIndex<UEObject>(int32 Index);
template UEField ObjectArray::GetByIndex<UEField>(int32 Index);
template UEEnum ObjectArray::GetByIndex<UEEnum>(int32 Index);
template UEStruct ObjectArray::GetByIndex<UEStruct>(int32 Index);
template UEClass ObjectArray::GetByIndex<UEClass>(int32 Index);
template UEFunction ObjectArray::GetByIndex<UEFunction>(int32 Index);
template UEProperty ObjectArray::GetByIndex<UEProperty>(int32 Index);
template UEByteProperty ObjectArray::GetByIndex<UEByteProperty>(int32 Index);
template UEBoolProperty ObjectArray::GetByIndex<UEBoolProperty>(int32 Index);
template UEObjectProperty ObjectArray::GetByIndex<UEObjectProperty>(int32 Index);
template UEClassProperty ObjectArray::GetByIndex<UEClassProperty>(int32 Index);
template UEStructProperty ObjectArray::GetByIndex<UEStructProperty>(int32 Index);
template UEArrayProperty ObjectArray::GetByIndex<UEArrayProperty>(int32 Index);
template UEMapProperty ObjectArray::GetByIndex<UEMapProperty>(int32 Index);
template UESetProperty ObjectArray::GetByIndex<UESetProperty>(int32 Index);
template UEEnumProperty ObjectArray::GetByIndex<UEEnumProperty>(int32 Index);
