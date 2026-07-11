#include <format>

#include "Utils.h"

#include "OffsetFinder/Offsets.h"
#include "OffsetFinder/OffsetFinder.h"

#include "Generators/Generator.h"

#include "Unreal/ObjectArray.h"
#include "Unreal/NameArray.h"

#include "Platform.h"
#include "Architecture.h"


void Off::InSDK::ProcessEvent::InitPE_Windows()
{
#ifdef PLATFORM_WINDOWS

	void** Vft = *(void***)ObjectArray::GetByIndex(0).GetAddress();

#if defined(_WIN64)
	/* Primary, and more reliable, check for ProcessEvent */
	auto IsProcessEvent = [](const uint8_t* FuncAddress, [[maybe_unused]] int32_t Index) -> bool
	{
		return Platform::FindPatternInRange({ 0xF7, -0x1, Off::UFunction::FunctionFlags, 0x0, 0x0, 0x0, 0x0, 0x04, 0x0, 0x0 }, FuncAddress, 0x400)
			&& Platform::FindPatternInRange({ 0xF7, -0x1, Off::UFunction::FunctionFlags, 0x0, 0x0, 0x0, 0x0, 0x0, 0x40, 0x0 }, FuncAddress, 0xF00);
	};
#elif defined(_WIN32)
	/* Primary, and more reliable, check for ProcessEvent */
	auto IsProcessEvent = [](const uint8_t* FuncAddress, [[maybe_unused]] int32_t Index) -> bool
	{
		return Platform::FindPatternInRange({ 0xF7, -0x1, Off::UFunction::FunctionFlags, 0x0, 0x4, 0x0, 0x0 }, FuncAddress, 0x400)
			&& Platform::FindPatternInRange({ 0xF7, -0x1, Off::UFunction::FunctionFlags, 0x0, 0x0, 0x40, 0x0 }, FuncAddress, 0xF00);
	};
#endif

	const void* ProcessEventAddr = nullptr;
	int32_t ProcessEventIdx = 0;

	const auto [FuncPtr, FuncIdx] = Platform::IterateVTableFunctions(Vft, IsProcessEvent);

	ProcessEventAddr = FuncPtr;
	ProcessEventIdx = FuncIdx;

	if (!FuncPtr)
	{
		const void* StringRefAddr = Platform::FindByStringInAllSections(L"Accessed None", 0x0, 0x0, Settings::General::bSearchOnlyExecutableSectionsForStrings);
		/* ProcessEvent is sometimes located right after a func with the string L"Accessed None. Might as well check for it, because else we're going to crash anyways. */
		const void* PossiblePEAddr = reinterpret_cast<void*>(Architecture_x86_64::FindNextFunctionStart(StringRefAddr));

		auto IsSameAddr = [PossiblePEAddr](const uint8_t* FuncAddress, [[maybe_unused]] int32_t Index) -> bool
		{
			return FuncAddress == PossiblePEAddr;
		};

		const auto [FuncPtr2, FuncIdx2] = Platform::IterateVTableFunctions(Vft, IsSameAddr);
		ProcessEventAddr = FuncPtr2;
		ProcessEventIdx = FuncIdx2;
	}

	if (ProcessEventAddr)
	{
		Off::InSDK::ProcessEvent::PEIndex = ProcessEventIdx;
		Off::InSDK::ProcessEvent::PEOffset = Platform::GetOffset(ProcessEventAddr);

		std::cerr << std::format("PE-Offset: 0x{:X}\n", Off::InSDK::ProcessEvent::PEOffset);
		std::cerr << std::format("PE-Index: 0x{:X}\n\n", ProcessEventIdx);
		return;
	}

	std::cerr << "\nCouldn't find ProcessEvent!\n\n" << std::endl;

#endif // PLATFORM_WINDOWS
}

void Off::InSDK::ProcessEvent::InitPE(const int32 Index, const char* const ModuleName)
{
	Off::InSDK::ProcessEvent::PEIndex = Index;

	void** VFT = *reinterpret_cast<void***>(ObjectArray::GetByIndex(0).GetAddress());

	Off::InSDK::ProcessEvent::PEOffset = Platform::GetOffset(VFT[Off::InSDK::ProcessEvent::PEIndex], ModuleName);

	std::cerr << std::format("PE-Offset: 0x{:X}\n", Off::InSDK::ProcessEvent::PEOffset);
}

