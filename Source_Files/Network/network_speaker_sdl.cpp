/*
 *  network_speaker_sdl.cpp
 *  created for Marathon: Aleph One <http://source.bungie.org/>

	Copyright (C) 2002 and beyond by Woody Zenfell, III
	and the "Aleph One" developers.
 
	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	This license is contained in the file "COPYING",
	which is included with this source code; it is available online at
	http://www.gnu.org/licenses/gpl.html

 *  The code in this file is licensed to you under the GNU GPL.  As the copyright holder,
 *  however, I reserve the right to use this code as I see fit, without being bound by the
 *  GPL's terms.  This exemption is not intended to apply to modified versions of this file -
 *  if I were to use a modified version, I would be a licensee of whomever modified it, and
 *  thus must observe the GPL terms.
 *
 *  Realtime network audio playback support for SDL platforms.
 *
 *  Created by woody Mar 3-8, 2002.
 */

#include    "network_speaker_sdl.h"

#include    "network_distribution_types.h"
#include    "CircularQueue.h"
#include    "world.h"   // local_random()
#include    "mysound.h"
#include    "network_data_formats.h"    // in support of received_network_audio_proc
#include    "player.h"                  // in support of received_network_audio_proc

enum {
    kSoundBufferQueueSize = 256,    // should never get anywhere near here, but at 12 bytes/struct these are cheap.
    kNoiseBufferSize = 1280,        // how big a buffer we should use for noise (at 11025 this is about 1/9th of a second)
    kMaxDryDequeues = 1,            // how many consecutive empty-buffers before we stop playing?
    kNumPumpPrimes = 1              // how many noise-buffers should we start with while buffering incoming data?
};

static  CircularQueue<NetworkSpeakerSoundBuffer>    sSoundBuffers(kSoundBufferQueueSize);

// We can provide static noise instead of a "real" buffer once in a while if we need to.
// Also, we provide kNumPumpPrimes of static noise before getting to the "meat" as well.
static  byte*                       sNoiseBufferStorage = NULL;
static  NetworkSpeakerSoundBuffer   sNoiseBufferDesc;
static  int                         sDryDequeues = 0;
static  bool                        sSpeakerIsOn = false;


void
open_network_speaker() {
    // Allocate storage for noise data - assume if pointer not NULL, already have storage.
    if(sNoiseBufferStorage == NULL) {
        assert(kNoiseBufferSize % 2 == 0);
        uint16* theBuffer = new uint16[kNoiseBufferSize / 2];

        // Fill in noise data (use whole width of local_random())
        for(int i = 0; i < kNoiseBufferSize / 2; i++)
            theBuffer[i] = local_random();

        sNoiseBufferStorage = (byte*) theBuffer;
    }

    // Fill out the noise-buffer descriptor
    sNoiseBufferDesc.mData      = sNoiseBufferStorage;
    sNoiseBufferDesc.mLength    = kNoiseBufferSize;
    sNoiseBufferDesc.mFlags     = 0;

    // Reset the buffer queue
    sSoundBuffers.reset();

    // Reset a couple others to sane values
    sDryDequeues    = 0;
    sSpeakerIsOn    = false;
}

void
queue_network_speaker_data(byte* inData, short inLength) {
    if(inLength > 0) {
        // Fill out a descriptor for a new chunk of storage
        NetworkSpeakerSoundBuffer   theBufferDesc;
        theBufferDesc.mData     = new byte[inLength];
        theBufferDesc.mLength   = inLength;
        theBufferDesc.mFlags    = kSoundDataIsDisposable;

        // and copy the data
        memcpy(theBufferDesc.mData, inData, inLength);

        // If we're just turning on, prime the queue with a few buffers of noise.
        if(!sSpeakerIsOn) {
            for(int i = 0; i < kNumPumpPrimes; i++) {
                sSoundBuffers.enqueue(sNoiseBufferDesc);
            }

            sSpeakerIsOn = true;
        }

        // Enqueue the actual sound data.
        sSoundBuffers.enqueue(theBufferDesc);
    }
}


void
network_speaker_idle_proc() {
    if(sSpeakerIsOn)
        ensure_network_audio_playing();
}


NetworkSpeakerSoundBuffer*
dequeue_network_speaker_data() {
    // We need this to stick around between calls
    static NetworkSpeakerSoundBuffer    sBufferDesc;

    // If there is actual sound data, reset the "ran dry" count and return a pointer to the buffer descriptor
    if(sSoundBuffers.getCountOfElements() > 0) {
        sDryDequeues = 0;
        sBufferDesc = sSoundBuffers.peek();
        sSoundBuffers.dequeue();
        return &sBufferDesc;
    }
    // If there's no data available, inc the "ran dry" count and return either a noise buffer or NULL.
    else {
        sDryDequeues++;
        if(sDryDequeues > kMaxDryDequeues) {
            sSpeakerIsOn = false;
            return NULL;
        }
        else
            return &sNoiseBufferDesc;
    }
}


void
close_network_speaker() {
    // Tell the audio system not to get our data anymore
    stop_network_audio();

    // Bleed the queue dry of any leftover data
    NetworkSpeakerSoundBuffer*  theDesc;
    while((theDesc = dequeue_network_speaker_data()) != NULL) {
        if(is_sound_data_disposable(theDesc))
            delete [] theDesc->mData;
    }

    // Free the noise buffer and restore some values
    if(sNoiseBufferStorage != NULL) {
        delete [] sNoiseBufferStorage;
        sNoiseBufferStorage = NULL;
    }
    sDryDequeues    = 0;
    sSpeakerIsOn    = false;
}



// This is what the network distribution system calls when audio is received.
void
received_network_audio_proc(void *buffer, short buffer_size, short player_index) {
    network_audio_header_NET* theHeader_NET = (network_audio_header_NET*) buffer;

    network_audio_header    theHeader;

    netcpy(&theHeader, theHeader_NET);

    // For now, this should always be 0
    assert(theHeader.mReserved == 0);

    if(!(theHeader.mFlags & kNetworkAudioForTeammatesOnlyFlag) || (local_player->team == get_player_data(player_index)->team)) {
        byte* theSoundData = ((byte*)buffer) + sizeof(network_audio_header_NET);
        queue_network_speaker_data(theSoundData, buffer_size - sizeof(network_audio_header_NET));
    }
}
