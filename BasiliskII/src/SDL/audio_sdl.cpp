/*
 *  audio_sdl.cpp - Audio support, SDL implementation
 *
 *  Basilisk II (C) 1997-2008 Christian Bauer
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "sysdeps.h"

#include "my_sdl.h"
#if !SDL_VERSION_ATLEAST(3, 0, 0)

#include "cpu_emulation.h"
#include "main.h"
#include "prefs.h"
#include "user_strings.h"
#include "audio.h"
#include "audio_defs.h"

#define DEBUG 0
#include "debug.h"

#if defined(BINCUE)
#include "bincue.h"
#endif

#include <cstring>
#include <algorithm>

#define MAC_MAX_VOLUME 0x0100

// The currently selected audio parameters (indices in audio_sample_rates[] etc. vectors)
static int audio_sample_rate_index = 0;
static int audio_sample_size_index = 0;
static int audio_channel_count_index = 0;

// Global variables
static uint8 silence_byte;							// Byte value to use to fill sound buffers with silence
static int main_volume = MAC_MAX_VOLUME;
static int speaker_volume = MAC_MAX_VOLUME;
static bool main_mute = false;
static bool speaker_mute = false;

// --- Lock-free ring buffer ---
// Written by the emulation thread (AudioInterrupt), read by the SDL callback.
// Single-producer, single-consumer — no locks needed.
static uint8 *ring_buffer = NULL;
static uint32 ring_write_pos = 0;	// Written by emulator, read by callback
static uint32 ring_read_pos = 0;	// Written by callback, read by emulator
static uint32 ring_size = 0;				// Total size in bytes (power of 2)
static uint32 ring_mask = 0;				// ring_size - 1
static int audio_block_size = 0;			// Size of one audio block in bytes

// How many bytes are available to read from the ring
static inline uint32 ring_readable() {
	const uint32 wp = __atomic_load_n(&ring_write_pos, __ATOMIC_ACQUIRE);
	const uint32 rp = __atomic_load_n(&ring_read_pos, __ATOMIC_ACQUIRE);
	return (wp - rp) & ring_mask;
}

// How many bytes of free space in the ring
static inline uint32 ring_writable() {
	return ring_size - 1 - ring_readable();
}

// Write data into the ring buffer
static inline void ring_write(const uint8 *data, uint32 len) {
	uint32 wp = __atomic_load_n(&ring_write_pos, __ATOMIC_RELAXED);
	for (uint32 i = 0; i < len; i++) {
		ring_buffer[wp] = data[i];
		wp = (wp + 1) & ring_mask;
	}
	// Memory barrier: ensure data is written before advancing write_pos
	__atomic_store_n(&ring_write_pos, wp, __ATOMIC_RELEASE);
}

// Read data from the ring buffer
static inline void ring_read(uint8 *data, uint32 len) {
	uint32 rp = __atomic_load_n(&ring_read_pos, __ATOMIC_RELAXED);
	for (uint32 i = 0; i < len; i++) {
		data[i] = ring_buffer[rp];
		rp = (rp + 1) & ring_mask;
	}
	// Memory barrier: ensure data is read before advancing read_pos
	__atomic_store_n(&ring_read_pos, rp, __ATOMIC_RELEASE);
}

// Prototypes
static void stream_func(void *arg, uint8 *stream, int stream_len);
static int get_audio_volume();


/*
 *  Initialization
 */

// Set AudioStatus to reflect current audio stream format
static void set_audio_status_format(void)
{
	AudioStatus.sample_rate = audio_sample_rates[audio_sample_rate_index];
	AudioStatus.sample_size = audio_sample_sizes[audio_sample_size_index];
	AudioStatus.channels = audio_channel_counts[audio_channel_count_index];
}