/* UWorld */
void Off::InSDK::World::InitGWorld()
{
	UEClass UWorld = ObjectArray::FindClassFast("World");

	for (UEObject Obj : ObjectArray())
	{
		if (Obj.HasAnyFlags(EObjectFlags::ClassDefaultObject) || !Obj.IsA(UWorld))
			continue;

		/* Try to find a pointer to the word, aka UWorld** GWorld */
		auto Results = Platform::FindAllAlignedValuesInProcess(Obj.GetAddress());

		void* Result = nullptr;
		if (Results.size())
		{
			if (Results.size() == 1)
			{
				Result = Results[0];
			}
			else if (Results.size() == 2)
			{
				auto ObjAddress = reinterpret_cast<uintptr_t>(Obj.GetAddress());
				auto PossibleGWorld = reinterpret_cast<volatile uintptr_t*>(Results[0]);
				auto CurrentValue = *PossibleGWorld;

				for (int i = 0; CurrentValue == ObjAddress && i < 50; ++i)
				{
					::Sleep(1);
					CurrentValue = *PossibleGWorld;
				}
				if (CurrentValue == ObjAddress)
				{
					Result = Results[0];
				}
				else
				{
					Result = Results[1];
					std::cerr << std::format("Filter GActiveLogWorld at 0x{:X}\n\n", reinterpret_cast<uintptr_t>(PossibleGWorld));
				}
			}
			else
			{
				std::cerr << std::format("Detected {} GWorld \n\n", Results.size());
			}
		}

		/* Pointer to UWorld* couldn't be found */
		if (Result)
		{
			Off::InSDK::World::GWorld = Platform::GetOffset(Result);
			std::cerr << std::format("GWorld-Offset: 0x{:X}\n\n", Off::InSDK::World::GWorld);
			break;
		}
	}

	if (Off::InSDK::World::GWorld == 0x0)
		std::cerr << std::format("\nGWorld WAS NOT FOUND!!!!!!!!!\n\n");
}

/* FText */
void Off::InSDK::Text::InitTextOffsets()
{
	// Mark unresolved by default. These will be overwritten when detection succeeds.
	Off::InSDK::Text::TextDatOffset = -1;
	Off::InSDK::Text::InTextDataStringOffset = -1;
	Off::InSDK::Text::TextSize = 0;

	if (Off::ExternalFFieldLayout.IsCompactUE58Layout() && Off::ExternalFTextLayout.IsValid())
	{
		Generator::ReportProgress("FText layout (UEVR metadata, opaque)");
		Off::InSDK::Text::TextSize = Off::ExternalFTextLayout.TextSize;
		std::cerr << std::format(
			"Dumper-7: Using exact-size opaque FText from UEVR metadata (size: 0x{:X}).\n\n",
			Off::InSDK::Text::TextSize);
		return;
	}

	if (Off::InSDK::ProcessEvent::PEIndex == 0 && Off::InSDK::ProcessEvent::PEOffset == 0)
	{
		std::cerr << std::format("\nDumper-7: Error, 'InitInSDKTextOffsets' was called before ProcessEvent was initialized!\n") << std::endl;
		return;
	}

	auto IsValidPtr = [](void* a) -> bool
	{
		return !Platform::IsBadReadPtr(a) /* && (uintptr_t(a) & 0x1) == 0*/; // realistically, there wont be any pointers to unaligned memory
	};


	const UEFunction Conv_StringToText = ObjectArray::FindObjectFast<UEFunction>("Conv_StringToText", EClassCastFlags::Function);

	UEProperty InStringProp = nullptr;
	UEProperty ReturnProp = nullptr;

	if (!Conv_StringToText)
	{
		std::cerr << "Conv_StringToText is invalid!\n";
		return;
	}

	for (UEProperty Prop : Conv_StringToText.GetProperties())
	{
		/* Func has 2 params, if the param is the return value assign to ReturnProp, else InStringProp*/
		if (Prop.HasPropertyFlags(EPropertyFlags::ReturnParm))
		{
			ReturnProp = Prop;
		}
		else
		{
			InStringProp = Prop;
		}
	}

	if (!InStringProp || !ReturnProp)
	{
		std::cerr << std::format("\nDumper-7: Error, failed to resolve Conv_StringToText parameters (InString: {}, Return: {}).\n",
			static_cast<bool>(InStringProp), static_cast<bool>(ReturnProp)) << std::endl;
		return;
	}

	const int32 ParamSize = Conv_StringToText.GetStructSize();
	const int32 FTextSize = ReturnProp.GetSize();

	const int32 StringOffset = InStringProp.GetOffset();
	const int32 ReturnValueOffset = ReturnProp.GetOffset();

	if (ParamSize <= 0 || FTextSize <= 0 || StringOffset < 0 || ReturnValueOffset < 0 || StringOffset >= ParamSize || ReturnValueOffset >= ParamSize)
	{
		std::cerr << std::format("\nDumper-7: Error, invalid Conv_StringToText layout (ParamSize: 0x{:X}, FTextSize: 0x{:X}, StringOffset: 0x{:X}, ReturnOffset: 0x{:X}).\n",
			ParamSize, FTextSize, StringOffset, ReturnValueOffset) << std::endl;
		return;
	}

	Off::InSDK::Text::TextSize = FTextSize;


	/* Allocate and zero-initialize ParamStruct */
#pragma warning(disable: 6255)
	uint8_t* ParamPtr = static_cast<uint8_t*>(alloca(ParamSize));
	memset(ParamPtr, 0, ParamSize);

	/* Choose a, fairly random, string to later search for in FTextData */
	constexpr const wchar_t* StringText = L"ThisIsAGoodString!";
	constexpr int32 StringLength = (sizeof(L"ThisIsAGoodString!") / sizeof(wchar_t));
	constexpr int32 StringLengthBytes = (sizeof(L"ThisIsAGoodString!"));

	/* Initialize 'InString' in the ParamStruct */
	*reinterpret_cast<FString*>(ParamPtr + StringOffset) = StringText;

	/* This function is 'static' so the object on which we call it doesn't matter */
	ObjectArray::GetByIndex(0).ProcessEvent(Conv_StringToText, ParamPtr);

	uint8_t* FTextDataPtr = nullptr;

	/* Search for the first valid pointer inside of the FText and make the offset our 'TextDatOffset' */
	for (int32 i = 0; i < (FTextSize - static_cast<int32>(sizeof(void*))); i += static_cast<int32>(sizeof(void*)))
	{
		void* PossibleTextDataPtr = *reinterpret_cast<void**>(ParamPtr + ReturnValueOffset + i);

		if (IsValidPtr(PossibleTextDataPtr))
		{
			FTextDataPtr = static_cast<uint8_t*>(PossibleTextDataPtr);
			Off::InSDK::Text::TextDatOffset = i;
			break;
		}
	}

	if (!FTextDataPtr)
	{
		std::cerr << std::format("\nDumper-7: Error, 'FTextDataPtr' could not be found!\n") << std::endl;
		return;
	}

	constexpr int32 MaxOffset = 0x50;
	constexpr int32 StartOffset = sizeof(void*); // FString::NumElements offset

	/* Search for a pointer pointing to a int32 Value (FString::NumElements) equal to StringLength */
	for (int32 i = StartOffset; i < MaxOffset; i += sizeof(int32))
	{
		wchar_t* PosibleStringPtr = *reinterpret_cast<wchar_t**>((FTextDataPtr + i) - sizeof(void*));
		const int32 PossibleLength = *reinterpret_cast<int32*>(FTextDataPtr + i);

		if (PossibleLength == StringLength && PosibleStringPtr && IsValidPtr(PosibleStringPtr) && memcmp(StringText, PosibleStringPtr, StringLengthBytes) == 0)
		{
			Off::InSDK::Text::InTextDataStringOffset = (i - sizeof(void*));
			break;
		}
	}

	if (Off::InSDK::Text::InTextDataStringOffset < 0)
	{
		std::cerr << std::format("\nDumper-7: Warning, FTextData::TextSource offset could not be found.\n") << std::endl;
	}

	std::cerr << std::format("Off::InSDK::Text::TextSize: 0x{:X}\n", Off::InSDK::Text::TextSize);
	std::cerr << std::format("Off::InSDK::Text::TextDatOffset: 0x{:X}\n", Off::InSDK::Text::TextDatOffset);
	std::cerr << std::format("Off::InSDK::Text::InTextDataStringOffset: 0x{:X}\n\n", Off::InSDK::Text::InTextDataStringOffset);
}

