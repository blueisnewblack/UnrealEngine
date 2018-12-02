// Copyright 1998-2018 Epic Games, Inc. All Rights Reserved.

#include "MediaSoundComponent.h"
#include "MediaAssetsPrivate.h"

#include "Components/BillboardComponent.h"
#include "Engine/Texture2D.h"
#include "IMediaAudioSample.h"
#include "IMediaPlayer.h"
#include "MediaAudioResampler.h"
#include "Misc/ScopeLock.h"
#include "Sound/AudioSettings.h"
#include "UObject/UObjectGlobals.h"

#include "MediaPlayer.h"
#include "MediaPlayerFacade.h"

#if PLATFORM_HTML5
	#include "AudioDevice.h"
	#include "Engine/Engine.h"
#endif


static int32 SyncAudioAfterDropoutsCVar = 1;
FAutoConsoleVariableRef CVarSyncAudioAfterDropouts(
	TEXT("m.SyncAudioAfterDropouts"),
	SyncAudioAfterDropoutsCVar,
	TEXT("Skip over delayed contiguous audio samples.\n")
	TEXT("0: Not Enabled, 1: Enabled"),
	ECVF_Default);

DECLARE_FLOAT_COUNTER_STAT(TEXT("MediaUtils MediaSoundComponent Sync"), STAT_MediaUtils_MediaSoundComponent, STATGROUP_Media);

/* Static initialization
 *****************************************************************************/

USoundClass* UMediaSoundComponent::DefaultMediaSoundClassObject = nullptr;


/* UMediaSoundComponent structors
 *****************************************************************************/

UMediaSoundComponent::UMediaSoundComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
	, Channels(EMediaSoundChannels::Stereo)
	, DynamicRateAdjustment(false)
	, RateAdjustmentFactor(0.00000001f)
	, RateAdjustmentRange(FFloatRange(0.995f, 1.005f))
	, CachedRate(0.0f)
	, CachedTime(FTimespan::Zero())
	, RateAdjustment(1.0f)
	, Resampler(new FMediaAudioResampler)
	, FrameSyncOffset(0)
	, bSyncAudioAfterDropouts(false)
{
	PrimaryComponentTick.bCanEverTick = true;
	bAutoActivate = true;

#if PLATFORM_MAC
	PreferredBufferLength = 4 * 1024; // increase buffer callback size on macOS to prevent underruns
#endif

#if WITH_EDITORONLY_DATA
	bVisualizeComponent = true;
#endif

#if PLATFORM_PS4 || PLATFORM_SWITCH || PLATFORM_XBOXONE
	bSyncAudioAfterDropouts = true;
#else
	bSyncAudioAfterDropouts = false;
#endif
}


UMediaSoundComponent::~UMediaSoundComponent()
{
	delete Resampler;
}


/* UMediaSoundComponent interface
 *****************************************************************************/

bool UMediaSoundComponent::BP_GetAttenuationSettingsToApply(FSoundAttenuationSettings& OutAttenuationSettings)
{
	const FSoundAttenuationSettings* SelectedAttenuationSettings = GetSelectedAttenuationSettings();

	if (SelectedAttenuationSettings == nullptr)
	{
		return false;
	}

	OutAttenuationSettings = *SelectedAttenuationSettings;

	return true;
}


UMediaPlayer* UMediaSoundComponent::GetMediaPlayer() const
{
	return CurrentPlayer.Get();
}


void UMediaSoundComponent::SetMediaPlayer(UMediaPlayer* NewMediaPlayer)
{
	CurrentPlayer = NewMediaPlayer;
}

#if WITH_EDITOR

void UMediaSoundComponent::SetDefaultMediaPlayer(UMediaPlayer* NewMediaPlayer)
{
	MediaPlayer = NewMediaPlayer;
	CurrentPlayer = MediaPlayer;
}

#endif


void UMediaSoundComponent::UpdatePlayer()
{
	if (!CurrentPlayer.IsValid())
	{
		CachedRate = 0.0f;
		CachedTime = FTimespan::Zero();

		FScopeLock Lock(&CriticalSection);
		SampleQueue.Reset();
		FrameSyncOffset = 0;

		return;
	}

	// create a new sample queue if the player changed
	TSharedRef<FMediaPlayerFacade, ESPMode::ThreadSafe> PlayerFacade = CurrentPlayer->GetPlayerFacade();

	if (PlayerFacade != CurrentPlayerFacade)
	{
		const auto NewSampleQueue = MakeShared<FMediaAudioSampleQueue, ESPMode::ThreadSafe>();
		PlayerFacade->AddAudioSampleSink(NewSampleQueue);
		{
			FScopeLock Lock(&CriticalSection);
			SampleQueue = NewSampleQueue;
			FrameSyncOffset = 0;
		}

		CurrentPlayerFacade = PlayerFacade;
	}

	// caching play rate and time for audio thread (eventual consistency is sufficient)
	CachedRate = PlayerFacade->GetRate();
	CachedTime = PlayerFacade->GetTime();
}


