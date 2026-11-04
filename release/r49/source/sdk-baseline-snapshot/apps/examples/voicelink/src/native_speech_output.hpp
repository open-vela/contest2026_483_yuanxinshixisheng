#pragma once
#include "voicelink/tts_runtime.h"
#include "voicelink/speech_pcm.hpp"
#include "k7sound_api.h"
#include <cerrno>
#include <algorithm>
#include <string>
#include <vector>

// Asset paths are supplied only by the validated board resource loader.
// No model path or text is printed. Playback completes before returning.
class NativeSpeechOutput {
 public:
  NativeSpeechOutput(const char* model, const char* tokens, const char* lexicon)
      : model_(model), tokens_(tokens), lexicon_(lexicon) {}

  int say(const std::string& text, unsigned capture_frames = 0) noexcept {
    // Keep every synthesis request within the runtime's bounded UTF-8 input.
    // Only the final audio segment transitions into recording.
    if (text.empty() || text.size() > 4096) return -EINVAL;
    try {
      std::size_t offset=0;
      while(offset<text.size()) {
        std::size_t end=std::min(offset+180,text.size());
        while(end<text.size() && (static_cast<unsigned char>(text[end])&0xc0)==0x80) --end;
        if(end==offset) return -EINVAL;
        const int result=segment(text.substr(offset,end-offset),end==text.size()?capture_frames:0);
        if(result) return result;
        offset=end;
      }
      return 0;
    } catch (const std::bad_alloc&) { return -ENOMEM; }
      catch (...) { return -EINVAL; }
  }
 private:
  int segment(const std::string& text,unsigned capture_frames) {
      std::vector<std::int16_t> pcm(8000 * 30);
      std::size_t frames = 0;
      unsigned rate = 0;
      k7_tts_request request{model_, tokens_, lexicon_, text.c_str(), nullptr, nullptr};
      const int result = k7_tts_synthesize(&request, pcm.data(), pcm.size(), &frames, &rate);
      if (result) return result;
      auto stereo = voicelink::speechPcm(pcm.data(), frames, rate);
      return k7sound_speak_pcm(stereo.data(), unsigned(stereo.size() / 2), capture_frames);
  }
  const char *model_, *tokens_, *lexicon_;
};
