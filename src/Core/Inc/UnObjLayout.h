/*=============================================================================
	UnObjLayout.h: Recovered memory layouts of Shark Tale's Core types.

	Target ABI: i686 MSVC 7.0 (clang-cl --target=i686-pc-windows-msvc).
	Source: Core.dll (build 2004-08-21). Each field names the function it was
	confirmed from (decompiled into docs/decomp/). "UT2003 order" means the
	slot is consistent with UT2003 and the class size but was not accessed
	directly. UNKNOWN fields are licensee additions not yet identified.

	These are plain layout structs used to pin offsets; the real C++ class
	declarations (with vtables, see symbols/vtables/) are built on top.
=============================================================================*/
#pragma once

#include <stddef.h>

/* The engine is built with 4-byte struct packing (Unreal's #pragma pack(4)).
   Proof: UClass registers as 1340 bytes; with default 8-byte packing the
   QWORD masks in UState would pad it to 1344. */
#pragma pack(push, 4)

typedef unsigned char      BYTE;
typedef unsigned short     _WORD;
typedef unsigned short     TCHAR;   // UTF-16, 2 bytes (MSVC wchar_t)
typedef int                INT;
typedef unsigned int       DWORD;
typedef unsigned long long QWORD;

class UClass;
class UObject;
class UField;
class UProperty;
class UState;
class UStruct;
class UTextBuffer;
class ULinkerLoad;

/*-----------------------------------------------------------------------------
	Containers and names.
-----------------------------------------------------------------------------*/

/* TArray<T> / FArray: Data, ArrayNum, ArrayMax. Growth rule seen in
   UStruct::Serialize and UObject::AddObject: Max = Num + 3*Num/8 + 32. */
struct FArrayLayout
{
	void* Data;
	INT   ArrayNum;
	INT   ArrayMax;
};

/* FString is TArray<TCHAR> with the terminating zero counted in ArrayNum. */
typedef FArrayLayout FStringLayout;

/* FName is one index into the global Names table (FNameEntry*[]). */
struct FNameLayout
{
	INT Index;
};

enum { NAME_SIZE = 64, NAME_HASH_SIZE = 4096 };

struct FNameEntryLayout
{
	INT               Index;            // 0x00  UT2003 order
	DWORD             Flags;            // 0x04  UT2003 order
	FNameEntryLayout* HashNext;         // 0x08  FName::Hardcode (NameHash[h & 0xfff])
	TCHAR             Name[NAME_SIZE];  // 0x0C  FName::Hardcode, UObject::GetFullName
};

/*-----------------------------------------------------------------------------
	FArchive (0x40 bytes). Defaults from FArchive::FArchive().
	Vtable: 22 slots, UT2003 order (symbols/vtables/Core.FArchive.txt).
-----------------------------------------------------------------------------*/

enum
{
	SHARKTALE_PACKAGE_VERSION  = 0x81,  // 129; matches STGame.u header
	SHARKTALE_LICENSEE_VERSION = 3,     // matches STGame.u header
};

struct FArchiveLayout
{
	void** VfTable;            // 0x00
	INT    ArVer;              // 0x04  = 0x81; UStruct::Serialize tests > 0x77
	INT    ArNetVer;           // 0x08  = 2000
	INT    ArLicenseeVer;      // 0x0C  = 3
	INT    ArIsLoading;        // 0x10  UStruct::Serialize
	INT    ArIsSaving;         // 0x14  UT2003 order
	INT    ArIsTrans;          // 0x18  UT2003 order
	INT    ArIsPersistent;     // 0x1C  UT2003 order
	INT    ArForEdit;          // 0x20  = 1
	INT    ArForClient;        // 0x24  = 1
	INT    ArForServer;        // 0x28  = 1
	INT    ArIsError;          // 0x2C  UT2003 order
	INT    Unknown30;          // 0x30  UNKNOWN (= 0)
	INT    Unknown34;          // 0x34  UNKNOWN (= 0); UStruct::Serialize sets it to 1
	INT    Unknown38;          // 0x38  UNKNOWN (= 0)
	INT    Unknown3C;          // 0x3C  UNKNOWN (= -1)
};

/*-----------------------------------------------------------------------------
	Script execution frames.
-----------------------------------------------------------------------------*/

/* FFrame : FOutputDevice (FOutputDevice is vtable-only, 4 bytes). */
struct FFrameLayout
{
	void**   VfTable;          // 0x00
	UStruct* Node;             // 0x04  GotoState sets StateFrame->Node
	UObject* Object;           // 0x08  UT2003 order
	BYTE*    Code;             // 0x0C  GotoState clears it
	BYTE*    Locals;           // 0x10  UT2003 order
};

