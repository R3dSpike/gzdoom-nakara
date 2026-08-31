/*
**
** music.cpp
**
** music engine
**
** Copyright 1999-2016 Randy Heit
** Copyright 2002-2016 Christoph Oelckers
**
**---------------------------------------------------------------------------
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions
** are met:
**
** 1. Redistributions of source code must retain the above copyright
**    notice, this list of conditions and the following disclaimer.
** 2. Redistributions in binary form must reproduce the above copyright
**    notice, this list of conditions and the following disclaimer in the
**    documentation and/or other materials provided with the distribution.
** 3. The name of the author may not be used to endorse or promote products
**    derived from this software without specific prior written permission.
**
** THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
** IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
** OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
** IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
** INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
** NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
** DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
** THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
** (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
** THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**---------------------------------------------------------------------------
**
*/

#include <stdio.h>
#include <stdlib.h>
#include <stdexcept>
#include <memory>
#include <map>
#include <vector>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <chrono>

#include "i_sound.h"
#include "i_music.h"
#include "printf.h"
#include "s_playlist.h"
#include "c_dispatch.h"
#include "filesystem.h"
#include "cmdlib.h"
#include "s_music.h"
#include "filereadermusicinterface.h"
#include <zmusic.h>
#include "md5.h"
#include "gain_analysis.h"
#include "i_specialpaths.h"
#include "configfile.h"
#include "c_cvars.h"
#include "md5.h"


// EXTERNAL FUNCTION PROTOTYPES --------------------------------------------

extern int nomusic;
extern float S_GetMusicVolume (const char *music);

static void S_ActivatePlayList(bool goBack);
static void StopLegacyMainMusicOnly(bool rememberLastSong);

// PRIVATE DATA DEFINITIONS ------------------------------------------------

static bool		MusicPaused;		// whether music is paused
MusPlayingInfo mus_playing;	// music currently being played
static FPlayList PlayList;
float	relative_volume = 1.f;
float	saved_relative_volume = 1.0f;	// this could be used to implement an ACS FadeMusic function
MusicVolumeMap MusicVolumes;
MidiDeviceMap MidiDevices;
TMap<int, int> ModPlayers;

static int DefaultFindMusic(const char* fn)
{
	return -1;
}

MusicCallbacks mus_cb = { nullptr, DefaultFindMusic };


// PUBLIC DATA DEFINITIONS -------------------------------------------------
EXTERN_CVAR(Bool, mus_enabled)
EXTERN_CVAR(Float, snd_musicvolume)
EXTERN_CVAR(Int, snd_mididevice)
EXTERN_CVAR(Float, mod_dumb_mastervolume)
EXTERN_CVAR(Float, fluid_gain)


CVAR(Bool, mus_calcgain, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG) // changing this will only take effect for the next song.
CVAR(Bool, mus_usereplaygain, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG) // changing this will only take effect for the next song.
CVAR(Int, mod_preferred_player, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)// toggle between libXMP and Dumb. Unlike other sound CVARs this is not directly mapped to ZMusic's config.

// CODE --------------------------------------------------------------------

//==========================================================================
//
// OpenMusic
//
// opens a FileReader for the music - used as a callback to keep
// implementation details out of the core player.
//
//==========================================================================

static FileReader OpenMusic(const char* musicname)
{
	FileReader reader;
	if (!FileExists(musicname))
	{
		int lumpnum;
		lumpnum = mus_cb.FindMusic(musicname);
		if (lumpnum == -1) lumpnum = fileSystem.CheckNumForName(musicname, FileSys::ns_music);
		if (lumpnum == -1)
		{
			Printf("Music \"%s\" not found\n", musicname);
		}
		else if (fileSystem.FileLength(lumpnum) != 0)
		{
			reader = fileSystem.ReopenFileReader(lumpnum);
		}
	}
	else
	{
		// Load an external file.
		reader.OpenFile(musicname);
	}
	return reader;
}

bool MusicExists(const char* music_name)
{
	if (music_name == nullptr)
		return false;

	if (FileExists(music_name))
		return true;
	else
	{
		int lumpnum;
		lumpnum = mus_cb.FindMusic(music_name);
		if (lumpnum == -1) lumpnum = fileSystem.CheckNumForName(music_name, FileSys::ns_music);
		if (lumpnum != -1 && fileSystem.FileLength(lumpnum) != 0)
			return true;
	}
	return false;
}

void S_SetMusicCallbacks(MusicCallbacks* cb)
{
	mus_cb = *cb;
	if (mus_cb.FindMusic == nullptr) mus_cb.FindMusic = DefaultFindMusic;	// without this we are dead in the water.
}

int MusicEnabled() // int return is for scripting
{
	return mus_enabled && !nomusic;
} 

//==========================================================================
//
// 
//
// Create a sound system stream for the currently playing song 
//==========================================================================

static std::unique_ptr<SoundStream> musicStream;
static TArray<SoundStream*> customStreams;

// Slot 0 reuses the legacy main music playback objects above. Keep only the
// extra per-slot controls here instead of creating a second stream for slot 0.
static float mainMusicSlotVolume = 1.f;
static bool mainMusicSlotPaused = false;

static void ApplyMainMusicSlotGain(float gain)
{
	if (mus_playing.handle == nullptr) return;
	if (gain < 0.f) gain = 0.f;
	if (gain > 1.f) gain = 1.f;
	float volume = mainMusicSlotVolume * gain;

	if (musicStream)
	{
		musicStream->SetVolume(volume);
	}
	else
	{
		// Non-streaming backends (for example an external MIDI device) do not
		// have a SoundStream, so apply the slot multiplier directly to ZMusic.
		ChangeMusicSetting(zmusic_relative_volume, mus_playing.handle, relative_volume * volume);
		ZMusic_VolumeChanged(mus_playing.handle);
	}
}

static void ApplyMainMusicSlotVolume()
{
	ApplyMainMusicSlotGain(1.f);
}

static void ApplyMainMusicSlotPause(bool globalPaused)
{
	if (mus_playing.handle == nullptr) return;

	bool pause = globalPaused || mainMusicSlotPaused;
	if (pause) ZMusic_Pause(mus_playing.handle);
	else ZMusic_Resume(mus_playing.handle);
	if (musicStream) musicStream->SetPaused(pause);
}

// [NKS] Music slot system. Slot 0 is the engine's existing main music
// (mus_playing/musicStream). Slots 1+ are independent additional streams.
struct MusicSlotPlayback
{
	int id;
	FString name;
	ZMusic_MusicStream handle;
	std::unique_ptr<SoundStream> stream;
	TArray<int16_t> convert;
	int order;
	float volume;
	float trackVolume;
	bool looping;
	bool isfloat;
	bool paused;

	MusicSlotPlayback()
		: id(-1), handle(nullptr), order(0), volume(1.f), trackVolume(1.f),
		  looping(false), isfloat(false), paused(false)
	{
	}
};

static std::map<int, std::unique_ptr<MusicSlotPlayback>> musicSlots;

// [NKS] Intro -> Loop -> Outro sequence playback. Sequences use ZMusic's
// SoundDecoder interface so a short final read can be followed immediately by
// the next segment in the same output callback. This path is intentionally for
// decoded PCM formats (OGG/FLAC/WAV/MP3-style sources), not MIDI/module music.
enum EMusicSequenceStage
{
	MSEQ_None = 0,
	MSEQ_Intro,
	MSEQ_Loop,
	MSEQ_Outro,
	MSEQ_Finished
};

struct MusicSequenceSegment
{
	FString name;
	std::vector<uint8_t> data;
	SoundDecoder* decoder[2];
	int decoderCount;
	int sampleRate;
	ChannelConfig channels;
	SampleType sampleType;
	float trackVolume;

	MusicSequenceSegment()
		: decoder{ nullptr, nullptr }, decoderCount(0), sampleRate(0),
		  channels(ChannelConfig_Stereo), sampleType(SampleType_Float32), trackVolume(1.f)
	{
	}
};

struct MusicSequencePlayback
{
	int id;
	MusicSequenceSegment intro;
	MusicSequenceSegment loop;
	MusicSequenceSegment outro;
	std::unique_ptr<SoundStream> stream;
	std::vector<uint8_t> convert;
	std::vector<float> crossfadeMix;
	std::atomic<int> stage;
	std::atomic<bool> endRequested;
	std::atomic<bool> immediateEndRequested;
	std::atomic<bool> immediateTransitionStarted;
	std::atomic<int64_t> immediateFadeFrames;
	std::atomic<bool> finished;
	std::atomic<int> activeLoopDecoder;
	std::atomic<bool> loopResetNeeded[2];
	std::atomic<bool> loopReady[2];
	float volume;
	bool paused;
	int sampleRate;
	ChannelConfig channels;

	// The fields below are audio-callback state. They are only changed by
	// FillMusicSequenceStream after an immediate-end request has been observed.
	bool immediateCrossfadeActive;
	int immediateSourceStage;
	int immediateSourceLoopDecoder;
	int64_t immediateActiveFadeFrames;
	int64_t immediateFadeFramePos;
	bool immediateSourceExhausted;
	bool immediateOutroExhausted;

	MusicSequencePlayback()
		: id(-1), stage(MSEQ_None), endRequested(false), immediateEndRequested(false),
		  immediateTransitionStarted(false), immediateFadeFrames(0), finished(false),
		  activeLoopDecoder(0), volume(1.f), paused(false), sampleRate(0),
		  channels(ChannelConfig_Stereo), immediateCrossfadeActive(false),
		  immediateSourceStage(MSEQ_None), immediateSourceLoopDecoder(0),
		  immediateActiveFadeFrames(0), immediateFadeFramePos(0),
		  immediateSourceExhausted(false), immediateOutroExhausted(false)
	{
		loopResetNeeded[0] = false;
		loopResetNeeded[1] = false;
		loopReady[0] = false;
		loopReady[1] = false;
	}
};

static std::map<int, std::unique_ptr<MusicSequencePlayback>> musicSequences;

// [NKS] Temporary A -> B crossfade state for ordinary music changes. The
// incoming song is stored in musicSequences while a transition is active; the
// outgoing playback is retained here until the fade completes. This lets slot
// 0 keep sharing the legacy main-music slot without consuming a permanent
// extra user-visible slot.
enum EMusicSlotCrossfadeOutgoing
{
	MCF_None = 0,
	MCF_LegacyMain,
	MCF_NormalSlot,
	MCF_Sequence
};

struct MusicSlotCrossfadePlayback
{
	int slotId;
	int outgoingKind;
	std::unique_ptr<MusicSlotPlayback> outgoingSlot;
	std::unique_ptr<MusicSequencePlayback> outgoingSequence;
	MusicSequencePlayback* incomingIdentity;
	float targetVolume;
	double durationSeconds;
	std::chrono::steady_clock::time_point startTime;

	MusicSlotCrossfadePlayback()
		: slotId(-1), outgoingKind(MCF_None), incomingIdentity(nullptr),
		  targetVolume(1.f), durationSeconds(0.0)
	{
	}
};

static std::map<int, std::unique_ptr<MusicSlotCrossfadePlayback>> musicSlotCrossfades;

// [NKS] Per-slot volume fades. These operate on the slot's logical volume,
// so they work for the legacy main music (slot 0), ordinary slots, sequences,
// and ordinary-music crossfades without consuming another playback slot.
struct MusicSlotVolumeFade
{
	int slotId;
	float startVolume;
	float targetVolume;
	double durationSeconds;
	std::chrono::steady_clock::time_point startTime;

	MusicSlotVolumeFade()
		: slotId(-1), startVolume(1.f), targetVolume(1.f), durationSeconds(0.0)
	{
	}
};

static std::map<int, MusicSlotVolumeFade> musicSlotVolumeFades;


// [NKS] Sample-synchronized stem groups. A group owns one user-visible music
// slot and mixes several decoded PCM stems into a single SoundStream. Stem 0
// is the timeline master: every other stem advances by exactly the same number
// of samples, and loop boundaries are taken from stem 0 so timing drift cannot
// accumulate across repeated loops.
struct MusicStemLayer
{
	int id;
	MusicSequenceSegment source;
	std::atomic<float> volume;
	bool fadeActive;
	float fadeStartVolume;
	float fadeTargetVolume;
	double fadeDurationSeconds;
	std::chrono::steady_clock::time_point fadeStartTime;

	MusicStemLayer()
		: id(-1), volume(1.f), fadeActive(false), fadeStartVolume(1.f),
		  fadeTargetVolume(1.f), fadeDurationSeconds(0.0)
	{
	}
};

struct MusicStemGroupPlayback
{
	int id;
	std::map<int, std::unique_ptr<MusicStemLayer>> stems;
	std::unique_ptr<SoundStream> stream;
	std::vector<uint8_t> convert;
	std::vector<float> stemBuffer;
	std::atomic<int> activeDecoder;
	std::atomic<bool> resetNeeded[2];
	std::atomic<bool> ready[2];
	std::atomic<bool> finished;
	std::atomic<bool> lengthMismatch;
	bool mismatchReported;
	bool looping;
	float volume;
	bool paused;
	int sampleRate;
	ChannelConfig channels;

	MusicStemGroupPlayback()
		: id(-1), activeDecoder(0), finished(false), lengthMismatch(false),
		  mismatchReported(false), looping(true), volume(1.f), paused(false),
		  sampleRate(0), channels(ChannelConfig_Stereo)
	{
		resetNeeded[0] = false;
		resetNeeded[1] = false;
		ready[0] = false;
		ready[1] = false;
	}
};

static std::map<int, std::unique_ptr<MusicStemGroupPlayback>> pendingMusicStemGroups;
static std::map<int, std::unique_ptr<MusicStemGroupPlayback>> musicStemGroups;

static int SequenceSampleSize(SampleType type)
{
	switch (type)
	{
	case SampleType_UInt8: return 1;
	case SampleType_Int16: return 2;
	case SampleType_Float32: return 4;
	default: return 0;
	}
}

