#include "MusicDevice.h"
#include <stdlib.h>
#include <string.h>

//------------------------------------------------------------------------------
// Dummy MIDI player

static int NullMidiInit(MusicDevice *dev, const unsigned int outputIndex, unsigned samplerate)
{
    if (!dev || dev->isOpen) return 0;

    dev->isOpen = 1;
    dev->outputIndex = 0;

    // suppress compiler warnings
    (void)outputIndex;
    (void)samplerate;

    return 0;
}

static void NullMidiDestroy(MusicDevice *dev)
{
    if (!dev) return;

    free(dev);
}

static void NullMidiSetupMode(MusicDevice *dev, MusicMode mode)
{
    // suppress compiler warnings
    (void)dev;
    (void)mode;
}

static void NullMidiReset(MusicDevice *dev)
{
    // suppress compiler warnings
    (void)dev;
}

static void NullMidiGenerate(MusicDevice *dev, short *samples, int numframes)
{
    memset(samples, 0, 2 * (unsigned int)numframes * sizeof(short));

    // suppress compiler warnings
    (void)dev;
}

static void NullMidiSendNoteOff(MusicDevice *dev, int channel, int note, int vel)
{
    // suppress compiler warnings
    (void)dev;
    (void)channel;
    (void)note;
    (void)vel;
}

static void NullMidiSendNoteOn(MusicDevice *dev, int channel, int note, int vel)
{
    // suppress compiler warnings
    (void)dev;
    (void)channel;
    (void)note;
    (void)vel;
}

static void NullMidiSendNoteAfterTouch(MusicDevice *dev, int channel, int note, int touch)
{
    // suppress compiler warnings
    (void)dev;
    (void)channel;
    (void)note;
    (void)touch;
}

static void NullMidiSendControllerChange(MusicDevice *dev, int channel, int ctl, int val)
{
    // suppress compiler warnings
    (void)dev;
    (void)channel;
    (void)ctl;
    (void)val;
}

static void NullMidiSendProgramChange(MusicDevice *dev, int channel, int pgm)
{
    // suppress compiler warnings
    (void)dev;
    (void)channel;
    (void)pgm;
}

static void NullMidiSendChannelAfterTouch(MusicDevice *dev, int channel, int touch)
{
    // suppress compiler warnings
    (void)dev;
    (void)channel;
    (void)touch;
}

static void NullMidiSendPitchBendML(MusicDevice *dev, int channel, int msb, int lsb)
{
    // suppress compiler warnings
    (void)dev;
    (void)channel;
    (void)msb;
    (void)lsb;
}

static unsigned int NullMidiGetOutputCount(MusicDevice *dev)
{
    // suppress compiler warnings
    (void)dev;

    return 1;
}

static void NullMidiGetOutputName(MusicDevice *dev, const unsigned int outputIndex, char *buffer, const unsigned int bufferSize)
{
    if (!buffer || bufferSize < 1) return;
    // save last position for NULL character
    strncpy(buffer, "NullMidi", bufferSize - 1);
    // put NULL in last position in case we filled up everything else
    *(buffer + bufferSize - 1) = '\0';

    // suppress compiler warnings
    (void)dev;
    (void)outputIndex;
}

static MusicDevice *createNullMidiDevice()
{
    MusicDevice *dev = malloc(sizeof(MusicDevice));
    dev->init = &NullMidiInit;
    dev->destroy = &NullMidiDestroy;
    dev->setupMode = &NullMidiSetupMode;
    dev->reset = &NullMidiReset;
    dev->generate = &NullMidiGenerate;
    dev->sendNoteOff = &NullMidiSendNoteOff;
    dev->sendNoteOn = &NullMidiSendNoteOn;
    dev->sendNoteAfterTouch = &NullMidiSendNoteAfterTouch;
    dev->sendControllerChange = &NullMidiSendControllerChange;
    dev->sendProgramChange = &NullMidiSendProgramChange;
    dev->sendChannelAfterTouch = &NullMidiSendChannelAfterTouch;
    dev->sendPitchBendML = &NullMidiSendPitchBendML;
    dev->getOutputCount = &NullMidiGetOutputCount;
    dev->getOutputName = &NullMidiGetOutputName;
    dev->isOpen = 0;
    dev->outputIndex = 0;
    dev->deviceType = Music_None;
    dev->musicType = MUSICTYPE_SBLASTER;
    return dev;
}