// Init SDL audio system
static bool open_sdl_audio(void)
{
	// SDL supports a variety of twisted little audio formats, all different
	if (audio_sample_sizes.empty()) {
		audio_sample_rates.push_back(11025 << 16);
		audio_sample_rates.push_back(22050 << 16);
		audio_sample_rates.push_back(44100 << 16);
		audio_sample_sizes.push_back(8);
		audio_sample_sizes.push_back(16);
		audio_channel_counts.push_back(1);
		audio_channel_counts.push_back(2);

		// Default to highest supported values
		audio_sample_rate_index = audio_sample_rates.size() - 1;
		audio_sample_size_index = audio_sample_sizes.size() - 1;
		audio_channel_count_index = audio_channel_counts.size() - 1;
	}

	SDL_AudioSpec audio_spec;
	memset(&audio_spec, 0, sizeof(audio_spec));
	audio_spec.freq = audio_sample_rates[audio_sample_rate_index] >> 16;
	audio_spec.format = (audio_sample_sizes[audio_sample_size_index] == 8) ? AUDIO_U8 : AUDIO_S16MSB;
	audio_spec.channels = audio_channel_counts[audio_channel_count_index];
	audio_spec.samples = 2048 >> PrefsFindInt32("sound_buffer");
	audio_spec.callback = stream_func;
	audio_spec.userdata = NULL;

	// Open the audio device, forcing the desired format
	if (SDL_OpenAudio(&audio_spec, NULL) < 0) {
		fprintf(stderr, "WARNING: Cannot open audio: %s\n", SDL_GetError());
		return false;
	}
	
#if SDL_VERSION_ATLEAST(2,0,0)
	// HACK: workaround a bug in SDL pre-2.0.6 (reported via https://bugzilla.libsdl.org/show_bug.cgi?id=3710 )
	// whereby SDL does not update audio_spec.size
	if (audio_spec.size == 0) {
		audio_spec.size = (SDL_AUDIO_BITSIZE(audio_spec.format) / 8) * audio_spec.channels * audio_spec.samples;
	}
#endif

#if defined(BINCUE)
	OpenAudio_bincue(audio_spec.freq, audio_spec.format, audio_spec.channels,
	audio_spec.silence, get_audio_volume());
#endif

#if SDL_VERSION_ATLEAST(2,0,0)
	const char * driver_name = SDL_GetCurrentAudioDriver();
#else
	char driver_name[32];
	SDL_AudioDriverName(driver_name, sizeof(driver_name) - 1);
#endif
	printf("Using SDL/%s audio output\n", driver_name ? driver_name : "");
	silence_byte = audio_spec.silence;
	SDL_PauseAudio(0);

	// Sound buffer size
	audio_frames_per_block = audio_spec.samples;
	audio_block_size = audio_spec.size;

	// Allocate ring buffer: 4× the block size, rounded up to power of 2
	uint32 desired = audio_block_size * 4;
	ring_size = 1;
	while (ring_size < desired) ring_size <<= 1;
	ring_mask = ring_size - 1;
	ring_buffer = (uint8 *)calloc(ring_size, 1);
	ring_write_pos = 0;
	ring_read_pos = 0;

	printf("Audio: %d Hz, %d-bit, %d ch, %d frames/block, ring %u bytes\n",
		   audio_spec.freq, audio_sample_sizes[audio_sample_size_index],
		   audio_spec.channels, audio_frames_per_block, ring_size);

	return true;
}

static bool open_audio(void)
{
	// Try to open SDL audio
	if (!open_sdl_audio()) {
		WarningAlert(GetString(STR_NO_AUDIO_WARN));
		return false;
	}

	// Device opened, set AudioStatus
	set_audio_status_format();

	// Everything went fine
	audio_open = true;
	return true;
}

void AudioInit(void)
{
	// Init audio status and feature flags
	AudioStatus.sample_rate = 44100 << 16;
	AudioStatus.sample_size = 16;
	AudioStatus.channels = 2;
	AudioStatus.mixer = 0;
	AudioStatus.num_sources = 0;
	audio_component_flags = cmpWantsRegisterMessage | kStereoOut | k16BitOut;

	// Sound disabled in prefs? Then do nothing
	if (PrefsFindBool("nosound"))
		return;

#ifdef BINCUE
	InitBinCue();
#endif
	// Open and initialize audio device
	open_audio();
}


/*
 *  Deinitialization
 */

