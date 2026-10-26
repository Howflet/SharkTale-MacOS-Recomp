/*=============================================================================
	StubAudio.cpp: Null audio subsystem (UStubAudioSubsystem), rebuilt from
	Shark Tale's StubAudio.dll (docs/decomp/StubAudio_all.c).

	Layout (84 bytes): UAudioSubsystem (60) + Viewport + FStubAudioStats.
	Behaviour matches the original, minus guard/unguard.
=============================================================================*/

#include "SharkCore.h"
#include "EngineClasses.h"

IMPLEMENT_PACKAGE(StubAudio);

/* FStats internals used here (from the original's code; FStats' full layout
   is not recovered yet). */
static inline INT& StatsAudioRegistered() { return *(INT*)((BYTE*)&GStats + 0x1C); }
static inline INT*& StatsDWORDData()      { return *(INT**)((BYTE*)&GStats + 0x24); }
static inline BYTE*& StatsEntryData()     { return *(BYTE**)((BYTE*)&GStats + 0xF0); }
static inline INT& StatsEntryCount()      { return *(INT*)((BYTE*)&GStats + 0xF4); }

/* USound::Handle (+0x6C): non-null once a sound is registered. */
static inline void*& SoundHandle(USound* Sound) { return *(void**)((BYTE*)Sound + 0x6C); }

enum { STATSTYPE_Audio = 5, STATSDATATYPE_DWORD = 0, STATSUNIT_Default = 0 };

/*-----------------------------------------------------------------------------
	FStubAudioStats.
-----------------------------------------------------------------------------*/

struct FStubAudioStats
{
	INT STATS_FirstEntry;
	INT STATS_PlaySound;
	INT STATS_PlayMusic;
	INT STATS_Update;
	INT STATS_LastEntry;

	FStubAudioStats()
	{
		appMemset(this, 0xFF, 4 * sizeof(INT));
	}

	void Init()
	{
		if (!StatsAudioRegistered())
		{
			STATS_PlaySound = GStats.RegisterStats((EStatsType)STATSTYPE_Audio, (EStatsDataType)STATSDATATYPE_DWORD,
				FString(TEXT("Audio")), FString(TEXT("PlaySound")), (EStatsUnit)STATSUNIT_Default);
			STATS_PlayMusic = GStats.RegisterStats((EStatsType)STATSTYPE_Audio, (EStatsDataType)STATSDATATYPE_DWORD,
				FString(TEXT("Audio")), FString(TEXT("PlayMusic")), (EStatsUnit)STATSUNIT_Default);
			STATS_Update = GStats.RegisterStats((EStatsType)STATSTYPE_Audio, (EStatsDataType)STATSDATATYPE_DWORD,
				FString(TEXT("Audio")), FString(TEXT("Update")), (EStatsUnit)STATSUNIT_Default);
			StatsAudioRegistered() = 1;
		}
		else
		{
			// Already registered: reuse the indices from GStats' registry
			// (12-byte entries, index first). The original copies every entry;
			// we stop at the end of this struct.
			INT* Out = &STATS_PlaySound;
			for (INT i = 0; i < StatsEntryCount() && i < 4; i++)
				Out[i] = *(INT*)(StatsEntryData() + i * 12);
		}
	}
};

static inline void BumpStat(INT Index) { StatsDWORDData()[Index]++; }

/*-----------------------------------------------------------------------------
	UStubAudioSubsystem.
-----------------------------------------------------------------------------*/

class DLL_EXPORT UStubAudioSubsystem : public UAudioSubsystem
{
	DECLARE_CLASS_BOILERPLATE(UStubAudioSubsystem)

	UViewport*      Viewport;
	FStubAudioStats AudioStats;

	UStubAudioSubsystem() { SHARK_MARK("rebuilt UStubAudioSubsystem constructed"); }
	void StaticConstructor() {}

	// UObject.
	void Destroy();
	void PostEditChange() {}
	void ShutdownAfterError();
	void Serialize(FArchive& Ar) { UObject::Serialize(Ar); }

