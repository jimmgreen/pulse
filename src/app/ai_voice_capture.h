// ai_voice_capture.h - The composer's microphone (third batch, flow V).
//
// WASAPI shared-mode capture on its own thread, downmixed and resampled to
// 16 kHz mono 16-bit and split into sentences where the speaker pauses.
// Audio only ever lives in memory: nothing is written to disk, and a finished
// sentence is handed to the UI thread, which uploads it once and drops it.
#pragma once

#include <windows.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pulse::app::ai {

constexpr int kVoiceRate = 16000;   // what is uploaded: 16 kHz mono 16-bit PCM

struct VoiceMic {
    std::wstring id;     // the endpoint id (IMMDevice::GetId)
    std::wstring name;   // 耳机 (USB Audio)
};

// Active capture endpoints. `default_id` gets the console default ("" when
// there is none). A few milliseconds; fine on the UI thread.
std::vector<VoiceMic> ListVoiceMics(std::wstring* default_id = nullptr);

// Windows' microphone privacy switches (设置 › 隐私和安全性 › 麦克风):
// DeniedAll - 麦克风访问权限 or 让应用访问麦克风 is off; DeniedDesktop - 让桌面
// 应用访问麦克风 is off.
enum class MicAccess : uint8_t { Allowed, DeniedAll, DeniedDesktop };
MicAccess ReadMicAccess();

class VoiceRecorder {
public:
    struct Options {
        std::wstring mic_id;     // empty or gone: the default capture endpoint
        int pause_ms = 1000;     // quiet that ends a sentence; 0: one sentence until Stop
        std::wstring test_wav;   // --test-instance only: this file plays as the microphone
    };
    enum class State : uint8_t { Starting, Listening, Failed, Ended };
    static constexpr size_t kLevels = 14;
    struct Snapshot {
        State state = State::Starting;
        HRESULT error = S_OK;              // Failed: why
        uint32_t elapsed_ms = 0;           // audio time since listening started
        bool speaking = false;             // inside a sentence
        std::array<float, kLevels> levels{};   // 0..1, oldest first, one per 60 ms
        std::wstring device;               // the endpoint's name
    };

    static std::shared_ptr<VoiceRecorder> Start(Options options);
    ~VoiceRecorder();
    VoiceRecorder(const VoiceRecorder&) = delete;
    VoiceRecorder& operator=(const VoiceRecorder&) = delete;

    // Ends the capture; the sentence in progress still comes out (PopSentence)
    // once the state reads Ended.
    void Stop();
    Snapshot Read() const;
    // The next finished sentence, oldest first; false when there is none.
    bool PopSentence(std::vector<int16_t>& pcm);

    struct Shared;

private:
    VoiceRecorder() = default;
    std::shared_ptr<Shared> shared_;
};

// A RIFF/WAVE image of 16 kHz mono 16-bit PCM, in memory.
std::string EncodeVoiceWav(const std::vector<int16_t>& pcm);
// 0.6 s of a soft hum: the connection card's test recognition.
std::vector<int16_t> VoiceTestClip();

} // namespace pulse::app::ai
