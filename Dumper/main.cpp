#define _SILENCE_ALL_CXX17_DEPRECATION_WARNINGS

#include <Windows.h>
#include <Xinput.h>

#include <atomic>
#include <chrono>
#include <codecvt>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <locale>
#include <memory>
#include <string>
#include <string_view>
#include <sstream>
#include <vector>

#include "Generators/CppGenerator.h"
#include "Generators/DumpspaceGenerator.h"
#include "Generators/Generator.h"
#include "Generators/IDAMappingGenerator.h"
#include "Generators/MappingGenerator.h"

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

	g_uevr_object_array = object_array;
	g_uevr_raw_object_array = object_array;
	g_uevr_item_stride = item_stride;
	g_uevr_object_snapshot.swap(snapshot);
	API::get()->log_info("dump.dll: captured UEVR object snapshot with %d entries", object_count);
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
			const std::string message = "Dumper-7 failed to initialize the UE object array" + (error.empty() ? std::string{} : ": " + error);
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
