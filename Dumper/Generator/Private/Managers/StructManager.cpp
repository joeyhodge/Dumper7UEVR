#include "Unreal/ObjectArray.h"
#include "Managers/StructManager.h"
#include "Generators/Generator.h"
#include "OffsetFinder/Offsets.h"
#include "Platform.h"

namespace
{
	bool TryGetCurrentObjectCastFlags(UEObject Object, EClassCastFlags& OutCastFlags)
	{
		OutCastFlags = EClassCastFlags::None;

		const auto* Address = static_cast<const uint8*>(Object.GetAddress());
		if (Address == nullptr ||
			Platform::IsBadReadPtr(Address + Off::UObject::Index) ||
			Platform::IsBadReadPtr(Address + Off::UObject::Index + sizeof(int32) - 1) ||
			Platform::IsBadReadPtr(Address + Off::UObject::Class) ||
			Platform::IsBadReadPtr(Address + Off::UObject::Class + sizeof(void*) - 1))
		{
			return false;
		}

		const int32 Index = *reinterpret_cast<const int32*>(Address + Off::UObject::Index);
		if (Index < 0 || Index >= ObjectArray::Num() || ObjectArray::GetByIndex(Index).GetAddress() != Object.GetAddress())
			return false;

		const auto* Class = *reinterpret_cast<uint8* const*>(Address + Off::UObject::Class);
		if (Class == nullptr ||
			Platform::IsBadReadPtr(Class + Off::UClass::CastFlags) ||
			Platform::IsBadReadPtr(Class + Off::UClass::CastFlags + sizeof(EClassCastFlags) - 1))
		{
			return false;
		}

		OutCastFlags = *reinterpret_cast<const EClassCastFlags*>(Class + Off::UClass::CastFlags);
		return true;
	}

	bool IsCurrentStructObject(UEStruct Struct)
	{
		EClassCastFlags CastFlags{};
		if (!TryGetCurrentObjectCastFlags(Struct, CastFlags) || !(CastFlags & EClassCastFlags::Struct))
			return false;

		const auto* Address = static_cast<const uint8*>(Struct.GetAddress());
		if (Platform::IsBadReadPtr(Address + Off::UStruct::Size) ||
			Platform::IsBadReadPtr(Address + Off::UStruct::Size + sizeof(int32) - 1))
		{
			return false;
		}

		return true;
	}
}

StructInfoHandle::StructInfoHandle(const StructInfo& InInfo)
	: Info(&InInfo)
{
}

int32 StructInfoHandle::GetLastMemberEnd() const
{
	return Info->LastMemberEnd;
}

int32 StructInfoHandle::GetSize() const
{
	return Align(Info->Size, Info->Alignment);
}

int32 StructInfoHandle::GetUnalignedSize() const
{
	return Info->Size;
}

int32 StructInfoHandle::GetAlignment() const
{
	return Info->Alignment;
}

bool StructInfoHandle::ShouldUseExplicitAlignment() const
{
	return Info->bUseExplicitAlignment;
}

const StringEntry& StructInfoHandle::GetName() const
{
	return StructManager::GetName(*Info);
}

bool StructInfoHandle::IsFinal() const
{
	return Info->bIsFinal;
}

bool StructInfoHandle::HasReusedTrailingPadding() const
{
	return Info->bHasReusedTrailingPadding;
}

bool StructInfoHandle::IsPartOfCyclicPackage() const
{
	return Info->bIsPartOfCyclicPackage;
}