//------------------------------------------------------------------------------
// ADLMIDI player for OPL3

#include "adlmidi.h"

typedef struct AdlMidiDevice
{
    MusicDevice dev;
    struct ADL_MIDIPlayer *adl;
    MusicOpl3Emu emu;
} AdlMidiDevice;

static int AdlMidiEmulatorId(MusicOpl3Emu emu)
{
    return (emu == Music_Opl3DosBox) ? ADLMIDI_EMU_DOSBOX : ADLMIDI_EMU_NUKED_174;
}

// DOSBox only matches the real chip at its native rate: run at the PCM rate
// it plays about 7 cents sharp and feedback-heavy instruments lose their
// movement. Nuked 1.7.4 can't run at the PCM rate and ignores the setting.
static int AdlMidiRunAtPcmRate(MusicOpl3Emu emu)
{
    return (emu == Music_Opl3DosBox) ? 0 : 1;
}

void AdlMidiSetEmulator(MusicDevice *dev, MusicOpl3Emu emu)
{
    AdlMidiDevice *adev = (AdlMidiDevice *)dev;
    if (!adev || adev->dev.deviceType != Music_AdlMidi || adev->emu == emu) return;

    adev->emu = emu;
    if (adev->dev.isOpen)
    {
        adl_switchEmulator(adev->adl, AdlMidiEmulatorId(emu));
        adl_setRunAtPcmRate(adev->adl, AdlMidiRunAtPcmRate(emu));
    }
}

int AdlMidiGetEmulator(MusicDevice *dev)
{
    if (!dev || dev->deviceType != Music_AdlMidi) return -1;
    return ((AdlMidiDevice *)dev)->emu;
}

static int AdlMidiInit(MusicDevice *dev, const unsigned int outputIndex, unsigned samplerate)
{
    AdlMidiDevice *adev = (AdlMidiDevice *)dev;
    if (!adev || adev->dev.isOpen) return 0;
    struct ADL_MIDIPlayer *adl = adl_init(samplerate);

    adl_switchEmulator(adl, AdlMidiEmulatorId(adev->emu));
    adl_setNumChips(adl, 1);
    adl_setVolumeRangeModel(adl, ADLMIDI_VolumeModel_AUTO);
    adl_setRunAtPcmRate(adl, AdlMidiRunAtPcmRate(adev->emu));

    adev->adl = adl;

    adev->dev.isOpen = 1;
    adev->dev.outputIndex = outputIndex;

    return 0;
}

static void AdlMidiDestroy(MusicDevice *dev)
{
    AdlMidiDevice *adev = (AdlMidiDevice *)dev;
    if (!adev) return;
    if (adev->dev.isOpen)
    {
        adl_close(adev->adl);
    }
    free(adev);
}

static void AdlMidiSetupMode(MusicDevice *dev, MusicMode mode)
{
    AdlMidiDevice *adev = (AdlMidiDevice *)dev;
    if (!adev || !adev->dev.isOpen) return;

    //Use sound bank 45 for res/sound/sblaster, 0 for res/sound/genmidi
    adl_setBank(adev->adl, (mode == Music_SoundBlaster) ? 45 : 0);
}

static void AdlMidiReset(MusicDevice *dev)
{
    AdlMidiDevice *adev = (AdlMidiDevice *)dev;
    if (!adev || !adev->dev.isOpen) return;

    adl_reset(adev->adl);
}

static void AdlMidiGenerate(MusicDevice *dev, short *samples, int numframes)
{
    AdlMidiDevice *adev = (AdlMidiDevice *)dev;
    if (!adev || !adev->dev.isOpen) return;

    const int numSamples = numframes * 2;
    adl_generate(adev->adl, numSamples, samples);
    // ugly hack: libadlmidi has quiet output, so double all values
    short *sample = samples;
    for (int i = 0; i < numSamples; ++i, ++sample)
    {
        *sample *= 2;
    }
}

static void AdlMidiSendNoteOff(MusicDevice *dev, int channel, int note, int vel)
{
    AdlMidiDevice *adev = (AdlMidiDevice *)dev;
    if (!adev || !adev->dev.isOpen) return;

    adl_rt_noteOff(adev->adl, channel, note);
    (void)vel;
}