void Off::Init()
{
	auto ReportOffsetStage = [](std::string_view stage)
	{
		Generator::ReportProgress("Core offsets: " + std::string(stage));
	};

	auto OverwriteIfInvalidOffset = [](int32& Offset, int32 DefaultValue)
	{
		if (Offset == OffsetFinder::OffsetNotFound)
		{
			std::cerr << std::format("Defaulting to offset: 0x{:X}\n", DefaultValue);
			Offset = DefaultValue;
		}
	};

	ReportOffsetStage("UObject flags");
	Off::UObject::Flags = OffsetFinder::FindUObjectFlagsOffset();
	OverwriteIfInvalidOffset(Off::UObject::Flags, sizeof(void*)); // Default to right after VTable
	std::cerr << std::format("Off::UObject::Flags: 0x{:X}\n", Off::UObject::Flags);

	ReportOffsetStage("UObject internal index");
	Off::UObject::Index = OffsetFinder::FindUObjectIndexOffset();
	OverwriteIfInvalidOffset(Off::UObject::Index, (Off::UObject::Flags + sizeof(int32))); // Default to right after Flags
	std::cerr << std::format("Off::UObject::Index: 0x{:X}\n", Off::UObject::Index);

	ReportOffsetStage("UObject class");
	Off::UObject::Class = OffsetFinder::FindUObjectClassOffset();
	OverwriteIfInvalidOffset(Off::UObject::Class, (Off::UObject::Index + sizeof(int32))); // Default to right after Index
	std::cerr << std::format("Off::UObject::Class: 0x{:X}\n", Off::UObject::Class);

	ReportOffsetStage("UObject outer");
	Off::UObject::Outer = OffsetFinder::FindUObjectOuterOffset();
	std::cerr << std::format("Off::UObject::Outer: 0x{:X}\n", Off::UObject::Outer);

	ReportOffsetStage("UObject name");
	Off::UObject::Name = OffsetFinder::FindUObjectNameOffset();
	OverwriteIfInvalidOffset(Off::UObject::Name, (Off::UObject::Class + sizeof(void*))); // Default to right after Class
	std::cerr << std::format("Off::UObject::Name: 0x{:X}\n\n", Off::UObject::Name);

	OverwriteIfInvalidOffset(Off::UObject::Outer, (Off::UObject::Name + sizeof(int32) + sizeof(int32)));  // Default to right after Name

	ReportOffsetStage("FName settings");
	OffsetFinder::InitFNameSettings();

	ReportOffsetStage("name array post-init");
	::NameArray::PostInit();

	// Castflags needs to stay here since the FindChildOffset() uses CastFlags
	ReportOffsetStage("UClass cast flags");
	Off::UClass::CastFlags = OffsetFinder::FindCastFlagsOffset();
	std::cerr << std::format("Off::UClass::CastFlags: 0x{:X}\n", Off::UClass::CastFlags);

	ReportOffsetStage(Off::ExternalUStructLayout.HasChildren()
		? "UStruct children (UEVR metadata)"
		: "UStruct children");
	Off::UStruct::Children = Off::ExternalUStructLayout.HasChildren()
		? Off::ExternalUStructLayout.ChildrenOffset
		: OffsetFinder::FindChildOffset();
	std::cerr << std::format("Off::UStruct::Children: 0x{:X}\n", Off::UStruct::Children);

	ReportOffsetStage(Off::ExternalUStructLayout.HasUFieldNext()
		? "UField next (UEVR metadata)"
		: "UField next");
	Off::UField::Next = Off::ExternalUStructLayout.HasUFieldNext()
		? Off::ExternalUStructLayout.UFieldNextOffset
		: OffsetFinder::FindUFieldNextOffset();
	std::cerr << std::format("Off::UField::Next: 0x{:X}\n", Off::UField::Next);

	ReportOffsetStage(Off::ExternalUStructLayout.HasSuperStruct()
		? "UStruct super (UEVR metadata)"
		: "UStruct super");
	Off::UStruct::SuperStruct = Off::ExternalUStructLayout.HasSuperStruct()
		? Off::ExternalUStructLayout.SuperStructOffset
		: OffsetFinder::FindSuperOffset();
	std::cerr << std::format("Off::UStruct::SuperStruct: 0x{:X}\n", Off::UStruct::SuperStruct);

	ReportOffsetStage(Off::ExternalUStructLayout.HasSize()
		? "UStruct size (UEVR metadata)"
		: "UStruct size");
	Off::UStruct::Size = Off::ExternalUStructLayout.HasSize()
		? Off::ExternalUStructLayout.SizeOffset
		: OffsetFinder::FindStructSizeOffset();
	std::cerr << std::format("Off::UStruct::Size: 0x{:X}\n", Off::UStruct::Size);

	ReportOffsetStage(Off::ExternalUStructLayout.HasMinAlignment()
		? "UStruct alignment (UEVR metadata)"
		: "UStruct alignment");
	Off::UStruct::MinAlignment = Off::ExternalUStructLayout.HasMinAlignment()
		? Off::ExternalUStructLayout.MinAlignmentOffset
		: OffsetFinder::FindMinAlignmentOffset();
	std::cerr << std::format("Off::UStruct::MinAlignment: 0x{:X}\n", Off::UStruct::MinAlignment);

	ReportOffsetStage("UClass cast flags verification");
	Off::UClass::CastFlags = OffsetFinder::FindCastFlagsOffset();
	std::cerr << std::format("Off::UClass::CastFlags: 0x{:X}\n", Off::UClass::CastFlags);

	// Castflags become available for use

	if (Settings::Internal::bUseFProperty)
	{
		std::cerr << std::format("\nGame uses FProperty system\n\n");

		ReportOffsetStage(Off::ExternalUStructLayout.HasChildProperties()
			? "UStruct child properties (UEVR metadata)"
			: "UStruct child properties");
		Off::UStruct::ChildProperties = Off::ExternalUStructLayout.HasChildProperties()
			? Off::ExternalUStructLayout.ChildPropertiesOffset
			: OffsetFinder::FindChildPropertiesOffset();
		std::cerr << std::format("Off::UStruct::ChildProperties: 0x{:X}\n", Off::UStruct::ChildProperties);

		const bool bUsesUEVRFieldLayout = Off::ExternalFFieldLayout.IsValid() &&
			Off::ExternalFFieldLayout.FieldClassNameOffset == static_cast<int32>(sizeof(void*));
		const FChunkedFixedUObjectArrayLayout& ObjectArrayLayout = Off::FUObjectArray::ChunkedFixedLayout;
		const bool bUsesUE58ObjectArrayLayout = Off::FUObjectArray::bIsChunked &&
			ObjectArrayLayout.ObjectsOffset == 0x00 &&
			ObjectArrayLayout.NumElementsOffset == 0x08 &&
			ObjectArrayLayout.MaxElementsOffset == 0x0C &&
			ObjectArrayLayout.NumChunksOffset == 0x10 &&
			ObjectArrayLayout.MaxChunksOffset == 0x14;

		if (bUsesUEVRFieldLayout || bUsesUE58ObjectArrayLayout)
		{
			ReportOffsetStage(bUsesUEVRFieldLayout
				? "FField layout (UEVR metadata)"
				: "FField layout (UE5.8 fixed layout)");

			Off::FField::Vft = 0x00;
			Off::FField::Class = bUsesUEVRFieldLayout ? Off::ExternalFFieldLayout.ClassOffset : 0x08;
			Off::FField::Owner = bUsesUEVRFieldLayout ? Off::ExternalFFieldLayout.OwnerOffset : 0x10;
			Off::FField::Next = bUsesUEVRFieldLayout ? Off::ExternalFFieldLayout.NextOffset : 0x18;
			Off::FField::Name = bUsesUEVRFieldLayout ? Off::ExternalFFieldLayout.NameOffset : 0x20;
			Off::FField::Flags = Off::FField::Name + Off::InSDK::Name::FNameSize;
			Settings::Internal::bUseMaskForFieldOwner =
				Off::FField::Next == Off::FField::Owner + static_cast<int32>(sizeof(void*));

			// UE 5.8 added a virtual destructor and moved ClassFlags before Id.
			Off::FFieldClass::Name = bUsesUEVRFieldLayout ? Off::ExternalFFieldLayout.FieldClassNameOffset : 0x08;
			Off::FFieldClass::ClassFlags = Off::FFieldClass::Name + Off::InSDK::Name::FNameSize;
			Off::FFieldClass::Id = Align(
				Off::FFieldClass::ClassFlags + static_cast<int32>(sizeof(EClassFlags)),
				static_cast<int32>(alignof(uint64)));
			Off::FFieldClass::CastFlags = Off::FFieldClass::Id + static_cast<int32>(sizeof(uint64));
			Off::FFieldClass::SuperClass = Off::FFieldClass::CastFlags + static_cast<int32>(sizeof(EClassCastFlags));
		}
		else
		{
			ReportOffsetStage("FField hardcoded fixups");
			OffsetFinder::FixupHardcodedOffsets(); // must be called after FindChildPropertiesOffset

			ReportOffsetStage("FField next");
			Off::FField::Next = OffsetFinder::FindFFieldNextOffset();
			std::cerr << std::format("Off::FField::Next: 0x{:X}\n", Off::FField::Next);
			if (Off::FField::Next == Off::FField::Owner + static_cast<int32>(sizeof(void*)))
				Settings::Internal::bUseMaskForFieldOwner = true;

			ReportOffsetStage("FField class");
			Off::FField::Class = OffsetFinder::FindFFieldClassOffset();
			std::cerr << std::format("Off::FField::Class: 0x{:X}\n", Off::FField::Class);

			ReportOffsetStage("FField class cast flags");
			const int32 FFieldClassCastFlags = OffsetFinder::FindFieldClassCastFlagsOffset();
			if (FFieldClassCastFlags != OffsetFinder::OffsetNotFound)
				Off::FFieldClass::CastFlags = FFieldClassCastFlags;

			ReportOffsetStage("FField name");
			Off::FField::Name = OffsetFinder::FindFFieldNameOffset();
			if (Off::FField::Name == OffsetFinder::OffsetNotFound)
				Off::FField::Name = OffsetFinder::NewFindFFieldNameOffset();

			Off::FField::Flags = Off::FField::Name + Off::InSDK::Name::FNameSize;
		}

		std::cerr << std::format("Off::FField::Class: 0x{:X}\n", Off::FField::Class);
		std::cerr << std::format("Off::FField::Next: 0x{:X}\n", Off::FField::Next);
		std::cerr << std::format("Off::FField::Name: 0x{:X}\n", Off::FField::Name);
		std::cerr << std::format("Off::FField::Flags: 0x{:X}\n", Off::FField::Flags);
		std::cerr << std::format("Off::FFieldClass::Name: 0x{:X}\n", Off::FFieldClass::Name);
		std::cerr << std::format("Off::FFieldClass::CastFlags: 0x{:X}\n", Off::FFieldClass::CastFlags);
	}

	ReportOffsetStage("UClass default object");
	Off::UClass::ClassDefaultObject = OffsetFinder::FindDefaultObjectOffset();
	std::cerr << std::format("Off::UClass::ClassDefaultObject: 0x{:X}\n", Off::UClass::ClassDefaultObject);

	ReportOffsetStage("UClass interfaces");
	Off::UClass::ImplementedInterfaces = OffsetFinder::FindImplementedInterfacesOffset();
	std::cerr << std::format("Off::UClass::ImplementedInterfaces: 0x{:X}\n", Off::UClass::ImplementedInterfaces);

	ReportOffsetStage("UEnum names");
	Off::UEnum::Names = OffsetFinder::FindEnumNamesOffset();
	std::cerr << std::format("Off::UEnum::Names: 0x{:X}\n", Off::UEnum::Names) << std::endl;

	ReportOffsetStage("UFunction flags");
	Off::UFunction::FunctionFlags = OffsetFinder::FindFunctionFlagsOffset();
	std::cerr << std::format("Off::UFunction::FunctionFlags: 0x{:X}\n", Off::UFunction::FunctionFlags);

	ReportOffsetStage(Off::ExternalUFunctionLayout.IsValid()
		? "UFunction native pointer (UEVR metadata)"
		: "UFunction native pointer");
	Off::UFunction::ExecFunction = Off::ExternalUFunctionLayout.IsValid()
		? Off::ExternalUFunctionLayout.ExecFunctionOffset
		: OffsetFinder::FindFunctionNativeFuncOffset();
	std::cerr << std::format("Off::UFunction::ExecFunction: 0x{:X}\n", Off::UFunction::ExecFunction) << std::endl;

	if (Off::ExternalFPropertyLayout.IsValid())
	{
		ReportOffsetStage("Property layout (UEVR metadata)");
		Off::Property::ArrayDim = Off::ExternalFPropertyLayout.ArrayDimOffset;
		Off::Property::ElementSize = Off::ExternalFPropertyLayout.ElementSizeOffset;
		Off::Property::PropertyFlags = Off::ExternalFPropertyLayout.PropertyFlagsOffset;
		Off::Property::Offset_Internal = Off::ExternalFPropertyLayout.OffsetInternalOffset;
		Off::InSDK::Properties::PropertySize = Off::ExternalFPropertyLayout.PropertySize;
		Off::BoolProperty::Base = Off::ExternalFPropertyLayout.BoolPropertyBase;
		Off::EnumProperty::Base = Off::ExternalFPropertyLayout.EnumPropertyBase;
		Off::ObjectProperty::PropertyClass = Off::InSDK::Properties::PropertySize;
		Off::ByteProperty::Enum = Off::InSDK::Properties::PropertySize;
		Off::StructProperty::Struct = Off::ExternalFPropertyLayout.StructPropertyStructOffset;
		Off::DelegateProperty::SignatureFunction = Off::InSDK::Properties::PropertySize;
		Off::ArrayProperty::Inner = Off::ExternalFPropertyLayout.ArrayInnerOffset;
		Off::SetProperty::ElementProp = Off::InSDK::Properties::PropertySize;
		Off::MapProperty::Base = Off::InSDK::Properties::PropertySize;
	}
	else
	{
	ReportOffsetStage("Property element size");
	Off::Property::ElementSize = OffsetFinder::FindElementSizeOffset();
	std::cerr << std::format("Off::Property::ElementSize: 0x{:X}\n", Off::Property::ElementSize);

	ReportOffsetStage("Property array dimension");
	Off::Property::ArrayDim = OffsetFinder::FindArrayDimOffset();
	std::cerr << std::format("Off::Property::ArrayDim: 0x{:X}\n", Off::Property::ArrayDim);

	ReportOffsetStage("Property internal offset");
	Off::Property::Offset_Internal = OffsetFinder::FindOffsetInternalOffset();
	std::cerr << std::format("Off::Property::Offset_Internal: 0x{:X}\n", Off::Property::Offset_Internal);

	ReportOffsetStage("Property flags");
	Off::Property::PropertyFlags = OffsetFinder::FindPropertyFlagsOffset();
	std::cerr << std::format("Off::Property::PropertyFlags: 0x{:X}\n", Off::Property::PropertyFlags);

	ReportOffsetStage("BoolProperty base");
	Off::BoolProperty::Base = OffsetFinder::FindBoolPropertyBaseOffset();
	std::cerr << std::format("UBoolProperty::Base: 0x{:X}\n", Off::BoolProperty::Base) << std::endl;

	ReportOffsetStage("EnumProperty base");
	Off::EnumProperty::Base = OffsetFinder::FindEnumPropertyBaseOffset();
	std::cerr << std::format("Off::EnumProperty::Base: 0x{:X}\n", Off::EnumProperty::Base) << std::endl;


	if (Off::EnumProperty::Base == OffsetFinder::OffsetNotFound)
	{
		Off::InSDK::Properties::PropertySize = Off::BoolProperty::Base;
		Off::EnumProperty::Base = Off::BoolProperty::Base;
	}
	else
	{
		Off::InSDK::Properties::PropertySize = Off::EnumProperty::Base;
	}

	std::cerr << std::format("UPropertySize: 0x{:X}\n", Off::InSDK::Properties::PropertySize) << std::endl;

	ReportOffsetStage("ObjectProperty class");
	Off::ObjectProperty::PropertyClass = OffsetFinder::FindObjectPropertyClassOffset();
	std::cerr << std::format("Off::ObjectProperty::PropertyClass: 0x{:X}", Off::ObjectProperty::PropertyClass) << std::endl;
	OverwriteIfInvalidOffset(Off::ObjectProperty::PropertyClass, Off::InSDK::Properties::PropertySize);

	ReportOffsetStage("ByteProperty enum");
	Off::ByteProperty::Enum = OffsetFinder::FindBytePropertyEnumOffset();
	OverwriteIfInvalidOffset(Off::ByteProperty::Enum, Off::InSDK::Properties::PropertySize);
	std::cerr << std::format("Off::ByteProperty::Enum: 0x{:X}", Off::ByteProperty::Enum) << std::endl;

	ReportOffsetStage("StructProperty struct");
	Off::StructProperty::Struct = OffsetFinder::FindStructPropertyStructOffset();
	OverwriteIfInvalidOffset(Off::StructProperty::Struct, Off::InSDK::Properties::PropertySize);
	std::cerr << std::format("Off::StructProperty::Struct: 0x{:X}\n", Off::StructProperty::Struct) << std::endl;

	ReportOffsetStage("DelegateProperty signature");
	Off::DelegateProperty::SignatureFunction = OffsetFinder::FindDelegatePropertySignatureFunctionOffset();
	OverwriteIfInvalidOffset(Off::DelegateProperty::SignatureFunction, Off::InSDK::Properties::PropertySize);
	std::cerr << std::format("Off::DelegateProperty::SignatureFunction: 0x{:X}\n", Off::DelegateProperty::SignatureFunction) << std::endl;

	ReportOffsetStage("ArrayProperty inner");
	Off::ArrayProperty::Inner = OffsetFinder::FindInnerTypeOffset(Off::InSDK::Properties::PropertySize);
	std::cerr << std::format("Off::ArrayProperty::Inner: 0x{:X}\n", Off::ArrayProperty::Inner);

	ReportOffsetStage("SetProperty element");
	Off::SetProperty::ElementProp = OffsetFinder::FindSetPropertyBaseOffset(Off::InSDK::Properties::PropertySize);
	std::cerr << std::format("Off::SetProperty::ElementProp: 0x{:X}\n", Off::SetProperty::ElementProp);

	ReportOffsetStage("MapProperty base");
	Off::MapProperty::Base = OffsetFinder::FindMapPropertyBaseOffset(Off::InSDK::Properties::PropertySize);
	std::cerr << std::format("Off::MapProperty::Base: 0x{:X}\n", Off::MapProperty::Base) << std::endl;
	}

	std::cerr << std::format("Off::Property::ArrayDim: 0x{:X}\n", Off::Property::ArrayDim);
	std::cerr << std::format("Off::Property::ElementSize: 0x{:X}\n", Off::Property::ElementSize);
	std::cerr << std::format("Off::Property::PropertyFlags: 0x{:X}\n", Off::Property::PropertyFlags);
	std::cerr << std::format("Off::Property::Offset_Internal: 0x{:X}\n", Off::Property::Offset_Internal);
	std::cerr << std::format("Off::InSDK::Properties::PropertySize: 0x{:X}\n", Off::InSDK::Properties::PropertySize);

	ReportOffsetStage(Off::ExternalEngineLayout.HasLevelActors()
		? "ULevel actors (UEVR metadata)"
		: "ULevel actors");
	Off::InSDK::ULevel::Actors = Off::ExternalEngineLayout.HasLevelActors()
		? Off::ExternalEngineLayout.LevelActorsOffset
		: OffsetFinder::FindLevelActorsOffset();
	std::cerr << std::format("Off::InSDK::ULevel::Actors: 0x{:X}\n", Off::InSDK::ULevel::Actors) << std::endl;

	ReportOffsetStage(Off::ExternalEngineLayout.HasDataTableRowMap()
		? "UDataTable row map (UEVR metadata)"
		: "UDataTable row map");
	Off::InSDK::UDataTable::RowMap = Off::ExternalEngineLayout.HasDataTableRowMap()
		? Off::ExternalEngineLayout.DataTableRowMapOffset
		: OffsetFinder::FindDatatableRowMapOffset();
	std::cerr << std::format("Off::InSDK::UDataTable::RowMap: 0x{:X}\n", Off::InSDK::UDataTable::RowMap) << std::endl;

	const bool bTrustUEVRFNameLayout = Off::ExternalFFieldLayout.IsCompactUE58Layout() &&
		Off::ExternalFPropertyLayout.IsValid();
	ReportOffsetStage(bTrustUEVRFNameLayout
		? "FName post-init verification (UEVR metadata)"
		: "FName post-init verification");
	if (!bTrustUEVRFNameLayout)
		OffsetFinder::PostInitFNameSettings();

	std::cerr << std::endl;

	Off::FieldPathProperty::FieldClass = Off::InSDK::Properties::PropertySize;
	Off::OptionalProperty::ValueProperty = Off::InSDK::Properties::PropertySize;

	Off::ClassProperty::MetaClass = Off::ObjectProperty::PropertyClass + sizeof(void*); //0x8 inheritance from ObjectProperty
}