struct FStateFrameLayout : FFrameLayout
{
	FFrameLayout* CurrentFrame; // 0x14  UT2003 order
	INT      Unknown18;        // 0x18  UNKNOWN: one extra field vs UT2003
	UState*  StateNode;        // 0x1C  GotoState
	QWORD    ProbeMask;        // 0x20  IsProbing (64-bit mask, bit = name - 300)
	INT      LatentAction;     // 0x28  GotoState clears it
};

/*-----------------------------------------------------------------------------
	UObject (56 bytes; UT2003 is 40). Vtable: FUnknown's 3 COM slots,
	deleting dtor, then 23 UObject virtuals (symbols/vtables/Core.UObject.txt).
-----------------------------------------------------------------------------*/

struct UObjectLayout
{
	void**             VfTable;       // 0x00  ctor
	INT                Index;         // 0x04  AddObject
	UObject*           HashNext;      // 0x08  HashObject (GObjHash[Name & 0xfff])
	FStateFrameLayout* StateFrame;    // 0x0C  IsProbing, GotoState
	ULinkerLoad*       _Linker;       // 0x10  GetLinker, SetLinker
	INT                _LinkerIndex;  // 0x14  SetLinker
	UObject*           Outer;         // 0x18  IsIn
	DWORD              ObjectFlags;   // 0x1C  GotoState (0x1000, 0x2000 bits)
	FNameLayout        Name;          // 0x20  HashObject, GetFullName
	UClass*            Class;         // 0x24  IsA, GetFullName
	DWORD              Unknown28;     // 0x28  UNKNOWN licensee field, not set in ctor
	FArrayLayout       Unknown2C;     // 0x2C  UNKNOWN licensee TArray (zeroed in ctor)
};

/*-----------------------------------------------------------------------------
	UField (68), UProperty (partial), UStruct (144), UState (1192), UClass (1340).
-----------------------------------------------------------------------------*/

struct UFieldLayout : UObjectLayout
{
	UField* SuperField;        // 0x38  IsA walks Class->SuperField
	UField* Next;              // 0x3C  UStruct::Link walks Children->Next
	UField* HashNext;          // 0x40  UT2003 order
};

struct UPropertyLayout : UFieldLayout
{
	INT        ArrayDim;            // 0x44  UStruct::Link (size = ElementSize*ArrayDim + Offset)
	INT        ElementSize;         // 0x48  UStruct::Link
	DWORD      PropertyFlags;       // 0x4C  UStruct::Link (CPF_Config 0x4000, CPF_NeedCtorLink 0x400000)
	FNameLayout Category;           // 0x50  UT2003 order
	_WORD      RepOffset;           // 0x54  UStruct::Link
	_WORD      RepIndex;            // 0x56  UT2003 order
	INT        Offset;              // 0x58  UStruct::Link, InitProperties
	UProperty* PropertyLinkNext;    // 0x5C  UT2003 order
	UProperty* ConfigLinkNext;      // 0x60  UStruct::Link (config chain)
	UProperty* ConstructorLinkNext; // 0x64  InitProperties, ExitProperties
	// 0x68.. : NextRef, RepOwner, ... (UStruct::Link touches 0x6C and 0x7C); size not yet recovered.
};

struct UStructLayout : UFieldLayout
{
	UTextBuffer* ScriptText;        // 0x44  UStruct::Serialize
	UTextBuffer* CppText;           // 0x48  UStruct::Serialize (ArVer > 0x77)
	UField*      Children;          // 0x4C  UStruct::Serialize, Link
	INT          PropertiesSize;    // 0x50  ctor param, Link (4-byte aligned)
	FNameLayout  FriendlyName;      // 0x54  UStruct::Serialize (asserts != NAME_None)
	FArrayLayout Script;            // 0x58  UStruct::Serialize (TArray<BYTE>)
	INT          TextPos;           // 0x64  UStruct::Serialize
	INT          Line;              // 0x68  UStruct::Serialize
	INT          Unknown6C;         // 0x6C  UNKNOWN (copied, not zeroed in ctor)
	INT          Unknown70;         // 0x70  UNKNOWN (zeroed in ctor)
	UProperty*   PropertyLink;      // 0x74  UT2003 order (zeroed in ctor)
	UProperty*   ConfigLink;        // 0x78  UStruct::Link
	UProperty*   ConstructorLink;   // 0x7C  InitProperties, ExitProperties
	INT          Unknown80;         // 0x80  UNKNOWN (copied only)
	INT          Unknown84;         // 0x84  UNKNOWN (copied only)
	INT          Unknown88;         // 0x88  UNKNOWN (copied only)
	INT          Unknown8C;         // 0x8C  UNKNOWN (copied only)
};

struct UStateLayout : UStructLayout
{
	QWORD    ProbeMask;            // 0x90  UState::Serialize, GotoState
	QWORD    IgnoreMask;           // 0x98  UState::Serialize, GotoState
	DWORD    StateFlags;           // 0xA0  UState::Serialize
	_WORD    LabelTableOffset;     // 0xA4  UState::Serialize
	_WORD    PadA6;                // 0xA6
	UField*  VfHash[256];          // 0xA8  UT2003 order; fills size to exactly 0x4A8
};

