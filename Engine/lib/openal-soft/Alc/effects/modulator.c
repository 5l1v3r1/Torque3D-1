/**
 * OpenAL cross platform audio library
 * Copyright (C) 2009 by Chris Robinson.
 * This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Library General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 *  License along with this library; if not, write to the
 *  Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 * Or go to http://www.gnu.org/copyleft/lgpl.html
 */

#include "config.h"

#include <math.h>
#include <stdlib.h>

#include "alMain.h"
#include "alAuxEffectSlot.h"
#include "alError.h"
#include "alu.h"
#include "filters/defs.h"


#define MAX_UPDATE_SAMPLES 128

typedef struct ALmodulatorState {
    DERIVE_FROM_TYPE(ALeffectState);

    void (*GetSamples)(ALfloat*, ALsizei, const ALsizei, ALsizei);

    ALsizei index;
    ALsizei step;

    alignas(16) ALfloat ModSamples[MAX_UPDATE_SAMPLES];

    struct {
        BiquadFilter Filter;

        ALfloat CurrentGains[MAX_OUTPUT_CHANNELS];
        ALfloat TargetGains[MAX_OUTPUT_CHANNELS];
    } Chans[MAX_EFFECT_CHANNELS];
} ALmodulatorState;

static ALvoid ALmodulatorState_Destruct(ALmodulatorState *state);
static ALboolean ALmodulatorState_deviceUpdate(ALmodulatorState *state, ALCdevice *device);
static ALvoid ALmodulatorState_update(ALmodulatorState *state, const ALCcontext *context, const ALeffectslot *slot, const ALeffectProps *props);
static ALvoid ALmodulatorState_process(ALmodulatorState *state, ALsizei SamplesToDo, const ALfloat (*restrict SamplesIn)[BUFFERSIZE], ALfloat (*restrict SamplesOut)[BUFFERSIZE], ALsizei NumChannels);
DECLARE_DEFAULT_ALLOCATORS(ALmodulatorState)

DEFINE_ALEFFECTSTATE_VTABLE(ALmodulatorState);


#define WAVEFORM_FRACBITS  24
#define WAVEFORM_FRACONE   (1<<WAVEFORM_FRACBITS)
#define WAVEFORM_FRACMASK  (WAVEFORM_FRACONE-1)

static inline ALfloat Sin(ALsizei index)
{
    return sinf(index*(F_TAU/WAVEFORM_FRACONE) - F_PI)*0.5f + 0.5f;
}

static inline ALfloat Saw(ALsizei index)
{
    return (ALfloat)index / WAVEFORM_FRACONE;
}

static inline ALfloat Square(ALsizei index)
{
    return (ALfloat)((index >> (WAVEFORM_FRACBITS - 1)) & 1);
}

#define DECL_TEMPLATE(func)                                                   \
static void Modulate##func(ALfloat *restrict dst, ALsizei index,              \
                           const ALsizei step, ALsizei todo)                  \
{                                                                             \
    ALsizei i;                                                                \
    for(i = 0;i < todo;i++)                                                   \
    {                                                                         \
        index += step;                                                        \
        index &= WAVEFORM_FRACMASK;                                           \
        dst[i] = func(index);                                                 \
    }                                                                         \
}

DECL_TEMPLATE(Sin)
DECL_TEMPLATE(Saw)
DECL_TEMPLATE(Square)

#undef DECL_TEMPLATE


static void ALmodulatorState_Construct(ALmodulatorState *state)
{
    ALeffectState_Construct(STATIC_CAST(ALeffectState, state));
    SET_VTABLE2(ALmodulatorState, ALeffectState, state);

    state->index = 0;
    state->step = 1;
}

static ALvoid ALmodulatorState_Destruct(ALmodulatorState *state)
{
    ALeffectState_Destruct(STATIC_CAST(ALeffectState,state));
}

static ALboolean ALmodulatorState_deviceUpdate(ALmodulatorState *state, ALCdevice *UNUSED(device))
{
    ALsizei i, j;
    for(i = 0;i < MAX_EFFECT_CHANNELS;i++)
    {
        BiquadFilter_clear(&state->Chans[i].Filter);
        for(j = 0;j < MAX_OUTPUT_CHANNELS;j++)
            state->Chans[i].CurrentGains[j] = 0.0f;
    }
    return AL_TRUE;
}

