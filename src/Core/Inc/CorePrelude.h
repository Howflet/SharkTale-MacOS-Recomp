/*=============================================================================
	CorePrelude.h: Basic types and container layouts shared by the generated
	headers (src/<Module>/Inc/<Module>Classes.h).

	Class keys (class vs struct) and template arities match the MSVC-mangled
	exports, since mangling encodes both. Layouts are from UnObjLayout.h.
=============================================================================*/
#pragma once

#if defined(_MSC_VER)
	#define DLL_IMPORT __declspec(dllimport)
	#define DLL_EXPORT __declspec(dllexport)
#else
	#define DLL_IMPORT
	#define DLL_EXPORT
#endif

typedef unsigned char      BYTE;
typedef unsigned short     _WORD;
typedef unsigned short     TCHAR;   // UTF-16 (MSVC wchar_t is mangled as G = unsigned short)
typedef char               ANSICHAR;
typedef int                INT;
typedef unsigned int       UINT;
typedef unsigned long      DWORD;
typedef float              FLOAT;
typedef double             DOUBLE;
typedef long long          SQWORD;
typedef unsigned long long QWORD;
typedef unsigned long      BITFIELD;  // DWORD-based bitfields, packed like UE2 bool properties
typedef int                UBOOL;

#pragma pack(push, 4)

/* Placement new (we do not include <new>). */
inline void* operator new(unsigned int, void* Place) { return Place; }
inline void operator delete(void*, void*) {}

#ifndef CORE_API
#define CORE_API DLL_IMPORT
#endif

enum ENoInit : int;
class FArchive;

/* FArray: untyped dynamic array. Layout and exported members of Core.dll's
   FArray (12 bytes: Data, ArrayNum, ArrayMax). TArray<T> builds on it. */
class CORE_API FArray
{
public:
	void* Data;
	INT   ArrayNum;
	INT   ArrayMax;

	__thiscall FArray(void);
	__thiscall FArray(enum ENoInit);
	__thiscall ~FArray(void);
	class FArray& __thiscall operator=(class FArray const&);
	int __thiscall Add(int Count, int ElementSize);
	int __thiscall AddZeroed(int Count, int ElementSize);
	void __thiscall CountBytes(class FArchive& Ar, int ElementSize);
	void __thiscall Empty(int ElementSize, int Slack);
	void* __thiscall GetData(void);
	void const* __thiscall GetData(void) const;
	void __thiscall Insert(int Index, int Count, int ElementSize);
	void __thiscall InsertZeroed(int Index, int Count, int ElementSize);
	int __thiscall IsValidIndex(int Index) const;
	int __thiscall Num(void) const;
	void __thiscall Remove(int Index, int Count, int ElementSize);
	void __thiscall Shrink(int ElementSize);
protected:
	__thiscall FArray(int InNum, int ElementSize);
	void __thiscall Realloc(int ElementSize);
};

/* TArray<T>: UE2 semantics (UnTemplate.h). Element constructors/destructors
   run on copy, Remove and Empty, as in the shipped template instantiations
   (e.g. ALAudio's TArray<FString-holding struct> code). */
template<class T> class TArray : public FArray
{
public:
	typedef T ElementType;

	TArray() : FArray() {}
	TArray(int InNum) : FArray(InNum, sizeof(T)) {}
	TArray(enum ENoInit Tag) : FArray(Tag) {}
	TArray(const TArray& Other) : FArray(Other.ArrayNum, sizeof(T))
	{
		ArrayNum = 0;
		for (int i = 0; i < Other.ArrayNum; i++)
			new(*this) T(Other(i));
	}
	~TArray()
	{
		Remove(0, ArrayNum);
	}

	T& operator()(int i) { return ((T*)Data)[i]; }
	const T& operator()(int i) const { return ((const T*)Data)[i]; }
	T& Last(int c = 0) { return ((T*)Data)[ArrayNum - c - 1]; }
	int Num() const { return ArrayNum; }

	TArray& operator=(const TArray& Other)
	{
		if (this != &Other)
		{
			Empty(Other.ArrayNum);
			for (int i = 0; i < Other.ArrayNum; i++)
				new(*this) T(Other(i));
		}
		return *this;
	}

	int Add(int n = 1) { return FArray::Add(n, sizeof(T)); }
	int AddZeroed(int n = 1) { return FArray::AddZeroed(n, sizeof(T)); }
	void Insert(int Index, int Count = 1) { FArray::Insert(Index, Count, sizeof(T)); }
	void InsertZeroed(int Index, int Count = 1) { FArray::InsertZeroed(Index, Count, sizeof(T)); }
	void Shrink() { FArray::Shrink(sizeof(T)); }

	void Remove(int Index, int Count = 1)
	{
		for (int i = Index; i < Index + Count; i++)
			(&(*this)(i))->~T();
		FArray::Remove(Index, Count, sizeof(T));
	}
	void Empty(int Slack = 0)
	{
		for (int i = 0; i < ArrayNum; i++)
			(&(*this)(i))->~T();
		FArray::Empty(sizeof(T), Slack);
	}

	int AddItem(const T& Item)
	{
		int Index = Add();
		new(&(*this)(Index)) T(Item);
		return Index;
	}
	int FindItem(const T& Item) const
	{
		for (int i = 0; i < ArrayNum; i++)
			if ((*this)(i) == Item)
				return i;
		return -1;
	}
	int AddUniqueItem(const T& Item)
	{
		int Index = FindItem(Item);
		return Index >= 0 ? Index : AddItem(Item);
	}
	int RemoveItem(const T& Item)
	{
		int Count = 0;
		for (int i = 0; i < ArrayNum; i++)
			if ((*this)(i) == Item)
				Remove(i--), Count++;
		return Count;
	}
};

/* new(Array) T(...): construct in a freshly added slot. */
template<class T> inline void* operator new(unsigned int, TArray<T>& Array)
{
	int Index = Array.FArray::Add(1, sizeof(T));
	return &Array(Index);
}


/* FLazyLoader is never exported (inline in UE2); layout per UT2003: vptr,
   SavedAr, SavedPos. INFERRED: not yet checked against Shark Tale code. */
class FLazyLoader
{
public:
	FArchive* SavedAr;
	INT       SavedPos;
	virtual void Load() = 0;
	virtual void Unload() = 0;
};

/* TLazyArray<T>: TArray<T> + FLazyLoader (24 bytes). */
template<class T> class TLazyArray : public TArray<T>, public FLazyLoader
{
public:
	virtual void Load();
	virtual void Unload();
};

template<class T> class TTransArray;
template<class K, class V> class TMap;
template<class K, class V> class TMultiMap;

#pragma pack(pop)