static void CloseSequenceSegmentDecoders(MusicSequenceSegment& segment)
{
	for (int i = 0; i < 2; i++)
	{
		if (segment.decoder[i] != nullptr)
		{
			SoundDecoder_Close(segment.decoder[i]);
			segment.decoder[i] = nullptr;
		}
	}
	segment.decoderCount = 0;
}

static SoundDecoder* CreateSequenceDecoder(MusicSequenceSegment& segment)
{
	if (segment.data.empty()) return nullptr;
	return CreateDecoder(segment.data.data(), segment.data.size(), true);
}

static bool LoadMusicSequenceSegment(const char* inputName, MusicSequenceSegment& segment, int decoderCount)
{
	if (inputName == nullptr || inputName[0] == 0)
	{
		segment.name = "";
		return true;
	}

	FString lookedUpName;
	const char* musicname = inputName;
	int order = 0;
	if (mus_cb.LookupFileName)
	{
		lookedUpName = mus_cb.LookupFileName(musicname, order);
		musicname = lookedUpName.GetChars();
	}
	if (musicname == nullptr || musicname[0] == 0) return false;
	if (strncmp(musicname, "file://", 7) == 0) musicname += 7;

	FileReader reader = OpenMusic(musicname);
	if (!reader.isOpen()) return false;

	auto length = reader.GetLength();
	if (length <= 0) return false;
	segment.data.resize((size_t)length);
	if (reader.Read(segment.data.data(), (long)segment.data.size()) != (long)segment.data.size())
	{
		segment.data.clear();
		return false;
	}
	segment.name = musicname;

	int lumpnum = mus_cb.FindMusic(musicname);
	float* volp = MusicVolumes.CheckKey(lumpnum);
	if (volp) segment.trackVolume = *volp;

	segment.decoderCount = decoderCount;
	for (int i = 0; i < decoderCount; i++)
	{
		segment.decoder[i] = CreateSequenceDecoder(segment);
		if (segment.decoder[i] == nullptr)
		{
			CloseSequenceSegmentDecoders(segment);
			Printf("Unable to decode music sequence segment %s\n", musicname);
			return false;
		}
	}

	SoundDecoder_GetInfo(segment.decoder[0], &segment.sampleRate, &segment.channels, &segment.sampleType);
	if (segment.sampleRate <= 0 || SequenceSampleSize(segment.sampleType) == 0)
	{
		CloseSequenceSegmentDecoders(segment);
		return false;
	}
	return true;
}

static bool SequenceFormatsMatch(const MusicSequenceSegment& a, const MusicSequenceSegment& b)
{
	if (a.name.IsEmpty() || b.name.IsEmpty()) return true;
	return a.sampleRate == b.sampleRate && a.channels == b.channels;
}

static void ShutdownMusicSequence(MusicSequencePlayback* sequence)
{
	if (sequence == nullptr) return;
	if (sequence->stream)
	{
		sequence->stream->Stop();
		sequence->stream.reset();
	}
	CloseSequenceSegmentDecoders(sequence->intro);
	CloseSequenceSegmentDecoders(sequence->loop);
	CloseSequenceSegmentDecoders(sequence->outro);
	sequence->stage = MSEQ_Finished;
	sequence->finished = true;
}

static void StopMusicSequenceInternal(int slotId)
{
	auto it = musicSequences.find(slotId);
	if (it == musicSequences.end()) return;
	ShutdownMusicSequence(it->second.get());
	musicSequences.erase(it);
}

static void ApplyMusicSequenceGain(MusicSequencePlayback* sequence, float gain)
{
	if (sequence == nullptr || !sequence->stream) return;
	if (gain < 0.f) gain = 0.f;
	if (gain > 1.f) gain = 1.f;
	float volume = sequence->volume * gain;
	if (volume < 0.f) volume = 0.f;
	if (volume > 1.f) volume = 1.f;
	sequence->stream->SetVolume(volume);
}

static void ApplyMusicSequenceVolume(MusicSequencePlayback* sequence)
{
	ApplyMusicSequenceGain(sequence, 1.f);
}

static void ApplyMusicSequencePause(MusicSequencePlayback* sequence, bool globalPaused)
{
	if (sequence == nullptr || !sequence->stream) return;
	sequence->stream->SetPaused(globalPaused || sequence->paused);
}

static size_t ReadMusicSequenceSamples(MusicSequencePlayback* sequence, MusicSequenceSegment& segment,
	SoundDecoder* decoder, float* output, size_t sampleCount)
{
	if (sequence == nullptr || decoder == nullptr || sampleCount == 0) return 0;
	int sampleSize = SequenceSampleSize(segment.sampleType);
	if (sampleSize <= 0) return 0;

	size_t wantedBytes = sampleCount * (size_t)sampleSize;
	sequence->convert.resize(wantedBytes);
	size_t gotBytes = 0;
	while (gotBytes < wantedBytes)
	{
		size_t got = SoundDecoder_Read(decoder, sequence->convert.data() + gotBytes, wantedBytes - gotBytes);
		if (got == 0) break;
		gotBytes += got;
	}
	size_t gotSamples = gotBytes / (size_t)sampleSize;
	float gain = segment.trackVolume;

	if (segment.sampleType == SampleType_Float32)
	{
		const float* src = (const float*)sequence->convert.data();
		for (size_t i = 0; i < gotSamples; i++) output[i] = src[i] * gain;
	}
	else if (segment.sampleType == SampleType_Int16)
	{
		const int16_t* src = (const int16_t*)sequence->convert.data();
		for (size_t i = 0; i < gotSamples; i++) output[i] = src[i] * gain * (1.f / 32768.f);
	}
	else if (segment.sampleType == SampleType_UInt8)
	{
		const uint8_t* src = sequence->convert.data();
		for (size_t i = 0; i < gotSamples; i++) output[i] = ((int)src[i] - 128) * gain * (1.f / 128.f);
	}
	return gotSamples;
}

static bool SequenceHasSegment(const MusicSequenceSegment& segment)
{
	return !segment.name.IsEmpty() && !segment.data.empty();
}

static void FinishMusicSequence(MusicSequencePlayback* sequence)
{
	sequence->stage = MSEQ_Finished;
	sequence->finished = true;
}

static bool GetMusicSequenceStageSource(MusicSequencePlayback* sequence, int stage, int loopDecoder,
	MusicSequenceSegment*& segment, SoundDecoder*& decoder)
{
	segment = nullptr;
	decoder = nullptr;
	if (sequence == nullptr) return false;

	if (stage == MSEQ_Intro)
	{
		segment = &sequence->intro;
		decoder = segment->decoder[0];
	}
	else if (stage == MSEQ_Loop)
	{
		segment = &sequence->loop;
		if (loopDecoder < 0 || loopDecoder > 1) return false;
		decoder = segment->decoder[loopDecoder];
	}
	else if (stage == MSEQ_Outro)
	{
		segment = &sequence->outro;
		decoder = segment->decoder[0];
	}

	return segment != nullptr && decoder != nullptr;
}

static void BeginImmediateMusicSequenceEnd(MusicSequencePlayback* sequence)
{
	if (sequence == nullptr || sequence->finished.load()) return;

	int stage = sequence->stage.load();
	sequence->immediateTransitionStarted = true;

	// If the sequence is already in its outro, it has already reached the
	// requested destination. Do not restart the outro from the beginning.
	if (stage == MSEQ_Outro) return;

	int64_t fadeFrames = sequence->immediateFadeFrames.load();
	if (fadeFrames <= 0)
	{
		// Zero means a hard cut. The current decoder is abandoned at its exact
		// current position and the outro starts at sample zero in this callback.
		if (SequenceHasSegment(sequence->outro)) sequence->stage = MSEQ_Outro;
		else FinishMusicSequence(sequence);
		return;
	}

	if (stage != MSEQ_Intro && stage != MSEQ_Loop)
	{
		if (SequenceHasSegment(sequence->outro)) sequence->stage = MSEQ_Outro;
		else FinishMusicSequence(sequence);
		return;
	}

	sequence->immediateCrossfadeActive = true;
	sequence->immediateSourceStage = stage;
	sequence->immediateSourceLoopDecoder = stage == MSEQ_Loop ? sequence->activeLoopDecoder.load() : 0;
	sequence->immediateActiveFadeFrames = fadeFrames;
	sequence->immediateFadeFramePos = 0;
	sequence->immediateSourceExhausted = false;
	sequence->immediateOutroExhausted = false;
}

static size_t FillImmediateMusicSequenceCrossfade(MusicSequencePlayback* sequence, float* output, size_t sampleCount)
{
	if (sequence == nullptr || output == nullptr || sampleCount == 0 || !sequence->immediateCrossfadeActive) return 0;

	int channelCount = sequence->channels == ChannelConfig_Mono ? 1 : 2;
	int64_t framesLeft = sequence->immediateActiveFadeFrames - sequence->immediateFadeFramePos;
	if (framesLeft <= 0) return 0;

	size_t requestedFrames = sampleCount / (size_t)channelCount;
	size_t fadeFrames = (size_t)framesLeft;
	if (fadeFrames > requestedFrames) fadeFrames = requestedFrames;
	size_t fadeSamples = fadeFrames * (size_t)channelCount;
	if (fadeSamples == 0) return 0;

	MusicSequenceSegment* currentSegment = nullptr;
	SoundDecoder* currentDecoder = nullptr;
	if (!sequence->immediateSourceExhausted)
	{
		if (!GetMusicSequenceStageSource(sequence, sequence->immediateSourceStage,
			sequence->immediateSourceLoopDecoder, currentSegment, currentDecoder))
		{
			sequence->immediateSourceExhausted = true;
		}
	}

	size_t currentGot = 0;
	if (!sequence->immediateSourceExhausted)
	{
		currentGot = ReadMusicSequenceSamples(sequence, *currentSegment, currentDecoder, output, fadeSamples);
		if (currentGot < fadeSamples) sequence->immediateSourceExhausted = true;
	}
	if (currentGot < fadeSamples)
	{
		memset(output + currentGot, 0, (fadeSamples - currentGot) * sizeof(float));
	}

	if (sequence->crossfadeMix.size() < fadeSamples) sequence->crossfadeMix.resize(fadeSamples);
	float* outroBuffer = sequence->crossfadeMix.data();
	size_t outroGot = 0;
	if (SequenceHasSegment(sequence->outro) && !sequence->immediateOutroExhausted)
	{
		outroGot = ReadMusicSequenceSamples(sequence, sequence->outro, sequence->outro.decoder[0],
			outroBuffer, fadeSamples);
		if (outroGot < fadeSamples) sequence->immediateOutroExhausted = true;
	}
	if (outroGot < fadeSamples)
	{
		memset(outroBuffer + outroGot, 0, (fadeSamples - outroGot) * sizeof(float));
	}

	// Equal-power crossfade. Gain is calculated once per audio frame and shared
	// by both channels so stereo imaging is not skewed during the transition.
	const double halfPi = 1.57079632679489661923;
	double denom = sequence->immediateActiveFadeFrames > 1 ?
		(double)(sequence->immediateActiveFadeFrames - 1) : 1.0;
	for (size_t frame = 0; frame < fadeFrames; frame++)
	{
		double progress;
		if (sequence->immediateActiveFadeFrames <= 1) progress = 1.0;
		else progress = (double)(sequence->immediateFadeFramePos + (int64_t)frame) / denom;
		if (progress < 0.0) progress = 0.0;
		if (progress > 1.0) progress = 1.0;
		float currentGain = (float)std::cos(progress * halfPi);
		float outroGain = (float)std::sin(progress * halfPi);
		for (int channel = 0; channel < channelCount; channel++)
		{
			size_t index = frame * (size_t)channelCount + (size_t)channel;
			output[index] = output[index] * currentGain + outroBuffer[index] * outroGain;
		}
	}

	sequence->immediateFadeFramePos += (int64_t)fadeFrames;
	if (sequence->immediateFadeFramePos >= sequence->immediateActiveFadeFrames)
	{
		sequence->immediateCrossfadeActive = false;
		if (SequenceHasSegment(sequence->outro) && !sequence->immediateOutroExhausted)
		{
			// The outro decoder has already advanced by the crossfade duration.
			// Continue from that exact sample on the next loop iteration.
			sequence->stage = MSEQ_Outro;
		}
		else
		{
			FinishMusicSequence(sequence);
		}
	}

	return fadeSamples;
}

