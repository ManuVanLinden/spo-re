/*
 * sp_coreaudio.c - CoreAudio backend for the fake DirectSound buffer.
 *
 * One default-output AudioUnit (kAudioUnitSubType_DefaultOutput, i.e. AUHAL following the system
 * default device) per playing secondary buffer. The unit's input stream format is set to the
 * buffer's own PCM format, so the render callback copies straight out of the ring; the unit's
 * converter resamples to the device rate (48 kHz on the Mac measured).
 *
 * Timing: the render callback's AudioTimeStamp.mHostTime is the host time of the I/O cycle that
 * will output this block. We add the device's presentation latency (kAudioDevicePropertyLatency +
 * kAudioDevicePropertySafetyOffset + the stream's kAudioStreamPropertyLatency, in device frames)
 * and the unit's own kAudioUnitProperty_Latency, so the play cursor tracks what is heard.
 */
#include "sp_dsound.h"

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct ca {
    AudioUnit au;
    sp_dsbuf *b;
    uint64_t latency_ns;
    mach_timebase_info_data_t tb;
} ca;

static uint64_t mach_to_ns(const ca *c, uint64_t t) { return t * c->tb.numer / c->tb.denom; }

static OSStatus render_cb(void *ref, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts,
                          UInt32 bus, UInt32 nframes, AudioBufferList *io)
{
    (void)flags; (void)bus;
    ca *c = ref;
    uint64_t host_ns = (ts->mFlags & kAudioTimeStampHostTimeValid) ? mach_to_ns(c, ts->mHostTime)
                                                                   : sp_host_now_ns();
    sp_dsbuf_render(c->b, io->mBuffers[0].mData, nframes, host_ns + c->latency_ns);
    return noErr;
}

/* Device-side presentation latency of the default output device, in nanoseconds. */
uint64_t sp_coreaudio_output_latency_ns(double *rate_out, uint32_t *frames_out)
{
    AudioObjectID dev = 0;
    UInt32 sz = sizeof dev;
    AudioObjectPropertyAddress a = {kAudioHardwarePropertyDefaultOutputDevice,
                                    kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &sz, &dev) != noErr || !dev) return 0;
    Float64 rate = 0; sz = sizeof rate;
    a = (AudioObjectPropertyAddress){kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal,
                                     kAudioObjectPropertyElementMain};
    AudioObjectGetPropertyData(dev, &a, 0, NULL, &sz, &rate);
    UInt32 lat = 0, safety = 0, slat = 0;
    sz = sizeof lat;
    a = (AudioObjectPropertyAddress){kAudioDevicePropertyLatency, kAudioObjectPropertyScopeOutput,
                                     kAudioObjectPropertyElementMain};
    AudioObjectGetPropertyData(dev, &a, 0, NULL, &sz, &lat);
    sz = sizeof safety;
    a.mSelector = kAudioDevicePropertySafetyOffset;
    AudioObjectGetPropertyData(dev, &a, 0, NULL, &sz, &safety);
    AudioStreamID streams[8]; sz = sizeof streams;
    a.mSelector = kAudioDevicePropertyStreams;
    if (AudioObjectGetPropertyData(dev, &a, 0, NULL, &sz, streams) == noErr && sz >= sizeof(AudioStreamID)) {
        AudioObjectPropertyAddress sa = {kAudioStreamPropertyLatency, kAudioObjectPropertyScopeGlobal,
                                         kAudioObjectPropertyElementMain};
        UInt32 ssz = sizeof slat;
        AudioObjectGetPropertyData(streams[0], &sa, 0, NULL, &ssz, &slat);
    }
    uint32_t frames = lat + safety + slat;
    if (rate_out) *rate_out = rate;
    if (frames_out) *frames_out = frames;
    return rate > 0 ? (uint64_t)((double)frames * 1e9 / rate) : 0;
}

/* Current device sample time and the host time it corresponds to (for drift checks). */
int sp_coreaudio_device_time(double *sample_time, uint64_t *host_ns)
{
    AudioObjectID dev = 0;
    UInt32 sz = sizeof dev;
    AudioObjectPropertyAddress a = {kAudioHardwarePropertyDefaultOutputDevice,
                                    kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &sz, &dev) != noErr) return 0;
    AudioTimeStamp ts = {0};
    if (AudioDeviceGetCurrentTime(dev, &ts) != noErr) return 0;
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    *sample_time = ts.mSampleTime;
    *host_ns = ts.mHostTime * tb.numer / tb.denom;
    return 1;
}

static int ca_open(sp_dsbuf *b, const sp_waveformatex *f, void **handle)
{
    ca *c = calloc(1, sizeof *c);
    if (!c) return -1;
    c->b = b;
    mach_timebase_info(&c->tb);
    AudioComponentDescription desc = {kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput,
                                      kAudioUnitManufacturer_Apple, 0, 0};
    AudioComponent comp = AudioComponentFindNext(NULL, &desc);
    if (!comp || AudioComponentInstanceNew(comp, &c->au) != noErr) { free(c); return -1; }
    AudioStreamBasicDescription asbd = {0};
    asbd.mSampleRate = f->nSamplesPerSec;
    asbd.mFormatID = kAudioFormatLinearPCM;
    asbd.mFormatFlags = kAudioFormatFlagIsPacked | (f->wBitsPerSample > 8 ? kAudioFormatFlagIsSignedInteger : 0);
    asbd.mChannelsPerFrame = f->nChannels;
    asbd.mBitsPerChannel = f->wBitsPerSample;
    asbd.mBytesPerFrame = f->nBlockAlign;
    asbd.mFramesPerPacket = 1;
    asbd.mBytesPerPacket = f->nBlockAlign;
    OSStatus st = AudioUnitSetProperty(c->au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0,
                                       &asbd, sizeof asbd);
    AURenderCallbackStruct cb = {render_cb, c};
    if (st == noErr)
        st = AudioUnitSetProperty(c->au, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0,
                                  &cb, sizeof cb);
    if (st == noErr) st = AudioUnitInitialize(c->au);
    if (st != noErr) {
        fprintf(stderr, "sp_coreaudio: AudioUnit setup failed (%d)\n", (int)st);
        AudioComponentInstanceDispose(c->au);
        free(c);
        return -1;
    }
    Float64 au_lat = 0; UInt32 sz = sizeof au_lat;
    AudioUnitGetProperty(c->au, kAudioUnitProperty_Latency, kAudioUnitScope_Global, 0, &au_lat, &sz);
    c->latency_ns = sp_coreaudio_output_latency_ns(NULL, NULL) + (uint64_t)(au_lat * 1e9);
    *handle = c;
    return 0;
}

static int ca_start(void *h) { return AudioOutputUnitStart(((ca *)h)->au) == noErr ? 0 : -1; }
static int ca_stop(void *h) { return AudioOutputUnitStop(((ca *)h)->au) == noErr ? 0 : -1; }
static void ca_close(void *h)
{
    ca *c = h;
    AudioOutputUnitStop(c->au);
    AudioUnitUninitialize(c->au);
    AudioComponentInstanceDispose(c->au);
    free(c);
}
static int ca_set_volume(void *h, float gain)
{
    return AudioUnitSetParameter(((ca *)h)->au, kHALOutputParam_Volume, kAudioUnitScope_Global, 0, gain, 0) == noErr ? 0 : -1;
}

uint64_t sp_coreaudio_handle_latency_ns(void *h) { return ((ca *)h)->latency_ns; }

static const sp_audio_backend g_ca = {ca_open, ca_start, ca_stop, ca_close, ca_set_volume};
const sp_audio_backend *sp_coreaudio_backend(void) { return &g_ca; }