	// FExec.
	int Exec(const TCHAR* Cmd, FOutputDevice& Ar);

	// UAudioSubsystem.
	int Init();
	void SetViewport(UViewport* InViewport) { if (Viewport != InViewport) Viewport = InViewport; }
	UViewport* GetViewport() { return Viewport; }
	void Update(FSceneNode* SceneNode) { BumpStat(AudioStats.STATS_Update); }
	void RegisterSound(USound* Sound) {}
	void UnregisterSound(USound* Sound);
	int PlaySoundW(AActor* Actor, int Id, USound* Sound, FVector Location, float Volume, float Radius,
	               float Pitch, int Flags, float FadeInTime, float FadeOutTime);
	int StopSound(AActor* Actor, USound* Sound) { return 0; }
	int ModifySound(AActor* Actor, int Id, USound* Sound, BYTE Volume, float Pitch) { return 0; }
	int PlayMusic(FString Song, float FadeInTime, int Flags) { BumpStat(AudioStats.STATS_PlayMusic); return 0; }
	int StopMusic(int SongHandle, float FadeOutTime) { return 0; }
	int StopAllMusic(float FadeOutTime) { return 0; }
	FString FindCurrentMusic(FString Song) { return FString(TEXT("")); }
	void ChangeVoiceChatter(unsigned long IpAddr, unsigned long ControllerPort, int Add) {}
	void EnterVoiceChat() {}
	void LeaveVoiceChat() {}
	int LowQualitySound() { return 0; }
	void NoteDestroy(AActor* Actor);
};

IMPLEMENT_CLASS_EX(UStubAudioSubsystem, UAudioSubsystem, UObject, CLASS_Config);

int UStubAudioSubsystem::Init()
{
	SHARK_MARK("rebuilt UStubAudioSubsystem::Init");
	if (!USound::Audio)
	{
		AudioStats.Init();
		USound::Audio = this;
		GLog->Logf(NAME_Init, TEXT("StubAudio: subsystem initialized."));
	}
	return 1;
}

void UStubAudioSubsystem::Destroy()
{
	if (USound::Audio)
	{
		USound::Audio = NULL;
		if (Viewport)
		{
			UClass* SoundClass = USound::StaticClass();
			for (INT i = 0; i < UObject::GObjObjects.ArrayNum; i++)
			{
				UObject* Obj = UObject::GObjObjects(i);
				if (Obj && Obj->IsA(SoundClass))
					UnregisterSound((USound*)Obj);
			}
		}
		SetViewport(NULL);
		GLog->Logf(NAME_Exit, TEXT("Stubbed Audio subsystem shut down."));
	}
	UObject::Destroy();
}

void UStubAudioSubsystem::ShutdownAfterError()
{
	if (USound::Audio)
	{
		USound::Audio = NULL;
		GLog->Logf(NAME_Exit, TEXT("Stubbed Audio subsystem shut down."));
	}
	UObject::ShutdownAfterError();
}

void UStubAudioSubsystem::UnregisterSound(USound* Sound)
{
	if (SoundHandle(Sound))
	{
		GLog->Logf(NAME_DevSound, TEXT("Unregister sound: %s"), Sound->GetPathName(NULL, NULL));
		SoundHandle(Sound) = NULL;
	}
}

int UStubAudioSubsystem::PlaySoundW(AActor* Actor, int Id, USound* Sound, FVector Location, float Volume,
                                     float Radius, float Pitch, int Flags, float FadeInTime, float FadeOutTime)
{
	check(Radius);
	check(this);
	BumpStat(AudioStats.STATS_PlaySound);
	return 0;
}

void UStubAudioSubsystem::NoteDestroy(AActor* Actor)
{
	check(Actor);
	check(Actor->IsValid());
}

int UStubAudioSubsystem::Exec(const TCHAR* Cmd, FOutputDevice& Ar)
{
	if (ParseCommand(&Cmd, TEXT("WEAPONRADIUS")))
	{
		if (appStrcmp(Cmd, TEXT("")) != 0)
			GAudioDefaultRadius = appAtof(Cmd);
		return 1;
	}
	return 0;
}