static bool FillMusicSequenceStream(SoundStream* stream, void* buff, int len, void* userdata)
{
	auto sequence = static_cast<MusicSequencePlayback*>(userdata);
	float* output = (float*)buff;
	size_t totalSamples = (size_t)len / sizeof(float);
	size_t outputPos = 0;

	if (sequence == nullptr || sequence->finished)
	{
		memset(buff, 0, len);
		return false;
	}

	while (outputPos < totalSamples && !sequence->finished)
	{
		if (sequence->immediateEndRequested.exchange(false))
		{
			BeginImmediateMusicSequenceEnd(sequence);
			if (sequence->finished.load()) break;
		}

		if (sequence->immediateCrossfadeActive)
		{
			size_t mixed = FillImmediateMusicSequenceCrossfade(sequence, output + outputPos, totalSamples - outputPos);
			outputPos += mixed;
			if (mixed > 0) continue;
			if (sequence->finished.load()) break;
		}

		int stage = sequence->stage.load();
		MusicSequenceSegment* segment = nullptr;
		SoundDecoder* decoder = nullptr;

		if (stage == MSEQ_Intro)
		{
			segment = &sequence->intro;
			decoder = segment->decoder[0];
		}
		else if (stage == MSEQ_Loop)
		{
			segment = &sequence->loop;
			int active = sequence->activeLoopDecoder.load();
			if (!sequence->loopReady[active].load())
			{
				memset(output + outputPos, 0, (totalSamples - outputPos) * sizeof(float));
				return true;
			}
			decoder = segment->decoder[active];
		}
		else if (stage == MSEQ_Outro)
		{
			segment = &sequence->outro;
			decoder = segment->decoder[0];
		}
		else
		{
			FinishMusicSequence(sequence);
			break;
		}

		if (segment == nullptr || decoder == nullptr)
		{
			FinishMusicSequence(sequence);
			break;
		}

		size_t wanted = totalSamples - outputPos;
		size_t got = ReadMusicSequenceSamples(sequence, *segment, decoder, output + outputPos, wanted);
		outputPos += got;
		if (got == wanted) break;

		// The decoder returned a short read: this is the exact segment boundary.
		// Continue filling the same output buffer from the next segment.
		if (stage == MSEQ_Intro)
		{
			if (sequence->endRequested.load() || !SequenceHasSegment(sequence->loop))
			{
				if (SequenceHasSegment(sequence->outro)) sequence->stage = MSEQ_Outro;
				else FinishMusicSequence(sequence);
			}
			else
			{
				sequence->activeLoopDecoder = 0;
				sequence->stage = MSEQ_Loop;
			}
		}
		else if (stage == MSEQ_Loop)
		{
			int exhausted = sequence->activeLoopDecoder.load();
			sequence->loopReady[exhausted] = false;
			sequence->loopResetNeeded[exhausted] = true;

			if (sequence->endRequested.load())
			{
				if (SequenceHasSegment(sequence->outro)) sequence->stage = MSEQ_Outro;
				else FinishMusicSequence(sequence);
			}
			else
			{
				int next = 1 - exhausted;
				sequence->activeLoopDecoder = next;
				// If the spare decoder was not rebuilt yet, the next iteration will
				// output silence until S_UpdateMusicSequences prepares it. Normal
				// music loops are much longer than one game tick, so this is a guard.
			}
		}
		else if (stage == MSEQ_Outro)
		{
			FinishMusicSequence(sequence);
		}
	}

	if (outputPos < totalSamples)
	{
		memset(output + outputPos, 0, (totalSamples - outputPos) * sizeof(float));
	}
	return outputPos > 0 || !sequence->finished;
}

static bool CreateMusicSequenceStream(MusicSequencePlayback* sequence)
{
	if (sequence == nullptr || sequence->sampleRate <= 0) return false;
	int channels = sequence->channels == ChannelConfig_Mono ? 1 : 2;
	int flags = SoundStream::Float;
	if (channels == 1) flags |= SoundStream::Mono;

	// 4096 frames gives a modest streaming buffer while keeping transition
	// latency low. Segment boundaries themselves are filled within one callback.
	int bufferSize = 4096 * channels * (int)sizeof(float);
	sequence->convert.resize((size_t)bufferSize);
	sequence->crossfadeMix.resize((size_t)bufferSize / sizeof(float));
	sequence->stream.reset(GSnd->CreateStream(FillMusicSequenceStream, bufferSize, flags,
		sequence->sampleRate, sequence));
	if (!sequence->stream) return false;
	if (!sequence->stream->Play(true, 1.f))
	{
		sequence->stream.reset();
		return false;
	}
	ApplyMusicSequenceVolume(sequence);
	ApplyMusicSequencePause(sequence, MusicPaused);
	return true;
}

static void UpdateMusicSequencePlayback(MusicSequencePlayback* sequence)
{
	if (sequence == nullptr) return;

	// Rebuild exhausted loop decoders on the game thread. Two decoder
	// instances ping-pong so the audio callback normally never allocates.
	if (SequenceHasSegment(sequence->loop))
	{
		for (int i = 0; i < 2; i++)
		{
			if (sequence->loopResetNeeded[i].exchange(false))
			{
				if (sequence->loop.decoder[i] != nullptr)
				{
					SoundDecoder_Close(sequence->loop.decoder[i]);
					sequence->loop.decoder[i] = nullptr;
				}
				sequence->loop.decoder[i] = CreateSequenceDecoder(sequence->loop);
				sequence->loopReady[i] = sequence->loop.decoder[i] != nullptr;
			}
		}
	}
}

static void S_UpdateMusicSequences()
{
	for (auto it = musicSequences.begin(); it != musicSequences.end(); )
	{
		auto sequence = it->second.get();
		UpdateMusicSequencePlayback(sequence);

		if (sequence->finished.load())
		{
			ShutdownMusicSequence(sequence);
			it = musicSequences.erase(it);
		}
		else
		{
			++it;
		}
	}
}


static void ShutdownMusicStemLayer(MusicStemLayer* stem)
{
	if (stem == nullptr) return;
	CloseSequenceSegmentDecoders(stem->source);
}

static void ShutdownMusicStemGroup(MusicStemGroupPlayback* group)
{
	if (group == nullptr) return;
	if (group->stream)
	{
		group->stream->Stop();
		group->stream.reset();
	}
	for (auto& entry : group->stems)
	{
		ShutdownMusicStemLayer(entry.second.get());
	}
	group->finished = true;
}

static void StopMusicStemGroupInternal(int slotId)
{
	auto it = musicStemGroups.find(slotId);
	if (it == musicStemGroups.end()) return;
	ShutdownMusicStemGroup(it->second.get());
	musicStemGroups.erase(it);
}

static void ApplyMusicStemGroupVolume(MusicStemGroupPlayback* group)
{
	if (group == nullptr || !group->stream) return;
	float volume = group->volume;
	if (volume < 0.f) volume = 0.f;
	if (volume > 1.f) volume = 1.f;
	group->stream->SetVolume(volume);
}

static void ApplyMusicStemGroupPause(MusicStemGroupPlayback* group, bool globalPaused)
{
	if (group == nullptr || !group->stream) return;
	group->stream->SetPaused(globalPaused || group->paused);
}

static size_t ReadMusicStemSamples(MusicStemGroupPlayback* group, MusicStemLayer* stem,
	SoundDecoder* decoder, float* output, size_t sampleCount)
{
	if (group == nullptr || stem == nullptr || decoder == nullptr || output == nullptr || sampleCount == 0) return 0;
	int sampleSize = SequenceSampleSize(stem->source.sampleType);
	if (sampleSize <= 0) return 0;

	size_t wantedBytes = sampleCount * (size_t)sampleSize;
	if (group->convert.size() < wantedBytes) group->convert.resize(wantedBytes);
	size_t gotBytes = 0;
	while (gotBytes < wantedBytes)
	{
		size_t got = SoundDecoder_Read(decoder, group->convert.data() + gotBytes, wantedBytes - gotBytes);
		if (got == 0) break;
		gotBytes += got;
	}

	size_t gotSamples = gotBytes / (size_t)sampleSize;
	float gain = stem->source.trackVolume;
	if (stem->source.sampleType == SampleType_Float32)
	{
		const float* src = (const float*)group->convert.data();
		for (size_t i = 0; i < gotSamples; i++) output[i] = src[i] * gain;
	}
	else if (stem->source.sampleType == SampleType_Int16)
	{
		const int16_t* src = (const int16_t*)group->convert.data();
		for (size_t i = 0; i < gotSamples; i++) output[i] = src[i] * gain * (1.f / 32768.f);
	}
	else if (stem->source.sampleType == SampleType_UInt8)
	{
		const uint8_t* src = group->convert.data();
		for (size_t i = 0; i < gotSamples; i++) output[i] = ((int)src[i] - 128) * gain * (1.f / 128.f);
	}
	return gotSamples;
}

static bool FillMusicStemGroupStream(SoundStream* stream, void* buff, int len, void* userdata)
{
	auto group = static_cast<MusicStemGroupPlayback*>(userdata);
	float* output = (float*)buff;
	size_t totalSamples = (size_t)len / sizeof(float);
	size_t outputPos = 0;

	if (group == nullptr || group->finished.load())
	{
		memset(buff, 0, len);
		return false;
	}

	memset(buff, 0, len);
	auto masterIt = group->stems.find(0);
	if (masterIt == group->stems.end())
	{
		group->finished = true;
		return false;
	}

	while (outputPos < totalSamples && !group->finished.load())
	{
		int active = group->activeDecoder.load();
		if (active < 0 || active > 1 || !group->ready[active].load())
		{
			// The spare decoder set is rebuilt on the game thread. Keeping the
			// stream alive here avoids allocating decoders in the audio callback.
			return true;
		}

		size_t wanted = totalSamples - outputPos;
		if (group->stemBuffer.size() < wanted) group->stemBuffer.resize(wanted);

		auto master = masterIt->second.get();
		SoundDecoder* masterDecoder = master->source.decoder[active];
		size_t masterGot = ReadMusicStemSamples(group, master, masterDecoder,
			group->stemBuffer.data(), wanted);

		if (masterGot > 0)
		{
			float stemGain = master->volume.load();
			for (size_t i = 0; i < masterGot; i++)
			{
				output[outputPos + i] += group->stemBuffer[i] * stemGain;
			}

			for (auto& entry : group->stems)
			{
				if (entry.first == 0) continue;
				auto stem = entry.second.get();
				SoundDecoder* decoder = stem->source.decoder[active];
				size_t got = ReadMusicStemSamples(group, stem, decoder,
					group->stemBuffer.data(), masterGot);
				float gain = stem->volume.load();
				for (size_t i = 0; i < got; i++)
				{
					output[outputPos + i] += group->stemBuffer[i] * gain;
				}
				if (got < masterGot) group->lengthMismatch = true;
			}
			outputPos += masterGot;
		}

		if (masterGot == wanted) break;

		// Stem 0 reached the exact group boundary. All other stems abandon
		// their current decoder at this same sample position, so a bad source
		// length can never accumulate synchronization drift over later loops.
		group->ready[active] = false;
		group->resetNeeded[active] = true;

		if (!group->looping)
		{
			group->finished = true;
			break;
		}

		int next = 1 - active;
		group->activeDecoder = next;
		if (!group->ready[next].load()) return true;
	}

	return outputPos > 0 || !group->finished.load();
}

static bool CreateMusicStemGroupStream(MusicStemGroupPlayback* group)
{
	if (group == nullptr || group->sampleRate <= 0 || group->stems.empty()) return false;
	int channels = group->channels == ChannelConfig_Mono ? 1 : 2;
	int flags = SoundStream::Float;
	if (channels == 1) flags |= SoundStream::Mono;

	int bufferSize = 4096 * channels * (int)sizeof(float);
	group->convert.resize((size_t)bufferSize);
	group->stemBuffer.resize((size_t)bufferSize / sizeof(float));
	group->stream.reset(GSnd->CreateStream(FillMusicStemGroupStream, bufferSize, flags,
		group->sampleRate, group));
	if (!group->stream) return false;
	if (!group->stream->Play(true, 1.f))
	{
		group->stream.reset();
		return false;
	}
	ApplyMusicStemGroupVolume(group);
	ApplyMusicStemGroupPause(group, MusicPaused);
	return true;
}

static void UpdateMusicStemGroupPlayback(MusicStemGroupPlayback* group)
{
	if (group == nullptr) return;

	for (int decoderIndex = 0; decoderIndex < 2; decoderIndex++)
	{
		if (!group->resetNeeded[decoderIndex].exchange(false)) continue;
		if (!group->looping && decoderIndex == 0) continue;

		bool ready = true;
		for (auto& entry : group->stems)
		{
			auto stem = entry.second.get();
			if (stem->source.decoder[decoderIndex] != nullptr)
			{
				SoundDecoder_Close(stem->source.decoder[decoderIndex]);
				stem->source.decoder[decoderIndex] = nullptr;
			}
			stem->source.decoder[decoderIndex] = CreateSequenceDecoder(stem->source);
			if (stem->source.decoder[decoderIndex] == nullptr) ready = false;
		}
		group->ready[decoderIndex] = ready;
		if (!ready) group->finished = true;
	}

	const float halfPi = 1.57079632679489661923f;
	auto now = std::chrono::steady_clock::now();
	for (auto& entry : group->stems)
	{
		auto stem = entry.second.get();
		if (!stem->fadeActive) continue;
		double elapsed = std::chrono::duration<double>(now - stem->fadeStartTime).count();
		double raw = stem->fadeDurationSeconds > 0.0 ? elapsed / stem->fadeDurationSeconds : 1.0;
		if (raw < 0.0) raw = 0.0;
		if (raw > 1.0) raw = 1.0;
		float t = (float)raw;
		float shaped = stem->fadeTargetVolume >= stem->fadeStartVolume
			? std::sin(t * halfPi)
			: 1.f - std::cos(t * halfPi);
		float volume = stem->fadeStartVolume + (stem->fadeTargetVolume - stem->fadeStartVolume) * shaped;
		if (raw >= 1.0) volume = stem->fadeTargetVolume;
		stem->volume = volume;
		if (raw >= 1.0) stem->fadeActive = false;
	}

	if (group->lengthMismatch.load() && !group->mismatchReported)
	{
		Printf("Music stem group slot %d has a stem shorter than master stem 0; silence is inserted until the master loop boundary\n", group->id);
		group->mismatchReported = true;
	}
}

static void S_UpdateMusicStemGroups()
{
	for (auto it = musicStemGroups.begin(); it != musicStemGroups.end(); )
	{
		auto group = it->second.get();
		UpdateMusicStemGroupPlayback(group);
		if (group->finished.load())
		{
			ShutdownMusicStemGroup(group);
			it = musicStemGroups.erase(it);
		}
		else ++it;
	}
}

bool S_BeginMusicStemGroup(int slotId, bool looping)
{
	if (slotId < 0 || !MusicEnabled()) return false;
	auto oldIt = pendingMusicStemGroups.find(slotId);
	if (oldIt != pendingMusicStemGroups.end())
	{
		ShutdownMusicStemGroup(oldIt->second.get());
		pendingMusicStemGroups.erase(oldIt);
	}
	auto group = std::make_unique<MusicStemGroupPlayback>();
	group->id = slotId;
	group->looping = looping;
	pendingMusicStemGroups[slotId] = std::move(group);
	return true;
}

