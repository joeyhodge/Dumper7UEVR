#pragma once

#include <filesystem>
#include <functional>
#include <string_view>

#include "Unreal/ObjectArray.h"
#include "Managers/DependencyManager.h"
#include "Managers/MemberManager.h"
#include "HashStringTable.h"


namespace fs = std::filesystem;

void DumpEditorOnlyMetadata(const fs::path& DumperFolder);

void DumpEditorOnlyMetadata(const fs::path& DumperFolder);

template<typename GeneratorType>
concept GeneratorImplementation = requires(GeneratorType t)
{
    /* Require static variables of type */
    GeneratorType::PredefinedMembers;
    requires(std::same_as<decltype(GeneratorType::PredefinedMembers), PredefinedMemberLookupMapType>);

    GeneratorType::MainFolderName;
    requires(std::same_as<decltype(GeneratorType::MainFolderName), std::string>);
    GeneratorType::SubfolderName;
    requires(std::same_as<decltype(GeneratorType::SubfolderName), std::string>);

    GeneratorType::MainFolder;
    requires(std::same_as<decltype(GeneratorType::MainFolder), fs::path>);
    GeneratorType::Subfolder;
    requires(std::same_as<decltype(GeneratorType::Subfolder), fs::path>);
    
    /* Require static functions */
    GeneratorType::Generate();

    GeneratorType::InitPredefinedMembers();
    GeneratorType::InitPredefinedFunctions();
};

class Generator
{
public:
    using ProgressCallback = std::function<void(std::string_view)>;

    static std::string SDKFolder;

private:
    friend class GeneratorTest;

private:
    static inline fs::path DumperFolder;
    static inline bool bDumpedGObjects = false;
    static inline ProgressCallback ProgressReporter{};
	static inline bool bDumpedEditorOnlyMetadata = false;

public:
	static bool InitEngineCore();
    static void InitInternal();
    static bool PrepareOutputFolder();
    static void SetProgressCallback(ProgressCallback callback);
    static void ReportProgress(std::string_view progress);

private:
    static bool SetupDumperFolder();

    static bool SetupFolders(std::string& FolderName, fs::path& OutFolder);
    static bool SetupFolders(std::string& FolderName, fs::path& OutFolder, std::string& SubfolderName, fs::path& OutSubFolder);

public:
    template<GeneratorImplementation GeneratorType>
    static bool Generate()
    {
        if (DumperFolder.empty() && !PrepareOutputFolder())
            return false;

        if (!bDumpedGObjects)
        {
            bDumpedGObjects = true;
			ReportProgress("Generator: GObjects dump");
            ObjectArray::DumpObjects(DumperFolder);

            if (Settings::Internal::bUseFProperty)
			{
				ReportProgress("Generator: GObjects property dump");
                ObjectArray::DumpObjectsWithProperties(DumperFolder);
			}

			ReportProgress("Generator: GObjects dumps complete");
        }

		if (!bDumpedEditorOnlyMetadata)
		{
			bDumpedEditorOnlyMetadata = true;
			ReportProgress("Generator: editor-only metadata");
			DumpEditorOnlyMetadata(DumperFolder);
		}

		ReportProgress("Generator: output folders");
        if (!SetupFolders(GeneratorType::MainFolderName, GeneratorType::MainFolder, GeneratorType::SubfolderName, GeneratorType::Subfolder))
            return false;

		ReportProgress("Generator: predefined members");
        GeneratorType::InitPredefinedMembers();
		ReportProgress("Generator: predefined functions");
        GeneratorType::InitPredefinedFunctions();

        MemberManager::SetPredefinedMemberLookupPtr(&GeneratorType::PredefinedMembers);

		ReportProgress("Generator: implementation");
        GeneratorType::Generate();
		ReportProgress("Generator: implementation complete");
        return true;
    };
};