struct UClassLayout : UStateLayout
{
	DWORD        ClassFlags;              // 0x4A8  ctor (|= 0x12), Serialize
	INT          ClassUnique;             // 0x4AC  ctor, Serialize (reset to 0)
	DWORD        ClassGuid[4];            // 0x4B0  ctor (FGuid param), Serialize
	UClass*      ClassWithin;             // 0x4C0  ctor, Serialize (<< UObject*&)
	FNameLayout  ClassConfigName;         // 0x4C4  ctor (from Config string), Serialize
	FArrayLayout ClassReps;               // 0x4C8  UT2003 order (zeroed in ctor)
	FArrayLayout NetFields;               // 0x4D4  UT2003 order (zeroed in ctor)
	FArrayLayout Dependencies;            // 0x4E0  Serialize (own element serializer)
	FArrayLayout PackageImports;          // 0x4EC  Serialize (TArray<FName> serializer)
	FArrayLayout Defaults;                // 0x4F8  Serialize -> SerializeBin(Defaults)
	FArrayLayout HideCategories;          // 0x504  Serialize (TArray<FName> serializer); name inferred
	FArrayLayout Unknown510;              // 0x510  UNKNOWN (zeroed in ctor, not serialized)
	FArrayLayout Unknown51C;              // 0x51C  UNKNOWN (serialized, distinct element type)
	void (__cdecl *ClassConstructor)(void*);  // 0x528  ctor (InClassConstructor)
	void*        ClassStaticConstructor;  // 0x52C  ctor (member-function pointer)
	FArrayLayout Unknown530;              // 0x530  UNKNOWN (zeroed in ctor)
};

#pragma pack(pop)

/*-----------------------------------------------------------------------------
	Checks. Sizes come from symbols.db class_sizes (UClass registration).
-----------------------------------------------------------------------------*/

#define LAYOUT_OFFSET(T, F, O) static_assert(offsetof(T, F) == (O), #T "::" #F)
#define LAYOUT_SIZE(T, S)      static_assert(sizeof(T) == (S), "sizeof " #T)

static_assert(sizeof(void*) == 4, "layouts assume the original 32-bit target");
LAYOUT_SIZE(FNameEntryLayout, 0x0C + NAME_SIZE * 2);
LAYOUT_SIZE(FArchiveLayout, 0x40);
LAYOUT_SIZE(FStateFrameLayout, 0x2C);  // inferred: ends after LatentAction; not yet seen in an allocation
LAYOUT_SIZE(UObjectLayout, 56);
LAYOUT_SIZE(UFieldLayout, 68);
LAYOUT_SIZE(UStructLayout, 144);
LAYOUT_SIZE(UStateLayout, 1192);
LAYOUT_SIZE(UClassLayout, 1340);

LAYOUT_OFFSET(UObjectLayout, StateFrame, 0x0C);
LAYOUT_OFFSET(UObjectLayout, _Linker, 0x10);
LAYOUT_OFFSET(UObjectLayout, Outer, 0x18);
LAYOUT_OFFSET(UObjectLayout, ObjectFlags, 0x1C);
LAYOUT_OFFSET(UObjectLayout, Name, 0x20);
LAYOUT_OFFSET(UObjectLayout, Class, 0x24);
LAYOUT_OFFSET(UFieldLayout, SuperField, 0x38);
LAYOUT_OFFSET(UPropertyLayout, Offset, 0x58);
LAYOUT_OFFSET(UPropertyLayout, ConstructorLinkNext, 0x64);
LAYOUT_OFFSET(UStructLayout, Children, 0x4C);
LAYOUT_OFFSET(UStructLayout, FriendlyName, 0x54);
LAYOUT_OFFSET(UStructLayout, Line, 0x68);
LAYOUT_OFFSET(UStructLayout, ConfigLink, 0x78);
LAYOUT_OFFSET(UStructLayout, ConstructorLink, 0x7C);
LAYOUT_OFFSET(UStateLayout, ProbeMask, 0x90);
LAYOUT_OFFSET(UStateLayout, LabelTableOffset, 0xA4);
LAYOUT_OFFSET(UClassLayout, ClassFlags, 0x4A8);
LAYOUT_OFFSET(UClassLayout, ClassWithin, 0x4C0);
LAYOUT_OFFSET(UClassLayout, Defaults, 0x4F8);
LAYOUT_OFFSET(UClassLayout, ClassConstructor, 0x528);
LAYOUT_OFFSET(FArchiveLayout, ArLicenseeVer, 0x0C);
LAYOUT_OFFSET(FStateFrameLayout, StateNode, 0x1C);
LAYOUT_OFFSET(FStateFrameLayout, ProbeMask, 0x20);
