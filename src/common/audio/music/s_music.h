
#ifndef __S_MUSIC__
#define __S_MUSIC__

#include "zstring.h"
#include "tarray.h"
#include "name.h"
#include "files.h"
#include <zmusic.h>

class SoundStream;


enum MusicCustomStreamType : bool {
	MusicSamples16bit,
	MusicSamplesFloat
};
int MusicEnabled();
typedef bool(*StreamCallback)(SoundStream* stream, void* buff, int len, void* userdata);
SoundStream *S_CreateCustomStream(size_t size, int samplerate, int numchannels, MusicCustomStreamType sampletype, StreamCallback cb, void *userdata);
void S_StopCustomStream(SoundStream* stream);
void S_PauseAllCustomStreams(bool on);

struct MusicCallbacks
{
	FString(*LookupFileName)(const char* fn, int &order);
	int(*FindMusic)(const char* fn);
};
void S_SetMusicCallbacks(MusicCallbacks* cb);

void S_CreateStream();
void S_PauseStream(bool pause);
void S_StopStream();
void S_SetStreamVolume(float vol);


//
void S_InitMusic ();
void S_ResetMusic ();


// Start music using <music_name>
bool S_StartMusic (const char *music_name);

// Start music using <music_name>, and set whether looping
bool S_ChangeMusic (const char *music_name, int order=0, bool looping=true, bool force=false);

// [NKS] Music slots. Slot 0 is the legacy main music; slots 1+ are additional streams.
bool S_PlayMusicSlot(int slotId, const char* music_name, int order=0, bool looping=false);
// Crossfade the current song in a slot to a new decoded PCM song. A time <= 0 performs an immediate change.
bool S_PlayMusicSlotCrossfade(int slotId, const char* music_name, bool looping=true, float crossfadeTime=0.5f);
// Convenience wrapper for slot 0 / legacy main music.
bool S_ChangeMusicCrossfade(const char* music_name, bool looping=true, float crossfadeTime=0.5f);
// Intro -> Loop -> Outro sequence playback for decoded PCM music. Empty intro/outro are allowed.
bool S_PlayMusicSequence(int slotId, const char* intro_name, const char* loop_name, const char* outro_name);
bool S_EndMusicSequence(int slotId);
bool S_EndMusicSequenceImmediate(int slotId, float crossfadeTime=0.5f);
bool S_IsMusicSequencePlaying(int slotId);
// Sample-synchronized PCM stem groups. Build the group first, then start all stems together.
bool S_BeginMusicStemGroup(int slotId, bool looping=true);
bool S_AddMusicStem(int slotId, int stemId, const char* music_name, float initialVolume=1.f);
bool S_StartMusicStemGroup(int slotId);
bool S_SetMusicStemVolume(int slotId, int stemId, float volume);
bool S_SetMusicStemVolumeFade(int slotId, int stemId, float targetVolume, float fadeTime);
bool S_IsMusicStemGroupPlaying(int slotId);
void S_StopMusicSlot(int slotId);
void S_StopAllMusicSlots();
// Internal engine helper: stop only slots 1+, preserving the legacy main song/restart state.
void S_StopAdditionalMusicSlots();
void S_SetMusicSlotVolume(int slotId, float volume);
bool S_SetMusicSlotVolumeFade(int slotId, float targetVolume, float fadeTime);
bool S_IsMusicSlotPlaying(int slotId);
void S_PauseMusicSlot(int slotId, bool paused);
void S_MusicSlotsVolumeChanged();

// Check if <music_name> exists
bool MusicExists(const char* music_name);

void S_RestartMusic ();
void S_MIDIDeviceChanged(int newdev);

int S_GetMusic (const char **name);

// Stops the music for sure.
void S_StopMusic (bool force);

// Stop and resume music, during game PAUSE.
void S_PauseMusic ();
void S_ResumeMusic ();

//
// Updates music & sounds
//
void S_UpdateMusic ();

struct MidiDeviceSetting
{
	int device;
	FString args;
};

typedef TMap<int, MidiDeviceSetting> MidiDeviceMap;
typedef TMap<int, float> MusicVolumeMap;

extern TMap<int, int> ModPlayers;
extern MidiDeviceMap MidiDevices;
extern MusicVolumeMap MusicVolumes;
extern MusicCallbacks mus_cb;

struct MusPlayingInfo
{
	FString name;
	ZMusic_MusicStream handle;
	int   lumpnum;
	int   baseorder;
	float musicVolume;
	bool  loop;
	bool isfloat;
	FString	 LastSong;			// last music that was played
	FString hash;				// for setting replay gain while playing.
};

extern MusPlayingInfo mus_playing;

extern float relative_volume, saved_relative_volume;


#endif