bool S_AddMusicStem(int slotId, int stemId, const char* musicName, float initialVolume)
{
	if (slotId < 0 || stemId < 0 || musicName == nullptr || musicName[0] == 0) return false;
	auto groupIt = pendingMusicStemGroups.find(slotId);
	if (groupIt == pendingMusicStemGroups.end()) return false;
	auto group = groupIt->second.get();

	if (initialVolume < 0.f) initialVolume = 0.f;
	if (initialVolume > 1.f) initialVolume = 1.f;
	auto stem = std::make_unique<MusicStemLayer>();
	stem->id = stemId;
	stem->volume = initialVolume;
	int decoderCount = group->looping ? 2 : 1;
	if (!LoadMusicSequenceSegment(musicName, stem->source, decoderCount))
	{
		ShutdownMusicStemLayer(stem.get());
		return false;
	}

	MusicStemLayer* formatStem = nullptr;
	for (auto& entry : group->stems)
	{
		if (entry.first != stemId)
		{
			formatStem = entry.second.get();
			break;
		}
	}
	if (formatStem != nullptr &&
		(formatStem->source.sampleRate != stem->source.sampleRate ||
		 formatStem->source.channels != stem->source.channels))
	{
		Printf("Music stem group slot %d requires matching sample rate and channel count for every stem\n", slotId);
		ShutdownMusicStemLayer(stem.get());
		return false;
	}

	auto oldIt = group->stems.find(stemId);
	if (oldIt != group->stems.end()) ShutdownMusicStemLayer(oldIt->second.get());
	group->stems[stemId] = std::move(stem);
	return true;
}

bool S_StartMusicStemGroup(int slotId)
{
	auto pendingIt = pendingMusicStemGroups.find(slotId);
	if (pendingIt == pendingMusicStemGroups.end()) return false;
	auto group = std::move(pendingIt->second);
	pendingMusicStemGroups.erase(pendingIt);

	auto masterIt = group->stems.find(0);
	if (masterIt == group->stems.end())
	{
		Printf("Music stem group slot %d requires stem 0 as its master timeline\n", slotId);
		ShutdownMusicStemGroup(group.get());
		return false;
	}

	group->sampleRate = masterIt->second->source.sampleRate;
	group->channels = masterIt->second->source.channels;
	bool decoder0Ready = true;
	bool decoder1Ready = group->looping;
	for (auto& entry : group->stems)
	{
		decoder0Ready = decoder0Ready && entry.second->source.decoder[0] != nullptr;
		if (group->looping) decoder1Ready = decoder1Ready && entry.second->source.decoder[1] != nullptr;
	}
	group->ready[0] = decoder0Ready;
	group->ready[1] = group->looping ? decoder1Ready : false;
	if (!decoder0Ready || (group->looping && !decoder1Ready))
	{
		ShutdownMusicStemGroup(group.get());
		return false;
	}

	float initialVolume = slotId == 0 ? mainMusicSlotVolume : 1.f;
	bool initialPaused = slotId == 0 ? mainMusicSlotPaused : false;
	group->volume = initialVolume;
	group->paused = initialPaused;

	S_StopMusicSlot(slotId);
	if (slotId == 0)
	{
		mainMusicSlotVolume = initialVolume;
		mainMusicSlotPaused = initialPaused;
	}

	if (!CreateMusicStemGroupStream(group.get()))
	{
		ShutdownMusicStemGroup(group.get());
		return false;
	}

	musicStemGroups[slotId] = std::move(group);
	return true;
}

bool S_SetMusicStemVolume(int slotId, int stemId, float volume)
{
	auto groupIt = musicStemGroups.find(slotId);
	if (groupIt == musicStemGroups.end()) return false;
	auto stemIt = groupIt->second->stems.find(stemId);
	if (stemIt == groupIt->second->stems.end()) return false;
	if (volume < 0.f) volume = 0.f;
	if (volume > 1.f) volume = 1.f;
	stemIt->second->fadeActive = false;
	stemIt->second->volume = volume;
	return true;
}

bool S_SetMusicStemVolumeFade(int slotId, int stemId, float targetVolume, float fadeTime)
{
	auto groupIt = musicStemGroups.find(slotId);
	if (groupIt == musicStemGroups.end()) return false;
	auto stemIt = groupIt->second->stems.find(stemId);
	if (stemIt == groupIt->second->stems.end()) return false;
	auto stem = stemIt->second.get();
	if (targetVolume < 0.f) targetVolume = 0.f;
	if (targetVolume > 1.f) targetVolume = 1.f;
	if (fadeTime <= 0.f) return S_SetMusicStemVolume(slotId, stemId, targetVolume);

	stem->fadeStartVolume = stem->volume.load();
	stem->fadeTargetVolume = targetVolume;
	stem->fadeDurationSeconds = fadeTime;
	stem->fadeStartTime = std::chrono::steady_clock::now();
	stem->fadeActive = true;
	return true;
}

bool S_IsMusicStemGroupPlaying(int slotId)
{
	auto it = musicStemGroups.find(slotId);
	return it != musicStemGroups.end() && !it->second->finished.load();
}

bool S_PlayMusicSequence(int slotId, const char* introName, const char* loopName, const char* outroName)
{
	if (slotId < 0 || !MusicEnabled()) return false;
	if ((introName == nullptr || introName[0] == 0) &&
		(loopName == nullptr || loopName[0] == 0) &&
		(outroName == nullptr || outroName[0] == 0))
	{
		S_StopMusicSlot(slotId);
		return true;
	}

	// Prepare and validate the new sequence before replacing the current slot.
	// This mirrors S_ChangeMusic's behavior of leaving the old song alive if the
	// replacement cannot be opened.
	float initialVolume = slotId == 0 ? mainMusicSlotVolume : 1.f;
	bool initialPaused = slotId == 0 ? mainMusicSlotPaused : false;
	auto sequence = std::make_unique<MusicSequencePlayback>();
	sequence->id = slotId;
	sequence->volume = initialVolume;
	sequence->paused = initialPaused;

	if (!LoadMusicSequenceSegment(introName, sequence->intro, 1) ||
		!LoadMusicSequenceSegment(loopName, sequence->loop, 2) ||
		!LoadMusicSequenceSegment(outroName, sequence->outro, 1))
	{
		ShutdownMusicSequence(sequence.get());
		return false;
	}

	MusicSequenceSegment* formatSource = nullptr;
	if (SequenceHasSegment(sequence->intro)) formatSource = &sequence->intro;
	else if (SequenceHasSegment(sequence->loop)) formatSource = &sequence->loop;
	else if (SequenceHasSegment(sequence->outro)) formatSource = &sequence->outro;
	if (formatSource == nullptr)
	{
		ShutdownMusicSequence(sequence.get());
		return false;
	}

	if (!SequenceFormatsMatch(*formatSource, sequence->intro) ||
		!SequenceFormatsMatch(*formatSource, sequence->loop) ||
		!SequenceFormatsMatch(*formatSource, sequence->outro))
	{
		Printf("Music sequence slot %d requires matching sample rate and channel count for all segments\n", slotId);
		ShutdownMusicSequence(sequence.get());
		return false;
	}

	sequence->sampleRate = formatSource->sampleRate;
	sequence->channels = formatSource->channels;
	sequence->loopReady[0] = sequence->loop.decoder[0] != nullptr;
	sequence->loopReady[1] = sequence->loop.decoder[1] != nullptr;

	// A sequence owns the slot exclusively. Slot 0 therefore replaces legacy
	// SetMusic/S_ChangeMusic playback, while slots 1+ replace their normal slot.
	S_StopMusicSlot(slotId);
	if (slotId == 0)
	{
		mainMusicSlotVolume = initialVolume;
		mainMusicSlotPaused = initialPaused;
	}

	if (SequenceHasSegment(sequence->intro)) sequence->stage = MSEQ_Intro;
	else if (SequenceHasSegment(sequence->loop)) sequence->stage = MSEQ_Loop;
	else sequence->stage = MSEQ_Outro;

	if (!CreateMusicSequenceStream(sequence.get()))
	{
		Printf("Unable to create output stream for music sequence slot %d\n", slotId);
		ShutdownMusicSequence(sequence.get());
		return false;
	}

	musicSequences[slotId] = std::move(sequence);
	return true;
}

bool S_EndMusicSequence(int slotId)
{
	auto it = musicSequences.find(slotId);
	if (it == musicSequences.end()) return false;
	auto sequence = it->second.get();
	if (sequence->finished.load()) return false;
	sequence->endRequested = true;
	return true;
}

bool S_EndMusicSequenceImmediate(int slotId, float crossfadeTime)
{
	auto it = musicSequences.find(slotId);
	if (it == musicSequences.end()) return false;
	auto sequence = it->second.get();
	if (sequence->finished.load()) return false;

	// Once the immediate transition has started, do not restart the outro or
	// reset the fade if the script calls this function again.
	if (sequence->immediateTransitionStarted.load()) return true;

	if (crossfadeTime < 0.f) crossfadeTime = 0.f;
	double frameCount = (double)crossfadeTime * (double)sequence->sampleRate;
	int64_t fadeFrames = frameCount > 0.0 ? (int64_t)std::llround(frameCount) : 0;
	if (crossfadeTime > 0.f && fadeFrames < 1) fadeFrames = 1;

	// Also mark a normal end request so a segment boundary reached before the
	// audio callback observes this request cannot start another loop iteration.
	sequence->endRequested = true;
	sequence->immediateFadeFrames = fadeFrames;
	sequence->immediateEndRequested = true;
	return true;
}

bool S_IsMusicSequencePlaying(int slotId)
{
	auto it = musicSequences.find(slotId);
	return it != musicSequences.end() && !it->second->finished.load();
}

static void ShutdownMusicSlot(MusicSlotPlayback* slot)
{
	if (slot == nullptr) return;

	// Stop the output stream first so its callback can no longer touch the handle.
	if (slot->stream)
	{
		slot->stream->Stop();
		slot->stream.reset();
	}

	if (slot->handle != nullptr)
	{
		ZMusic_Stop(slot->handle);
		auto handle = slot->handle;
		slot->handle = nullptr;
		ZMusic_Close(handle);
	}
}

static void ApplyMusicSlotGain(MusicSlotPlayback* slot, float gain)
{
	if (slot == nullptr || slot->handle == nullptr) return;
	if (gain < 0.f) gain = 0.f;
	if (gain > 1.f) gain = 1.f;

	float volume = slot->volume * slot->trackVolume * gain;
	if (volume < 0.f) volume = 0.f;
	if (volume > 1.f) volume = 1.f;

	if (slot->stream)
	{
		slot->stream->SetVolume(volume);
	}
	else
	{
		// Non-streaming music (for example, an external MIDI device) does not
		// have a SoundStream, so ask ZMusic to apply a per-song relative volume.
		ChangeMusicSetting(zmusic_relative_volume, slot->handle, volume);
		ZMusic_VolumeChanged(slot->handle);
	}
}

static void ApplyMusicSlotVolume(MusicSlotPlayback* slot)
{
	ApplyMusicSlotGain(slot, 1.f);
}

static bool FillMusicSlotStream(SoundStream* stream, void* buff, int len, void* userdata)
{
	auto slot = static_cast<MusicSlotPlayback*>(userdata);
	if (slot == nullptr || slot->handle == nullptr)
	{
		memset((char*)buff, 0, len);
		return false;
	}

	bool written;
	if (slot->isfloat)
	{
		written = ZMusic_FillStream(slot->handle, buff, len);
	}
	else
	{
		// Match the main music path: convert 16-bit ZMusic output to the
		// floating-point stream format expected by the sound renderer.
		slot->convert.Resize(len / 2);
		written = ZMusic_FillStream(slot->handle, slot->convert.Data(), len / 2);
		float* fbuf = (float*)buff;
		for (int i = 0; i < len / 4; i++)
		{
			fbuf[i] = slot->convert[i] * (1.f / 32768.f);
		}
	}

	if (!written)
	{
		memset((char*)buff, 0, len);
		return false;
	}
	return true;
}

static bool CreateMusicSlotStream(MusicSlotPlayback* slot)
{
	if (slot == nullptr || slot->handle == nullptr) return false;

	SoundStreamInfo fmt;
	ZMusic_GetStreamInfo(slot->handle, &fmt);
	slot->isfloat = fmt.mNumChannels > 0;

	// A zero buffer size means ZMusic is playing through an external backend.
	// That path can still be used as a slot; it simply has no SoundStream.
	if (fmt.mBufferSize == 0)
	{
		ApplyMusicSlotVolume(slot);
		return true;
	}

	if (!slot->isfloat) fmt.mBufferSize *= 2;

	int flags = SoundStream::Float;
	if (abs(fmt.mNumChannels) < 2) flags |= SoundStream::Mono;

	slot->stream.reset(GSnd->CreateStream(FillMusicSlotStream, fmt.mBufferSize, flags, fmt.mSampleRate, slot));
	if (!slot->stream) return false;

	if (!slot->stream->Play(true, 1.f))
	{
		slot->stream.reset();
		return false;
	}
	ApplyMusicSlotVolume(slot);
	return true;
}

static void ApplyMusicSlotPause(MusicSlotPlayback* slot, bool globalPaused)
{
	if (slot == nullptr || slot->handle == nullptr) return;

	bool pause = globalPaused || slot->paused;
	if (pause)
	{
		ZMusic_Pause(slot->handle);
	}
	else
	{
		ZMusic_Resume(slot->handle);
	}
	if (slot->stream) slot->stream->SetPaused(pause);
}

static std::unique_ptr<MusicSequencePlayback> CreateMusicCrossfadeTarget(int slotId, const char* musicname,
	bool looping, float volume, bool paused)
{
	auto sequence = std::make_unique<MusicSequencePlayback>();
	sequence->id = slotId;
	sequence->volume = 0.f; // Start silent; the transition owns the fade-in gain.
	sequence->paused = paused;

	bool loaded = looping
		? LoadMusicSequenceSegment(musicname, sequence->loop, 2)
		: LoadMusicSequenceSegment(musicname, sequence->intro, 1);
	if (!loaded)
	{
		ShutdownMusicSequence(sequence.get());
		return nullptr;
	}

	MusicSequenceSegment* formatSource = looping ? &sequence->loop : &sequence->intro;
	if (!SequenceHasSegment(*formatSource))
	{
		ShutdownMusicSequence(sequence.get());
		return nullptr;
	}

	sequence->sampleRate = formatSource->sampleRate;
	sequence->channels = formatSource->channels;
	sequence->loopReady[0] = sequence->loop.decoder[0] != nullptr;
	sequence->loopReady[1] = sequence->loop.decoder[1] != nullptr;
	sequence->stage = looping ? MSEQ_Loop : MSEQ_Intro;

	if (!CreateMusicSequenceStream(sequence.get()))
	{
		ShutdownMusicSequence(sequence.get());
		return nullptr;
	}

	sequence->volume = volume;
	ApplyMusicSequenceGain(sequence.get(), 0.f);
	return sequence;
}

