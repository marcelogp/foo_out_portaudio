#include "stdafx.h"
#include "../SDK/component.h"
#include "portaudio.h"

#include <windows.h>
#include <mmsystem.h>

DECLARE_COMPONENT_VERSION(
"PortAudio output support",
"0.0.1",
"foo_out_portaudio\n"
"\n"
"Output integration with PortAudio library.\n"
"(c) 2018 Marcelo Povoa\n"
);

VALIDATE_COMPONENT_FILENAME("foo_out_portaudio.dll");

// {B9C134FF-FD31-40AD-80FD-E1913D1A126E}
static const GUID portaudio_GUID =
{ 0xb9c134FF, 0xfd31, 0x40ad,{ 0x80, 0xfd, 0xe1, 0x91, 0x3d, 0x1a, 0x12, 0x6e } };

// {B9C13400-FD31-40AD-80FD-E1913D1A126E}
static const GUID portaudio_device_GUID =
{ 0xb9c13400, 0xfd31, 0x40ad,{ 0x80, 0xfd, 0xe1, 0x91, 0x3d, 0x1a, 0x12, 0x6e } };

#define MAX_BUFFER_SIZE	2097152
#define MAX_DEVICES 128
#define PA_LATENCY 1024

static PaStream *stream;
static char deviceName[MAX_DEVICES][256];
static float buffer[MAX_BUFFER_SIZE];
static int bufferWritePos;
static int bufferReadPos;
static boolean bufferEnded;
static boolean initialized;
static float volume;
static unsigned int srate, channels;

void resetBuffer() {
	bufferWritePos = 1;
	bufferReadPos = 0;
}

void initializePa() {
	if (!initialized) {
		initialized = true;
		Pa_Initialize();
	}
}

static int paCallback(const void *inputBuffer,
	void *outputBuffer,
	unsigned long framesPerBuffer,
	const PaStreamCallbackTimeInfo* timeInfo,
	PaStreamCallbackFlags statusFlags,
	void *userData)
{
	float *out = (float*)outputBuffer;
	int nextBufferReadPos;
	(void)inputBuffer; /* Prevent unused variable warning. */

	for (int i = 0; i < channels * framesPerBuffer; i++) {
		nextBufferReadPos = (bufferReadPos + 1) % MAX_BUFFER_SIZE;
		if (nextBufferReadPos == bufferWritePos) {
			*out++ = 0.0;
		}
		else {
			// PortAudio does not have volume API, so use software volume instead.
			if (volume < 1.0)
				*out++ = buffer[bufferReadPos] * volume;
			else
				*out++ = buffer[bufferReadPos];
			bufferReadPos = nextBufferReadPos;
		}
	}
	// Cannot return paAbort here otherwise the stream cannot be resumed anymore. 
	if (nextBufferReadPos == bufferWritePos)
		bufferEnded = true;
	return paContinue;
}

class portaudio : public output {
private:
	double buffer_length;
	t_uint32 buffer_samples;
	t_uint32 bitdepth;
	double vol;
	unsigned int outDeviceNum;

	PaError checkError(PaError err) {
		if (err != paNoError)
			console::printf(COMPONENT_NAME " PortAudio error: %s\n", Pa_GetErrorText(err));
		return err;
	}
public:

	portaudio(const GUID & p_device, double p_buffer_length, bool p_dither, t_uint32 p_bitdepth) :
		buffer_length(p_buffer_length), bitdepth(p_bitdepth) {
		outDeviceNum = p_device.Data1 % 0x100;
		srate = channels = 0;
		buffer_samples = MAX_BUFFER_SIZE / 2;
		volume = 1.0;
		stream = NULL;
		resetBuffer();
		initializePa();
	}

	~portaudio() {
		if (stream)
			checkError(Pa_AbortStream(stream));
		initialized = 0;
		checkError(Pa_Terminate());
	}

	void resumeStream() {
		if (stream && Pa_IsStreamStopped(stream) == 1)
			checkError(Pa_StartStream(stream));
	}