void StructManager::InitAlignmentsAndNames()
{
	constexpr int32 DefaultClassAlignment = sizeof(void*);

	const UEClass InterfaceClass = ObjectArray::FindClassFast("Interface");

	const UEClass OnlineEngineInterfaceImplClass = ObjectArray::FindClassFast("OnlineEngineInterfaceImpl");

	/*
	 *  Cache all struct objects to avoid multiple full ObjectArray iterations
	 */
	std::vector<UEStruct> AllStructs;
	AllStructs.reserve(10000);
	const int32 TotalObjects = ObjectArray::Num();
	int32 ProcessedObjects = 0;

	for (auto Obj : ObjectArray())
	{
		++ProcessedObjects;
		if (ProcessedObjects == 1 || (ProcessedObjects % 0x1000) == 0 || ProcessedObjects == TotalObjects)
		{
			Generator::ReportProgress(
				"Struct cache scan " + std::to_string(ProcessedObjects) + "/" + std::to_string(TotalObjects));
		}

		EClassCastFlags CastFlags{};
		if (TryGetCurrentObjectCastFlags(Obj, CastFlags) && (CastFlags & EClassCastFlags::Struct))
			AllStructs.push_back(Obj.Cast<UEStruct>());
	}

	Generator::ReportProgress("Struct alignment/name pass: " + std::to_string(AllStructs.size()) + " structs");
	int32 ProcessedStructs = 0;
	for (auto ObjAsStruct : AllStructs)
	{
		++ProcessedStructs;
		if (!IsCurrentStructObject(ObjAsStruct))
			continue;

		if ((ProcessedStructs % 0x400) == 0 || ProcessedStructs == static_cast<int32>(AllStructs.size()))
		{
			Generator::ReportProgress(
				"Struct alignment/name pass " + std::to_string(ProcessedStructs) + "/" +
				std::to_string(AllStructs.size()));
		}

		// Add name to override info
		StructInfo& NewOrExistingInfo = StructInfoOverrides[ObjAsStruct.GetIndex()];

		std::string CppName = ObjAsStruct.GetCppName();

		// Hardcoded fix for two 'UOnlineEngineInterfaceImpl' classes in the same package. Check will only match one of them.
		if (ObjAsStruct == OnlineEngineInterfaceImplClass) [[unlikely]]
			CppName += '2';

		NewOrExistingInfo.Name = UniqueNameTable.FindOrAdd(CppName, !ObjAsStruct.IsA(EClassCastFlags::Function)).first;

		// Interfaces inherit from UObject by default, but as a workaround to no virtual-inheritance we make them empty
		if (ObjAsStruct.HasType(InterfaceClass))
		{
			NewOrExistingInfo.Alignment = 0x1;
			NewOrExistingInfo.bHasReusedTrailingPadding = false;
			NewOrExistingInfo.bIsFinal = true;
			NewOrExistingInfo.Size = 0x0;

			continue;
		}

		const int32 MinAlignment = ObjAsStruct.GetMinAlignment();
		int32 HighestMemberAlignment = 0x1; // starting at 0x1 when checking **all**, not just struct-properties

		// Find member with the highest alignment
		for (UEProperty Property : ObjAsStruct.GetProperties())
		{
			int32 CurrentPropertyAlignment = Property.GetAlignment();

			if (CurrentPropertyAlignment > HighestMemberAlignment)
				HighestMemberAlignment = CurrentPropertyAlignment;
		}

		/* On some strange games there are BlueprintGeneratedClass UClasses which don't inherit from UObject. */
		const bool bHasSuperClass = static_cast<bool>(ObjAsStruct.GetSuper());

		// if Class alignment is below pointer-alignment (0x8), use pointer-alignment instead, else use whichever, MinAlignment or HighestAlignment, is bigger
		if (ObjAsStruct.IsA(EClassCastFlags::Class) && bHasSuperClass && HighestMemberAlignment < DefaultClassAlignment)
		{
			NewOrExistingInfo.bUseExplicitAlignment = false;
			NewOrExistingInfo.Alignment = DefaultClassAlignment;
		}
		else
		{
			NewOrExistingInfo.bUseExplicitAlignment = MinAlignment > HighestMemberAlignment;
			NewOrExistingInfo.Alignment = max(MinAlignment, HighestMemberAlignment);
		}
	}

	// Second pass: Fix alignments based on super classes (reuse cached list)
	Generator::ReportProgress("Struct super-alignment pass");
	ProcessedStructs = 0;
	for (auto ObjAsStruct : AllStructs)
	{
		++ProcessedStructs;
		if (!IsCurrentStructObject(ObjAsStruct))
			continue;

		if ((ProcessedStructs % 0x400) == 0 || ProcessedStructs == static_cast<int32>(AllStructs.size()))
		{
			Generator::ReportProgress(
				"Struct super-alignment pass " + std::to_string(ProcessedStructs) + "/" +
				std::to_string(AllStructs.size()));
		}

		if (ObjAsStruct.IsA(EClassCastFlags::Function) || ObjAsStruct.HasType(InterfaceClass))
			continue;

		constexpr int MaxNumSuperClasses = 0x30;

		std::array<UEStruct, MaxNumSuperClasses> StructStack;
		int32 NumElementsInStructStack = 0x0;

		// Get a top to bottom list of a struct and all of its supers
		for (UEStruct S = ObjAsStruct;
			IsCurrentStructObject(S) && NumElementsInStructStack < MaxNumSuperClasses;
			S = S.GetSuper())
		{
			StructStack[NumElementsInStructStack] = S;
			NumElementsInStructStack++;
		}

		int32 CurrentHighestAlignment = 0x0;

		for (int i = NumElementsInStructStack - 1; i >= 0; i--)
		{
			if (!IsCurrentStructObject(StructStack[i]))
				continue;

			auto It = StructInfoOverrides.find(StructStack[i].GetIndex());
			if (It == StructInfoOverrides.end())
				continue;

			StructInfo& Info = It->second;

			if (CurrentHighestAlignment < Info.Alignment)
			{
				CurrentHighestAlignment = Info.Alignment;
			}
			else
			{
				// We use the super classes' alignment, no need to explicitely set it
				Info.bUseExplicitAlignment = false; 
				Info.Alignment = CurrentHighestAlignment;
			}
		}
	}
}