void PropertySizes::Init()
{
	const bool bTrustUEVRPropertyLayout = Off::ExternalFFieldLayout.IsCompactUE58Layout() &&
		Off::ExternalFPropertyLayout.IsValid();

	Generator::ReportProgress(bTrustUEVRPropertyLayout
		? "Property sizes: delegate (UEVR metadata)"
		: "Property sizes: delegate");
	if (Off::ExternalPropertyValueSizes.HasDelegateProperty())
		PropertySizes::DelegateProperty = Off::ExternalPropertyValueSizes.DelegateProperty;
	else if (!bTrustUEVRPropertyLayout)
		InitTDelegateSize();

	Generator::ReportProgress(bTrustUEVRPropertyLayout
		? "Property sizes: field path (UEVR metadata)"
		: "Property sizes: field path");
	if (Off::ExternalPropertyValueSizes.HasFieldPathProperty())
		PropertySizes::FieldPathProperty = Off::ExternalPropertyValueSizes.FieldPathProperty;
	else if (!bTrustUEVRPropertyLayout)
		InitFFieldPathSize();

	Generator::ReportProgress(bTrustUEVRPropertyLayout
		? "Property sizes: multicast inline delegate (UEVR metadata)"
		: "Property sizes: multicast inline delegate");
	if (Off::ExternalPropertyValueSizes.HasMulticastInlineDelegateProperty())
		PropertySizes::MulticastInlineDelegateProperty =
			Off::ExternalPropertyValueSizes.MulticastInlineDelegateProperty;
	else if (!bTrustUEVRPropertyLayout)
		InitTMulticastInlineDelegateSize();
}