/* TAttenuatedComponentVisualizer interface
 *****************************************************************************/

void UMediaSoundComponent::CollectAttenuationShapesForVisualization(TMultiMap<EAttenuationShape::Type, FBaseAttenuationSettings::AttenuationShapeDetails>& ShapeDetailsMap) const
{
	const FSoundAttenuationSettings* SelectedAttenuationSettings = GetSelectedAttenuationSettings();

	if (SelectedAttenuationSettings != nullptr)
	{
		SelectedAttenuationSettings->CollectAttenuationShapesForVisualization(ShapeDetailsMap);
	}
}


/* UActorComponent interface
 *****************************************************************************/

void UMediaSoundComponent::OnRegister()
{
	Super::OnRegister();

#if WITH_EDITORONLY_DATA
	if (SpriteComponent != nullptr)
	{
		SpriteComponent->SpriteInfo.Category = TEXT("Sounds");
		SpriteComponent->SpriteInfo.DisplayName = NSLOCTEXT("SpriteCategory", "Sounds", "Sounds");

		if (bAutoActivate)
		{
			SpriteComponent->SetSprite(LoadObject<UTexture2D>(nullptr, TEXT("/Engine/EditorResources/AudioIcons/S_AudioComponent_AutoActivate.S_AudioComponent_AutoActivate")));
		}
		else
		{
			SpriteComponent->SetSprite(LoadObject<UTexture2D>(nullptr, TEXT("/Engine/EditorResources/AudioIcons/S_AudioComponent.S_AudioComponent")));
		}
	}
#endif
}


void UMediaSoundComponent::TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction *ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	UpdatePlayer();
}


/* USceneComponent interface
 *****************************************************************************/

void UMediaSoundComponent::Activate(bool bReset)
{
	if (bReset || ShouldActivate())
	{
		SetComponentTickEnabled(true);
	}

	Super::Activate(bReset);
}


void UMediaSoundComponent::Deactivate()
{
	if (!ShouldActivate())
	{
		SetComponentTickEnabled(false);
	}

	Super::Deactivate();
}


/* UObject interface
 *****************************************************************************/

void UMediaSoundComponent::PostInitProperties()
{
	Super::PostInitProperties();

	if (UMediaSoundComponent::DefaultMediaSoundClassObject == nullptr)
	{
		const FSoftObjectPath DefaultMediaSoundClassName = GetDefault<UAudioSettings>()->DefaultMediaSoundClassName;

		if (DefaultMediaSoundClassName.IsValid())
		{
			UMediaSoundComponent::DefaultMediaSoundClassObject = LoadObject<USoundClass>(nullptr, *DefaultMediaSoundClassName.ToString());
		}
	}

	// We have a different default sound class object for media sound components
	if (SoundClass == USoundBase::DefaultSoundClassObject || SoundClass == nullptr)
	{
		SoundClass = UMediaSoundComponent::DefaultMediaSoundClassObject;
	}
}


void UMediaSoundComponent::PostLoad()
{
	Super::PostLoad();

	CurrentPlayer = MediaPlayer;
}


#if WITH_EDITOR

void UMediaSoundComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	static const FName MediaPlayerName = GET_MEMBER_NAME_CHECKED(UMediaSoundComponent, MediaPlayer);

	UProperty* PropertyThatChanged = PropertyChangedEvent.Property;

	if (PropertyThatChanged != nullptr)
	{
		const FName PropertyName = PropertyThatChanged->GetFName();

		if (PropertyName == MediaPlayerName)
		{
			CurrentPlayer = MediaPlayer;
		}
	}

	Super::PostEditChangeProperty(PropertyChangedEvent);
}

#endif //WITH_EDITOR


/* USynthComponent interface
 *****************************************************************************/

bool UMediaSoundComponent::Init(int32& SampleRate)
{
	Super::Init(SampleRate);

	if (Channels == EMediaSoundChannels::Mono)
	{
		NumChannels = 1;
	}
	else //if (Channels == EMediaSoundChannels::Stereo)
	{
		NumChannels = 2;
	}/*
	else
	{
		NumChannels = 8;
	}*/

	// increase buffer callback size for media decoding. Media doesn't need fast response time so can decode more per callback.
	//PreferredBufferLength = NumChannels * 8196;

	Resampler->Initialize(NumChannels, SampleRate);

	return true;
}