static float GetMusicSlotCrossfadeProgress(const MusicSlotCrossfadePlayback* crossfade)
{
	if (crossfade == nullptr || crossfade->durationSeconds <= 0.0) return 1.f;
	auto now = std::chrono::steady_clock::now();
	double elapsed = std::chrono::duration<double>(now - crossfade->startTime).count();
	double value = elapsed / crossfade->durationSeconds;
	if (value < 0.0) value = 0.0;
	if (value > 1.0) value = 1.0;
	return (float)value;
}

static void ApplyMusicSlotCrossfadeGains(MusicSlotCrossfadePlayback* crossfade)
{
	if (crossfade == nullptr) return;
	float t = GetMusicSlotCrossfadeProgress(crossfade);
	const float halfPi = 1.57079632679489661923f;
	float outgoingGain = std::cos(t * halfPi);
	float incomingGain = std::sin(t * halfPi);

	switch (crossfade->outgoingKind)
	{
	case MCF_LegacyMain:
		ApplyMainMusicSlotGain(outgoingGain);
		break;
	case MCF_NormalSlot:
		ApplyMusicSlotGain(crossfade->outgoingSlot.get(), outgoingGain);
		break;
	case MCF_Sequence:
		ApplyMusicSequenceGain(crossfade->outgoingSequence.get(), outgoingGain);
		break;
	default:
		break;
	}

	auto incomingIt = musicSequences.find(crossfade->slotId);
	if (incomingIt != musicSequences.end() && incomingIt->second.get() == crossfade->incomingIdentity)
	{
		incomingIt->second->volume = crossfade->targetVolume;
		ApplyMusicSequenceGain(incomingIt->second.get(), incomingGain);
	}
}

static void ApplyMusicSlotCrossfadeOutgoingPause(MusicSlotCrossfadePlayback* crossfade, bool globalPaused)
{
	if (crossfade == nullptr) return;
	switch (crossfade->outgoingKind)
	{
	case MCF_LegacyMain:
		ApplyMainMusicSlotPause(globalPaused);
		break;
	case MCF_NormalSlot:
		ApplyMusicSlotPause(crossfade->outgoingSlot.get(), globalPaused);
		break;
	case MCF_Sequence:
		ApplyMusicSequencePause(crossfade->outgoingSequence.get(), globalPaused);
		break;
	default:
		break;
	}
}

static void ShutdownMusicSlotCrossfadeOutgoing(MusicSlotCrossfadePlayback* crossfade)
{
	if (crossfade == nullptr) return;
	switch (crossfade->outgoingKind)
	{
	case MCF_LegacyMain:
		StopLegacyMainMusicOnly(false);
		break;
	case MCF_NormalSlot:
		ShutdownMusicSlot(crossfade->outgoingSlot.get());
		crossfade->outgoingSlot.reset();
		break;
	case MCF_Sequence:
		ShutdownMusicSequence(crossfade->outgoingSequence.get());
		crossfade->outgoingSequence.reset();
		break;
	default:
		break;
	}
	crossfade->outgoingKind = MCF_None;
}

static void StopMusicSlotCrossfadeInternal(int slotId)
{
	auto it = musicSlotCrossfades.find(slotId);
	if (it == musicSlotCrossfades.end()) return;
	ShutdownMusicSlotCrossfadeOutgoing(it->second.get());
	musicSlotCrossfades.erase(it);
}

static void FinalizeMusicSlotCrossfade(int slotId)
{
	auto it = musicSlotCrossfades.find(slotId);
	if (it == musicSlotCrossfades.end()) return;
	auto crossfade = it->second.get();

	auto incomingIt = musicSequences.find(slotId);
	if (incomingIt != musicSequences.end() && incomingIt->second.get() == crossfade->incomingIdentity)
	{
		incomingIt->second->volume = crossfade->targetVolume;
		ApplyMusicSequenceVolume(incomingIt->second.get());
	}
	ShutdownMusicSlotCrossfadeOutgoing(crossfade);
	musicSlotCrossfades.erase(it);
}

static void S_UpdateMusicSlotCrossfades()
{
	for (auto it = musicSlotCrossfades.begin(); it != musicSlotCrossfades.end(); )
	{
		auto crossfade = it->second.get();
		auto incomingIt = musicSequences.find(crossfade->slotId);
		if (incomingIt == musicSequences.end() || incomingIt->second.get() != crossfade->incomingIdentity)
		{
			ShutdownMusicSlotCrossfadeOutgoing(crossfade);
			it = musicSlotCrossfades.erase(it);
			continue;
		}

		if (crossfade->outgoingKind == MCF_NormalSlot && crossfade->outgoingSlot)
		{
			if (crossfade->outgoingSlot->handle != nullptr) ZMusic_Update(crossfade->outgoingSlot->handle);
			if (crossfade->outgoingSlot->handle == nullptr ||
				(!(MusicPaused || crossfade->outgoingSlot->paused) && !ZMusic_IsPlaying(crossfade->outgoingSlot->handle)))
			{
				ShutdownMusicSlot(crossfade->outgoingSlot.get());
				crossfade->outgoingSlot.reset();
				crossfade->outgoingKind = MCF_None;
			}
		}
		else if (crossfade->outgoingKind == MCF_Sequence && crossfade->outgoingSequence)
		{
			UpdateMusicSequencePlayback(crossfade->outgoingSequence.get());
			if (crossfade->outgoingSequence->finished.load())
			{
				ShutdownMusicSequence(crossfade->outgoingSequence.get());
				crossfade->outgoingSequence.reset();
				crossfade->outgoingKind = MCF_None;
			}
		}
		else if (crossfade->outgoingKind == MCF_LegacyMain)
		{
			if (mus_playing.handle == nullptr ||
				(!(MusicPaused || mainMusicSlotPaused) && !ZMusic_IsPlaying(mus_playing.handle)))
			{
				StopLegacyMainMusicOnly(false);
				crossfade->outgoingKind = MCF_None;
			}
		}

		ApplyMusicSlotCrossfadeGains(crossfade);
		if (GetMusicSlotCrossfadeProgress(crossfade) >= 1.f || incomingIt->second->finished.load())
		{
			incomingIt->second->volume = crossfade->targetVolume;
			ApplyMusicSequenceVolume(incomingIt->second.get());
			ShutdownMusicSlotCrossfadeOutgoing(crossfade);
			it = musicSlotCrossfades.erase(it);
		}
		else
		{
			++it;
		}
	}
}

bool S_PlayMusicSlotCrossfade(int slotId, const char* musicname, bool looping, float crossfadeTime)
{
	if (slotId < 0 || !MusicEnabled()) return false;
	if (crossfadeTime <= 0.f) return S_PlayMusicSlot(slotId, musicname, 0, looping);
	if (slotId == 0 && PlayList.GetNumSongs()) return true;

	// Finish a previous transition first so a new request fades from the song
	// that was already becoming current rather than accumulating hidden streams.
	FinalizeMusicSlotCrossfade(slotId);

	// A synchronized stem group is already a multi-source mixer. Ordinary-song
	// crossfade does not borrow another user-visible slot, so keep this path
	// simple and perform an immediate replacement when a stem group owns it.
	if (musicStemGroups.find(slotId) != musicStemGroups.end())
		return S_PlayMusicSlot(slotId, musicname, 0, looping);

	bool hasCurrent = false;
	if (musicSequences.find(slotId) != musicSequences.end()) hasCurrent = true;
	else if (slotId == 0) hasCurrent = mus_playing.handle != nullptr && ZMusic_IsPlaying(mus_playing.handle);
	else
	{
		auto currentSlot = musicSlots.find(slotId);
		hasCurrent = currentSlot != musicSlots.end() && currentSlot->second->handle != nullptr &&
			ZMusic_IsPlaying(currentSlot->second->handle);
	}
	if (!hasCurrent) return S_PlayMusicSlot(slotId, musicname, 0, looping);

	float targetVolume = slotId == 0 ? mainMusicSlotVolume : 1.f;
	bool targetPaused = slotId == 0 ? mainMusicSlotPaused : false;
	if (slotId > 0)
	{
		auto seqIt = musicSequences.find(slotId);
		if (seqIt != musicSequences.end())
		{
			targetVolume = seqIt->second->volume;
			targetPaused = seqIt->second->paused;
		}
		else
		{
			auto slotIt = musicSlots.find(slotId);
			if (slotIt != musicSlots.end())
			{
				targetVolume = slotIt->second->volume;
				targetPaused = slotIt->second->paused;
			}
		}
	}

	auto incoming = CreateMusicCrossfadeTarget(slotId, musicname, looping, targetVolume, targetPaused);
	if (!incoming)
	{
		// SoundDecoder does not cover every ZMusic backend (notably some MIDI
		// paths). Preserve compatibility by falling back to an immediate change.
		return S_PlayMusicSlot(slotId, musicname, 0, looping);
	}

	auto crossfade = std::make_unique<MusicSlotCrossfadePlayback>();
	crossfade->slotId = slotId;
	crossfade->targetVolume = targetVolume;
	crossfade->durationSeconds = crossfadeTime;
	crossfade->startTime = std::chrono::steady_clock::now();
	crossfade->incomingIdentity = incoming.get();

	auto seqIt = musicSequences.find(slotId);
	if (seqIt != musicSequences.end())
	{
		crossfade->outgoingKind = MCF_Sequence;
		crossfade->outgoingSequence = std::move(seqIt->second);
		musicSequences.erase(seqIt);
	}
	else if (slotId == 0 && mus_playing.handle != nullptr)
	{
		crossfade->outgoingKind = MCF_LegacyMain;
	}
	else if (slotId > 0)
	{
		auto slotIt = musicSlots.find(slotId);
		if (slotIt != musicSlots.end())
		{
			crossfade->outgoingKind = MCF_NormalSlot;
			crossfade->outgoingSlot = std::move(slotIt->second);
			musicSlots.erase(slotIt);
		}
	}

	musicSequences[slotId] = std::move(incoming);
	musicSlotCrossfades[slotId] = std::move(crossfade);
	ApplyMusicSlotCrossfadeGains(musicSlotCrossfades[slotId].get());
	return true;
}

bool S_ChangeMusicCrossfade(const char* musicname, bool looping, float crossfadeTime)
{
	return S_PlayMusicSlotCrossfade(0, musicname, looping, crossfadeTime);
}

bool S_PlayMusicSlot(int slotId, const char* musicname, int order, bool looping)
{
	if (slotId < 0 || !MusicEnabled()) return false;

	// Empty names are a convenient way to clear a slot.
	if (musicname == nullptr || musicname[0] == 0)
	{
		S_StopMusicSlot(slotId);
		return true;
	}

	// Slot 0 is the existing GZDoom main music slot. Do not create a second
	// stream: route through S_ChangeMusic so SetMusic/S_ChangeMusic and
	// S_PlayMusicSlot(0, ...) always address the same playback object.
	if (slotId == 0)
	{
		mainMusicSlotPaused = false;
		bool changed = S_ChangeMusic(musicname, order, looping);
		if (changed)
		{
			ApplyMainMusicSlotVolume();
			ApplyMainMusicSlotPause(MusicPaused);
		}
		return changed;
	}

	// Perform the same game-specific music lookup used by S_ChangeMusic.
	FString lookedUpName;
	if (mus_cb.LookupFileName)
	{
		lookedUpName = mus_cb.LookupFileName(musicname, order);
		musicname = lookedUpName.GetChars();
	}
	if (musicname == nullptr || musicname[0] == 0)
	{
		S_StopMusicSlot(slotId);
		return true;
	}

	if (strncmp(musicname, "file://", 7) == 0)
	{
		musicname += 7;
	}

	FileReader reader = OpenMusic(musicname);
	if (!reader.isOpen()) return false;

	// Replacing one slot never touches any other slot or mus_playing.
	S_StopMusicSlot(slotId);

	auto slot = std::make_unique<MusicSlotPlayback>();
	slot->id = slotId;
	slot->name = musicname;
	slot->order = order;
	slot->looping = looping;

	int lumpnum = mus_cb.FindMusic(musicname);
	MidiDeviceSetting* devp = MidiDevices.CheckKey(lumpnum);
	int* mplay = ModPlayers.CheckKey(lumpnum);
	float* volp = MusicVolumes.CheckKey(lumpnum);
	if (volp) slot->trackVolume = *volp;

	auto mreader = GetMusicReader(reader);
	int modPlayer = mplay ? *mplay : *mod_preferred_player;
	int scratch;
	ChangeMusicSettingInt(zmusic_mod_preferredplayer, nullptr, modPlayer, &scratch);
	slot->handle = ZMusic_OpenSong(mreader, devp ? (EMidiDevice)devp->device : MDEV_DEFAULT, devp ? devp->args.GetChars() : "");
	if (slot->handle == nullptr)
	{
		Printf("Unable to load music slot %d (%s): %s\n", slotId, musicname, ZMusic_GetLastError());
		return false;
	}

	if (!ZMusic_Start(slot->handle, order, looping))
	{
		Printf("Unable to start music slot %d (%s): %s\n", slotId, musicname, ZMusic_GetLastError());
		ShutdownMusicSlot(slot.get());
		return false;
	}

	if (!CreateMusicSlotStream(slot.get()))
	{
		Printf("Unable to create output stream for music slot %d (%s)\n", slotId, musicname);
		ShutdownMusicSlot(slot.get());
		return false;
	}

	// Respect an already-active global pause when a slot is started while paused.
	ApplyMusicSlotPause(slot.get(), MusicPaused);
	musicSlots[slotId] = std::move(slot);
	return true;
}