void PropertySizes::InitTDelegateSize()
{
	/* If the AudioComponent class or the OnQueueSubtitles member weren't found, fallback to looping GObjects and looking for a Delegate. */
	auto OnPropertyNotFound = [&]() -> void
	{
		for (UEObject Obj : ObjectArray())
		{
			if (!Obj.IsA(EClassCastFlags::Struct))
				continue;

			for (UEProperty Prop : Obj.Cast<UEClass>().GetProperties())
			{
				if (Prop.IsA(EClassCastFlags::DelegateProperty))
				{
					PropertySizes::DelegateProperty = Prop.GetSize();
					return;
				}
			}
		}
	};

	const UEClass AudioComponentClass = ObjectArray::FindClassFast("AudioComponent");

	if (!AudioComponentClass)
		return OnPropertyNotFound();

	const UEProperty OnQueueSubtitlesProp = AudioComponentClass.FindMember("OnQueueSubtitles", EClassCastFlags::DelegateProperty);

	if (!OnQueueSubtitlesProp)
		return OnPropertyNotFound();

	PropertySizes::DelegateProperty = OnQueueSubtitlesProp.GetSize();
}

void PropertySizes::InitFFieldPathSize()
{
	if (!Settings::Internal::bUseFProperty)
		return;

	/* If the SetFieldPathPropertyByName function or the Value parameter weren't found, fallback to looping GObjects and looking for a Delegate. */
	auto OnPropertyNotFound = [&]() -> void
	{
		for (UEObject Obj : ObjectArray())
		{
			if (!Obj.IsA(EClassCastFlags::Struct))
				continue;

			for (UEProperty Prop : Obj.Cast<UEClass>().GetProperties())
			{
				if (Prop.IsA(EClassCastFlags::FieldPathProperty))
				{
					PropertySizes::FieldPathProperty = Prop.GetSize();
					return;
				}
			}
		}
	};

	const UEFunction SetFieldPathPropertyByNameFunc = ObjectArray::FindObjectFast<UEFunction>("SetFieldPathPropertyByName", EClassCastFlags::Function);

	if (!SetFieldPathPropertyByNameFunc)
		return OnPropertyNotFound();

	const UEProperty ValueParamProp = SetFieldPathPropertyByNameFunc.FindMember("Value", EClassCastFlags::FieldPathProperty);

	if (!ValueParamProp)
		return OnPropertyNotFound();

	PropertySizes::FieldPathProperty = ValueParamProp.GetSize();
}

void PropertySizes::InitTMulticastInlineDelegateSize()
{
	/* If the AudioComponent class or the OnQueueSubtitles member weren't found, fallback to looping GObjects and looking for a Delegate. */
	auto OnPropertyNotFound = [&]() -> void
		{
			for (UEObject Obj : ObjectArray())
			{
				if (!Obj.IsA(EClassCastFlags::Struct))
					continue;

				for (UEProperty Prop : Obj.Cast<UEClass>().GetProperties())
				{
					if (Prop.IsA(EClassCastFlags::MulticastInlineDelegateProperty))
					{
						PropertySizes::DelegateProperty = Prop.GetSize();
						return;
					}
				}
			}
		};

	const UEClass EmitterClass = ObjectArray::FindClassFast("Emitter");

	if (!EmitterClass)
		return OnPropertyNotFound();

	const UEProperty OnParticleSpawn = EmitterClass.FindMember("OnParticleSpawn", EClassCastFlags::MulticastDelegateProperty);

	if (!OnParticleSpawn)
		return OnPropertyNotFound();

	PropertySizes::MulticastInlineDelegateProperty = OnParticleSpawn.GetSize();
}