int32 UMediaSoundComponent::OnGenerateAudio(float* OutAudio, int32 NumSamples)
{
	int32 InitialSyncOffset = 0;
	TSharedPtr<FMediaAudioSampleQueue, ESPMode::ThreadSafe> PinnedSampleQueue;
	{
		FScopeLock Lock(&CriticalSection);
		PinnedSampleQueue = SampleQueue;
		InitialSyncOffset = FrameSyncOffset;
	}

	if (PinnedSampleQueue.IsValid() && (CachedRate != 0.0f))
	{
		const float Rate = CachedRate.Load();
		const FTimespan Time = CachedTime.Load();

		FTimespan OutTime = FTimespan::Zero();

		if (bSyncAudioAfterDropouts && SyncAudioAfterDropoutsCVar)
		{
			int32 SyncOffset = InitialSyncOffset + (NumSamples / NumChannels);
			while (SyncOffset > 0)
			{
				float* DestAudio = OutAudio;
				int32 FramesRequested = NumSamples / NumChannels;

				if (SyncOffset < FramesRequested)
				{
					// Handle final generate before audio resumes playback
					// Move frames left to sync them with expected playback time
					int32 FloatsMoved = (FramesRequested - SyncOffset) * NumChannels;
					FMemory::Memmove(OutAudio, OutAudio + (SyncOffset * NumChannels), FloatsMoved * sizeof(float));
					DestAudio = OutAudio + FloatsMoved;
					FramesRequested = SyncOffset;
				}

				uint32 JumpFrame = MAX_uint32;
				int32 FramesWritten = (int32)Resampler->Generate(DestAudio, OutTime, (uint32)FramesRequested, Rate, Time, *PinnedSampleQueue, JumpFrame);

				if (JumpFrame != MAX_uint32)
				{
					UE_LOG(LogMediaAssets, Verbose, TEXT("Audio ( JUMP ) SyncOffset was: %d"), SyncOffset);
					int32 JumpFramesRequested = FramesRequested - JumpFrame;
					int32 JumpFramesWritten = FramesWritten - JumpFrame;
					SyncOffset = JumpFramesRequested - JumpFramesWritten;
				}
				else
				{
					SyncOffset -= FramesWritten;
				}

				if (FramesWritten < FramesRequested)
				{
					if (FramesWritten > 0)
					{
						UE_LOG(LogMediaAssets, Verbose, TEXT("Audio partial generate, FramesWritten: %d"), FramesWritten);
					}
					// Source buffer is empty
					break;
				}
			}

			if (SyncOffset > 0)
			{
				UE_LOG(LogMediaAssets, Verbose, TEXT("Audio ( STARVED ) SyncOffset: %d, PlayerTime: %s, OutTime: %s"), SyncOffset, *Time.ToString(), *OutTime.ToString());
				FMemory::Memzero(OutAudio, NumSamples * sizeof(float));
			}
			else if (SyncOffset < 0)
			{
				UE_LOG(LogMediaAssets, Verbose, TEXT("Audio ( DESYNCED ) SyncOffset: %d"), SyncOffset);
			}

			{
				FScopeLock Lock(&CriticalSection);
				// Commit only if another thread did not change value
				if (InitialSyncOffset == FrameSyncOffset)
				{
					FrameSyncOffset = SyncOffset;
				}
			}
		}
		else
		{
			const int32 FramesRequested = NumSamples / NumChannels;
			uint32 JumpFrame = MAX_uint32;
			uint32 FramesWritten = Resampler->Generate(OutAudio, OutTime, (uint32)FramesRequested, Rate, Time, *PinnedSampleQueue, JumpFrame);
			if (FramesWritten == 0)
			{
				return 0; // no samples available
			}
		}

#if STATS
		SET_FLOAT_STAT(STAT_MediaUtils_MediaSoundComponent, (Time - OutTime).GetTotalMilliseconds());
#endif
	}
	else
	{
		Resampler->Flush();

		if (bSyncAudioAfterDropouts && SyncAudioAfterDropoutsCVar)
		{
			FScopeLock Lock(&CriticalSection);
			FrameSyncOffset = 0;
		}
	}
	return NumSamples;
}


/* UMediaSoundComponent implementation
 *****************************************************************************/

const FSoundAttenuationSettings* UMediaSoundComponent::GetSelectedAttenuationSettings() const
{
	if (bOverrideAttenuation)
	{
		return &AttenuationOverrides;
	}
	
	if (AttenuationSettings != nullptr)
	{
		return &AttenuationSettings->Attenuation;
	}

	return nullptr;
}