static void close_audio(void)
{
	// Close audio device
#if defined(BINCUE)
	CloseAudio_bincue();
#endif
	SDL_CloseAudio();
	free(ring_buffer);
	ring_buffer = NULL;
	ring_size = 0;
	ring_mask = 0;
	ring_write_pos = 0;
	ring_read_pos = 0;
	audio_open = false;
}

void AudioExit(void)
{
	// Close audio device
	close_audio();
#ifdef BINCUE
	ExitBinCue();
#endif
}


/*
 *  First source added, start audio stream
 */

void audio_enter_stream()
{
}


/*
 *  Last source removed, stop audio stream
 */

void audio_exit_stream()
{
}


/*
 *  Streaming function — SDL audio callback (real-time thread)
 *  Reads pre-filled data from the lock-free ring buffer.
 *  Never blocks. Signals the emulator to produce more data asynchronously.
 */

static void stream_func(void *arg, uint8 *stream, int stream_len)
{
	if (!AudioStatus.num_sources || main_mute || speaker_mute) {
		memset(stream, silence_byte, stream_len);
#if defined(BINCUE)
		MixAudio_bincue(stream, stream_len);
#endif
		return;
	}

	uint32 available = ring_readable();
	uint32 to_read = std::min(available, (uint32)stream_len);

	if (to_read > 0) {
		// Read from ring buffer and apply volume via SDL_MixAudio
		int vol = get_audio_volume();
		memset(stream, silence_byte, stream_len);
		if (vol == SDL_MIX_MAXVOLUME && to_read == (uint32)stream_len) {
			// Fast path: full volume, full buffer — direct copy
			ring_read(stream, to_read);
		} else {
			// Need volume scaling — read into a fixed stack chunk and mix incrementally
			uint8 temp_stack[4096];
			uint32 remaining = to_read;
			uint32 offset = 0;
			while (remaining > 0) {
				uint32 chunk = std::min(remaining, (uint32)sizeof(temp_stack));
				ring_read(temp_stack, chunk);
				SDL_MixAudio(stream + offset, temp_stack, chunk, vol);
				offset += chunk;
				remaining -= chunk;
			}
		}
	} else {
		// Ring empty — underrun, play silence
		memset(stream, silence_byte, stream_len);
	}

	// Request more audio data from the emulator (non-blocking)
	SetInterruptFlag(INTFLAG_AUDIO);
	TriggerInterrupt();

#if defined(BINCUE)
	MixAudio_bincue(stream, stream_len);
#endif
}


/*
 *  MacOS audio interrupt, read next data block
 *  Called on the emulation thread — fills the ring buffer with new audio data
 */

void AudioInterrupt(void)
{
	D(bug("AudioInterrupt\n"));

	if (!ring_buffer || !AudioStatus.num_sources)
		return;

	// Get data from apple mixer
	if (AudioStatus.mixer) {
		M68kRegisters r;
		r.a[0] = audio_data + adatStreamInfo;
		r.a[1] = AudioStatus.mixer;
		Execute68k(audio_data + adatGetSourceData, &r);
		D(bug(" GetSourceData() returns %08lx\n", r.d[0]));
	} else {
		WriteMacInt32(audio_data + adatStreamInfo, 0);
	}

	// Read the stream info and copy data into the ring buffer
	uint32 apple_stream_info = ReadMacInt32(audio_data + adatStreamInfo);
	if (!apple_stream_info)
		return;

	int work_size = ReadMacInt32(apple_stream_info + scd_sampleCount) * (AudioStatus.sample_size >> 3) * AudioStatus.channels;
	if (work_size <= 0)
		return;

	// Cap to available ring space
	uint32 space = ring_writable();
	if ((uint32)work_size > space)
		work_size = space;
	if (work_size <= 0)
		return;

	uint8 *src = Mac2HostAddr(ReadMacInt32(apple_stream_info + scd_buffer));

	// Handle mono-to-stereo doubling for 8-bit sources
	bool dbl = AudioStatus.channels == 2 &&
		ReadMacInt16(apple_stream_info + scd_numChannels) == 1 &&
		ReadMacInt16(apple_stream_info + scd_sampleSize) == 8;

	if (dbl) {
		// Expand mono to stereo inline and write
		uint8 temp[8192];
		int src_size = work_size / 2;
		for (int chunk_start = 0; chunk_start < src_size; ) {
			int chunk = std::min(src_size - chunk_start, (int)sizeof(temp) / 2);
			for (int i = 0; i < chunk; i++) {
				temp[i * 2] = temp[i * 2 + 1] = src[chunk_start + i];
			}
			ring_write(temp, chunk * 2);
			chunk_start += chunk;
		}
	} else {
		ring_write(src, work_size);
	}

	D(bug("AudioInterrupt: wrote %d bytes to ring (readable=%u)\n", work_size, ring_readable()));
}


