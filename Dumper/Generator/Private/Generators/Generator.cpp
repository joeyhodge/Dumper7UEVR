
#include "Generators/Generator.h"
#include "Managers/StructManager.h"
#include "Managers/EnumManager.h"
#include "Managers/MemberManager.h"
#include "Managers/PackageManager.h"

#include "HashStringTable.h"
#include "Utils.h"

#include "Platform.h"

#include <utility>

std::string Generator::SDKFolder{};

inline void InitSettings()
{
	Generator::ReportProgress("Generator settings: weak object pointer");
	Settings::InitWeakObjectPtrSettings();
	Generator::ReportProgress("Generator settings: large world coordinates");
	Settings::InitLargeWorldCoordinateSettings();

	Generator::ReportProgress("Generator settings: object pointer property");
	Settings::InitObjectPtrPropertySettings();
	Generator::ReportProgress("Generator settings: array dimension");
	Settings::InitArrayDimSizeSettings();
}


bool Generator::InitEngineCore()
{
	/* manual override */
	//ObjectArray::Init(/*GObjects*/, /*Layout = Default*/); // FFixedUObjectArray (UEVersion < UE4.21)
	//ObjectArray::Init(/*GObjects*/, /*ChunkSize*/, /*Layout = Default*/); // FChunkedFixedUObjectArray (UEVersion >= UE4.21)

	//FName::Init(/*bForceGNames = false*/);
	//FName::Init(/*AppendString, FName::EOffsetOverrideType::AppendString*/);
	//FName::Init(/*ToString, FName::EOffsetOverrideType::ToString*/);
	//FName::Init(/*GNames, FName::EOffsetOverrideType::GNames, true/false*/);
 
	//Off::InSDK::ProcessEvent::InitPE(/*PEIndex*/);

	/* Back4Blood (requires manual GNames override) */
	//InitObjectArrayDecryption([](void* ObjPtr) -> uint8* { return reinterpret_cast<uint8*>(uint64(ObjPtr) ^ 0x8375); });

	/* Multiversus [Unsupported, weird GObjects-struct] */
	//InitObjectArrayDecryption([](void* ObjPtr) -> uint8* { return reinterpret_cast<uint8*>(uint64(ObjPtr) ^ 0x1B5DEAFD6B4068C); });

	ReportProgress("ObjectArray initialization");
	if (!ObjectArray::IsInitialized() && !ObjectArray::Init())
		return false;

	ReportProgress("FName initialization");
	CALL_PLATFORM_SPECIFIC_FUNCTION(FName::Init);

	ReportProgress("Core offset discovery");
	Off::Init();
	ReportProgress("Property size discovery");
	PropertySizes::Init();

	ReportProgress("ProcessEvent discovery");
	CALL_PLATFORM_SPECIFIC_FUNCTION(Off::InSDK::ProcessEvent::InitPE); // Must be at this position, relies on offsets initialized in Off::Init()

	ReportProgress("GWorld discovery");
	Off::InSDK::World::InitGWorld(); // Must be at this position, relies on offsets initialized in Off::Init()

	ReportProgress("FText offset discovery");
	Off::InSDK::Text::InitTextOffsets(); // Must be at this position, relies on offsets initialized in Off::InitPE()

	ReportProgress("Generator settings initialization");
	InitSettings();
	ReportProgress("Engine core initialization complete");
	return true;
}

void Generator::InitInternal()
{
	// Initialize PackageManager with all packages, their names, structs, classes enums, functions and dependencies
	ReportProgress("PackageManager::Init");
	PackageManager::Init();

	// Initialize StructManager with all structs and their names
	ReportProgress("StructManager::Init");
	StructManager::Init();
	
	// Initialize EnumManager with all enums and their names
	ReportProgress("EnumManager::Init");
	EnumManager::Init();
	
	// Initialized all Member-Name collisions
	ReportProgress("MemberManager::Init");
	MemberManager::Init();

	// Post-Initialize PackageManager after StructManager has been initialized. 'PostInit()' handles Cyclic-Dependencies detection
	ReportProgress("PackageManager::PostInit");
	PackageManager::PostInit();
	ReportProgress("Metadata initialization complete");
}

bool Generator::PrepareOutputFolder()
{
	if (!DumperFolder.empty())
		return true;

	bDumpedGObjects = false;
	return SetupDumperFolder();
}

void Generator::SetProgressCallback(ProgressCallback callback)
{
	ProgressReporter = std::move(callback);
}

void Generator::ReportProgress(std::string_view progress)
{
	if (ProgressReporter)
		ProgressReporter(progress);
}

bool Generator::SetupDumperFolder()
{
	try
	{
		fs::path NewDumperFolder;

		if (!SDKFolder.empty())
		{
			NewDumperFolder = fs::path(SDKFolder);
		}
		else
		{
			std::string FolderName = (Settings::Generator::GameVersion + '-' + Settings::Generator::GameName);
			FileNameHelper::MakeValidFileName(FolderName);
			NewDumperFolder = fs::path(Settings::Generator::SDKGenerationPath) / FolderName;
		}

		if (fs::exists(NewDumperFolder))
		{
			fs::path Old = NewDumperFolder.generic_string() + "_OLD";

			fs::remove_all(Old);

			fs::rename(NewDumperFolder, Old);
		}

		fs::create_directories(NewDumperFolder);
		DumperFolder = std::move(NewDumperFolder);
	}
	catch (const std::filesystem::filesystem_error& fe)
	{
		DumperFolder.clear();
		std::cerr << "Could not create required folders! Info: \n";
		std::cerr << fe.what() << std::endl;
		return false;
	}

	return true;
}

bool Generator::SetupFolders(std::string& FolderName, fs::path& OutFolder)
{
	fs::path Dummy;
	std::string EmptyName = "";
	return SetupFolders(FolderName, OutFolder, EmptyName, Dummy);
}

bool Generator::SetupFolders(std::string& FolderName, fs::path& OutFolder, std::string& SubfolderName, fs::path& OutSubFolder)
{
	FileNameHelper::MakeValidFileName(FolderName);
	FileNameHelper::MakeValidFileName(SubfolderName);

	try
	{
		OutFolder = DumperFolder / FolderName;
		OutSubFolder = OutFolder / SubfolderName;
				
		if (fs::exists(OutFolder))
		{
			fs::path Old = OutFolder.generic_string() + "_OLD";

			fs::remove_all(Old);

			fs::rename(OutFolder, Old);
		}

		fs::create_directories(OutFolder);

		if (!SubfolderName.empty())
			fs::create_directories(OutSubFolder);
	}
	catch (const std::filesystem::filesystem_error& fe)
	{
		std::cerr << "Could not create required folders! Info: \n";
		std::cerr << fe.what() << std::endl;
		return false;
	}

	return true;
}