void S_StopMusicSlot(int slotId)
{
	musicSlotVolumeFades.erase(slotId);
	auto pendingStemIt = pendingMusicStemGroups.find(slotId);
	if (pendingStemIt != pendingMusicStemGroups.end())
	{
		ShutdownMusicStemGroup(pendingStemIt->second.get());
		pendingMusicStemGroups.erase(pendingStemIt);
	}
	StopMusicSlotCrossfadeInternal(slotId);
	StopMusicSequenceInternal(slotId);
	StopMusicStemGroupInternal(slotId);

	if (slotId == 0)
	{
		mainMusicSlotPaused = false;
		mainMusicSlotVolume = 1.f;
		S_StopMusic(true);
		mus_playing.LastSong = "";
		return;
	}

	auto it = musicSlots.find(slotId);
	if (it == musicSlots.end()) return;

	ShutdownMusicSlot(it->second.get());
	musicSlots.erase(it);
}

void S_StopAdditionalMusicSlots()
{
	for (auto it = musicSlotVolumeFades.begin(); it != musicSlotVolumeFades.end(); )
	{
		if (it->first > 0) it = musicSlotVolumeFades.erase(it);
		else ++it;
	}

	for (auto it = musicSlotCrossfades.begin(); it != musicSlotCrossfades.end(); )
	{
		if (it->first > 0)
		{
			ShutdownMusicSlotCrossfadeOutgoing(it->second.get());
			it = musicSlotCrossfades.erase(it);
		}
		else ++it;
	}

	for (auto it = pendingMusicStemGroups.begin(); it != pendingMusicStemGroups.end(); )
	{
		if (it->first > 0)
		{
			ShutdownMusicStemGroup(it->second.get());
			it = pendingMusicStemGroups.erase(it);
		}
		else ++it;
	}

	for (auto it = musicStemGroups.begin(); it != musicStemGroups.end(); )
	{
		if (it->first > 0)
		{
			ShutdownMusicStemGroup(it->second.get());
			it = musicStemGroups.erase(it);
		}
		else ++it;
	}

	for (auto& entry : musicSlots)
	{
		ShutdownMusicSlot(entry.second.get());
	}
	musicSlots.clear();

	for (auto it = musicSequences.begin(); it != musicSequences.end(); )
	{
		if (it->first > 0)
		{
			ShutdownMusicSequence(it->second.get());
			it = musicSequences.erase(it);
		}
		else
		{
			++it;
		}
	}
}

void S_StopAllMusicSlots()
{
	// Stop additional streams first. S_StopMusic may resume music internally
	// before closing the legacy main handle, so no extra slot should remain
	// alive to be unintentionally resumed during that operation.
	S_StopAdditionalMusicSlots();
	S_StopMusicSlot(0);
}

static bool GetMusicSlotLogicalVolume(int slotId, float& volume)
{
	if (slotId < 0) return false;

	auto crossfadeIt = musicSlotCrossfades.find(slotId);
	if (crossfadeIt != musicSlotCrossfades.end())
	{
		volume = crossfadeIt->second->targetVolume;
		return true;
	}

	auto stemGroupIt = musicStemGroups.find(slotId);
	if (stemGroupIt != musicStemGroups.end())
	{
		volume = stemGroupIt->second->volume;
		return true;
	}

	auto sequenceIt = musicSequences.find(slotId);
	if (sequenceIt != musicSequences.end())
	{
		volume = sequenceIt->second->volume;
		return true;
	}

	if (slotId == 0)
	{
		volume = mainMusicSlotVolume;
		return true;
	}

	auto slotIt = musicSlots.find(slotId);
	if (slotIt == musicSlots.end()) return false;
	volume = slotIt->second->volume;
	return true;
}

static void SetMusicSlotVolumeInternal(int slotId, float volume)
{
	if (volume < 0.f) volume = 0.f;
	if (volume > 1.f) volume = 1.f;

	if (slotId == 0) mainMusicSlotVolume = volume;

	auto crossfadeIt = musicSlotCrossfades.find(slotId);
	if (crossfadeIt != musicSlotCrossfades.end())
	{
		auto crossfade = crossfadeIt->second.get();
		crossfade->targetVolume = volume;
		if (crossfade->outgoingSlot) crossfade->outgoingSlot->volume = volume;
		if (crossfade->outgoingSequence) crossfade->outgoingSequence->volume = volume;
		auto incomingIt = musicSequences.find(slotId);
		if (incomingIt != musicSequences.end() && incomingIt->second.get() == crossfade->incomingIdentity)
			incomingIt->second->volume = volume;
		ApplyMusicSlotCrossfadeGains(crossfade);
		return;
	}

	auto stemGroupIt = musicStemGroups.find(slotId);
	if (stemGroupIt != musicStemGroups.end())
	{
		stemGroupIt->second->volume = volume;
		ApplyMusicStemGroupVolume(stemGroupIt->second.get());
		return;
	}

	auto sequenceIt = musicSequences.find(slotId);
	if (sequenceIt != musicSequences.end())
	{
		sequenceIt->second->volume = volume;
		ApplyMusicSequenceVolume(sequenceIt->second.get());
		return;
	}

	if (slotId == 0)
	{
		ApplyMainMusicSlotVolume();
		return;
	}

	auto it = musicSlots.find(slotId);
	if (it == musicSlots.end()) return;

	it->second->volume = volume;
	ApplyMusicSlotVolume(it->second.get());
}

void S_SetMusicSlotVolume(int slotId, float volume)
{
	// A direct volume assignment supersedes a pending fade on this slot.
	musicSlotVolumeFades.erase(slotId);
	SetMusicSlotVolumeInternal(slotId, volume);
}

bool S_SetMusicSlotVolumeFade(int slotId, float targetVolume, float fadeTime)
{
	if (slotId < 0) return false;
	if (targetVolume < 0.f) targetVolume = 0.f;
	if (targetVolume > 1.f) targetVolume = 1.f;

	if (fadeTime <= 0.f)
	{
		S_SetMusicSlotVolume(slotId, targetVolume);
		return true;
	}

	float startVolume = 1.f;
	if (!GetMusicSlotLogicalVolume(slotId, startVolume)) return false;

	// Slot 0 has a persistent logical volume even when no song is active. For
	// additional slots, only start a timed fade while a playback object exists.
	if (slotId > 0 && musicSlots.find(slotId) == musicSlots.end() &&
		musicSequences.find(slotId) == musicSequences.end() &&
		musicStemGroups.find(slotId) == musicStemGroups.end() &&
		musicSlotCrossfades.find(slotId) == musicSlotCrossfades.end())
	{
		return false;
	}

	if (std::fabs(startVolume - targetVolume) <= 0.000001f)
	{
		musicSlotVolumeFades.erase(slotId);
		SetMusicSlotVolumeInternal(slotId, targetVolume);
		return true;
	}

	MusicSlotVolumeFade fade;
	fade.slotId = slotId;
	fade.startVolume = startVolume;
	fade.targetVolume = targetVolume;
	fade.durationSeconds = fadeTime;
	fade.startTime = std::chrono::steady_clock::now();
	musicSlotVolumeFades[slotId] = fade;
	return true;
}

static void S_UpdateMusicSlotVolumeFades()
{
	const float halfPi = 1.57079632679489661923f;

	for (auto it = musicSlotVolumeFades.begin(); it != musicSlotVolumeFades.end(); )
	{
		auto& fade = it->second;

		// Additional slots cease to have a meaningful volume once their playback
		// has ended. Slot 0 keeps its logical volume as a persistent main-music
		// control even if the current song has stopped.
		if (fade.slotId > 0 && musicSlots.find(fade.slotId) == musicSlots.end() &&
			musicSequences.find(fade.slotId) == musicSequences.end() &&
			musicStemGroups.find(fade.slotId) == musicStemGroups.end() &&
			musicSlotCrossfades.find(fade.slotId) == musicSlotCrossfades.end())
		{
			it = musicSlotVolumeFades.erase(it);
			continue;
		}

		auto now = std::chrono::steady_clock::now();
		double elapsed = std::chrono::duration<double>(now - fade.startTime).count();
		double raw = fade.durationSeconds > 0.0 ? elapsed / fade.durationSeconds : 1.0;
		if (raw < 0.0) raw = 0.0;
		if (raw > 1.0) raw = 1.0;
		float t = (float)raw;

		// Direction-aware equal-power-style curves make paired 1->0 and 0->1
		// fades suitable for crossfading two independent sequence slots.
		float shaped;
		if (fade.targetVolume >= fade.startVolume)
		{
			shaped = std::sin(t * halfPi);
		}
		else
		{
			shaped = 1.f - std::cos(t * halfPi);
		}

		float volume = fade.startVolume + (fade.targetVolume - fade.startVolume) * shaped;
		if (raw >= 1.0) volume = fade.targetVolume;
		SetMusicSlotVolumeInternal(fade.slotId, volume);

		if (raw >= 1.0)
		{
			it = musicSlotVolumeFades.erase(it);
		}
		else
		{
			++it;
		}
	}
}

bool S_IsMusicSlotPlaying(int slotId)
{
	auto stemGroupIt = musicStemGroups.find(slotId);
	if (stemGroupIt != musicStemGroups.end())
	{
		return !stemGroupIt->second->finished.load();
	}

	auto sequenceIt = musicSequences.find(slotId);
	if (sequenceIt != musicSequences.end())
	{
		return !sequenceIt->second->finished.load();
	}

	if (slotId == 0)
	{
		return mus_playing.handle != nullptr && ZMusic_IsPlaying(mus_playing.handle);
	}

	auto it = musicSlots.find(slotId);
	return it != musicSlots.end() && it->second->handle != nullptr && ZMusic_IsPlaying(it->second->handle);
}

void S_PauseMusicSlot(int slotId, bool paused)
{
	if (slotId == 0) mainMusicSlotPaused = paused;

	auto crossfadeIt = musicSlotCrossfades.find(slotId);
	if (crossfadeIt != musicSlotCrossfades.end())
	{
		auto crossfade = crossfadeIt->second.get();
		if (crossfade->outgoingSlot) crossfade->outgoingSlot->paused = paused;
		if (crossfade->outgoingSequence) crossfade->outgoingSequence->paused = paused;
		auto incomingIt = musicSequences.find(slotId);
		if (incomingIt != musicSequences.end() && incomingIt->second.get() == crossfade->incomingIdentity)
		{
			incomingIt->second->paused = paused;
			ApplyMusicSequencePause(incomingIt->second.get(), MusicPaused);
		}
		ApplyMusicSlotCrossfadeOutgoingPause(crossfade, MusicPaused);
		return;
	}

	auto stemGroupIt = musicStemGroups.find(slotId);
	if (stemGroupIt != musicStemGroups.end())
	{
		stemGroupIt->second->paused = paused;
		ApplyMusicStemGroupPause(stemGroupIt->second.get(), MusicPaused);
		return;
	}

	auto sequenceIt = musicSequences.find(slotId);
	if (sequenceIt != musicSequences.end())
	{
		sequenceIt->second->paused = paused;
		ApplyMusicSequencePause(sequenceIt->second.get(), MusicPaused);
		return;
	}

	if (slotId == 0)
	{
		if (mus_playing.handle == nullptr) return;
		ApplyMainMusicSlotPause(MusicPaused);
		return;
	}

	auto it = musicSlots.find(slotId);
	if (it == musicSlots.end()) return;

	it->second->paused = paused;
	ApplyMusicSlotPause(it->second.get(), MusicPaused);
}

void S_MusicSlotsVolumeChanged()
{
	// Slot 0 may be using a non-streaming backend, where its volume multiplier
	// must be re-applied after snd_musicvolume/relative-volume changes.
	if (mus_playing.handle != nullptr && !musicStream) ApplyMainMusicSlotVolume();

	for (auto& entry : musicSlotCrossfades)
	{
		ApplyMusicSlotCrossfadeGains(entry.second.get());
	}

	for (auto& entry : musicSlots)
	{
		if (entry.second->handle != nullptr && !entry.second->stream)
		{
			ZMusic_VolumeChanged(entry.second->handle);
		}
	}
}

static void S_UpdateMusicSlots()
{
	for (auto it = musicSlots.begin(); it != musicSlots.end(); )
	{
		auto slot = it->second.get();
		if (slot->handle != nullptr) ZMusic_Update(slot->handle);

		// A paused handle may report that it is not actively playing. Keep the
		// slot alive until it is resumed or explicitly stopped.
		if (slot->handle != nullptr && (MusicPaused || slot->paused))
		{
			++it;
			continue;
		}

		if (slot->handle == nullptr || !ZMusic_IsPlaying(slot->handle))
		{
			ShutdownMusicSlot(slot);
			it = musicSlots.erase(it);
		}
		else
		{
			++it;
		}
	}
}

SoundStream *S_CreateCustomStream(size_t size, int samplerate, int numchannels, MusicCustomStreamType sampletype, StreamCallback cb, void *userdata)
{
	int flags = 0;
	if (numchannels < 2) flags |= SoundStream::Mono;
	if (sampletype == MusicSamplesFloat) flags |= SoundStream::Float;
	auto stream = GSnd->CreateStream(cb, int(size), flags, samplerate, userdata);
	if (stream)
	{
		stream->Play(true, 1);
		customStreams.Push(stream);
	}
	return stream;
}

void S_StopCustomStream(SoundStream *stream)
{
	if (stream)
	{
		stream->Stop();
		auto f = customStreams.Find(stream);
		if (f < customStreams.Size()) customStreams.Delete(f);
		delete stream;
	}
}

void S_PauseAllCustomStreams(bool on)
{
	static bool paused = false;

	if (paused == on) return;
	paused = on;
	for (auto s : customStreams)
	{
		s->SetPaused(on);
	}
}