	//! Retrieves amount of audio data queued for playback, in seconds.
	double get_latency() {
		if (bufferEnded) {
			checkError(Pa_AbortStream(stream));
			bufferEnded = 0;
		}
		int samples = (bufferWritePos - bufferReadPos + MAX_BUFFER_SIZE) % MAX_BUFFER_SIZE;
		//console::printf(COMPONENT_NAME " latency %d", samples);
		if (samples <= 1)
			return 0;
		return (float)samples / (channels * srate);
	}
	//! Sends new samples to the device. Allowed to be called only when update() indicates that the device is ready.
	void process_samples(const audio_chunk & p_chunk) {
		//console::printf(COMPONENT_NAME " process_samples");
		unsigned int new_srate = p_chunk.get_srate();
		unsigned int new_channels = p_chunk.get_channels();

		if (srate != new_srate || channels != new_channels || !stream) {
			srate = new_srate;
			channels = new_channels;
			buffer_samples = min(srate * channels * buffer_length, MAX_BUFFER_SIZE - srate * channels);
			if (stream) {
				Pa_CloseStream(stream);
				resetBuffer();
			}
			PaStreamParameters outputParameters;
			outputParameters.channelCount = channels;
			outputParameters.device = outDeviceNum;
			outputParameters.sampleFormat = paFloat32;
			outputParameters.suggestedLatency = Pa_GetDeviceInfo(outDeviceNum)->defaultLowOutputLatency;
			outputParameters.hostApiSpecificStreamInfo = NULL;
			checkError(Pa_OpenStream(&stream, NULL, &outputParameters, srate, PA_LATENCY, paNoFlag, paCallback, NULL));
		}
		unsigned int len = p_chunk.get_used_size();
		for (unsigned int i = 0; i < len; ++i) {
			// Stereo channels should be written in reverse order.
			buffer[bufferWritePos ^ 1] = p_chunk.get_data()[i];
			bufferWritePos = (bufferWritePos + 1) % MAX_BUFFER_SIZE;
		}
		resumeStream();
	}

	//! Updates playback; queries whether	the device is ready to receive new data.
	//! @param p_ready On success, receives value indicating whether the device is ready for next process_samples() call.
	void update(bool & p_ready) {
		int buffer_used = (bufferWritePos - bufferReadPos + MAX_BUFFER_SIZE) % MAX_BUFFER_SIZE;
		//console::printf(COMPONENT_NAME " update %d", buffer_used);
		p_ready = (buffer_used < buffer_samples);
	}
	//! Pauses/unpauses playback.
	void pause(bool p_state) {
		//console::printf(COMPONENT_NAME " pause");
		if (!stream || !initialized)
			return;
		if (p_state) checkError(Pa_AbortStream(stream));
		else resumeStream();
	}
	//! Flushes queued audio data. Called after seeking.
	void flush() {
		//console::printf(COMPONENT_NAME " flush");
		if (stream)
			checkError(Pa_AbortStream(stream));
		resetBuffer();
	}
	//! Forces playback of queued data. Called when there's no more data to send, to prevent infinite waiting if output implementation starts actually playing after amount of data in internal buffer reaches some level.
	void force_play() {
		resumeStream();
		//console::printf(COMPONENT_NAME " force_play");
	}

	//! Sets playback volume.
	//! @p_val Volume level in dB. Value of 0 indicates full ("100%") volume, negative values indciate different attenuation levels.
	void volume_set(double p_val) {
		volume = pow(10.0, p_val / 20.0);
	}

	static void g_enum_devices(output_device_enum_callback & p_callback) {
		initializePa();
		int numDevices = min(Pa_GetDeviceCount(), MAX_DEVICES);
		for (int i = 0; i < numDevices; ++i) {
			const PaDeviceInfo* deviceInfo = Pa_GetDeviceInfo(i);
			if (deviceInfo->maxOutputChannels <= 0)
				continue;

			GUID device_guid = portaudio_device_GUID;
			device_guid.Data1 += i;
			const char* hostApiName = Pa_GetHostApiInfo(deviceInfo->hostApi)->name;
			snprintf(deviceName[i], sizeof(deviceName[i]), "%s (%s)", deviceInfo->name, hostApiName);
			p_callback.on_device(device_guid, deviceName[i], strlen(deviceName[i]));
		}
	}

	static GUID g_get_guid() {
		return portaudio_GUID;
	}

	static const char * g_get_name() {
		return "PA";
	}

	static void g_advanced_settings_popup(HWND p_parent, POINT p_menupoint) {
	}

	static bool g_advanced_settings_query() {
		return false;
	}

	static bool g_needs_bitdepth_config() {
		return false;
	}
	static bool g_needs_dither_config() {
		return false;
	}
	static bool g_needs_device_list_prefixes() {
		return false;
	}

};

static output_factory_t<portaudio> g_portaudio;
