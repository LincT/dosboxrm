/*
 *  Copyright (C) 2022 Jon Dennis
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
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

//
// This file contains the reelmagic MPEG player code...
//

#include "reelmagic.h"
#include "setup.h"
#include "dos_system.h"
#include "mixer.h"


#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>

#include <exception>
#include <string>

//bring in the MPEG-1 decoder library...
#define PL_MPEG_IMPLEMENTATION
#include "./reelmagic_pl_mpeg.h"

//global config
static int _magicalFcodeOverride = 0; //0 = no override



//
// Internal class utilities...
//
namespace {
  struct RMException : ::std::exception { //XXX currently duplicating this in realmagic_*.cpp files to avoid header pollution... TDB if this is a good idea...
    std::string _msg;
    RMException(const char *fmt = "General ReelMagic Exception", ...) {
      va_list vl;
      va_start(vl, fmt); _msg.resize(vsnprintf(&_msg[0], 0, fmt, vl) + 1);   va_end(vl);
      va_start(vl, fmt); vsnprintf(&_msg[0], _msg.size(), fmt, vl);          va_end(vl);
      LOG(LOG_REELMAGIC, LOG_ERROR)("%s", _msg.c_str());
    }
    virtual ~RMException() throw() {}
    virtual const char* what() const throw() {return _msg.c_str();}
  };

  #define ARRAY_COUNT(T) (sizeof(T) / sizeof(T[0]))
  class AudioSampleFIFO {
    struct Frame {
      bool    produced;
      Bitu    samplesConsumed;
      struct {
        Bit16s left;
        Bit16s right;
      } samples[PLM_AUDIO_SAMPLES_PER_FRAME];
      inline Frame() : produced(false) {}
    };
    Frame _fifo[30]; //30 should be enough i think...
    Bitu _producePtr;
    Bitu _consumePtr;
    Bitu _sampleRate;

    inline void DisposeForProduction() {
      const Bitu disposeFrameCount = 2;
      //const Bitu disposeFrameCount = ARRAY_COUNT(_fifo) / 2;
      LOG(LOG_REELMAGIC, LOG_WARN)("Audio FIFO consumer not keeping up. Disposing %u Interleaved Samples", (unsigned)(disposeFrameCount * ARRAY_COUNT(_fifo[0].samples)));
      for (Bitu i = 0; i < disposeFrameCount; ++i) {
        _fifo[_consumePtr++].produced = false;
        if (_consumePtr >= ARRAY_COUNT(_fifo)) _consumePtr = 0;
      }
    }

    inline Bit16u ConvertSample(const double samp) {
#warning boosting audio too high !?
      return (Bit16u)(samp * 32767.0 * 1.5);
      return (Bit16u)(samp * 32767.0);
    }

  public:
    AudioSampleFIFO() : _producePtr(0), _consumePtr(0), _sampleRate(0) { }
    inline Bitu GetSampleRate() const {return _sampleRate;}
    inline void SetSampleRate(const Bitu value) {_sampleRate = value;}

    //consumer -- 1 sample include left and right
    inline Bitu SamplesAvailableForConsumption() {
      const Frame& f = _fifo[_consumePtr];
      if (!f.produced) return 0;
      return ARRAY_COUNT(f.samples) - f.samplesConsumed;
    }
    inline const Bit16s *GetConsumableInterleavedSamples() {
      const Frame& f = _fifo[_consumePtr];
      return &f.samples[f.samplesConsumed].left;
    }
    inline void Consume(const Bitu sampleCount) {
      Frame& f = _fifo[_consumePtr];
      f.samplesConsumed += sampleCount;
      if (f.samplesConsumed >= ARRAY_COUNT(f.samples)) {
        f.produced = false;
        if (++_consumePtr >= ARRAY_COUNT(_fifo)) _consumePtr = 0;
      }
    }

    //producer...
    inline void Produce(const plm_samples_t& s) {
      Frame& f = _fifo[_producePtr];
      if (f.produced) DisposeForProduction(); //WARNING dropping samples !?

      for (Bitu i = 0; i < ARRAY_COUNT(s.interleaved); i+=2) {
        f.samples[i >> 1].left  = ConvertSample(s.interleaved[i]);
        f.samples[i >> 1].right = ConvertSample(s.interleaved[i+1]);
      }

      f.samplesConsumed = 0;
      f.produced = true;
      if (++_producePtr >= ARRAY_COUNT(_fifo)) _producePtr = 0;
    }
  };
}

static void ActivatePlayerAudioFifo(AudioSampleFIFO& fifo);
static void DeactivatePlayerAudioFifo(AudioSampleFIFO& fifo);



//
// implementation of a "ReelMagic Media Player" and handles begins here...
//
namespace { class ReelMagic_MediaPlayerImplementation : public ReelMagic_MediaPlayer, public ReelMagic_VideoMixerUnderlayProvider {
  // creation parameters...
  ReelMagic_MediaPlayerFile * const   _file;
  const ReelMagic_MediaPlayer_Handle  _handle;
  ReelMagic_MediaPlayer_Handle        _demuxHandle;
  ReelMagic_MediaPlayer_Handle        _videoHandle;
  ReelMagic_MediaPlayer_Handle        _audioHandle;

  // running / adjustable variables...
  bool                                _underVga;
  bool                                _loop;
  bool                                _playing;

  // output state...
  float                               _vgaFps;
  double                              _vgaFramesPerMpegFrame;
  double                              _waitVgaFramesUntilNextMpegFrame;
  bool                                _drawNextFrame;

  //stuff about the MPEG decoder...
  plm_t                              *_plm;
  plm_frame_t                        *_nextFrame;
  Bit16u                              _width;
  Bit16u                              _height;
  double                              _framerate;
  Bit8u                               _magicalRSizeOverride;

  AudioSampleFIFO                     _audioFifo;

  static void plmBufferLoadCallback(plm_buffer_t *self, void *user) {
    //note: based on plm_buffer_load_file_callback()
    try {
      if (self->discard_read_bytes) {
        plm_buffer_discard_read_bytes(self);
      }
      const size_t bytes_available = self->capacity - self->length;
      const Bit32u bytes_read = ((ReelMagic_MediaPlayerImplementation*)user)->
        _file->Read(self->bytes + self->length, bytes_available);
      self->length += bytes_read;
      self->file_pos += bytes_read;

      if (bytes_read == 0) {
        self->has_ended = TRUE;
      }
    }
    catch (...) {
      self->has_ended = TRUE;
    }
  }
  static void plmBufferSeekCallback(plm_buffer_t *self, void *user, size_t absPos) {
    try {
      ((ReelMagic_MediaPlayerImplementation*)user)->
        _file->Seek(absPos, DOS_SEEK_SET);
    }
    catch (...) {
      //XXX what to do on failure !?
    }
  }

  static void plmDecodeMagicalPictureHeaderCallback(plm_video_t *self, void *user) {
    switch (self->picture_type) {
    case PLM_VIDEO_PICTURE_TYPE_B:
      self->motion_backward.r_size = ((ReelMagic_MediaPlayerImplementation*)user)->_magicalRSizeOverride;
      //fallthrough
    case PLM_VIDEO_PICTURE_TYPE_PREDICTIVE:
      self->motion_forward.r_size = ((ReelMagic_MediaPlayerImplementation*)user)->_magicalRSizeOverride;
    }
  }

  void advanceNextFrame() {
    _nextFrame = plm_decode_video(_plm);
    if (_nextFrame == NULL) {
      if (_loop) _nextFrame = plm_decode_video(_plm); //note: will return NULL frame once when looping... give it one more go...
      if (_nextFrame == NULL) _playing = false;
    }
  }

  void decodeBufferedAudio() {
    if (!_plm->audio_decoder) return;
    plm_samples_t *samples;
    while (plm_buffer_get_remaining(_plm->audio_decoder->buffer) > 0) {
      samples = plm_audio_decode(_plm->audio_decoder);
      if (samples == NULL) break;
      _audioFifo.Produce(*samples);
    }
  }

  unsigned FindMagicalFCode() {
    //now this is some mighty fine half assery...
    //i'm sure this is suppoed to be done on a per-picture basis, but for now, this hack seems
    //to work ok...
    //the idea here is that MPEG-1 assets with a picture_rate code >= 0x9 in the MPEG sequence
    //header have screwed up f_code values. i'm not sure why but this may be some form of copy
    //and/or clone protection for ReelMagic. pictures with a temporal sequence number of either
    //3 or 8 seem to contain a truthful f_code for Return to Zork and Lord of the Rings assets
    //4 seems to contain the truthful f_code for The Horde. The Horde also has an empty user
    //data chunk in the picture header too which is used to identify this.
    //
    //for now, this hack scrubs the MPEG file in search of the first P or B pictures with a
    //temporal sequence number of 3 or 8 (or 4 for The Horde / user data) and returns the
    //f_code value. then the player applies the f_code as a global static forward and
    //backward value for this entire asset.
    //
    //ultimately, this should probably be done on a per-picture basis using some sort of
    //algorithm to translate the screwed-up values on-the-fly...

    unsigned result = 0;

    const int audio_enabled = plm_get_audio_enabled(_plm);
    const int loop_enabled  = plm_get_loop(_plm);
    plm_rewind(_plm);
    plm_set_audio_enabled(_plm, FALSE);
    plm_set_loop(_plm, FALSE);

    do {
      if (plm_buffer_find_start_code(_plm->video_decoder->buffer, PLM_START_PICTURE) == -1) {
        break;
      }
      const unsigned temporal_seqnum = plm_buffer_read(_plm->video_decoder->buffer, 10);
      const unsigned picture_type = plm_buffer_read(_plm->video_decoder->buffer, 3);
      if ((picture_type == PLM_VIDEO_PICTURE_TYPE_PREDICTIVE) || (picture_type == PLM_VIDEO_PICTURE_TYPE_B)) {
        plm_buffer_skip(_plm->video_decoder->buffer, 16); // skip vbv_delay
        plm_buffer_skip(_plm->video_decoder->buffer, 1); //skip full_px
        result = plm_buffer_read(_plm->video_decoder->buffer, 3);
        if (plm_buffer_next_start_code(_plm->video_decoder->buffer) == PLM_START_USER_DATA) {
          // The Horde videos tsn=4 is truthful
          if (temporal_seqnum != 4) result = 0;
        }
        else {
          // Return to Zork and Lord of the Rings videos tsn=3 and tns=8 is truthful
          if ((temporal_seqnum != 3) && (temporal_seqnum != 8)) result = 0;
        }
      }
    } while (result == 0);

    plm_set_loop(_plm, loop_enabled);
    plm_set_audio_enabled(_plm, audio_enabled);
    plm_rewind(_plm);

    return result;
  }

  void CollectVideoStats() {
    _width = plm_get_width(_plm);
    _height = plm_get_height(_plm);
    if (_width && _height) {
      if (_plm->video_decoder->seqh_picture_rate >= 0x9) {
        LOG(LOG_REELMAGIC, LOG_NORMAL)("Detected a magical picture_rate code of 0x%X.", (unsigned)_plm->video_decoder->seqh_picture_rate);
        const unsigned magical_f_code = _magicalFcodeOverride ? _magicalFcodeOverride: FindMagicalFCode();
        if (magical_f_code) {
          _magicalRSizeOverride = magical_f_code - 1;
          plm_video_set_decode_picture_header_callback(_plm->video_decoder, &plmDecodeMagicalPictureHeaderCallback, this);
          LOG(LOG_REELMAGIC, LOG_NORMAL)("Applying static %u:%u f_code override", magical_f_code, magical_f_code);
        }
        else {
          LOG(LOG_REELMAGIC, LOG_WARN)("No magical f_code found. Playback will likely be screwed up!");
        }
        _plm->video_decoder->framerate = PLM_VIDEO_PICTURE_RATE[0x7 & _plm->video_decoder->seqh_picture_rate];
      }
      if (_plm->video_decoder->framerate == 0.000) {
        LOG(LOG_REELMAGIC, LOG_ERROR)("Detected a bad framerate. Hardcoding to 30. This video will likely not work at all.");
        _plm->video_decoder->framerate = 30.000;
      }
    }
    _framerate = plm_get_framerate(_plm);
  }

  void SetupVESOnlyDecode() {
    plm_set_audio_enabled(_plm, FALSE);
    if (_plm->audio_decoder) {
      plm_audio_destroy(_plm->audio_decoder);
      _plm->audio_decoder = NULL;
    }
    plm_demux_rewind(_plm->demux);
    _plm->has_decoders = TRUE;
    _plm->video_packet_type = PLM_DEMUX_PACKET_VIDEO_1;
    if (_plm->video_decoder) plm_video_destroy(_plm->video_decoder);
    _plm->video_decoder = plm_video_create_with_buffer(_plm->demux->buffer, FALSE);
  }

  static bool IsLoopFilename(const char * const filename) {
    const size_t len = strlen(filename);
    if (len < 2) return false;
    return strcmp(&filename[len - 2], "/l") == 0;
  }

public:
  ReelMagic_MediaPlayerImplementation(ReelMagic_MediaPlayerFile * const file, const ReelMagic_MediaPlayer_Handle handle) :
    _file(file),
    _handle(handle),
    _demuxHandle(0),
    _videoHandle(0),
    _audioHandle(0),
    _underVga(false),
    _loop(IsLoopFilename(_file->GetFileName())),
    _playing(false),
    _vgaFps(0.0f),
    _plm(NULL),
    _nextFrame(NULL),
    _magicalRSizeOverride(0) {

    bool detetectedFileTypeVesOnly = false;

    plm_buffer_t * const plmBuf = plm_buffer_create_with_virtual_file(
      &plmBufferLoadCallback,
      &plmBufferSeekCallback,
      this,
      _file->GetFileSize()
    );
    _plm = plm_create_with_buffer(plmBuf, TRUE); //TRUE = destroy buffer when done

    if (!plm_has_headers(_plm)) {
      //failed to detect an MPEG-1 PS (muxed) stream... try MPEG-ES assuming video-only...
      detetectedFileTypeVesOnly = true;
      SetupVESOnlyDecode();
      _videoHandle = _handle;
    }
    else {
      _demuxHandle = _handle;
    }

    //disable audio buffer load callback so pl_mpeg dont try to "auto fetch" audio samples
    //when we ask it for audio data...
    if (_plm->audio_decoder) {
      _plm->audio_decoder->buffer->load_callback = NULL;
      _audioFifo.SetSampleRate((Bitu)plm_get_samplerate(_plm));
    }

    plm_set_loop(_plm, _loop ? TRUE : FALSE);
    CollectVideoStats();
    advanceNextFrame(); //attempt to decode the first frame of video...
    if ((_nextFrame == NULL) || (_width == 0) || (_height == 0)) {
      //something failed... asset is deemed bad at this point...
      plm_destroy(_plm);
      _plm = NULL;
    }

    if (_plm == NULL) {
      LOG(LOG_REELMAGIC, LOG_ERROR)("Created%s Media Player #%u MPEG Type Detect Failed %s", _loop ? " Looping" : "", (unsigned)_handle, _file->GetFileName());
    }
    else {
      LOG(LOG_REELMAGIC, LOG_NORMAL)("Created%s Media Player #%u %s %ux%u @ %0.2ffps %s", _loop ? " Looping" : "", (unsigned)_handle, detetectedFileTypeVesOnly ? "MPEG-ES" : "MPEG-PS", (unsigned)_width, (unsigned)_height, _framerate, _file->GetFileName());
      if (_audioFifo.GetSampleRate())
        LOG(LOG_REELMAGIC, LOG_NORMAL)("Media Player #%u Audio Decoder Enabled @ %uHz", (unsigned)_handle, (unsigned)_audioFifo.GetSampleRate());
    }
  }
  virtual ~ReelMagic_MediaPlayerImplementation() {
    LOG(LOG_REELMAGIC, LOG_NORMAL)("Destroying Media Player #%u %s", (unsigned)_handle, _file->GetFileName());
    DeactivatePlayerAudioFifo(_audioFifo);
    ReelMagic_PopVideoMixerUnderlayProvider(*this);
    if (_plm != NULL) plm_destroy(_plm);
    delete _file;
  }


  //
  // function for accessing this class just in this file...
  //
  Bitu GetHandlesNeeded() const {
    Bitu rv = 0;
    if (HasSystem()) ++rv;
    if (HasVideo())  ++rv;
    if (HasAudio())  ++rv;
    if (rv == 0) rv=1;
    return rv;
  }
  void DeclareAuxHandle(const ReelMagic_MediaPlayer_Handle auxHandle) {
    //this is so damn hacky :-( 
    if (_videoHandle == 0) {
      _videoHandle = auxHandle;
      return;
    }
    if (_audioHandle == 0) {
      _audioHandle = auxHandle;
      return;
    }
    LOG(LOG_REELMAGIC, LOG_WARN)("Declaring too many handles!");
  }




  //
  // ReelMagic_VideoMixerUnderlayProvider implementation here...
  //
  void OnVerticalRefresh(void * const outputBuffer, const float fps) {
    if (!_playing) return;
    if (fps != _vgaFps) {
      _vgaFps = fps;
      _vgaFramesPerMpegFrame = _vgaFps;
      _vgaFramesPerMpegFrame /= _framerate;
      _waitVgaFramesUntilNextMpegFrame = _vgaFramesPerMpegFrame;
      _drawNextFrame = true;
    }

    if (_drawNextFrame) {
      plm_frame_to_rgb(_nextFrame, (uint8_t*)outputBuffer, _width * 3);
      decodeBufferedAudio();
      _drawNextFrame = false;
    }

    for (_waitVgaFramesUntilNextMpegFrame -= 1.00; _waitVgaFramesUntilNextMpegFrame < 0.00; _waitVgaFramesUntilNextMpegFrame += _vgaFramesPerMpegFrame) {
      advanceNextFrame();
      _drawNextFrame = true;
    }
  }

  //TODO: implement this!
  bool IsDisplayFullScreen()         { return true; }
  Bit16u GetDisplayPositionWidth()   { return 0; }
  Bit16u GetDisplayPositionHeight()  { return 0; }
  Bit16u GetDisplaySizeWidth()       { return 0; }
  Bit16u GetDisplaySizeHeight()      { return 0; }

  //Bit16u GetPictureWidth() const {return _width;} -- is implemented below as it exists in both base "interfaces"
  //Bit16u GetPictureHeight() const {return _height;} -- is implemented below as it exists in both base "interfaces"
  //bool   GetUnderVga() const -- is implemented below as it exists in both base "interfaces"



  //
  // ReelMagic_MediaPlayer implementation here...
  //
  ReelMagic_MediaPlayer_Handle GetBaseHandle() const {  return _handle; }
  ReelMagic_MediaPlayer_Handle GetDemuxHandle() const { return _demuxHandle; }
  ReelMagic_MediaPlayer_Handle GetVideoHandle() const { return _videoHandle; }
  ReelMagic_MediaPlayer_Handle GetAudioHandle() const { return _audioHandle; }

  void SetDisplayPosition(const Bit16u x, const Bit16u y) {
    LOG(LOG_REELMAGIC, LOG_ERROR)("Set Player Display Position Not Implemented!");
  }
  void SetDisplaySize(const Bit16u width, const Bit16u height) {
    LOG(LOG_REELMAGIC, LOG_ERROR)("Set Player Display Size Not Implemented!");
  }
  void SetUnderVga(const bool value) {
    if (_underVga == value) return;
    _underVga = value;
    if (_playing) ReelMagic_PushVideoMixerUnderlayProvider(*this);
  }
  void SetMagicDecodeKey(const uint32_t value) {
    //ignore for now...
  }
  void SetLooping(const bool value) {
    _loop = value;
    if (_plm != NULL) plm_set_loop(_plm, _loop ? TRUE : FALSE);
  }
  bool HasSystem() const {
    if (_plm == NULL) return false;
    return _plm->demux->buffer != _plm->video_decoder->buffer;
  }
  bool HasVideo() const {
    if (_plm == NULL) return false;
    return plm_get_video_enabled(_plm) != FALSE;
  }
  bool HasAudio() const {
    if (_plm == NULL) return false;
    return plm_get_audio_enabled(_plm) != FALSE;
  }
  bool IsLooping() const {
    return _loop;
  }
  bool IsPlaying() const {
    return _playing;
  }

  Bit16u GetPictureWidth()      const {return _width;}
  Bit16u GetPictureHeight()     const {return _height;}
  bool   GetUnderVga()          const {return _underVga;}

  void Play() {
    if (_plm == NULL) return;
    _playing = true;
    ReelMagic_PushVideoMixerUnderlayProvider(*this);
    ActivatePlayerAudioFifo(_audioFifo);
  }
  void Pause() {
    _playing = false;
  }
  void Stop() {
    _playing = false;
    ReelMagic_PopVideoMixerUnderlayProvider(*this);
  }
};};




//
//stuff to manage ReelMagic media/decoder/player handles...
//note: handles are _rmhandles[] index + 1 as "FMPDRV.EXE" uses 0 as invalid handle
//
static ReelMagic_MediaPlayerImplementation *_rmhandles[REELMAGIC_MAX_HANDLES] = {NULL};

static Bitu ComputeFreePlayerHandleCount() {
  Bitu rv = 0;
  for (Bitu i = 0; i < REELMAGIC_MAX_HANDLES; ++i) {
    if (_rmhandles[i] == NULL) ++rv;
  }
  return rv;
}

ReelMagic_MediaPlayer_Handle ReelMagic_NewPlayer(struct ReelMagic_MediaPlayerFile * const playerFile) {
  //so why all this mickey-mouse for simply allocating a handle?
  //the real setup allocates one handle per decoder resource
  //for example, if an MPEG file is opened that only contains a video ES,
  //then only one handle is allocated
  //however, if an MPEG PS file is openened that contains both A/V ES streams,
  //then three handles are allocated. One for system, one for audio, one for video
  //
  //to ensure maximum compatibility, we must also emulate this behavior

  const Bitu freeHandles = ComputeFreePlayerHandleCount();
  Bit8u handleIndex;

  try {
    if (freeHandles < 1) throw RMException("Out of handles!");
    for (handleIndex = 0; handleIndex < REELMAGIC_MAX_HANDLES; ++handleIndex) {
      if (_rmhandles[handleIndex] == NULL) {
        _rmhandles[handleIndex] = new ReelMagic_MediaPlayerImplementation(playerFile, handleIndex+1);
        break;
      }
    }
  }
  catch (...) {
    delete playerFile;
    throw;
  }

  Bitu handlesNeeded = _rmhandles[handleIndex]->GetHandlesNeeded();
  if (freeHandles < handlesNeeded) {
    delete _rmhandles[handleIndex];
    _rmhandles[handleIndex] = NULL;
    throw RMException("Out of handles!");
  }

  Bitu additionalHandleIndex = handleIndex;
  while (--handlesNeeded) {
    while (_rmhandles[++additionalHandleIndex] != NULL);
    _rmhandles[handleIndex]->DeclareAuxHandle(additionalHandleIndex+1);
    _rmhandles[additionalHandleIndex] = _rmhandles[handleIndex];
    LOG(LOG_REELMAGIC, LOG_NORMAL)("Consuming additional handle #%u for base handle #%u", (unsigned)(additionalHandleIndex+1), (unsigned)(handleIndex+1));
  }

  return handleIndex + 1; //all ReelMagic media handles are non-zero
}

void ReelMagic_DeletePlayer(const ReelMagic_MediaPlayer_Handle handle) {
  ReelMagic_MediaPlayer *player = &ReelMagic_HandleToMediaPlayer(handle);
  delete player;
  for (Bitu i = 0; i < REELMAGIC_MAX_HANDLES; ++i) {
    if (_rmhandles[i] == player) {
      _rmhandles[i] = NULL;
      LOG(LOG_REELMAGIC, LOG_NORMAL)("Freeing handle #%u", (unsigned)(i+1));
    }
  }
}

ReelMagic_MediaPlayer& ReelMagic_HandleToMediaPlayer(const ReelMagic_MediaPlayer_Handle handle) {
  if ((handle == 0) || (handle > REELMAGIC_MAX_HANDLES)) throw RMException("Invalid handle #%u", (unsigned)handle);
  ReelMagic_MediaPlayer * const player = _rmhandles[handle-1];
  if (player == NULL) throw RMException("No active player at handle #%u", (unsigned)handle);
  return *player;
}

void ReelMagic_DeleteAllPlayers() {
  for (Bit8u i = 0; i < REELMAGIC_MAX_HANDLES; ++i) {
    if (_rmhandles[i] == NULL) continue;
    delete _rmhandles[i];
    for (Bit8u j = i+1; j < REELMAGIC_MAX_HANDLES; ++j) {
      if (_rmhandles[j] == _rmhandles[i]) {
        _rmhandles[j] = NULL;
        LOG(LOG_REELMAGIC, LOG_NORMAL)("Freeing handle #%u", (unsigned)(j+1));
      }
    }
    _rmhandles[i] = NULL;
    LOG(LOG_REELMAGIC, LOG_NORMAL)("Freeing handle #%u", (unsigned)(i+1));
  }
}




//
// audio stuff begins here...
//
static MixerChannel *_rmaudio = NULL;
static AudioSampleFIFO * volatile _activePlayerAudioFifo = NULL;

static void ActivatePlayerAudioFifo(AudioSampleFIFO& fifo) {
  if (!fifo.GetSampleRate()) return;
  _activePlayerAudioFifo = &fifo;
  _rmaudio->SetFreq(_activePlayerAudioFifo->GetSampleRate());
}

static void DeactivatePlayerAudioFifo(AudioSampleFIFO& fifo) {
  if (_activePlayerAudioFifo == &fifo) _activePlayerAudioFifo = NULL;
}

static Bit16s _lastAudioSample[2];
static void RMMixerChannelCallback(Bitu samplesNeeded) {
  //samplesNeeded is sample count, including both channels...
  if (_activePlayerAudioFifo == NULL) {
    _rmaudio->AddSilence();
    return;
  }
  Bitu available;
  while (samplesNeeded) {
    available = _activePlayerAudioFifo->SamplesAvailableForConsumption();
    if (available == 0) {
      _rmaudio->AddSamples_s16(1, _lastAudioSample);
      --samplesNeeded;
      continue;
//      _rmaudio->AddSilence();
//      return;
    }
    if (samplesNeeded > available) {
      _rmaudio->AddSamples_s16(available, _activePlayerAudioFifo->GetConsumableInterleavedSamples());
      _lastAudioSample[0] = _activePlayerAudioFifo->GetConsumableInterleavedSamples()[available - 2];
      _lastAudioSample[1] = _activePlayerAudioFifo->GetConsumableInterleavedSamples()[available - 1];
      _activePlayerAudioFifo->Consume(available);
      samplesNeeded -= available;
    }
    else {
      _rmaudio->AddSamples_s16(samplesNeeded, _activePlayerAudioFifo->GetConsumableInterleavedSamples());
      _lastAudioSample[0] = _activePlayerAudioFifo->GetConsumableInterleavedSamples()[samplesNeeded - 2];
      _lastAudioSample[1] = _activePlayerAudioFifo->GetConsumableInterleavedSamples()[samplesNeeded - 1];
      _activePlayerAudioFifo->Consume(samplesNeeded);
      samplesNeeded = 0;
    }
  }
}

void ReelMagic_InitPlayer(Section* sec) {
  Section_prop * section=static_cast<Section_prop *>(sec);

  _rmaudio = MIXER_AddChannel(&RMMixerChannelCallback, 44100, "REELMAGC");
  _rmaudio->Enable(true); 

  //XXX Remove this as it is ONLY for debugging MPEG assets!!!
  _magicalFcodeOverride = section->Get_int("magicfhack");
  if ((_magicalFcodeOverride < 0) || (_magicalFcodeOverride > 7))
    E_Exit("Bad magicfhack value");
}