static void AdlMidiSendNoteOn(MusicDevice *dev, int channel, int note, int vel)
{
    AdlMidiDevice *adev = (AdlMidiDevice *)dev;
    if (!adev || !adev->dev.isOpen) return;

    adl_rt_noteOn(adev->adl, channel, note, vel);
}

static void AdlMidiSendNoteAfterTouch(MusicDevice *dev, int channel, int note, int touch)
{
    AdlMidiDevice *adev = (AdlMidiDevice *)dev;
    if (!adev || !adev->dev.isOpen) return;

    adl_rt_noteAfterTouch(adev->adl, channel, note, touch);
}

static void AdlMidiSendControllerChange(MusicDevice *dev, int channel, int ctl, int val)
{
    AdlMidiDevice *adev = (AdlMidiDevice *)dev;
    if (!adev || !adev->dev.isOpen) return;

    adl_rt_controllerChange(adev->adl, channel, ctl, val);
}

static void AdlMidiSendProgramChange(MusicDevice *dev, int channel, int pgm)
{
    AdlMidiDevice *adev = (AdlMidiDevice *)dev;
    if (!adev || !adev->dev.isOpen) return;

    adl_rt_patchChange(adev->adl, channel, pgm);
}

static void AdlMidiSendChannelAfterTouch(MusicDevice *dev, int channel, int touch)
{
    AdlMidiDevice *adev = (AdlMidiDevice *)dev;
    if (!adev || !adev->dev.isOpen) return;

    adl_rt_channelAfterTouch(adev->adl, channel, touch);
}

static void AdlMidiSendPitchBendML(MusicDevice *dev, int channel, int msb, int lsb)
{
    AdlMidiDevice *adev = (AdlMidiDevice *)dev;
    if (!adev || !adev->dev.isOpen) return;

    adl_rt_pitchBendML(adev->adl, channel, msb, lsb);
}

static unsigned int AdlMidiGetOutputCount(MusicDevice *dev)
{
    // suppress compiler warnings
    (void)dev;

    // TODO: add support for SB and GM modes?
    return 1;
}

static void AdlMidiGetOutputName(MusicDevice *dev, const unsigned int outputIndex, char *buffer, const unsigned int bufferSize)
{
    if (!buffer || bufferSize < 1) return;
    // save last position for NULL character
    strncpy(buffer, "AdlMidi", bufferSize - 1);
    // put NULL in last position in case we filled up everything else
    *(buffer + bufferSize - 1) = '\0';

    // suppress compiler warnings
    (void)dev;
    (void)outputIndex;
}

static MusicDevice *createAdlMidiDevice()
{
    AdlMidiDevice *adev = malloc(sizeof(AdlMidiDevice));
    adev->dev.init = &AdlMidiInit;
    adev->dev.destroy = &AdlMidiDestroy;
    adev->dev.setupMode = &AdlMidiSetupMode;
    adev->dev.reset = &AdlMidiReset;
    adev->dev.generate = &AdlMidiGenerate;
    adev->dev.sendNoteOff = &AdlMidiSendNoteOff;
    adev->dev.sendNoteOn = &AdlMidiSendNoteOn;
    adev->dev.sendNoteAfterTouch = &AdlMidiSendNoteAfterTouch;
    adev->dev.sendControllerChange = &AdlMidiSendControllerChange;
    adev->dev.sendProgramChange = &AdlMidiSendProgramChange;
    adev->dev.sendChannelAfterTouch = &AdlMidiSendChannelAfterTouch;
    adev->dev.sendPitchBendML = &AdlMidiSendPitchBendML;
    adev->dev.getOutputCount = &AdlMidiGetOutputCount;
    adev->dev.getOutputName = &AdlMidiGetOutputName;
    adev->dev.isOpen = 0;
    adev->dev.outputIndex = 0;
    adev->dev.deviceType = Music_AdlMidi;
    adev->dev.musicType = MUSICTYPE_SBLASTER;
    // Nuked keeps a Vita core about 45% busy; DOSBox sounds very close.
    adev->emu = Music_Opl3DosBox;
    return &adev->dev;
}

//------------------------------------------------------------------------------
MusicDevice *CreateMusicDevice(MusicType type)
{
    MusicDevice *dev = 0;

    switch (type)
    {
    case Music_None:
        dev = createNullMidiDevice();
        break;
    case Music_AdlMidi:
        dev = createAdlMidiDevice();
        break;
    }

    return dev;
}