/*
 *  Set sampling parameters
 *  "index" is an index into the audio_sample_rates[] etc. vectors
 *  It is guaranteed that AudioStatus.num_sources == 0
 */

bool audio_set_sample_rate(int index)
{
	close_audio();
	audio_sample_rate_index = index;
	return open_audio();
}

bool audio_set_sample_size(int index)
{
	close_audio();
	audio_sample_size_index = index;
	return open_audio();
}

bool audio_set_channels(int index)
{
	close_audio();
	audio_channel_count_index = index;
	return open_audio();
}


/*
 *  Get/set volume controls (volume values received/returned have the left channel
 *  volume in the upper 16 bits and the right channel volume in the lower 16 bits;
 *  both volumes are 8.8 fixed point values with 0x0100 meaning "maximum volume"))
 */

bool audio_get_main_mute(void)
{
	return main_mute;
}

uint32 audio_get_main_volume(void)
{
	uint32 chan = main_volume;
	return (chan << 16) + chan;
}

bool audio_get_speaker_mute(void)
{
	return speaker_mute;
}

uint32 audio_get_speaker_volume(void)
{
	uint32 chan = speaker_volume;
	return (chan << 16) + chan;
}

void audio_set_main_mute(bool mute)
{
	main_mute = mute;
}

void audio_set_main_volume(uint32 vol)
{
	// We only have one-channel volume right now.
	main_volume = ((vol >> 16) + (vol & 0xffff)) / 2;
	if (main_volume > MAC_MAX_VOLUME)
		main_volume = MAC_MAX_VOLUME;
}

void audio_set_speaker_mute(bool mute)
{
	speaker_mute = mute;
}

void audio_set_speaker_volume(uint32 vol)
{
	// We only have one-channel volume right now.
	speaker_volume = ((vol >> 16) + (vol & 0xffff)) / 2;
	if (speaker_volume > MAC_MAX_VOLUME)
		speaker_volume = MAC_MAX_VOLUME;
}

static int get_audio_volume() {
	return main_volume * speaker_volume * SDL_MIX_MAXVOLUME / (MAC_MAX_VOLUME * MAC_MAX_VOLUME);
}

#if SDL_VERSION_ATLEAST(2,0,0)
static int play_startup(void *arg) {
	SDL_AudioSpec wav_spec;
	Uint8 *wav_buffer;
	Uint32 wav_length;
	if (SDL_LoadWAV("startup.wav", &wav_spec, &wav_buffer, &wav_length)) {
		SDL_AudioSpec obtained;
		SDL_AudioDeviceID deviceId = SDL_OpenAudioDevice(NULL, 0, &wav_spec, &obtained, 0);
		if (deviceId) {
			SDL_QueueAudio(deviceId, wav_buffer, wav_length);
			SDL_PauseAudioDevice(deviceId, 0);
			while (SDL_GetQueuedAudioSize(deviceId)) SDL_Delay(10);
			SDL_Delay(500);
			SDL_CloseAudioDevice(deviceId);
		}
		else printf("play_startup: Audio driver failed to initialize\n");
		SDL_FreeWAV(wav_buffer);
	}
	return 0;
}

void PlayStartupSound() {
	SDL_CreateThread(play_startup, "", NULL);
}
#else
void PlayStartupSound() {
    // Not implemented
}
#endif
#endif	// SDL_VERSION_ATLEAST