static ALvoid ALmodulatorState_update(ALmodulatorState *state, const ALCcontext *context, const ALeffectslot *slot, const ALeffectProps *props)
{
    const ALCdevice *device = context->Device;
    ALfloat cw, a;
    ALsizei i;

    if(props->Modulator.Waveform == AL_RING_MODULATOR_SINUSOID)
        state->GetSamples = ModulateSin;
    else if(props->Modulator.Waveform == AL_RING_MODULATOR_SAWTOOTH)
        state->GetSamples = ModulateSaw;
    else /*if(Slot->Params.EffectProps.Modulator.Waveform == AL_RING_MODULATOR_SQUARE)*/
        state->GetSamples = ModulateSquare;

    state->step = float2int(props->Modulator.Frequency*WAVEFORM_FRACONE/device->Frequency + 0.5f);
    state->step = clampi(state->step, 1, WAVEFORM_FRACONE-1);

    /* Custom filter coeffs, which match the old version instead of a low-shelf. */
    cw = cosf(F_TAU * props->Modulator.HighPassCutoff / device->Frequency);
    a = (2.0f-cw) - sqrtf(powf(2.0f-cw, 2.0f) - 1.0f);

    state->Chans[0].Filter.b0 = a;
    state->Chans[0].Filter.b1 = -a;
    state->Chans[0].Filter.b2 = 0.0f;
    state->Chans[0].Filter.a1 = -a;
    state->Chans[0].Filter.a2 = 0.0f;
    for(i = 1;i < MAX_EFFECT_CHANNELS;i++)
        BiquadFilter_copyParams(&state->Chans[i].Filter, &state->Chans[0].Filter);

    STATIC_CAST(ALeffectState,state)->OutBuffer = device->FOAOut.Buffer;
    STATIC_CAST(ALeffectState,state)->OutChannels = device->FOAOut.NumChannels;
    for(i = 0;i < MAX_EFFECT_CHANNELS;i++)
        ComputeFirstOrderGains(&device->FOAOut, IdentityMatrixf.m[i],
                               slot->Params.Gain, state->Chans[i].TargetGains);
}

static ALvoid ALmodulatorState_process(ALmodulatorState *state, ALsizei SamplesToDo, const ALfloat (*restrict SamplesIn)[BUFFERSIZE], ALfloat (*restrict SamplesOut)[BUFFERSIZE], ALsizei NumChannels)
{
    ALfloat *restrict modsamples = ASSUME_ALIGNED(state->ModSamples, 16);
    const ALsizei step = state->step;
    ALsizei base;

    for(base = 0;base < SamplesToDo;)
    {
        alignas(16) ALfloat temps[2][MAX_UPDATE_SAMPLES];
        ALsizei td = mini(MAX_UPDATE_SAMPLES, SamplesToDo-base);
        ALsizei c, i;

        state->GetSamples(modsamples, state->index, step, td);
        state->index += (step*td) & WAVEFORM_FRACMASK;
        state->index &= WAVEFORM_FRACMASK;

        for(c = 0;c < MAX_EFFECT_CHANNELS;c++)
        {
            BiquadFilter_process(&state->Chans[c].Filter, temps[0], &SamplesIn[c][base], td);
            for(i = 0;i < td;i++)
                temps[1][i] = temps[0][i] * modsamples[i];

            MixSamples(temps[1], NumChannels, SamplesOut, state->Chans[c].CurrentGains,
                       state->Chans[c].TargetGains, SamplesToDo-base, base, td);
        }

        base += td;
    }
}


typedef struct ModulatorStateFactory {
    DERIVE_FROM_TYPE(EffectStateFactory);
} ModulatorStateFactory;

static ALeffectState *ModulatorStateFactory_create(ModulatorStateFactory *UNUSED(factory))
{
    ALmodulatorState *state;

    NEW_OBJ0(state, ALmodulatorState)();
    if(!state) return NULL;

    return STATIC_CAST(ALeffectState, state);
}

DEFINE_EFFECTSTATEFACTORY_VTABLE(ModulatorStateFactory);

EffectStateFactory *ModulatorStateFactory_getFactory(void)
{
    static ModulatorStateFactory ModulatorFactory = { { GET_VTABLE2(ModulatorStateFactory, EffectStateFactory) } };

    return STATIC_CAST(EffectStateFactory, &ModulatorFactory);
}