static TArray<int16_t> convert;
static bool FillStream(SoundStream* stream, void* buff, int len, void* userdata)
{
	bool written;
	if (mus_playing.isfloat)
	{
		written = ZMusic_FillStream(mus_playing.handle, buff, len);
		if (mus_playing.musicVolume != 1.f)
		{
			float* fbuf = (float*)buff;
			for (int i = 0; i < len / 4; i++)
			{
				fbuf[i] *= mus_playing.musicVolume;
			}
		}
	}
	else
	{
		// To apply replay gain we need floating point streaming data, so 16 bit input needs to be converted here.
		convert.Resize(len / 2);
		written = ZMusic_FillStream(mus_playing.handle, convert.Data(), len/2);
		float* fbuf = (float*)buff;
		for (int i = 0; i < len / 4; i++)
		{
			fbuf[i] = convert[i] * mus_playing.musicVolume * (1.f/32768.f);
		}
	}

	if (!written)
	{
		memset((char*)buff, 0, len);
		return false;
	}
	return true;
}


void S_CreateStream()
{
	if (!mus_playing.handle) return;
	SoundStreamInfo fmt;
	ZMusic_GetStreamInfo(mus_playing.handle, &fmt);
	// always create a floating point streaming buffer so we can apply replay gain without risk of integer overflows.
	mus_playing.isfloat = fmt.mNumChannels > 0;
	if (!mus_playing.isfloat) fmt.mBufferSize *= 2;
	if (fmt.mBufferSize > 0) // if buffer size is 0 the library will play the song itself (e.g. Windows system synth.)
	{
		int flags = SoundStream::Float;
		if (abs(fmt.mNumChannels) < 2) flags |= SoundStream::Mono;

		musicStream.reset(GSnd->CreateStream(FillStream, fmt.mBufferSize, flags, fmt.mSampleRate, nullptr));
		if (musicStream)
		{
			musicStream->Play(true, 1);
			musicStream->SetVolume(mainMusicSlotVolume);
		}
	}
	else
	{
		ApplyMainMusicSlotVolume();
	}
}


void S_PauseStream(bool paused)
{
	if (musicStream) musicStream->SetPaused(paused);
}

void S_StopStream()
{
	if (musicStream)
	{
		musicStream->Stop();
		musicStream.reset();
	}
}


//==========================================================================
//
// starts playing this song
//
//==========================================================================

static bool S_StartMusicPlaying(ZMusic_MusicStream song, bool loop, float rel_vol, int subsong)
{
	if (rel_vol > 0.f && !mus_usereplaygain)
	{
		float factor = relative_volume / saved_relative_volume;
		saved_relative_volume = rel_vol;
		I_SetRelativeVolume(saved_relative_volume * factor);
	}
	ZMusic_Stop(song);
	// make sure the volume modifiers update properly in case replay gain settings have changed.
	fluid_gain->Callback();
	mod_dumb_mastervolume->Callback();
	if (!ZMusic_Start(song, subsong, loop))
	{
		return false;
	}

	// Notify the sound system of the changed relative volume
	snd_musicvolume->Callback();
	return true;
}


//==========================================================================
//
// S_PauseMusic
//
// Stop music, during game PAUSE.
//==========================================================================

void S_PauseMusic ()
{
	if (!MusicPaused && (mus_playing.handle || !musicSlots.empty() || !musicSequences.empty() || !musicStemGroups.empty()))
	{
		if (mus_playing.handle) ApplyMainMusicSlotPause(true);
		for (auto& entry : musicSlots) ApplyMusicSlotPause(entry.second.get(), true);
		for (auto& entry : musicSequences) ApplyMusicSequencePause(entry.second.get(), true);
		for (auto& entry : musicStemGroups) ApplyMusicStemGroupPause(entry.second.get(), true);
		for (auto& entry : musicSlotCrossfades) ApplyMusicSlotCrossfadeOutgoingPause(entry.second.get(), true);
		MusicPaused = true;
	}
}

//==========================================================================
//
// S_ResumeMusic
//
// Resume music, after game PAUSE.
//==========================================================================

void S_ResumeMusic ()
{
	if (MusicPaused)
	{
		MusicPaused = false;
		if (mus_playing.handle) ApplyMainMusicSlotPause(false);
		for (auto& entry : musicSlots) ApplyMusicSlotPause(entry.second.get(), false);
		for (auto& entry : musicSequences) ApplyMusicSequencePause(entry.second.get(), false);
		for (auto& entry : musicStemGroups) ApplyMusicStemGroupPause(entry.second.get(), false);
		for (auto& entry : musicSlotCrossfades) ApplyMusicSlotCrossfadeOutgoingPause(entry.second.get(), false);
	}
}

//==========================================================================
//
// S_UpdateSound
//
//==========================================================================

void S_UpdateMusic ()
{
	S_UpdateMusicSlotVolumeFades();
	S_UpdateMusicSlotCrossfades();
	S_UpdateMusicSequences();
	S_UpdateMusicStemGroups();
	S_UpdateMusicSlots();

	if (mus_playing.handle != nullptr)
	{
		ZMusic_Update(mus_playing.handle);

		// [RH] Update music and/or playlist. IsPlaying() must be called
		// to attempt to reconnect to broken net streams and to advance the
		// playlist when the current song finishes.
		if (!MusicPaused && !mainMusicSlotPaused && !ZMusic_IsPlaying(mus_playing.handle))
		{
			auto crossfadeIt = musicSlotCrossfades.find(0);
			if (crossfadeIt != musicSlotCrossfades.end() && crossfadeIt->second->outgoingKind == MCF_LegacyMain)
			{
				StopLegacyMainMusicOnly(false);
				crossfadeIt->second->outgoingKind = MCF_None;
			}
			else if (PlayList.GetNumSongs())
			{
				PlayList.Advance();
				S_ActivatePlayList(false);
			}
			else
			{
				// A non-looping main/slot-0 song has completed naturally. Stop it
				// and forget the restart candidate so a later volume/CVAR change
				// cannot accidentally start the one-shot again.
				bool oneShotFinished = !mus_playing.loop;
				S_StopMusic(true);
				if (oneShotFinished) mus_playing.LastSong = "";
			}
		}
	}
}

//==========================================================================
//
// Resets the music player if music playback was paused.
//
//==========================================================================

void S_ResetMusic ()
{
	// Additional slots are transient game playback channels; do not let them
	// leak across a level reset/map transition. Slot 0 remains the legacy main
	// music and follows the original S_ResetMusic behavior below.
	S_StopAdditionalMusicSlots();

	// Slot 0 shares the legacy main music path, but its logical volume and
	// volume fade are persistent. Reset both on a level reset/map transition
	// so a fade from the previous map cannot leave the next map muted.
	S_SetMusicSlotVolume(0, 1.f);

	// stop the old music if it has been paused.
	// This ensures that the new music is started from the beginning
	// if it's the same as the last one and it has been paused.
	if (MusicPaused) S_StopMusic(true);

	// start new music for the level
	MusicPaused = false;
}


//==========================================================================
//
// S_ActivatePlayList
//
// Plays the next song in the playlist. If no songs in the playlist can be
// played, then it is deleted.
//==========================================================================

void S_ActivatePlayList (bool goBack)
{
	int startpos, pos;

	startpos = pos = PlayList.GetPosition ();
	S_StopMusic (true);
	while (!S_ChangeMusic (PlayList.GetSong (pos), 0, false, true))
	{
		pos = goBack ? PlayList.Backup () : PlayList.Advance ();
		if (pos == startpos)
		{
			PlayList.Clear();
			Printf ("Cannot play anything in the playlist.\n");
			return;
		}
	}
}

//==========================================================================
//
// S_StartMusic
//
// Starts some music with the given name.
//==========================================================================

bool S_StartMusic (const char *m_id)
{
	return S_ChangeMusic (m_id, 0, false);
}

//==========================================================================
//
// S_ChangeMusic
//
// initiates playback of a song
//
//==========================================================================
static TMap<FString, float> gainMap;

EXTERN_CVAR(String, fluid_patchset)
EXTERN_CVAR(String, timidity_config)
EXTERN_CVAR(String, midi_config)
EXTERN_CVAR(String, wildmidi_config)
EXTERN_CVAR(String, adl_custom_bank)
EXTERN_CVAR(Int, adl_bank)
EXTERN_CVAR(Bool, adl_use_custom_bank)
EXTERN_CVAR(String, opn_custom_bank)
EXTERN_CVAR(Bool, opn_use_custom_bank)
EXTERN_CVAR(Int, opl_core)

static FString ReplayGainHash(ZMusicCustomReader* reader, int flength, int playertype, const char* _playparam)
{
	std::string playparam = _playparam;

	TArray<uint8_t> buffer(50000, true);	// for performance reasons only hash the start of the file. If we wanted to do this to large waveform songs it'd cause noticable lag.
	uint8_t digest[16];
	char digestout[33];
	auto length = reader->read(reader, buffer.data(), 50000);
	reader->seek(reader, 0, SEEK_SET);
	MD5Context md5;
	md5.Init();
	md5.Update(buffer.data(), (int)length);
	md5.Final(digest);

	for (size_t j = 0; j < sizeof(digest); ++j)
	{
		snprintf(digestout + (j * 2), 3, "%02X", digest[j]);
	}
	digestout[32] = 0;

	auto type = ZMusic_IdentifyMIDIType((uint32_t*)buffer.data(), 32);
	if (type == MIDI_NOTMIDI) return FStringf("%d:%s", flength, digestout);

	// get the default for MIDI synth
	if (playertype == -1)
	{
		switch (snd_mididevice)
		{
		case -1:		playertype = MDEV_FLUIDSYNTH; break;
		case -2:		playertype = MDEV_TIMIDITY; break;
		case -3:		playertype = MDEV_OPL; break;
		case -4:		playertype = MDEV_GUS; break;
		case -5:		playertype = MDEV_FLUIDSYNTH; break;
		case -6:		playertype = MDEV_WILDMIDI; break;
		case -7:		playertype = MDEV_ADL; break;
		case -8:		playertype = MDEV_OPN; break;
		default:		return "";
		}
	}
	else if (playertype == MDEV_SNDSYS) return "";

	// get the default for used sound font.
	if (playparam.empty())
	{
		switch (playertype)
		{
		case MDEV_FLUIDSYNTH:		playparam = fluid_patchset; break;
		case MDEV_TIMIDITY:			playparam = timidity_config; break;
		case MDEV_GUS:				playparam = midi_config; break;
		case MDEV_WILDMIDI:			playparam = wildmidi_config; break;
		case MDEV_ADL:				playparam = adl_use_custom_bank ? *adl_custom_bank : std::to_string(adl_bank); break;
		case MDEV_OPN:				playparam = opn_use_custom_bank ? *opn_custom_bank : ""; break;
		case MDEV_OPL:				playparam = std::to_string(opl_core); break;

		}
	}
	return FStringf("%d:%s:%d:%s", flength, digestout, playertype, playparam.c_str()).MakeUpper();
}

static void SaveGains()
{
	auto path = M_GetAppDataPath(true);
	path << "/replaygain.ini";
	FConfigFile gains(path.GetChars());
	TMap<FString, float>::Iterator it(gainMap);
	TMap<FString, float>::Pair* pair;

	if (gains.SetSection("Gains", true))
	{
		while (it.NextPair(pair))
		{
			gains.SetValueForKey(pair->Key.GetChars(), std::to_string(pair->Value).c_str());
		}
	}
	gains.WriteConfigFile();
}

static void ReadGains()
{
	static bool done = false;
	if (done) return;
	done = true;
	auto path = M_GetAppDataPath(true);
	path << "/replaygain.ini";
	FConfigFile gains(path.GetChars());
	if (gains.SetSection("Gains"))
	{
		const char* key;
		const char* value;

		while (gains.NextInSection(key, value))
		{
			gainMap.Insert(key, (float)strtod(value, nullptr));
		}
	}
}

CCMD(setreplaygain)
{
	// sets replay gain for current song to a fixed value
	if (!mus_playing.handle || mus_playing.hash.IsEmpty())
	{
		Printf("setreplaygain needs some music playing\n");
		return;
	}
	if (argv.argc() < 2)
	{
		Printf("Usage: setreplaygain {dB}\n");
		Printf("Current replay gain is %f dB\n", AmplitudeTodB(mus_playing.musicVolume));
		return;
	}
	float dB = (float)strtod(argv[1], nullptr);
	if (dB > 10) dB = 10; // don't blast the speakers. Values above 2 or 3 are very rare.
	gainMap.Insert(mus_playing.hash, dB);
	SaveGains();
	mus_playing.musicVolume = (float)dBToAmplitude(dB);
}