void StructManager::InitSizesAndIsFinal()
{
	const UEClass InterfaceClass = ObjectArray::FindClassFast("Interface");

	// Reuse cached struct list from InitAlignmentsAndNames
	const int32 TotalStructs = static_cast<int32>(StructInfoOverrides.size());
	int32 ProcessedStructs = 0;
	for (const auto& [Index, Info] : StructInfoOverrides)
	{
		++ProcessedStructs;
		if (ProcessedStructs == 1 || (ProcessedStructs % 0x400) == 0 || ProcessedStructs == TotalStructs)
		{
			Generator::ReportProgress(
				"Struct size/finality pass " + std::to_string(ProcessedStructs) + "/" +
				std::to_string(TotalStructs));
		}

		UEStruct ObjAsStruct = ObjectArray::GetByIndex<UEStruct>(Index);
		if (!IsCurrentStructObject(ObjAsStruct))
			continue;

		if (ObjAsStruct.HasType(InterfaceClass))
			continue;

		StructInfo& NewOrExistingInfo = StructInfoOverrides[Index];

		// Initialize struct-size if it wasn't set already
		if (NewOrExistingInfo.Size > ObjAsStruct.GetStructSize())
			NewOrExistingInfo.Size = ObjAsStruct.GetStructSize();

		UEStruct Super = ObjAsStruct.GetSuper();

		if (NewOrExistingInfo.Size == 0x0 && IsCurrentStructObject(Super))
			NewOrExistingInfo.Size = Super.GetStructSize();

		int32 LastMemberEnd = 0x0;
		int32 LowestOffset = INT_MAX;

		// Find member with the lowest offset
		for (UEProperty Property : ObjAsStruct.GetProperties())
		{
			const int32 PropertyOffset = Property.GetOffset();
			const int32 PropertySize = Property.GetSize();

			if (PropertyOffset < LowestOffset)
				LowestOffset = PropertyOffset;

			if ((PropertyOffset + PropertySize) > LastMemberEnd)
				LastMemberEnd = PropertyOffset + PropertySize;
		}

		/* No need to check any other structs, as finding the LastMemberEnd only involves this struct */
		NewOrExistingInfo.LastMemberEnd = LastMemberEnd;

		if (!Super || ObjAsStruct.IsA(EClassCastFlags::Function))
			continue;

		/*
		* Loop all super-structs and set their struct-size to the lowest offset we found. Sets this size on the direct Super and all higher *empty* supers
		* 
		* breaks out of the loop after encountering a super-struct which is not empty (aka. has member-variables)
		*/
		for (UEStruct S = Super; IsCurrentStructObject(S); S = S.GetSuper())
		{
			const int32 SuperIndex = S.GetIndex();
			if (SuperIndex < 0)
				break;

			auto It = StructInfoOverrides.find(SuperIndex);

			if (It == StructInfoOverrides.end())
			{
				std::cerr << "Dumper-7: struct wasn't found in 'StructInfoOverrides'; skipping its trailing-padding adjustment.\n" << std::endl;
				continue;
			}

			StructInfo& Info = It->second;

			// Struct is not final, as it is another structs' super
			Info.bIsFinal = false;

			const int32 SizeToCheck = Info.Size == INT_MAX ? S.GetStructSize() : Info.Size;

			// Only change lowest offset if it's lower than the already found lowest offset (by default: struct-size)
			if (Align(SizeToCheck, Info.Alignment) > LowestOffset)
			{
				if (Info.Size > LowestOffset)
					Info.Size = LowestOffset;

				Info.bHasReusedTrailingPadding = true;
			}

			if (S.HasMembers())
				break;
		}
	}
}

void StructManager::Init()
{
	if (bIsInitialized)
		return;

	bIsInitialized = true;

	StructInfoOverrides.reserve(0x2000);

	Generator::ReportProgress("StructManager::InitAlignmentsAndNames");
	InitAlignmentsAndNames();
	Generator::ReportProgress("StructManager::InitSizesAndIsFinal");
	InitSizesAndIsFinal();

	/* 
	* The default class-alignment of 0x8 is only set for classes with a valid Super-class, because they inherit from UObject. 
	* UObject however doesn't have a super, so this needs to be set manually.
	*/
	const UEObject UObjectClass = Off::ExternalEngineLayout.ObjectClassIndex >= 0
		? ObjectArray::GetByIndex(Off::ExternalEngineLayout.ObjectClassIndex)
		: ObjectArray::FindClassFast("Object");
	if (UObjectClass)
	{
		if (auto It = StructInfoOverrides.find(UObjectClass.GetIndex()); It != StructInfoOverrides.end())
			It->second.Alignment = sizeof(void*);
	}

	/* I still hate whoever decided to call "UStruct" "Ustruct" on some UE versions. */
	const UEObject UStructClass = Off::ExternalEngineLayout.StructClassIndex >= 0
		? ObjectArray::GetByIndex(Off::ExternalEngineLayout.StructClassIndex)
		: ObjectArray::FindClassFast("struct");
	if (UStructClass)
	{
		if (auto It = StructInfoOverrides.find(UStructClass.GetIndex()); It != StructInfoOverrides.end())
			It->second.Name = UniqueNameTable.FindOrAdd(std::string("UStruct"), false).first;
	}

	Generator::ReportProgress("StructManager::Init complete");
}
