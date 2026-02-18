#define _SILENCE_ALL_CXX17_DEPRECATION_WARNINGS

#include <Windows.h>
#include <Xinput.h>

#include <chrono>
#include <codecvt>
#include <fstream>
#include <iostream>
#include <locale>
#include <memory>
#include <string>

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
        print_all_objects();

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

        file << "Chunked: " << API::FUObjectArray::is_chunked() << "\n";
        file << "Inlined: " << API::FUObjectArray::is_inlined() << "\n";
        file << "Objects offset: " << API::FUObjectArray::get_objects_offset() << "\n";
        file << "Item distance: " << API::FUObjectArray::get_item_distance() << "\n";
        file << "Object count: " << API::FUObjectArray::get()->get_object_count() << "\n";
        file << "------------\n";

        const auto objects = API::FUObjectArray::get();
        if (objects == nullptr)
        {
            file << "Failed to get FUObjectArray\n";
            return;
        }

        for (int32_t i = 0; i < objects->get_object_count(); ++i)
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
    }
};

std::unique_ptr<DumpPlugin> g_plugin{new DumpPlugin()};

DWORD MainThread(HMODULE module)
{
    AllocConsole();
    FILE* dummy = nullptr;
    freopen_s(&dummy, "CONOUT$", "w", stderr);
    freopen_s(&dummy, "CONIN$", "r", stdin);

    std::cerr << "Started Generation [Dumper-7]!\n";

    Settings::Config::Load();

    if (Settings::Config::SleepTimeout > 0)
    {
        std::cerr << "Sleeping for " << Settings::Config::SleepTimeout << "ms...\n";
        Sleep(Settings::Config::SleepTimeout);
    }

    auto dumpStartTime = std::chrono::high_resolution_clock::now();

    Generator::InitEngineCore();
    Generator::InitInternal();

    if (Settings::Generator::GameName.empty() && Settings::Generator::GameVersion.empty())
    {
        FString name;
        FString version;
        UEClass kismet = ObjectArray::FindClassFast("KismetSystemLibrary");
        UEFunction getGameName = kismet.GetFunction("KismetSystemLibrary", "GetGameName");
        UEFunction getEngineVersion = kismet.GetFunction("KismetSystemLibrary", "GetEngineVersion");

        kismet.ProcessEvent(getGameName, &name);
        kismet.ProcessEvent(getEngineVersion, &version);

        Settings::Generator::GameName = name.ToString();
        Settings::Generator::GameVersion = version.ToString();
    }

    std::cerr << "GameName: " << Settings::Generator::GameName << "\n";
    std::cerr << "GameVersion: " << Settings::Generator::GameVersion << "\n\n";
    std::cerr << "FolderName: " << (Settings::Generator::GameVersion + '-' + Settings::Generator::GameName) << "\n\n";

    Generator::Generate<CppGenerator>();
    Generator::Generate<MappingGenerator>();
    Generator::Generate<IDAMappingGenerator>();
    Generator::Generate<DumpspaceGenerator>();

    auto dumpFinishTime = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> dumpTime = dumpFinishTime - dumpStartTime;

    std::cerr << "\n\nGenerating SDK took (" << dumpTime.count() << "ms)\n\n\n";

    fclose(stderr);
    if (dummy)
        fclose(dummy);
    FreeConsole();
    ExitThread(0);

    return 0;
}

BOOL StartUEDump(const std::string& DumpLocation, HANDLE hModule)
{
    Generator::SDKFolder = DumpLocation;
    CreateThread(0, 0, (LPTHREAD_START_ROUTINE)MainThread, hModule, 0, 0);
    return TRUE;
}