static void CheckReplayGain(const char *musicname, EMidiDevice playertype, const char *playparam)
{
	mus_playing.musicVolume = 1;
	fluid_gain->Callback();
	mod_dumb_mastervolume->Callback();
	if (!mus_usereplaygain) return;

	FileReader reader = OpenMusic(musicname);
	if (!reader.isOpen()) return;
	int flength = (int)reader.GetLength();
	auto mreader = GetMusicReader(reader);	// this passes the file reader to the newly created wrapper.

	ReadGains();
	auto hash = ReplayGainHash(mreader, flength, playertype, playparam);
	if (hash.IsEmpty()) return; // got nothing to measure.
	mus_playing.hash = hash;
	auto entry = gainMap.CheckKey(hash);
	if (entry)
	{
		mus_playing.musicVolume = dBToAmplitude(*entry);
		return;
	}
	if (!mus_calcgain) return;

	auto handle = ZMusic_OpenSong(mreader, playertype, playparam);
	if (handle == nullptr) return; // not a music file

	if (!ZMusic_Start(handle, 0, false))
	{
		ZMusic_Close(handle);
		return; // unable to open
	}

	SoundStreamInfo fmt;
	ZMusic_GetStreamInfo(handle, &fmt);
	if (fmt.mBufferSize == 0)
	{
		ZMusic_Close(handle);
		return; // external player.
	}

	int flags = SoundStream::Float;
	if (abs(fmt.mNumChannels) < 2) flags |= SoundStream::Mono;

	TArray<uint8_t> readbuffer(fmt.mBufferSize, true);
	TArray<float> lbuffer;
	TArray<float> rbuffer;
	while (ZMusic_FillStream(handle, readbuffer.Data(), fmt.mBufferSize))
	{
		unsigned index;
		// 4 cases, all with different preparation needs.
		if (fmt.mNumChannels == -2) // 16 bit stereo
		{
			int16_t* sbuf = (int16_t*)readbuffer.Data();
			int numsamples = fmt.mBufferSize / 4;
			index = lbuffer.Reserve(numsamples);
			rbuffer.Reserve(numsamples);

			for (int i = 0; i < numsamples; i++)
			{
				lbuffer[index + i] = sbuf[i * 2];
				rbuffer[index + i] = sbuf[i * 2 + 1];
			}
		}
		else if (fmt.mNumChannels == -1) // 16 bit mono
		{
			int16_t* sbuf = (int16_t*)readbuffer.Data();
			int numsamples = fmt.mBufferSize / 2;
			index = lbuffer.Reserve(numsamples);

			for (int i = 0; i < numsamples; i++)
			{
				lbuffer[index + i] = sbuf[i];
			}
		}
		else if (fmt.mNumChannels == 1) // float mono
		{
			float* sbuf = (float*)readbuffer.Data();
			int numsamples = fmt.mBufferSize / 4;
			index = lbuffer.Reserve(numsamples);
			for (int i = 0; i < numsamples; i++)
			{
				lbuffer[index + i] = sbuf[i] * 32768.f;
			}
		}
		else if (fmt.mNumChannels == 2) // float stereo
		{
			float* sbuf = (float*)readbuffer.Data();
			int numsamples = fmt.mBufferSize / 8;
			auto addr = lbuffer.Reserve(numsamples);
			rbuffer.Reserve(numsamples);

			for (int i = 0; i < numsamples; i++)
			{
				lbuffer[addr + i] = sbuf[i * 2] * 32768.f;
				rbuffer[addr + i] = sbuf[i * 2 + 1] * 32768.f;
			}
		}
		float accTime = lbuffer.Size() / (float)fmt.mSampleRate;
		if (accTime > 8 * 60) break; // do at most 8 minutes, if the song forces a loop.
	}
	ZMusic_Close(handle);

	auto analyzer = std::make_unique<GainAnalyzer>();
	int result = analyzer->InitGainAnalysis(fmt.mSampleRate);
	if (result == GAIN_ANALYSIS_OK)
	{
		result = analyzer->AnalyzeSamples(lbuffer.Data(), rbuffer.Size() == 0 ? nullptr : rbuffer.Data(), lbuffer.Size(), rbuffer.Size() == 0 ? 1 : 2);
		if (result == GAIN_ANALYSIS_OK)
		{
			auto gain = analyzer->GetTitleGain();
			Printf("Calculated replay gain for %s (%s) at %f dB\n", musicname, hash.GetChars(), gain);

			gainMap.Insert(hash, gain);
			mus_playing.musicVolume = dBToAmplitude(gain);
			SaveGains();
		}
	}
}

bool S_ChangeMusic(const char* musicname, int order, bool looping, bool force)
{
	if (!MusicEnabled()) return false;	// skip the entire procedure if music is globally disabled.

	if (!force && PlayList.GetNumSongs())
	{ // Don't change if a playlist is active
		return true; // do not report an error here.
	}
	// Do game specific lookup.
	FString musicname_;
	if (mus_cb.LookupFileName)
	{
		musicname_ = mus_cb.LookupFileName(musicname, order);
		musicname = musicname_.GetChars();
	}

	if (musicname == nullptr || musicname[0] == 0)
	{
		// Don't choke if the map doesn't have a song attached
		S_StopMusic (true);
		mus_playing.name = "";
		mus_playing.LastSong = "";
		return true;
	}

	if (!mus_playing.name.IsEmpty() &&
		mus_playing.handle != nullptr &&
		mus_playing.name.CompareNoCase(musicname) == 0 &&
		ZMusic_IsLooping(mus_playing.handle) == zmusic_bool(looping))
	{
		if (order != mus_playing.baseorder)
		{
			if (ZMusic_SetSubsong(mus_playing.handle, order))
			{
				mus_playing.baseorder = order;
			}
		}
		else if (!ZMusic_IsPlaying(mus_playing.handle))
		{
			if (!ZMusic_Start(mus_playing.handle, order, looping))
			{
				Printf("Unable to start %s: %s\n", mus_playing.name.GetChars(), ZMusic_GetLastError());
			}
			S_CreateStream();
			ApplyMainMusicSlotVolume();
			ApplyMainMusicSlotPause(MusicPaused);

		}
		return true;
	}

	ZMusic_MusicStream handle = nullptr;

	// Strip off any leading file:// component.
	if (strncmp(musicname, "file://", 7) == 0)
	{
		musicname += 7;
	}

	// opening the music must be done by the game because it's different depending on the game's file system use.
	FileReader reader = OpenMusic(musicname);
	if (!reader.isOpen()) return false;
	auto m = reader.Read();
	reader.Seek(0, FileReader::SeekSet);

	// shutdown old music
	S_StopMusic(true);

	// Just record it if volume is 0 or music was disabled
	if (snd_musicvolume <= 0 || !mus_enabled)
	{
		mus_playing.loop = looping;
		mus_playing.name = musicname;
		mus_playing.baseorder = order;
		mus_playing.LastSong = musicname;
		return true;
	}

	// load & register it
	if (handle != nullptr)
	{
		mus_playing.handle = handle;
	}
	else
	{
		int lumpnum = mus_cb.FindMusic(musicname);
		MidiDeviceSetting* devp = MidiDevices.CheckKey(lumpnum);
		int* mplay = ModPlayers.CheckKey(lumpnum);

		auto volp = MusicVolumes.CheckKey(lumpnum);
		if (volp)
		{
			mus_playing.musicVolume = *volp;

		}
		else
		{
			CheckReplayGain(musicname, devp ? (EMidiDevice)devp->device : MDEV_DEFAULT, devp ? devp->args.GetChars() : "");
		}
		auto mreader = GetMusicReader(reader);	// this passes the file reader to the newly created wrapper.
		int mod_player = mplay? *mplay : *mod_preferred_player;
		int scratch;

		// This config var is only effective when opening a music stream so there's no need for active synchronization. Setting it here is sufficient.
		// Ideally this should have been a parameter to ZMusic_OpenSong, but that would have necessitated an API break.
		ChangeMusicSettingInt(zmusic_mod_preferredplayer, mus_playing.handle, mod_player, &scratch);
		mus_playing.handle = ZMusic_OpenSong(mreader, devp ? (EMidiDevice)devp->device : MDEV_DEFAULT, devp ? devp->args.GetChars() : "");
		if (mus_playing.handle == nullptr)
		{
			Printf("Unable to load %s: %s\n", mus_playing.name.GetChars(), ZMusic_GetLastError());
		}
	}

	mus_playing.loop = looping;
	mus_playing.name = musicname;
	mus_playing.baseorder = 0;
	mus_playing.LastSong = "";

	if (mus_playing.handle != 0)
	{ // play it
		if (!S_StartMusicPlaying(mus_playing.handle, looping, 1.f, order))
		{
			Printf("Unable to start %s: %s\n", mus_playing.name.GetChars(), ZMusic_GetLastError());
			return false;
		}

		S_CreateStream();
		mus_playing.baseorder = order;
		ApplyMainMusicSlotVolume();
		ApplyMainMusicSlotPause(MusicPaused);
		return true;
	}
	return false;
}

//==========================================================================
//
// S_RestartMusic
//
//==========================================================================

void S_RestartMusic ()
{
	if (S_IsMusicSequencePlaying(0)) return;
	if (snd_musicvolume <= 0) return;
	if (!mus_playing.LastSong.IsEmpty() && mus_enabled)
	{
		FString song = mus_playing.LastSong;
		mus_playing.LastSong = "";
		S_ChangeMusic (song.GetChars(), mus_playing.baseorder, mus_playing.loop, true);
	}
	else
	{
		S_StopMusic(true);
	}
}

//==========================================================================
//
// S_MIDIDeviceChanged
//
//==========================================================================


void S_MIDIDeviceChanged(int newdev)
{
	auto song = mus_playing.handle;
	if (song != nullptr && ZMusic_IsMIDI(song) && ZMusic_IsPlaying(song))
	{
		// Reload the song to change the device
		auto mi = mus_playing;
		S_StopMusic(true);
		S_ChangeMusic(mi.name.GetChars(), mi.baseorder, mi.loop);
	}
}

//==========================================================================
//
// S_GetMusic
//
//==========================================================================

int S_GetMusic (const char **name)
{
	int order;

	if (mus_playing.name.IsNotEmpty())
	{
		*name = mus_playing.name.GetChars();
		order = mus_playing.baseorder;
	}
	else
	{
		*name = nullptr;
		order = 0;
	}
	return order;
}

//==========================================================================
//
// S_StopMusic
//
//==========================================================================

static void StopLegacyMainMusicOnly(bool rememberLastSong)
{
	try
	{
		if (mus_playing.handle != nullptr)
		{
			if (MusicPaused || mainMusicSlotPaused)
			{
				ZMusic_Resume(mus_playing.handle);
				if (musicStream) musicStream->SetPaused(false);
			}
			S_StopStream();
			ZMusic_Stop(mus_playing.handle);
			auto h = mus_playing.handle;
			mus_playing.handle = nullptr;
			ZMusic_Close(h);
		}

		if (rememberLastSong) mus_playing.LastSong = std::move(mus_playing.name);
		else
		{
			mus_playing.name = "";
			mus_playing.LastSong = "";
		}
	}
	catch (const std::runtime_error&)
	{
		if (mus_playing.handle != nullptr)
		{
			auto h = mus_playing.handle;
			mus_playing.handle = nullptr;
			ZMusic_Close(h);
		}
		mus_playing.name = "";
		if (!rememberLastSong) mus_playing.LastSong = "";
	}
}

void S_StopMusic (bool force)
{
	if (force || PlayList.GetNumSongs() == 0) StopMusicSlotCrossfadeInternal(0);

	// Slot 0 sequences share the legacy main-music slot logically, so any
	// SetMusic/S_ChangeMusic stop or replacement must also stop the sequence.
	// Preserve the playlist exception used by the legacy main-music path.
	if (force || PlayList.GetNumSongs() == 0) StopMusicSequenceInternal(0);

	// [RH] Don't stop if a playlist is active.
	if ((force || PlayList.GetNumSongs() == 0) && !mus_playing.name.IsEmpty())
	{
		StopLegacyMainMusicOnly(true);
	}
}

//==========================================================================
//
// CCMD changemus
//
//==========================================================================

CCMD (changemus)
{
	if (MusicEnabled())
	{
		if (argv.argc() > 1)
		{
			PlayList.Clear();
			S_ChangeMusic (argv[1], argv.argc() > 2 ? atoi (argv[2]) : 0);
		}
		else
		{
			const char *currentmus = mus_playing.name.GetChars();
			if(currentmus != nullptr && *currentmus != 0)
			{
				Printf ("currently playing %s\n", currentmus);
			}
			else
			{
				Printf ("no music playing\n");
			}
		}
	}
	else
	{
		Printf("Music is disabled\n");
	}
}

//==========================================================================
//
// CCMD stopmus
//
//==========================================================================

CCMD (stopmus)
{
	PlayList.Clear();
	S_StopMusic (false);
	mus_playing.LastSong = "";	// forget the last played song so that it won't get restarted if some volume changes occur
}

//==========================================================================
//
// CCMD playlist
//
//==========================================================================

UNSAFE_CCMD (playlist)
{
	int argc = argv.argc();

	if (argc < 2 || argc > 3)
	{
		Printf ("playlist <playlist.m3u> [<position>|shuffle]\n");
	}
	else
	{
		if (!PlayList.ChangeList(argv[1]))
		{
			Printf("Could not open " TEXTCOLOR_BOLD "%s" TEXTCOLOR_NORMAL ": %s\n", argv[1], strerror(errno));
			return;
		}
		if (PlayList.GetNumSongs () > 0)
		{
			if (argc == 3)
			{
				if (stricmp (argv[2], "shuffle") == 0)
				{
					PlayList.Shuffle ();
				}
				else
				{
					PlayList.SetPosition (atoi (argv[2]));
				}
			}
			S_ActivatePlayList (false);
		}
	}
}

//==========================================================================
//
// CCMD playlistpos
//
//==========================================================================

static bool CheckForPlaylist ()
{
	if (PlayList.GetNumSongs() == 0)
	{
		Printf ("No playlist is playing.\n");
		return false;
	}
	return true;
}

CCMD (playlistpos)
{
	if (CheckForPlaylist() && argv.argc() > 1)
	{
		PlayList.SetPosition (atoi (argv[1]) - 1);
		S_ActivatePlayList (false);
	}
}

//==========================================================================
//
// CCMD playlistnext
//
//==========================================================================

CCMD (playlistnext)
{
	if (CheckForPlaylist())
	{
		PlayList.Advance ();
		S_ActivatePlayList (false);
	}
}

//==========================================================================
//
// CCMD playlistprev
//
//==========================================================================

CCMD (playlistprev)
{
	if (CheckForPlaylist())
	{
		PlayList.Backup ();
		S_ActivatePlayList (true);
	}
}

//==========================================================================
//
// CCMD playliststatus
//
//==========================================================================

CCMD (playliststatus)
{
	if (CheckForPlaylist ())
	{
		Printf ("Song %d of %d:\n%s\n",
			PlayList.GetPosition () + 1,
			PlayList.GetNumSongs (),
			PlayList.GetSong (PlayList.GetPosition ()));
	}
}

//==========================================================================
//
// 
//
//==========================================================================

CCMD(currentmusic)
{
	if (mus_playing.name.IsNotEmpty())
	{
		Printf("Currently playing music '%s'\n", mus_playing.name.GetChars());
	}
	else
	{
		Printf("Currently no music playing\n");
	}
}
