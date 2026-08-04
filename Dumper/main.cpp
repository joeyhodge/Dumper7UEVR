#define _SILENCE_ALL_CXX17_DEPRECATION_WARNINGS

#include <Windows.h>
#include <Xinput.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <codecvt>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <locale>
#include <memory>
#include <string>
#include <string_view>
#include <sstream>
#include <unordered_set>
#include <vector>

#include "Generators/CppGenerator.h"
#include "Generators/DumpspaceGenerator.h"
#include "Generators/Generator.h"
#include "Generators/IDAMappingGenerator.h"
#include "Generators/MappingGenerator.h"

#include "OffsetFinder/Offsets.h"
#include "Unreal/UnrealTypes.h"

#include "uevr/Plugin.hpp"

#define MAX_PATH_SIZE 512

BOOL StartUEDump(const std::string& DumpLocation, HANDLE hModule);

/*
This file (Plugin.cpp) is licensed under the MIT license and is separate from the rest of the UEVR codebase.

Copyright (c) 2023 praydog

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

using namespace uevr;

namespace {
bool capture_uevr_object_snapshot();
}

class DumpPlugin : public uevr::Plugin {
public:
    const UEVR_PluginInitializeParam* m_Param{};
    const UEVR_VRData* m_VR{};
    int m_DumpCount{};
    std::string m_PersistentDir{};
    std::string m_DumperOutputPath{};
    HANDLE m_ModuleHandle{};

public:
    void on_dllmain(HANDLE handle) override
    {
        m_DumpCount = 0;
        m_ModuleHandle = handle;
    }

    void on_initialize() override
    {
        wchar_t persistentDir[MAX_PATH_SIZE]{};
        m_Param = API::get()->param();
        m_VR = m_Param != nullptr ? m_Param->vr : nullptr;

        API::get()->param()->functions->get_persistent_dir(persistentDir, MAX_PATH_SIZE);

        std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
        m_PersistentDir = converter.to_bytes(persistentDir);
        m_DumperOutputPath = m_PersistentDir + "\\Dumper7SDK";

        API::get()->log_info("dump.dll: dumper7 output path: %s", m_DumperOutputPath.c_str());
    }

    void on_present() override
    {
        static bool dumped = false;

        if (dumped)
            return;

        if (GetAsyncKeyState(VK_F8) & 1)
        {
            dumped = true;
            trigger_dump(false);
        }
    }

    void on_xinput_get_state(uint32_t* retval, uint32_t user_index, XINPUT_STATE* state) override
    {
        static bool dumped = false;
        if (dumped || state == nullptr)
            return;

        if (state->Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_THUMB &&
            state->Gamepad.bLeftTrigger >= 200 &&
            state->Gamepad.bRightTrigger >= 200)
        {
            dumped = true;
            trigger_dump(true);
        }
    }

private:
    void trigger_dump(bool useHaptics)
    {
        API::get()->log_info("dump.dll: dumping values");

        // Capture through UEVR on the render thread so the worker only reads stable pointers.
        if (!capture_uevr_object_snapshot())
        {
            API::get()->log_warn("dump.dll: could not capture the UEVR object snapshot; Dumper-7 will use its guarded fallback");
        }

        try
        {
            print_all_objects();
        }
        catch (const std::exception& e)
        {
            API::get()->log_error("dump.dll: object pre-pass failed: %s", e.what());
        }
        catch (...)
        {
            API::get()->log_error("dump.dll: object pre-pass failed with unknown exception");
        }

        if (useHaptics && m_VR != nullptr)
        {
            UEVR_InputSourceHandle leftController = m_VR->get_left_joystick_source();
            UEVR_InputSourceHandle rightController = m_VR->get_right_joystick_source();

            m_VR->trigger_haptic_vibration(0.0f, 0.05f, 1.0f, 1.0f, leftController);
            m_VR->trigger_haptic_vibration(0.0f, 0.05f, 1.0f, 1.0f, rightController);
            m_VR->trigger_haptic_vibration(0.0f, 0.05f, 1.0f, 1.0f, leftController);
            m_VR->trigger_haptic_vibration(0.0f, 0.05f, 1.0f, 1.0f, rightController);
        }

        API::get()->log_info("dump.dll: starting Dumper-7 worker thread");
        StartUEDump(m_DumperOutputPath, m_ModuleHandle);
    }

    void print_all_objects()
    {
        m_DumpCount++;
        std::string filePath = m_PersistentDir + "\\object_dump_" + std::to_string(m_DumpCount) + ".txt";
        API::get()->log_info("dump.dll: writing object list to %s", filePath.c_str());

        std::ofstream file(filePath);
        if (!file.is_open())
            return;

        const auto objects = API::FUObjectArray::get();
        if (objects == nullptr)
        {
            file << "Chunked: unavailable\n";
            file << "Inlined: unavailable\n";
            file << "Objects offset: unavailable\n";
            file << "Item distance: unavailable\n";
            file << "Object count: unavailable\n";
            file << "------------\n";
            file << "Failed to get FUObjectArray\n";
            return;
        }

        file << "Chunked: " << API::FUObjectArray::is_chunked() << "\n";
        file << "Inlined: " << API::FUObjectArray::is_inlined() << "\n";
        file << "Objects offset: " << API::FUObjectArray::get_objects_offset() << "\n";
        file << "Item distance: " << API::FUObjectArray::get_item_distance() << "\n";

        int32_t objectCount = 0;

        try
        {
            objectCount = objects->get_object_count();
            file << "Object count: " << objectCount << "\n";
        }
        catch (const std::exception& e)
        {
            file << "Object count: exception: " << e.what() << "\n";
            file << "------------\n";
            return;
        }
        catch (...)
        {
            file << "Object count: exception\n";
            file << "------------\n";
            return;
        }

        file << "------------\n";

        for (int32_t i = 0; i < objectCount; ++i)
        {
            try
            {
                const auto object = objects->get_object(i);
                if (object == nullptr)
                    continue;

                const auto name = object->get_full_name();
                if (name.empty())
                    continue;

                std::string nameNarrow = std::wstring_convert<std::codecvt_utf8<wchar_t>>{}.to_bytes(name);
                file << i << " " << nameNarrow << "\n";
            }
            catch (const std::exception& e)
            {
                file << i << " <exception: " << e.what() << ">\n";
            }
            catch (...)
            {
                file << i << " <exception>\n";
            }
        }
    }
};

std::unique_ptr<DumpPlugin> g_plugin{new DumpPlugin()};

namespace {
std::atomic_bool g_dump_running{false};
API::FUObjectArray* g_uevr_object_array{nullptr};
void* g_uevr_raw_object_array{nullptr};
std::vector<void*> g_uevr_object_snapshot{};
uint32_t g_uevr_item_stride{0};
thread_local DWORD g_last_seh_code{0};
thread_local uintptr_t g_last_seh_address{0};
thread_local uintptr_t g_last_seh_rip{0};

bool is_readable_uevr_range(const void* address, size_t size)
{
	if (address == nullptr || size == 0)
		return false;

	MEMORY_BASIC_INFORMATION info{};
	if (VirtualQuery(address, &info, sizeof(info)) == 0 || info.State != MEM_COMMIT ||
		(info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
	{
		return false;
	}

	const uintptr_t start = reinterpret_cast<uintptr_t>(address);
	const uintptr_t end = start + size;
	const uintptr_t region_end = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
	return end >= start && end <= region_end;
}

std::wstring uevr_fname_to_string(const void* name)
{
	if (!is_readable_uevr_range(name, sizeof(API::FName)))
		return L"None";

	try
	{
		std::wstring result = reinterpret_cast<const API::FName*>(name)->to_string();
		while (!result.empty() && result.back() == L'\0')
			result.pop_back();
		return result.empty() ? L"None" : result;
	}
	catch (...)
	{
		return L"None";
	}
}

bool try_uevr_is_a(API::UObject* object, API::UClass* type)
{
	if (object == nullptr || type == nullptr)
		return false;

#if defined(_MSC_VER)
	__try
	{
		return object->is_a(type);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
#else
	return object->is_a(type);
#endif
}

API::UStruct* try_get_uevr_super(API::UStruct* object)
{
	if (object == nullptr)
		return nullptr;

#if defined(_MSC_VER)
	__try
	{
		return object->get_super_struct();
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return nullptr;
	}
#else
	return object->get_super_struct();
#endif
}

API::UField* try_get_uevr_children(API::UStruct* object)
{
	if (object == nullptr)
		return nullptr;

#if defined(_MSC_VER)
	__try
	{
		return object->get_children();
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return nullptr;
	}
#else
	return object->get_children();
#endif
}

API::UField* try_get_uevr_next(API::UField* field)
{
	if (field == nullptr)
		return nullptr;

#if defined(_MSC_VER)
	__try
	{
		return field->get_next();
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return nullptr;
	}
#else
	return field->get_next();
#endif
}

int32_t find_uevr_internal_index_offset(const std::vector<void*>& snapshot)
{
	for (int32_t offset = 0; offset <= 0x30; offset += static_cast<int32_t>(sizeof(int32_t)))
	{
		int32_t tested = 0;
		int32_t matched = 0;

		for (size_t index = 0; index < snapshot.size() && tested < 64; ++index)
		{
			const auto* object = static_cast<const uint8_t*>(snapshot[index]);
			if (object == nullptr || !is_readable_uevr_range(object + offset, sizeof(int32_t)))
				continue;

			int32_t candidate = -1;
			std::memcpy(&candidate, object + offset, sizeof(candidate));
			++tested;
			if (candidate == static_cast<int32_t>(index))
				++matched;
		}

		if (tested >= 16 && matched >= 16 && (matched * 10) >= (tested * 9))
			return offset;
	}

	return -1;
}

size_t repair_uevr_reflection_snapshot(std::vector<void*>& snapshot)
{
	const auto* runtime_version = API::get()->param()->version;
	const bool has_extended_ustruct_api = runtime_version != nullptr &&
		(runtime_version->major > 2 || (runtime_version->major == 2 && runtime_version->minor >= 39));
	if (!has_extended_ustruct_api)
		return 0;

	const int32_t index_offset = find_uevr_internal_index_offset(snapshot);
	auto* struct_class = API::get()->find_uobject<API::UClass>(L"Class /Script/CoreUObject.Struct");
	if (index_offset < 0 || struct_class == nullptr)
	{
		API::get()->log_warn(
			"dump.dll: could not repair the UEVR reflection snapshot (index_offset=0x%X struct_class=%p)",
			index_offset,
			struct_class);
		return 0;
	}

	std::vector<API::UStruct*> pending_structs;
	pending_structs.reserve(0x2000);
	std::unordered_set<void*> queued_structs;
	std::unordered_set<void*> visited_fields;

	auto queue_struct = [&](API::UStruct* object)
	{
		if (object != nullptr && queued_structs.insert(object).second)
			pending_structs.push_back(object);
	};

	size_t recovered = 0;
	auto register_reference = [&](API::UObject* object)
	{
		const auto* address = reinterpret_cast<const uint8_t*>(object);
		if (address == nullptr || !is_readable_uevr_range(address + index_offset, sizeof(int32_t)))
			return;

		int32_t index = -1;
		std::memcpy(&index, address + index_offset, sizeof(index));
		if (index < 0 || static_cast<size_t>(index) >= snapshot.size())
			return;

		void*& slot = snapshot[static_cast<size_t>(index)];
		if (slot == nullptr)
		{
			slot = object;
			++recovered;
		}
	};

	for (void* raw_object : snapshot)
	{
		auto* object = reinterpret_cast<API::UObject*>(raw_object);
		if (try_uevr_is_a(object, struct_class))
			queue_struct(reinterpret_cast<API::UStruct*>(object));
	}

	for (size_t position = 0; position < pending_structs.size(); ++position)
	{
		API::UStruct* object = pending_structs[position];

		if (API::UStruct* super = try_get_uevr_super(object); super != nullptr)
		{
			register_reference(super);
			queue_struct(super);
		}

		API::UField* child = try_get_uevr_children(object);
		for (int32_t depth = 0; child != nullptr && depth < 0x4000; ++depth)
		{
			if (!visited_fields.insert(child).second)
				break;

			register_reference(child);
			if (try_uevr_is_a(child, struct_class))
				queue_struct(reinterpret_cast<API::UStruct*>(child));

			child = try_get_uevr_next(child);
		}
	}

	API::get()->log_info(
		"dump.dll: repaired UEVR reflection snapshot index_offset=0x%X recovered=%zu structs=%zu",
		index_offset,
		recovered,
		pending_structs.size());
	return recovered;
}

int32_t get_uevr_object_count()
{
	return static_cast<int32_t>(g_uevr_object_snapshot.size());
}

void* get_uevr_object_by_index(int32_t index)
{
	if (index < 0 || static_cast<size_t>(index) >= g_uevr_object_snapshot.size())
		return nullptr;

	return g_uevr_object_snapshot[static_cast<size_t>(index)];
}

bool capture_uevr_object_snapshot()
{
	auto* object_array = API::FUObjectArray::get();
	if (object_array == nullptr)
		return false;

	const int32_t object_count = object_array->get_object_count();
	const uint32_t item_stride = static_cast<uint32_t>(API::FUObjectArray::get_item_distance());
	if (object_count <= 0 || object_count > 4'000'000 || item_stride < sizeof(void*))
		return false;

	std::vector<void*> snapshot(static_cast<size_t>(object_count));
	for (int32_t index = 0; index < object_count; ++index)
	{
		snapshot[static_cast<size_t>(index)] = object_array->get_object(index);
	}

	repair_uevr_reflection_snapshot(snapshot);

	g_uevr_object_array = object_array;
	g_uevr_raw_object_array = object_array;
	g_uevr_item_stride = item_stride;
	g_uevr_object_snapshot.swap(snapshot);
	API::get()->log_info("dump.dll: captured UEVR object snapshot with %d entries", object_count);

	auto FindPointerOffset = [](const void* Base, const void* Value, int32 MinOffset, int32 MaxOffset) -> int32
	{
		if (Base == nullptr || Value == nullptr)
			return -1;

		for (int32 Offset = MinOffset; Offset <= MaxOffset; Offset += static_cast<int32>(sizeof(void*)))
		{
			const void* Candidate = nullptr;
			std::memcpy(&Candidate, static_cast<const uint8_t*>(Base) + Offset, sizeof(Candidate));
			if (Candidate == Value)
				return Offset;
		}

		return -1;
	};

	auto FindContainedOffset = [](const void* Base, const void* Member, int32 MaxOffset) -> int32
	{
		if (Base == nullptr || Member == nullptr)
			return -1;

		const intptr_t Offset = reinterpret_cast<const uint8_t*>(Member) - static_cast<const uint8_t*>(Base);
		return Offset >= 0 && Offset <= MaxOffset ? static_cast<int32>(Offset) : -1;
	};

	auto FindSharedValueOffset = []<typename T>(
		const void* BaseA, T ValueA, const void* BaseB, T ValueB, int32 MinOffset, int32 MaxOffset, int32 Step) -> int32
	{
		if (BaseA == nullptr || BaseB == nullptr)
			return -1;

		for (int32 Offset = MinOffset; Offset <= MaxOffset; Offset += Step)
		{
			T CandidateA{};
			T CandidateB{};
			std::memcpy(&CandidateA, static_cast<const uint8_t*>(BaseA) + Offset, sizeof(T));
			std::memcpy(&CandidateB, static_cast<const uint8_t*>(BaseB) + Offset, sizeof(T));
			if (CandidateA == ValueA && CandidateB == ValueB)
				return Offset;
		}

		return -1;
	};

	auto IsReadableRange = [](const void* Address, size_t Size) -> bool
	{
		if (Address == nullptr || Size == 0)
			return false;

		const uintptr_t Start = reinterpret_cast<uintptr_t>(Address);
		if (Start > (std::numeric_limits<uintptr_t>::max)() - Size)
			return false;

		const uintptr_t End = Start + Size;
		uintptr_t Current = Start;
		while (Current < End)
		{
			MEMORY_BASIC_INFORMATION MemoryInfo{};
			if (VirtualQuery(reinterpret_cast<const void*>(Current), &MemoryInfo, sizeof(MemoryInfo)) == 0 ||
				MemoryInfo.State != MEM_COMMIT ||
				(MemoryInfo.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
			{
				return false;
			}

			const uintptr_t RegionStart = reinterpret_cast<uintptr_t>(MemoryInfo.BaseAddress);
			if (RegionStart > (std::numeric_limits<uintptr_t>::max)() - MemoryInfo.RegionSize)
				return false;

			const uintptr_t RegionEnd = RegionStart + MemoryInfo.RegionSize;
			if (RegionEnd <= Current)
				return false;

			Current = (std::min)(End, RegionEnd);
		}

		return true;
	};

	Off::ExternalFFieldLayout = {};
	auto* vector_struct = API::get()->find_uobject<API::UStruct>(L"ScriptStruct /Script/CoreUObject.Vector");
	if (vector_struct != nullptr)
	{
		auto* field = vector_struct->get_child_properties();
		auto* next = field != nullptr ? field->get_next() : nullptr;
		auto* field_class = field != nullptr ? field->get_class() : nullptr;
		auto* field_name = field != nullptr ? field->get_fname() : nullptr;
		auto* field_class_name = field_class != nullptr ? field_class->get_fname() : nullptr;

		Off::ExternalFFieldLayout.ClassOffset = FindPointerOffset(field, field_class, 0x00, 0x30);
		Off::ExternalFFieldLayout.NextOffset = FindPointerOffset(field, next, 0x00, 0x40);
		Off::ExternalFFieldLayout.NameOffset = FindContainedOffset(field, field_name, 0x40);
		Off::ExternalFFieldLayout.FieldClassNameOffset = FindContainedOffset(field_class, field_class_name, 0x40);
		Off::ExternalFFieldLayout.OwnerOffset = Off::ExternalFFieldLayout.ClassOffset >= 0
			? Off::ExternalFFieldLayout.ClassOffset + static_cast<int32>(sizeof(void*))
			: -1;

		if (Off::ExternalFFieldLayout.IsValid())
		{
			API::get()->log_info(
				"dump.dll: captured UEVR FField layout class=0x%X owner=0x%X next=0x%X name=0x%X field_class_name=0x%X",
				Off::ExternalFFieldLayout.ClassOffset,
				Off::ExternalFFieldLayout.OwnerOffset,
				Off::ExternalFFieldLayout.NextOffset,
				Off::ExternalFFieldLayout.NameOffset,
				Off::ExternalFFieldLayout.FieldClassNameOffset);
		}
		else
		{
			API::get()->log_warn("dump.dll: UEVR FField layout capture was incomplete");
		}
	}
	else
	{
		API::get()->log_warn("dump.dll: UEVR could not resolve CoreUObject.Vector for FField layout capture");
	}

	Off::ExternalFPropertyLayout = {};
	auto* guid_struct = API::get()->find_uobject<API::UStruct>(L"ScriptStruct /Script/CoreUObject.Guid");
	if (vector_struct != nullptr && guid_struct != nullptr)
	{
		auto* vector_x = reinterpret_cast<API::FProperty*>(vector_struct->get_child_properties());
		auto* vector_y = vector_x != nullptr ? reinterpret_cast<API::FProperty*>(vector_x->get_next()) : nullptr;
		auto* vector_z = vector_y != nullptr ? reinterpret_cast<API::FProperty*>(vector_y->get_next()) : nullptr;
		auto* guid_a = reinterpret_cast<API::FProperty*>(guid_struct->get_child_properties());
		auto* guid_b = guid_a != nullptr ? reinterpret_cast<API::FProperty*>(guid_a->get_next()) : nullptr;

		if (vector_y != nullptr && vector_z != nullptr && guid_a != nullptr && guid_b != nullptr)
		{
			const int32 VectorElementSize = vector_z->get_offset() - vector_y->get_offset();
			const int32 GuidElementSize = guid_b->get_offset() - guid_a->get_offset();
			Off::ExternalFPropertyLayout.ElementSizeOffset = FindSharedValueOffset(
				vector_z, VectorElementSize, guid_b, GuidElementSize, 0x20, 0x70, 0x4);
			Off::ExternalFPropertyLayout.OffsetInternalOffset = FindSharedValueOffset(
				vector_z, vector_z->get_offset(), guid_b, guid_b->get_offset(), 0x20, 0x70, 0x4);
			Off::ExternalFPropertyLayout.PropertyFlagsOffset = FindSharedValueOffset(
				vector_z, vector_z->get_property_flags(), guid_b, guid_b->get_property_flags(), 0x20, 0x70, 0x4);

			const int32 ArrayDimCandidate = Off::ExternalFPropertyLayout.ElementSizeOffset - static_cast<int32>(sizeof(int32));
			int32 VectorArrayDim = 0;
			int32 GuidArrayDim = 0;
			if (ArrayDimCandidate >= 0)
			{
				std::memcpy(&VectorArrayDim, reinterpret_cast<const uint8_t*>(vector_z) + ArrayDimCandidate, sizeof(VectorArrayDim));
				std::memcpy(&GuidArrayDim, reinterpret_cast<const uint8_t*>(guid_b) + ArrayDimCandidate, sizeof(GuidArrayDim));
				if (VectorArrayDim == 1 && GuidArrayDim == 1)
					Off::ExternalFPropertyLayout.ArrayDimOffset = ArrayDimCandidate;
			}
		}
	}

	auto* world_class = API::get()->find_uobject<API::UStruct>(L"Class /Script/Engine.World");
	auto* levels_property = world_class != nullptr
		? reinterpret_cast<API::FArrayProperty*>(world_class->find_property(L"Levels"))
		: nullptr;
	if (levels_property != nullptr)
	{
		auto* inner = levels_property->get_inner();
		Off::ExternalFPropertyLayout.ArrayInnerOffset = FindPointerOffset(levels_property, inner, 0x50, 0xA0);
		Off::ExternalFPropertyLayout.PropertySize = Off::ExternalFPropertyLayout.ArrayInnerOffset;
	}

	auto* player_controller_class = API::get()->find_uobject<API::UStruct>(L"Class /Script/Engine.PlayerController");
	auto* bool_property = player_controller_class != nullptr
		? reinterpret_cast<API::FBoolProperty*>(player_controller_class->find_property(L"bAutoManageActiveCameraTarget"))
		: nullptr;
	if (bool_property != nullptr && Off::ExternalFPropertyLayout.PropertySize >= 0)
	{
		for (int32 Offset = Off::ExternalFPropertyLayout.PropertySize; Offset <= Off::ExternalFPropertyLayout.PropertySize + 0x20; ++Offset)
		{
			const auto* Bytes = reinterpret_cast<const uint8_t*>(bool_property) + Offset;
			if (Bytes[0] == static_cast<uint8_t>(bool_property->get_field_size()) &&
				Bytes[1] == static_cast<uint8_t>(bool_property->get_byte_offset()) &&
				Bytes[2] == static_cast<uint8_t>(bool_property->get_byte_mask()) &&
				Bytes[3] == static_cast<uint8_t>(bool_property->get_field_mask()))
			{
				Off::ExternalFPropertyLayout.BoolPropertyBase = Offset;
				break;
			}
		}
	}

	auto* actor_component_class = API::get()->find_uobject<API::UStruct>(L"Class /Script/Engine.ActorComponent");
	auto* enum_property = actor_component_class != nullptr
		? reinterpret_cast<API::FEnumProperty*>(actor_component_class->find_property(L"CreationMethod"))
		: nullptr;
	if (enum_property != nullptr)
	{
		Off::ExternalFPropertyLayout.EnumPropertyBase = FindPointerOffset(
			enum_property, enum_property->get_underlying_prop(), 0x50, 0xA0);
	}

	auto* transform_struct = API::get()->find_uobject<API::UStruct>(L"ScriptStruct /Script/CoreUObject.Transform");
	auto* struct_property = transform_struct != nullptr
		? reinterpret_cast<API::FStructProperty*>(transform_struct->find_property(L"Rotation"))
		: nullptr;
	if (struct_property != nullptr)
	{
		Off::ExternalFPropertyLayout.StructPropertyStructOffset = FindPointerOffset(
			struct_property, struct_property->get_struct(), 0x50, 0xA0);
	}

	// These property subclasses all place their first member immediately after
	// FProperty. EnumProperty gave us that base even when optional examples such
	// as World.Levels or Transform.Rotation were absent from this shipping build.
	if (Off::ExternalFPropertyLayout.PropertySize < 0)
		Off::ExternalFPropertyLayout.PropertySize = Off::ExternalFPropertyLayout.EnumPropertyBase;
	if (Off::ExternalFPropertyLayout.BoolPropertyBase < 0)
		Off::ExternalFPropertyLayout.BoolPropertyBase = Off::ExternalFPropertyLayout.PropertySize;
	if (Off::ExternalFPropertyLayout.ArrayInnerOffset < 0)
		Off::ExternalFPropertyLayout.ArrayInnerOffset = Off::ExternalFPropertyLayout.PropertySize;
	if (Off::ExternalFPropertyLayout.StructPropertyStructOffset < 0)
		Off::ExternalFPropertyLayout.StructPropertyStructOffset = Off::ExternalFPropertyLayout.PropertySize;

	if (Off::ExternalFPropertyLayout.IsValid())
	{
		API::get()->log_info(
			"dump.dll: captured UEVR FProperty layout array_dim=0x%X element_size=0x%X flags=0x%X offset=0x%X size=0x%X bool=0x%X enum=0x%X array_inner=0x%X struct=0x%X",
			Off::ExternalFPropertyLayout.ArrayDimOffset,
			Off::ExternalFPropertyLayout.ElementSizeOffset,
			Off::ExternalFPropertyLayout.PropertyFlagsOffset,
			Off::ExternalFPropertyLayout.OffsetInternalOffset,
			Off::ExternalFPropertyLayout.PropertySize,
			Off::ExternalFPropertyLayout.BoolPropertyBase,
			Off::ExternalFPropertyLayout.EnumPropertyBase,
			Off::ExternalFPropertyLayout.ArrayInnerOffset,
			Off::ExternalFPropertyLayout.StructPropertyStructOffset);
	}
	else
	{
		API::get()->log_warn(
			"dump.dll: UEVR FProperty layout capture incomplete array_dim=0x%X element_size=0x%X flags=0x%X offset=0x%X size=0x%X bool=0x%X enum=0x%X array_inner=0x%X struct=0x%X",
			Off::ExternalFPropertyLayout.ArrayDimOffset,
			Off::ExternalFPropertyLayout.ElementSizeOffset,
			Off::ExternalFPropertyLayout.PropertyFlagsOffset,
			Off::ExternalFPropertyLayout.OffsetInternalOffset,
			Off::ExternalFPropertyLayout.PropertySize,
			Off::ExternalFPropertyLayout.BoolPropertyBase,
			Off::ExternalFPropertyLayout.EnumPropertyBase,
			Off::ExternalFPropertyLayout.ArrayInnerOffset,
			Off::ExternalFPropertyLayout.StructPropertyStructOffset);
	}

	Off::ExternalUStructLayout = {};
	const auto* runtime_version = API::get()->param()->version;
	const bool has_extended_ustruct_api = runtime_version != nullptr &&
		(runtime_version->major > 2 || (runtime_version->major == 2 && runtime_version->minor >= 39));
	if (has_extended_ustruct_api && API::get()->sdk()->ustruct != nullptr &&
		API::get()->sdk()->ustruct->get_children != nullptr && API::get()->sdk()->ufield != nullptr)
	{
		auto* kismet_system_library = API::get()->find_uobject<API::UStruct>(L"Class /Script/Engine.KismetSystemLibrary");
		auto* actor_class = API::get()->find_uobject<API::UStruct>(L"Class /Script/Engine.Actor");
		auto* children = kismet_system_library != nullptr ? kismet_system_library->get_children() : nullptr;
		auto* next_child = children != nullptr ? children->get_next() : nullptr;
		auto* actor_super = actor_class != nullptr ? actor_class->get_super_struct() : nullptr;
		auto* vector_children = vector_struct != nullptr ? vector_struct->get_child_properties() : nullptr;

		Off::ExternalUStructLayout.ChildrenOffset =
			FindPointerOffset(kismet_system_library, children, 0x20, 0x100);
		Off::ExternalUStructLayout.UFieldNextOffset =
			FindPointerOffset(children, next_child, 0x20, 0x80);
		Off::ExternalUStructLayout.SuperStructOffset =
			FindPointerOffset(actor_class, actor_super, 0x20, 0x100);
		Off::ExternalUStructLayout.ChildPropertiesOffset =
			FindPointerOffset(vector_struct, vector_children, 0x20, 0x100);

		if (vector_struct != nullptr && guid_struct != nullptr)
		{
			Off::ExternalUStructLayout.SizeOffset = FindSharedValueOffset(
				vector_struct, vector_struct->get_properties_size(),
				guid_struct, guid_struct->get_properties_size(), 0x20, 0x100, sizeof(int32));

			Off::ExternalUStructLayout.MinAlignmentOffset = FindSharedValueOffset(
				vector_struct, static_cast<int16>(vector_struct->get_min_alignment()),
				guid_struct, static_cast<int16>(guid_struct->get_min_alignment()), 0x20, 0x100, sizeof(int16));
		}

		const char* log_format = Off::ExternalUStructLayout.IsValid()
			? "dump.dll: captured UEVR UStruct layout children=0x%X ufield_next=0x%X super=0x%X child_properties=0x%X size=0x%X alignment=0x%X"
			: "dump.dll: UEVR UStruct layout capture incomplete children=0x%X ufield_next=0x%X super=0x%X child_properties=0x%X size=0x%X alignment=0x%X";
		if (Off::ExternalUStructLayout.IsValid())
		{
			API::get()->log_info(
				log_format,
				Off::ExternalUStructLayout.ChildrenOffset,
				Off::ExternalUStructLayout.UFieldNextOffset,
				Off::ExternalUStructLayout.SuperStructOffset,
				Off::ExternalUStructLayout.ChildPropertiesOffset,
				Off::ExternalUStructLayout.SizeOffset,
				Off::ExternalUStructLayout.MinAlignmentOffset);
		}
		else
		{
			API::get()->log_warn(
				log_format,
				Off::ExternalUStructLayout.ChildrenOffset,
				Off::ExternalUStructLayout.UFieldNextOffset,
				Off::ExternalUStructLayout.SuperStructOffset,
				Off::ExternalUStructLayout.ChildPropertiesOffset,
				Off::ExternalUStructLayout.SizeOffset,
				Off::ExternalUStructLayout.MinAlignmentOffset);
		}
	}
	else
	{
		API::get()->log_warn("dump.dll: current UEVR runtime does not expose the extended UStruct/UField API");
	}

	Off::ExternalPropertyValueSizes = {};
	auto ReadPropertyElementSize = [](const API::FProperty* Property) -> int32
	{
		if (Property == nullptr || Off::ExternalFPropertyLayout.ElementSizeOffset < 0)
			return -1;

		int32 ElementSize = -1;
		std::memcpy(
			&ElementSize,
			reinterpret_cast<const uint8_t*>(Property) + Off::ExternalFPropertyLayout.ElementSizeOffset,
			sizeof(ElementSize));
		return ElementSize > 0 && ElementSize <= 0x1000 ? ElementSize : -1;
	};

	auto* audio_component_class = API::get()->find_uobject<API::UStruct>(L"Class /Script/Engine.AudioComponent");
	Off::ExternalPropertyValueSizes.DelegateProperty = ReadPropertyElementSize(
		audio_component_class != nullptr ? audio_component_class->find_property(L"OnQueueSubtitles") : nullptr);

	auto* set_field_path_function = API::get()->find_uobject<API::UStruct>(
		L"Function /Script/Engine.KismetSystemLibrary.SetFieldPathPropertyByName");
	Off::ExternalPropertyValueSizes.FieldPathProperty = ReadPropertyElementSize(
		set_field_path_function != nullptr ? set_field_path_function->find_property(L"Value") : nullptr);

	auto* primitive_component_class = API::get()->find_uobject<API::UStruct>(L"Class /Script/Engine.PrimitiveComponent");
	Off::ExternalPropertyValueSizes.MulticastInlineDelegateProperty = ReadPropertyElementSize(
		primitive_component_class != nullptr ? primitive_component_class->find_property(L"OnComponentHit") : nullptr);

	API::get()->log_info(
		"dump.dll: captured UEVR property value sizes delegate=0x%X field_path=0x%X multicast_inline=0x%X",
		Off::ExternalPropertyValueSizes.DelegateProperty,
		Off::ExternalPropertyValueSizes.FieldPathProperty,
		Off::ExternalPropertyValueSizes.MulticastInlineDelegateProperty);

	Off::ExternalFTextLayout = {};
	auto* conv_string_to_text = API::get()->find_uobject<API::UStruct>(
		L"Function /Script/Engine.KismetTextLibrary.Conv_StringToText");
	auto* text_return_property = conv_string_to_text != nullptr
		? conv_string_to_text->find_property(L"ReturnValue")
		: nullptr;
	Off::ExternalFTextLayout.TextSize = ReadPropertyElementSize(text_return_property);
	if (Off::ExternalFTextLayout.IsValid())
	{
		API::get()->log_info(
			"dump.dll: captured UEVR FText size=0x%X",
			Off::ExternalFTextLayout.TextSize);
	}
	else
	{
		API::get()->log_warn("dump.dll: UEVR FText size capture was incomplete");
	}

	Off::ExternalGeneratorSettings = {};
	auto* load_asset_function = API::get()->find_uobject<API::UStruct>(
		L"Function /Script/Engine.KismetSystemLibrary.LoadAsset");
	auto* load_asset_property = load_asset_function != nullptr
		? load_asset_function->find_property(L"Asset")
		: nullptr;
	auto* soft_object_path_struct = API::get()->find_uobject<API::UStruct>(
		L"ScriptStruct /Script/CoreUObject.SoftObjectPath");
	const int32 LoadAssetPropertySize = ReadPropertyElementSize(load_asset_property);
	const int32 SoftObjectPathSize = soft_object_path_struct != nullptr
		? soft_object_path_struct->get_properties_size()
		: -1;
	if (LoadAssetPropertySize > 0 && SoftObjectPathSize > 0)
	{
		Off::ExternalGeneratorSettings.HasWeakObjectPtrWithoutTag = true;
		Off::ExternalGeneratorSettings.WeakObjectPtrWithoutTag =
			LoadAssetPropertySize <= SoftObjectPathSize + 0x8;
	}

	auto* vector_x_property = vector_struct != nullptr
		? vector_struct->find_property(L"X")
		: nullptr;
	const int32 VectorComponentSize = ReadPropertyElementSize(vector_x_property);
	if (VectorComponentSize == static_cast<int32>(sizeof(float)) ||
		VectorComponentSize == static_cast<int32>(sizeof(double)))
	{
		Off::ExternalGeneratorSettings.HasLargeWorldCoordinates = true;
		Off::ExternalGeneratorSettings.LargeWorldCoordinates =
			VectorComponentSize == static_cast<int32>(sizeof(double));
	}

	// Standard FField-based engines keep FieldPathProperty and ObjectPtrProperty
	// distinct. When UEVR has already supplied a validated FField layout, avoid
	// rescanning its UObject snapshot for the legacy UClass-based probe: modern
	// property descriptors are FFields, and stale class entries can fault there.
	if (Off::ExternalFFieldLayout.IsValid())
	{
		Off::ExternalGeneratorSettings.HasObjectPtrInsteadOfFieldPath = true;
		Off::ExternalGeneratorSettings.ObjectPtrInsteadOfFieldPath = false;
	}

	if (vector_x_property != nullptr && Off::ExternalFPropertyLayout.ArrayDimOffset >= 0)
	{
		int32 ArrayDim = 0;
		std::memcpy(
			&ArrayDim,
			reinterpret_cast<const uint8_t*>(vector_x_property) + Off::ExternalFPropertyLayout.ArrayDimOffset,
			sizeof(ArrayDim));
		if (ArrayDim > 0)
		{
			Off::ExternalGeneratorSettings.HasUint8ArrayDim = true;
			Off::ExternalGeneratorSettings.Uint8ArrayDim = ArrayDim >= 0x000F0001;
		}
	}

	API::get()->log_info(
		"dump.dll: captured UEVR generator settings valid=%d weak_no_tag=%d lwc=%d object_ptr_field_path=%d uint8_array_dim=%d",
		Off::ExternalGeneratorSettings.IsValid(),
		Off::ExternalGeneratorSettings.WeakObjectPtrWithoutTag,
		Off::ExternalGeneratorSettings.LargeWorldCoordinates,
		Off::ExternalGeneratorSettings.ObjectPtrInsteadOfFieldPath,
		Off::ExternalGeneratorSettings.Uint8ArrayDim);

	Off::ExternalUFunctionLayout = {};
	auto* native_function = API::get()->find_uobject<API::UFunction>(
		L"Function /Script/Engine.PlayerController.WasInputKeyJustPressed");
	if (native_function != nullptr)
	{
		const void* native_address = native_function->get_native_function();
		if (native_address != nullptr)
		{
			for (int32 Offset = 0x30; Offset < 0x140; Offset += static_cast<int32>(sizeof(void*)))
			{
				const void* Candidate = nullptr;
				std::memcpy(&Candidate, reinterpret_cast<const uint8_t*>(native_function) + Offset, sizeof(Candidate));
				if (Candidate == native_address)
				{
					Off::ExternalUFunctionLayout.ExecFunctionOffset = Offset;
					break;
				}
			}
		}
	}

	if (Off::ExternalUFunctionLayout.IsValid())
	{
		API::get()->log_info(
			"dump.dll: captured UEVR UFunction layout exec_function=0x%X",
			Off::ExternalUFunctionLayout.ExecFunctionOffset);
	}
	else
	{
		API::get()->log_warn("dump.dll: UEVR UFunction native-pointer layout capture was incomplete");
	}

	Off::ExternalEngineLayout = {};
	auto* level_class = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.Level");
	auto* object_class = API::get()->find_uobject<API::UStruct>(L"Class /Script/CoreUObject.Object");
	auto* struct_class = API::get()->find_uobject<API::UStruct>(L"Class /Script/CoreUObject.Struct");
	auto* url_struct = API::get()->find_uobject<API::UStruct>(L"ScriptStruct /Script/Engine.URL");
	auto* owning_world_property = level_class != nullptr
		? level_class->find_property(L"OwningWorld")
		: nullptr;

	if (level_class != nullptr && object_class != nullptr && url_struct != nullptr && owning_world_property != nullptr)
	{
		const int32 SearchStartUnaligned = object_class->get_properties_size() + url_struct->get_properties_size();
		const int32 SearchStart = (SearchStartUnaligned + 0x7) & ~0x7;
		const int32 SearchEnd = owning_world_property->get_offset();

		struct FRawArrayHeader
		{
			void* Data;
			int32 Count;
			int32 Max;
		};

		std::unordered_set<void*> KnownObjects;
		KnownObjects.reserve(g_uevr_object_snapshot.size());
		for (void* Object : g_uevr_object_snapshot)
		{
			if (Object != nullptr)
				KnownObjects.insert(Object);
		}

		if (SearchStart >= 0 && SearchEnd > SearchStart && SearchEnd <= 0x2000)
		{
			for (void* RawObject : g_uevr_object_snapshot)
			{
				auto* Object = reinterpret_cast<API::UObject*>(RawObject);
				if (Object == nullptr || Object->get_class() != level_class ||
					!IsReadableRange(Object, static_cast<size_t>(SearchEnd)))
				{
					continue;
				}

				for (int32 Offset = SearchStart; Offset <= SearchEnd - static_cast<int32>(sizeof(FRawArrayHeader)); Offset += 0x8)
				{
					FRawArrayHeader Candidate{};
					std::memcpy(&Candidate, reinterpret_cast<const uint8_t*>(Object) + Offset, sizeof(Candidate));
					if (Candidate.Data == nullptr || Candidate.Count <= 0 || Candidate.Max < Candidate.Count ||
						Candidate.Max > 4'000'000)
					{
						continue;
					}

					const int32 SampleCount = (std::min)(Candidate.Count, 32);
					if (!IsReadableRange(Candidate.Data, static_cast<size_t>(SampleCount) * sizeof(void*)))
						continue;

					int32 NonNullObjects = 0;
					int32 KnownObjectCount = 0;
					for (int32 Index = 0; Index < SampleCount; ++Index)
					{
						void* Actor = nullptr;
						std::memcpy(&Actor, static_cast<const uint8_t*>(Candidate.Data) +
							(static_cast<size_t>(Index) * sizeof(void*)), sizeof(Actor));
						if (Actor == nullptr)
							continue;

						++NonNullObjects;
						if (KnownObjects.contains(Actor))
							++KnownObjectCount;
					}

					if (KnownObjectCount > 0 && KnownObjectCount * 2 >= NonNullObjects)
					{
						Off::ExternalEngineLayout.LevelActorsOffset = Offset;
						break;
					}
				}

				if (Off::ExternalEngineLayout.HasLevelActors())
					break;
			}
		}
	}

	auto* data_table_class = API::get()->find_uobject<API::UStruct>(L"Class /Script/Engine.DataTable");
	auto* row_struct_property = data_table_class != nullptr
		? data_table_class->find_property(L"RowStruct")
		: nullptr;
	if (row_struct_property != nullptr)
	{
		// RowMap immediately follows the reflected TObjectPtr<UScriptStruct> RowStruct.
		Off::ExternalEngineLayout.DataTableRowMapOffset =
			row_struct_property->get_offset() + static_cast<int32>(sizeof(void*));
	}

	auto FindSnapshotIndex = [](const void* Object) -> int32
	{
		if (Object == nullptr)
			return -1;

		for (size_t Index = 0; Index < g_uevr_object_snapshot.size(); ++Index)
		{
			if (g_uevr_object_snapshot[Index] == Object)
				return static_cast<int32>(Index);
		}

		return -1;
	};
	Off::ExternalEngineLayout.ObjectClassIndex = FindSnapshotIndex(object_class);
	Off::ExternalEngineLayout.StructClassIndex = FindSnapshotIndex(struct_class);

	if (Off::ExternalEngineLayout.HasLevelActors() || Off::ExternalEngineLayout.HasDataTableRowMap() ||
		Off::ExternalEngineLayout.HasCoreClassIndices())
	{
		API::get()->log_info(
			"dump.dll: captured UEVR engine layout level_actors=0x%X datatable_row_map=0x%X object_class=%d struct_class=%d",
			Off::ExternalEngineLayout.LevelActorsOffset,
			Off::ExternalEngineLayout.DataTableRowMapOffset,
			Off::ExternalEngineLayout.ObjectClassIndex,
			Off::ExternalEngineLayout.StructClassIndex);
	}
	else
	{
		API::get()->log_warn("dump.dll: UEVR engine layout capture was incomplete");
	}

	return true;
}

std::string get_process_name()
{
	char module_path[MAX_PATH_SIZE]{};
	const DWORD length = GetModuleFileNameA(nullptr, module_path, MAX_PATH_SIZE);
	if (length == 0 || length >= MAX_PATH_SIZE)
		return {};

	return std::filesystem::path(std::string(module_path, length)).stem().string();
}

void append_status_line(const std::string& line)
{
	API::get()->log_info("dump.dll: %s", line.c_str());

    if (Generator::SDKFolder.empty())
        return;

    try
    {
		std::error_code error;
		std::filesystem::create_directories(Generator::SDKFolder, error);
		if (error)
		{
			API::get()->log_error("dump.dll: failed to create Dumper-7 status directory: %s", error.message().c_str());
			return;
		}

		std::ofstream status(Generator::SDKFolder + "\\dumper7_status.txt", std::ios::app);
		if (!status.is_open())
		{
			API::get()->log_error("dump.dll: failed to open Dumper-7 status file");
			return;
		}

		status << line << "\n";
	}
	catch (const std::exception& e)
	{
		API::get()->log_error("dump.dll: failed to write Dumper-7 status: %s", e.what());
	}
}

DWORD MainThreadImpl(HMODULE module)
{
    (void)module;

    try
    {
		FName::SetExternalToStringCallback(uevr_fname_to_string);
        Settings::Config::Load();

		if (!Generator::PrepareOutputFolder())
		{
			const std::string message = "Dumper-7 failed to create its output folder";
			append_status_line(message);
			API::get()->log_error("dump.dll: %s", message.c_str());
			return 1;
		}

		append_status_line("Started Generation [Dumper-7]");
		Generator::SetProgressCallback([](std::string_view progress)
		{
			append_status_line("Progress: " + std::string(progress));
		});
		struct ProgressCallbackReset
		{
			~ProgressCallbackReset()
			{
				Generator::SetProgressCallback({});
			}
		} progress_callback_reset;

		if (Settings::Config::SleepTimeout > 0)
		{
			append_status_line("Sleeping for " + std::to_string(Settings::Config::SleepTimeout) + "ms");
			Sleep(Settings::Config::SleepTimeout);
		}

		auto dumpStartTime = std::chrono::high_resolution_clock::now();

		if (!g_uevr_object_snapshot.empty())
		{
			if (ObjectArray::InitWithExternalAccess(g_uevr_raw_object_array, get_uevr_object_count(), g_uevr_item_stride, get_uevr_object_count, get_uevr_object_by_index))
			{
				append_status_line("Using UEVR render-thread object snapshot: count=" + std::to_string(get_uevr_object_count()) + " item_stride=" + std::to_string(g_uevr_item_stride));
			}
			else
			{
				append_status_line("UEVR object accessor was rejected; falling back to Dumper-7 raw discovery: " + ObjectArray::GetInitializationError());
			}
		}
		else
		{
			append_status_line("UEVR object snapshot is unavailable; falling back to Dumper-7 raw discovery");
		}

		append_status_line("Stage: initializing engine core");
		if (!Generator::InitEngineCore())
		{
			const auto& error = ObjectArray::GetInitializationError();
			const std::string message = "Dumper-7 failed to initialize engine core" + (error.empty() ? std::string{} : ": " + error);
			append_status_line(message);
			API::get()->log_error("dump.dll: %s", message.c_str());
			return 1;
		}

		const auto objectArraySummary = ObjectArray::GetInitializationSummary();
		append_status_line("Resolved FUObjectArray: " + objectArraySummary);

		append_status_line("Stage: initializing metadata");
		Generator::InitInternal();

		if (Settings::Generator::GameName.empty())
			Settings::Generator::GameName = get_process_name();
		if (Settings::Generator::GameName.empty())
			Settings::Generator::GameName = "UEGame";
		if (Settings::Generator::GameVersion.empty())
			Settings::Generator::GameVersion = "UEVR";

		append_status_line("GameName: " + Settings::Generator::GameName);
		append_status_line("GameVersion: " + Settings::Generator::GameVersion);
		append_status_line("FolderName: " + (Settings::Generator::GameVersion + '-' + Settings::Generator::GameName));

		auto generate = [](std::string_view name, auto generator) -> bool
		{
			append_status_line("Stage: generating " + std::string(name));
			const auto started = std::chrono::high_resolution_clock::now();
			const bool success = generator();
			const auto finished = std::chrono::high_resolution_clock::now();
			const std::chrono::duration<double, std::milli> elapsed = finished - started;
			append_status_line(std::string(success ? "Completed " : "Failed ") + std::string(name) + " (" + std::to_string(elapsed.count()) + "ms)");
			return success;
		};

		if (!generate("C++ SDK", [] { return Generator::Generate<CppGenerator>(); }) ||
			!generate("mappings", [] { return Generator::Generate<MappingGenerator>(); }) ||
			!generate("IDA mappings", [] { return Generator::Generate<IDAMappingGenerator>(); }) ||
			!generate("dumpspace", [] { return Generator::Generate<DumpspaceGenerator>(); }))
		{
			append_status_line("Dumper-7 stopped because an output generator failed");
			return 1;
		}

		const auto dumpFinishTime = std::chrono::high_resolution_clock::now();
		const std::chrono::duration<double, std::milli> dumpTime = dumpFinishTime - dumpStartTime;

		append_status_line("Generating SDK took (" + std::to_string(dumpTime.count()) + "ms)");
		append_status_line("Dumper-7 finished successfully");
		Generator::SetProgressCallback({});
		return 0;
	}
	catch (const std::exception& e)
	{
		Generator::SetProgressCallback({});
		const std::string message = "Dumper-7 terminated with C++ exception: " + std::string(e.what());
		append_status_line(message);
		API::get()->log_error("dump.dll: %s", message.c_str());
		return 1;
	}
	catch (...)
	{
		Generator::SetProgressCallback({});
		append_status_line("Dumper-7 terminated with an unknown C++ exception");
		API::get()->log_error("dump.dll: Dumper-7 terminated with an unknown C++ exception");
		return 1;
	}
}

int CaptureMainThreadException(EXCEPTION_POINTERS* exception_info)
{
	g_last_seh_code = exception_info != nullptr && exception_info->ExceptionRecord != nullptr
		? exception_info->ExceptionRecord->ExceptionCode
		: 0;
	g_last_seh_address = exception_info != nullptr && exception_info->ExceptionRecord != nullptr
		? reinterpret_cast<uintptr_t>(exception_info->ExceptionRecord->ExceptionAddress)
		: 0;
#if defined(_M_X64)
	g_last_seh_rip = exception_info != nullptr && exception_info->ContextRecord != nullptr
		? static_cast<uintptr_t>(exception_info->ContextRecord->Rip)
		: 0;
#endif
	return EXCEPTION_EXECUTE_HANDLER;
}

DWORD HandleMainThreadException()
{
	Generator::SetProgressCallback({});

    std::ostringstream ss;
    ss << "Dumper-7 terminated with SEH exception 0x" << std::hex << g_last_seh_code
		<< " at 0x" << g_last_seh_address;
#if defined(_M_X64)
	ss << " (RIP 0x" << g_last_seh_rip << ')';

	MEMORY_BASIC_INFORMATION memory_info{};
	if (g_last_seh_rip != 0 &&
		VirtualQuery(reinterpret_cast<const void*>(g_last_seh_rip), &memory_info, sizeof(memory_info)) == sizeof(memory_info) &&
		memory_info.AllocationBase != nullptr)
	{
		char module_path[MAX_PATH]{};
		const DWORD path_length = GetModuleFileNameA(
			static_cast<HMODULE>(memory_info.AllocationBase), module_path, static_cast<DWORD>(std::size(module_path)));
		if (path_length > 0 && path_length < std::size(module_path))
		{
			const char* module_name = std::strrchr(module_path, '\\');
			module_name = module_name != nullptr ? module_name + 1 : module_path;
			ss << " [" << module_name << "+0x"
				<< (g_last_seh_rip - reinterpret_cast<uintptr_t>(memory_info.AllocationBase)) << ']';
		}
	}
#endif
    append_status_line(ss.str());
    return 1;
}
}

DWORD MainThread(HMODULE module)
{
	DWORD result = 1;

    __try
    {
        result = MainThreadImpl(module);
    }
    __except (CaptureMainThreadException(GetExceptionInformation()))
    {
        result = HandleMainThreadException();
    }

	g_dump_running.store(false, std::memory_order_release);
	return result;
}

BOOL StartUEDump(const std::string& DumpLocation, HANDLE hModule)
{
	bool expected = false;
	if (!g_dump_running.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
	{
		API::get()->log_warn("dump.dll: Dumper-7 generation is already running; ignoring duplicate trigger");
		return TRUE;
	}

    Generator::SDKFolder = DumpLocation;
    const auto thread = CreateThread(0, 0, (LPTHREAD_START_ROUTINE)MainThread, hModule, 0, 0);
    if (thread == nullptr)
    {
		g_dump_running.store(false, std::memory_order_release);
        append_status_line("Failed to create Dumper-7 worker thread");
        return FALSE;
    }

    CloseHandle(thread);
    return TRUE;
}
