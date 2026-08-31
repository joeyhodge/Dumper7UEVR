#pragma once

#include <string>
#include <vector>
#include <filesystem>

#include "Unreal/UnrealObjects.h"
#include "OffsetFinder/Offsets.h"

namespace fs = std::filesystem;

class ObjectArray
{
private:
	friend struct FChunkedFixedUObjectArray;
	friend struct FFixedUObjectArray;
	friend class ObjectArrayValidator;

	friend bool IsAddressValidGObjects(const uintptr_t, const struct FFixedUObjectArrayLayout&);
	friend bool IsAddressValidGObjects(const uintptr_t, const struct FChunkedFixedUObjectArrayLayout&);

private:
	static inline uint8* GObjects = nullptr;
	static inline uint32 NumElementsPerChunk = 0x10000;
	static inline uint32 SizeOfFUObjectItem = sizeof(void*) + sizeof(int32) + sizeof(int32);
	static inline uint32 FUObjectItemInitialOffset = 0x0;
	static inline bool bUsesExternalObjectAccess = false;
	static inline bool bExternalLayoutValidated = false;

public:
	static inline std::string DecryptionLambdaStr;
	using ExternalObjectCountFn = int32(*)();
	using ExternalObjectLookupFn = void*(*)(int32);

private:
	static inline void*(*ByIndex)(void* ObjectsArray, int32 Index, uint32 FUObjectItemSize, uint32 FUObjectItemOffset, uint32 PerChunk) = nullptr;
	static inline ExternalObjectCountFn ExternalObjectCount = nullptr;
	static inline ExternalObjectLookupFn ExternalObjectLookup = nullptr;

	static inline uint8_t* (*DecryptPtr)(void* ObjPtr) = [](void* Ptr) -> uint8* { return static_cast<uint8*>(Ptr); };

private:
	static bool InitializeFUObjectItem(uint8_t* FirstItemPtr);

public:
	static void InitDecryption(uint8_t* (*DecryptionFunction)(void* ObjPtr), const char* DecryptionLambdaAsStr);

	// Returns false instead of terminating the host process when no valid object-array layout is found.
	static bool Init(bool bScanAllMemory = false, const char* const ModuleName = Settings::General::DefaultModuleName);
	// Uses UEVR's validated accessor for traversal while retaining a raw layout only when it can be verified.
	static bool InitWithExternalAccess(void* RawGObjects, int32 ExpectedObjectCount, uint32 ItemStride, ExternalObjectCountFn CountFn, ExternalObjectLookupFn LookupFn);

	static void Init(int32 GObjectsOffset, const FFixedUObjectArrayLayout& ObjectArrayLayout = FFixedUObjectArrayLayout(), const char* const ModuleName = Settings::General::DefaultModuleName);
	static void Init(int32 GObjectsOffset, int32 ElementsPerChunk, const FChunkedFixedUObjectArrayLayout& ObjectArrayLayout = FChunkedFixedUObjectArrayLayout(), const char* const ModuleName = Settings::General::DefaultModuleName);

	static void DumpObjects(const fs::path& Path, bool bWithPathname = false);
	static void DumpObjectsWithProperties(const fs::path& Path, bool bWithPathname = false);

	static int32 Num();
	static int32 Max();
	static int32 NumChunks();
	static int32 MaxChunks();

	static inline const std::string& GetInitializationError()
	{
		return InitializationError;
	}

	static inline bool IsInitialized()
	{
		return (bUsesExternalObjectAccess && ExternalObjectCount != nullptr && ExternalObjectLookup != nullptr) || (GObjects != nullptr && ByIndex != nullptr);
	}

	static inline bool UsesExternalObjectAccess()
	{
		return bUsesExternalObjectAccess;
	}

	static std::string GetInitializationSummary();

	template<typename UEType = UEObject>
	static UEType GetByIndex(int32 Index);

	template<typename UEType = UEObject>
	static UEType FindObject(const std::string& FullName, EClassCastFlags RequiredType = EClassCastFlags::None);

	template<typename UEType = UEObject>
	static UEType FindObjectFast(const std::string& Name, EClassCastFlags RequiredType = EClassCastFlags::None);

	template<typename UEType = UEObject>
	static UEType FindObjectFastInOuter(const std::string& Name, std::string Outer);

	static UEStruct FindStruct(const std::string& FullName);
	static UEStruct FindStructFast(const std::string& Name);

	static UEClass FindClass(const std::string& FullName);
	static UEClass FindClassFast(const std::string& Name);

	class ObjectsIterator
	{
		UEObject CurrentObject;
		int32 CurrentIndex;

	public:
		ObjectsIterator(int32 StartIndex = 0);

		UEObject operator*() const;
		ObjectsIterator& operator++();
		bool operator==(const ObjectsIterator& Other) const;
		bool operator!=(const ObjectsIterator& Other) const;

		int32 GetIndex() const;
	};

	ObjectsIterator begin();
	ObjectsIterator end();

	static inline void* DEBUGGetGObjects()
	{
		return GObjects;
	}

private:
	static inline std::string InitializationError;
};

#ifndef InitObjectArrayDecryption
#define InitObjectArrayDecryption(DecryptionLambda) ObjectArray::InitDecryption(DecryptionLambda, #DecryptionLambda)
#endif

class AllFieldIterator
{
private:
	ObjectArray::ObjectsIterator ObjectEndIterator;
	ObjectArray::ObjectsIterator CurrentObject;
	std::vector<UEProperty> Fields;
	int PropertyIndex = 0;

public:
	AllFieldIterator()
		: CurrentObject(ObjectArray().begin()), ObjectEndIterator(ObjectArray().end())
	{
		if (!IsCurrentObjectStruct())
			IterateToNextStructWithMembers();
	}

	AllFieldIterator(ObjectArray::ObjectsIterator StartPos)
		: CurrentObject(StartPos), ObjectEndIterator(ObjectArray().end())
	{

	}

public:
	inline AllFieldIterator begin() const
	{
		return AllFieldIterator();
	}
	inline AllFieldIterator end() const
	{
		return AllFieldIterator(ObjectArray().end());
	}

	bool operator!=(const AllFieldIterator& Other) const;

	AllFieldIterator& operator++();
	UEProperty operator*() const;

private:
	inline void IterateToNextStruct();
	inline void IterateToNextStructWithMembers();

private:
	inline bool CurrenStructHasMoreMembers() const
	{
		return (static_cast<size_t>(PropertyIndex) + 1) < Fields.size();
	}

	inline UEStruct GetCurrentStruct()
	{
		return (*CurrentObject).Cast<UEStruct>();
	}

	inline bool IsCurrentObjectStruct()
	{
		return (*CurrentObject).IsA(EClassCastFlags::Struct);
	}

	inline bool IsEndIterator() const
	{
		return CurrentObject == ObjectEndIterator;
	}
};
