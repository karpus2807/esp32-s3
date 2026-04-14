#pragma once
#include <Arduino.h>
#include <SD_MMC.h>

// Writes MJPEG AVI files to SD card compatible with VLC, Windows Media Player.
// No dynamic memory — header is written with placeholders, patched on close.
class AviRecorder {
public:
  bool begin(const String &path, uint16_t w, uint16_t h, uint8_t fps) {
    _w = w; _h = h; _fps = fps ? fps : 15;
    _frames = 0; _moviBytes = 0;
    _file = SD_MMC.open(path, FILE_WRITE);
    if (!_file) return false;
    _writeHeader();
    return true;
  }

  bool writeFrame(const uint8_t *jpeg, size_t len) {
    if (!_file || !len) return false;
    uint8_t chunk[8];
    chunk[0]='0'; chunk[1]='0'; chunk[2]='d'; chunk[3]='c';
    chunk[4]=(uint8_t)(len);      chunk[5]=(uint8_t)(len>>8);
    chunk[6]=(uint8_t)(len>>16);  chunk[7]=(uint8_t)(len>>24);
    if (_file.write(chunk, 8) != 8) return false;
    if (_file.write(jpeg, len) != (int)len) return false;
    if (len & 1) { uint8_t pad = 0; _file.write(&pad, 1); }
    _frames++;
    _moviBytes += 8u + len + (len & 1u);
    return true;
  }

  // Flush header with real frame count and close.
  // wall_clock_ms: wall time for this segment (e.g. millis() - segStart). When > 0 and
  // frames were written, patches microSecPerFrame + strh rate so playback matches real time
  // (fixes "timestamp runs faster than video" when capture is slower than nominal fps).
  void end(uint32_t wall_clock_ms = 0) {
    if (!_file) return;
    if (wall_clock_ms > 0u && _frames > 0u) {
      uint64_t uspf = (uint64_t)wall_clock_ms * 1000ULL / (uint64_t)_frames;
      if (uspf < 33333ULL) {
        uspf = 33333ULL;
      }
      if (uspf > 10000000ULL) {
        uspf = 10000000ULL;
      }
      _file.seek(32);
      _u32((uint32_t)uspf);
      const uint32_t rate = (uint32_t)((1000000ULL + uspf / 2) / uspf);
      _file.seek(128);
      _u32(1);
      _file.seek(132);
      _u32(rate);
    }
    _patchHeader();
    _file.close();
  }

  bool    isOpen()      const { return (bool)_file; }
  uint32_t frameCount() const { return _frames; }

private:
  File     _file;
  uint16_t _w = 0, _h = 0;
  uint8_t  _fps = 15;
  uint32_t _frames = 0;
  uint32_t _moviBytes = 0;

  void _u32(uint32_t v) {
    uint8_t b[4] = {(uint8_t)v,(uint8_t)(v>>8),(uint8_t)(v>>16),(uint8_t)(v>>24)};
    _file.write(b, 4);
  }
  void _u16(uint16_t v) {
    uint8_t b[2] = {(uint8_t)v,(uint8_t)(v>>8)};
    _file.write(b, 2);
  }
  void _tag(const char *t) { _file.write((const uint8_t*)t, 4); }

  // Writes a 224-byte placeholder header.
  // Key offsets (byte positions):
  //  [4]   riffSize        — patched on end()
  //  [48]  avih.totalFrames— patched on end()
  //  [140] strh.length     — patched on end()
  //  [216] moviListSize    — patched on end()
  //  [224] first frame chunk
  void _writeHeader() {
    // RIFF AVI  [0]
    _tag("RIFF"); _u32(0);           // [4]  riffSize = PLACEHOLDER
    _tag("AVI ");

    // LIST hdrl  [12]  size=192 (fixed for 1 video stream)
    _tag("LIST"); _u32(192);
    _tag("hdrl");

    // avih  [24]
    _tag("avih"); _u32(56);
    _u32(1000000u / _fps);           // microSecPerFrame
    _u32(0);                         // maxBytesPerSec
    _u32(0);                         // paddingGranularity
    _u32(0);                         // flags (no AVIF_HASINDEX)
    _u32(0);                         // [48] totalFrames = PLACEHOLDER
    _u32(0);                         // initialFrames
    _u32(1);                         // streams
    _u32(0);                         // suggestedBufferSize
    _u32(_w);                        // [64] width
    _u32(_h);                        // [68] height
    _u32(0); _u32(0); _u32(0); _u32(0); // reserved[4]

    // LIST strl  [88]  size=116
    _tag("LIST"); _u32(116);
    _tag("strl");

    // strh  [100]
    _tag("strh"); _u32(56);
    _tag("vids");                    // fccType
    _tag("MJPG");                    // fccHandler
    _u32(0);                         // flags
    _u16(0); _u16(0);                // priority, language
    _u32(0);                         // initialFrames
    _u32(1);                         // scale
    _u32(_fps);                      // [132] rate  (fps = rate/scale)
    _u32(0);                         // start
    _u32(0);                         // [140] length = PLACEHOLDER
    _u32(0);                         // suggestedBufferSize
    _u32(0xFFFFFFFFu);               // quality
    _u32(0);                         // sampleSize
    _u16(0); _u16(0); _u16(_w); _u16(_h); // rcFrame

    // strf (BITMAPINFOHEADER)  [164]
    _tag("strf"); _u32(40);
    _u32(40);                        // biSize
    _u32(_w);                        // biWidth
    _u32(_h);                        // biHeight
    _u16(1);                         // biPlanes
    _u16(24);                        // biBitCount
    _tag("MJPG");                    // biCompression
    _u32((uint32_t)_w * _h * 3);    // biSizeImage
    _u32(0); _u32(0);                // XPels, YPels
    _u32(0); _u32(0);                // ClrUsed, ClrImportant

    // LIST movi  [212]
    _tag("LIST"); _u32(0);           // [216] moviListSize = PLACEHOLDER
    _tag("movi");
    // [224] — frame data follows
  }

  void _patchHeader() {
    // moviListSize = 4("movi") + _moviBytes
    // riffSize     = fileSize - 8
    //              = (224 + _moviBytes) - 8
    //              = 216 + _moviBytes
    _file.seek(4);   _u32(216u + _moviBytes);   // riffSize
    _file.seek(48);  _u32(_frames);              // avih.totalFrames
    _file.seek(140); _u32(_frames);              // strh.length
    _file.seek(216); _u32(4u + _moviBytes);      // moviListSize
  }
};