void ALmodulator_setParamf(ALeffect *effect, ALCcontext *context, ALenum param, ALfloat val)
{
    ALeffectProps *props = &effect->Props;
    switch(param)
    {
        case AL_RING_MODULATOR_FREQUENCY:
            if(!(val >= AL_RING_MODULATOR_MIN_FREQUENCY && val <= AL_RING_MODULATOR_MAX_FREQUENCY))
                SETERR_RETURN(context, AL_INVALID_VALUE,, "Modulator frequency out of range");
            props->Modulator.Frequency = val;
            break;

        case AL_RING_MODULATOR_HIGHPASS_CUTOFF:
            if(!(val >= AL_RING_MODULATOR_MIN_HIGHPASS_CUTOFF && val <= AL_RING_MODULATOR_MAX_HIGHPASS_CUTOFF))
                SETERR_RETURN(context, AL_INVALID_VALUE,, "Modulator high-pass cutoff out of range");
            props->Modulator.HighPassCutoff = val;
            break;

        default:
            alSetError(context, AL_INVALID_ENUM, "Invalid modulator float property 0x%04x", param);
    }
}
void ALmodulator_setParamfv(ALeffect *effect, ALCcontext *context, ALenum param, const ALfloat *vals)
{ ALmodulator_setParamf(effect, context, param, vals[0]); }
void ALmodulator_setParami(ALeffect *effect, ALCcontext *context, ALenum param, ALint val)
{
    ALeffectProps *props = &effect->Props;
    switch(param)
    {
        case AL_RING_MODULATOR_FREQUENCY:
        case AL_RING_MODULATOR_HIGHPASS_CUTOFF:
            ALmodulator_setParamf(effect, context, param, (ALfloat)val);
            break;

        case AL_RING_MODULATOR_WAVEFORM:
            if(!(val >= AL_RING_MODULATOR_MIN_WAVEFORM && val <= AL_RING_MODULATOR_MAX_WAVEFORM))
                SETERR_RETURN(context, AL_INVALID_VALUE,, "Invalid modulator waveform");
            props->Modulator.Waveform = val;
            break;

        default:
            alSetError(context, AL_INVALID_ENUM, "Invalid modulator integer property 0x%04x", param);
    }
}
void ALmodulator_setParamiv(ALeffect *effect, ALCcontext *context, ALenum param, const ALint *vals)
{ ALmodulator_setParami(effect, context, param, vals[0]); }

void ALmodulator_getParami(const ALeffect *effect, ALCcontext *context, ALenum param, ALint *val)
{
    const ALeffectProps *props = &effect->Props;
    switch(param)
    {
        case AL_RING_MODULATOR_FREQUENCY:
            *val = (ALint)props->Modulator.Frequency;
            break;
        case AL_RING_MODULATOR_HIGHPASS_CUTOFF:
            *val = (ALint)props->Modulator.HighPassCutoff;
            break;
        case AL_RING_MODULATOR_WAVEFORM:
            *val = props->Modulator.Waveform;
            break;

        default:
            alSetError(context, AL_INVALID_ENUM, "Invalid modulator integer property 0x%04x", param);
    }
}
void ALmodulator_getParamiv(const ALeffect *effect, ALCcontext *context, ALenum param, ALint *vals)
{ ALmodulator_getParami(effect, context, param, vals); }
void ALmodulator_getParamf(const ALeffect *effect, ALCcontext *context, ALenum param, ALfloat *val)
{
    const ALeffectProps *props = &effect->Props;
    switch(param)
    {
        case AL_RING_MODULATOR_FREQUENCY:
            *val = props->Modulator.Frequency;
            break;
        case AL_RING_MODULATOR_HIGHPASS_CUTOFF:
            *val = props->Modulator.HighPassCutoff;
            break;

        default:
            alSetError(context, AL_INVALID_ENUM, "Invalid modulator float property 0x%04x", param);
    }
}
void ALmodulator_getParamfv(const ALeffect *effect, ALCcontext *context, ALenum param, ALfloat *vals)
{ ALmodulator_getParamf(effect, context, param, vals); }

DEFINE_ALEFFECT_VTABLE(ALmodulator);
