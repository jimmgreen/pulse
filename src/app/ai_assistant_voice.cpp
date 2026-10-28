// ai_assistant_voice.cpp - 语音输入: listening, the ordered recognition queue,
// the voice cards in the conversation, the 语音设置 menu and the 语音识别连接
// card (its EDITs, the model list + test recognition, saving).
#include "ai_assistant_voice.h"

#include "ai_assistant.h"
#include "ai_assistant_chat.h"
#include "ai_model_key.h"
#include "ai_model_stream.h"
#include "ai_settings.h"
#include "ai_voice_capture.h"
#include "ai_voice_service.h"
#include "app_commands.h"
#include "app_internal.h"
#include "../common/json_value.h"
#include "../common/localization.h"
#include "../ui/ai_panel_voice.h"
#include "../ui/fluent_menu.h"
#include "../ui/typography.h"
#include "../ui/ui_motion.h"
#include "../ui/ui_renderer.h"
#include "../ui/ui_view_model.h"

#include <commctrl.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <thread>

namespace pulse {

struct AiVoiceUi {
    // ai.json assistant.voice and whether voice_key.bin holds a key.
    app::ai::VoicePrefs prefs;
    bool key_saved = false;
    // Listening.
    std::shared_ptr<app::ai::VoiceRecorder> rec;
    app::ai::VoiceRecorder::Snapshot snap;
    bool failed = false;         // this session already showed a failure card
    std::wstring host;           // the service's host, for the cards
    struct Job {
        std::atomic<bool> done{false};
        app::ai::VoiceAnswer answer;
    };
    std::deque<std::shared_ptr<Job>> jobs;   // in the order spoken
    uint64_t shake_at = 0;
    bool timer_on = false;
    uint64_t last_tick = 0;   // the frame pump ticks too (AiVoiceRunDue)
    uint64_t test_due = 0;    // when the armed test starts; 0 = not armed
    // The voice cards in the conversation and where 打开 Windows 设置 goes.
    struct Card {
        uint32_t id = 0;
        bool need = false;
        std::wstring uri;
    };
    std::vector<Card> cards;
    std::vector<std::pair<uint32_t, uint64_t>> retiring;   // fading out since
    // 语音设置 menu.
    bool menu_open = false;
    uint64_t menu_closed_at = 0;
    std::vector<app::ai::VoiceMic> mics;
    std::wstring default_mic;
    // 语音识别连接 card.
    bool card_open = false;
    bool manage = false;
    uint64_t card_closed_at = 0;
    int service = 0;
    HWND url_edit = nullptr, key_edit = nullptr;
    HFONT font = nullptr;
    float font_scale = 0.0f;
    int line_px = 0;
    HBRUSH brush = nullptr;
    COLORREF brush_color = 0;
    bool url_shown = false, key_shown = false;
    bool url_focus_pending = false, key_focus_pending = false;
    int key_len = 0;
    std::wstring key_tail;
    std::wstring saved_mask;
    std::vector<std::string> models;
    std::string model;
    bool listed = false;
    ui::AiVoiceTest test = ui::AiVoiceTest::None;
    uint32_t test_ms = 0;
    std::wstring test_error;
    struct TestJob {
        uint64_t gen = 0;
        std::atomic<bool> done{false};
        app::ai::VoiceAnswer list, run;
        std::string model;
    };
    std::shared_ptr<TestJob> test_job;
    uint64_t gen = 0;
};

namespace {

namespace ai = app::ai;
using I = l10n::StringId;
using P = ui::AiPanelPart;
constexpr UINT_PTR kVoiceTimer = 0x4157;       // listening, recognition, the test
constexpr UINT_PTR kVoiceTestTimer = 0x4158;   // 600 ms after the key is in
constexpr UINT_PTR kVoiceSubclass = 0x4159;
constexpr uint64_t kMenuFadeMs = 240;
constexpr uint64_t kCardFadeMs = 360;
constexpr uint64_t kGoneMs = 260;
constexpr wchar_t kMaskChar = L'\u2022';

std::wstring Text(I id) { return l10n::Get(id).c_str(); }

std::wstring Fill(std::wstring text, std::wstring_view key, const std::wstring& value) {
    if (const size_t at = text.find(key); at != std::wstring::npos) text.replace(at, key.size(), value);
    return text;
}

COLORREF ToColorRef(D2D1_COLOR_F c) {
    auto b = [](float v) { return static_cast<BYTE>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return RGB(b(c.r), b(c.g), b(c.b));
}

void Redraw(AppState& s) {
    if (s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
}

uint64_t Now() { return ui::motion::NowMs(); }

std::wstring EditText(HWND edit) {
    if (!edit) return {};
    const int n = GetWindowTextLengthW(edit);
    std::wstring text(static_cast<size_t>(n) + 1, L'\0');
    GetWindowTextW(edit, text.data(), n + 1);
    text.resize(static_cast<size_t>(n));
    return text;
}

void Wipe(std::wstring& text) {
    if (!text.empty()) SecureZeroMemory(text.data(), text.size() * sizeof(wchar_t));
    text.clear();
}

std::wstring Trim(std::wstring text) {
    auto space = [](wchar_t ch) { return ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n' || ch == 0x3000; };
    size_t a = 0, b = text.size();
    while (a < b && space(text[a])) ++a;
    while (b > a && space(text[b - 1])) --b;
    return text.substr(a, b - a);
}

AiVoiceUi& Ui(AppState& s) {
    auto& slot = s.aiAssistant.voice;
    if (!slot) {
        slot = std::make_shared<AiVoiceUi>();
        slot->prefs = ai::LoadSettings().assistant.voice;
        slot->key_saved = ai::HasVoiceKey();
    }
    return *slot;
}

AiVoiceUi* Peek(const AppState& s) { return s.aiAssistant.voice.get(); }

// Another Pulse window may have saved or removed the connection since this
// one read ai.json: read it again when the user reaches for the mic, unless
// the connection card here is open with edits of its own.
void Reload(AiVoiceUi& u) {
    if (u.card_open || u.rec) return;
    bool malformed = false;
    const ai::Settings settings = ai::LoadSettings(&malformed);
    if (!malformed) u.prefs = settings.assistant.voice;
    u.key_saved = ai::HasVoiceKey();
}

bool Connected(const AiVoiceUi& u) { return u.prefs.Configured() && u.key_saved; }

void SavePrefs(const AiVoiceUi& u) {
    bool malformed = false;
    ai::Settings settings = ai::LoadSettings(&malformed);
    if (malformed) return;   // never overwrite a file we could not read
    settings.assistant.voice = u.prefs;
    (void)ai::SaveSettings(settings);
}

int PresetIndex(std::string_view id) {
    size_t n = 0;
    const ai::VoicePreset* presets = ai::VoicePresets(n);
    for (size_t i = 0; i < n; ++i)
        if (id == presets[i].id) return static_cast<int>(i);
    return 0;
}

const ai::VoicePreset& Preset(int index) {
    size_t n = 0;
    const ai::VoicePreset* presets = ai::VoicePresets(n);
    return presets[static_cast<size_t>(std::clamp(index, 0, static_cast<int>(n) - 1))];
}

std::wstring ServiceName(std::string_view id, const std::wstring& base_url) {
    if (id == "siliconflow") return Text(I::AiVoiceSiliconFlow);
    if (id == "openai") return L"OpenAI";
    const std::wstring host = ai::ModelHost(base_url);
    return host.empty() ? Text(I::AiVoiceCustom) : host;
}

// 硅基流动 · SenseVoiceSmall
std::wstring ConnName(const AiVoiceUi& u) {
    return ServiceName(u.prefs.service, json::Wide(u.prefs.base_url)) + L" \u00B7 " + ai::ShortVoiceModel(u.prefs.model);
}

int PauseIndex(int pause) { return pause == 2 ? 1 : pause == 0 ? 2 : 0; }

std::wstring MicName(const AiVoiceUi& u) {
    const std::wstring want = json::Wide(u.prefs.mic);
    for (const ai::VoiceMic& m : u.mics)
        if (!want.empty() && m.id == want) return m.name;
    for (const ai::VoiceMic& m : u.mics)
        if (m.id == u.default_mic) return m.name;
    return u.mics.empty() ? std::wstring() : u.mics.front().name;
}

// --test-instance only: a WAV file plays as the microphone.
std::wstring TestWav(const AppState& s) {
    if (!s.isolatedTest) return {};
    wchar_t path[MAX_PATH]{};
    const DWORD n = GetEnvironmentVariableW(L"PULSE_VOICE_TEST_WAV", path, MAX_PATH);
    return n > 0 && n < MAX_PATH ? std::wstring(path, n) : std::wstring();
}

void CALLBACK TimerProc(HWND hwnd, UINT, UINT_PTR id, DWORD);

void EnsureTimer(AppState& s, AiVoiceUi& u) {
    if (u.timer_on || !s.hwnd) return;
    u.timer_on = true;
    SetTimer(s.hwnd, kVoiceTimer, 40, TimerProc);
}

// ---- The cards in the conversation -------------------------------------

void RetireCards(AppState& s, AiVoiceUi& u) {
    if (u.cards.empty() || !s.aiAssistant.chat) {
        u.cards.clear();
        return;
    }
    AiChat& c = Chat(s);
    const uint64_t now = Now();
    for (const AiVoiceUi::Card& card : u.cards)
        if (ui::AiBlockView* b = c.Find(card.id); b && !b->gone_ms) {
            b->gone_ms = now;
            u.retiring.push_back({card.id, now});
        }
    u.cards.clear();
    EnsureTimer(s, u);
}

ui::AiErrButton Button(ui::AiErrAct act, I text, bool primary) {
    ui::AiErrButton b;
    b.act = act;
    b.icon = ui::AiIcon::Count;   // plain (.b2)
    b.text = Text(text);
    b.primary = primary;
    return b;
}

ui::AiBlockView& AddCard(AppState& s, AiVoiceUi& u, ui::AiErrTone tone, bool need, std::wstring uri) {
    RetireCards(s, u);
    AiChat& c = Chat(s);
    ui::AiBlockView& b = c.Add(ui::AiBlockKind::Error, Now());
    b.tone = tone;
    b.icon = ui::AiIcon::Mic;
    u.cards.push_back({b.id, need, std::move(uri)});
    s.renderer.AiPanel().ScrollToEnd();
    Redraw(s);
    return b;
}

void NeedCard(AppState& s, AiVoiceUi& u) {
    // Already asking: leave it where it is.
    if (!u.cards.empty() && u.cards.back().need && s.aiAssistant.chat && Chat(s).Find(u.cards.back().id)) {
        s.renderer.AiPanel().ScrollToEnd();
        return;
    }
    ui::AiBlockView& b = AddCard(s, u, ui::AiErrTone::Accent, true, {});
    b.title = Text(I::AiVoiceNeedTitle);
    b.sub = Text(I::AiVoiceNeedSub);
    b.actions.push_back(Button(ui::AiErrAct::VoiceAdd, I::AiVoiceNeedButton, true));
    b.note = Text(I::AiVoiceNeedNote);
}

// 没找到麦克风 / Windows 不允许 Pulse 使用麦克风: what was checked, and where to fix it.
void PrecheckCard(AppState& s, AiVoiceUi& u, ai::MicAccess access) {
    const bool no_mic = u.mics.empty();
    u.shake_at = Now();
    ui::AiBlockView& b = AddCard(s, u, ui::AiErrTone::Warn, false,
                                 no_mic ? L"ms-settings:sound" : L"ms-settings:privacy-microphone");
    b.title = Text(no_mic ? I::AiVoiceNoMicTitle : I::AiVoiceDeniedTitle);
    b.sub = Text(no_mic ? I::AiVoiceNoMicSub : I::AiVoiceDeniedSub);
    if (no_mic) b.checklist.push_back({Text(I::AiVoiceCheckNoMic), true});
    else b.checklist.push_back({Fill(Text(I::AiVoiceCheckMic), L"{name}", MicName(u)), false});
    if (access == ai::MicAccess::DeniedAll) b.checklist.push_back({Text(I::AiVoiceCheckAll), true});
    else if (access == ai::MicAccess::DeniedDesktop) b.checklist.push_back({Text(I::AiVoiceCheckDesktop), true});
    b.checklist.push_back({Fill(Text(I::AiVoiceCheckConn), L"{name}", ServiceName(u.prefs.service, json::Wide(u.prefs.base_url))),
                           false});
    b.hint = Text(no_mic ? I::AiVoiceNoMicHint : I::AiVoiceDeniedHint);
    b.hint_icon = ui::AiIcon::WinSettings;
    b.actions.push_back(Button(ui::AiErrAct::VoiceSettings, I::AiVoiceOpenSettings, true));
    b.actions.push_back(Button(ui::AiErrAct::VoiceRetry, I::AiVoiceRetry, false));
    b.note = Text(I::AiVoiceTypeInstead);
}

void FailCard(AppState& s, AiVoiceUi& u, const ai::VoiceAnswer& answer) {
    ui::AiBlockView& b = AddCard(s, u, ui::AiErrTone::Warn, false, {});
    b.title = Text(I::AiVoiceFailTitle);
    if (answer.http_status == 0) b.sub = Fill(Text(I::AiVoiceFailNet), L"{host}", u.host);
    else if (answer.http_status == 401) b.sub = Text(I::AiVoiceFailKey);
    else b.sub = Fill(Text(I::AiVoiceFailOther), L"{code}", std::to_wstring(answer.http_status));
    b.actions.push_back(Button(ui::AiErrAct::VoiceRetry, I::AiVoiceSayAgain, true));
    b.actions.push_back(Button(ui::AiErrAct::VoiceManage, I::AiAsstManage, false));
}

void MicFailCard(AppState& s, AiVoiceUi& u, HRESULT hr) {
    u.shake_at = Now();
    wchar_t code[16]{};
    swprintf_s(code, L"0x%08X", static_cast<unsigned>(hr));
    ui::AiBlockView& b = AddCard(s, u, ui::AiErrTone::Warn, false, L"ms-settings:sound");
    b.title = Text(I::AiVoiceMicFailTitle);
    b.sub = Fill(Text(I::AiVoiceMicFailSub), L"{code}", code);
    b.actions.push_back(Button(ui::AiErrAct::VoiceRetry, I::AiVoiceRetry, true));
    b.actions.push_back(Button(ui::AiErrAct::VoiceSettings, I::AiVoiceOpenSettings, false));
    b.note = Text(I::AiVoiceTypeInstead);
}

void Note(AppState& s, std::wstring text) {
    ui::AiBlockView& note = Chat(s).Add(ui::AiBlockKind::Note, Now());
    note.text = std::move(text);
    s.renderer.AiPanel().ScrollToEnd();
}

// ---- Listening ----------------------------------------------------------

void StopRec(AiVoiceUi& u) {
    if (u.rec) u.rec->Stop();   // the sentence in progress still comes out
}

void Upload(AiVoiceUi& u, std::vector<int16_t> pcm) {
    ai::VoiceEndpoint endpoint;
    endpoint.base_url = json::Wide(u.prefs.base_url);
    endpoint.model = u.prefs.model;
    if (!ai::LoadVoiceKey(endpoint.key) || pcm.empty()) {
        SecureZeroMemory(pcm.data(), pcm.size() * sizeof(int16_t));
        return;
    }
    auto job = std::make_shared<AiVoiceUi::Job>();
    u.jobs.push_back(job);
    std::thread([job, endpoint = std::move(endpoint), pcm = std::move(pcm)]() mutable {
        std::string wav = ai::EncodeVoiceWav(pcm);
        SecureZeroMemory(pcm.data(), pcm.size() * sizeof(int16_t));
        job->answer = ai::TranscribeVoice(std::move(endpoint), wav);   // wipes the key
        SecureZeroMemory(wav.data(), wav.size());
        job->done.store(true, std::memory_order_release);
    }).detach();
}

void Recognized(AppState& s, AiVoiceUi& u, const ai::VoiceAnswer& answer) {
    if (answer.Ok()) {
        const std::wstring words = Trim(json::Wide(answer.text));
        if (!words.empty()) AiAssistantAppendComposer(s, words);
        return;
    }
    // Once per session: stop and say why; the recording is gone either way.
    StopRec(u);
    if (u.failed) return;
    u.failed = true;
    FailCard(s, u, answer);
}

void Tick(AppState& s, AiVoiceUi& u) {
    u.last_tick = Now();
    bool redraw = false;
    if (u.rec) {
        u.snap = u.rec->Read();
        std::vector<int16_t> pcm;
        while (u.rec->PopSentence(pcm)) {
            Upload(u, std::move(pcm));
            pcm.clear();
        }
        if (u.snap.state == ai::VoiceRecorder::State::Failed) {
            const HRESULT hr = u.snap.error;
            u.rec.reset();
            if (hr == E_ACCESSDENIED) {
                u.mics = ai::ListVoiceMics(&u.default_mic);
                const ai::MicAccess access = ai::ReadMicAccess();
                PrecheckCard(s, u, access == ai::MicAccess::Allowed ? ai::MicAccess::DeniedDesktop : access);
            } else {
                MicFailCard(s, u, hr);
            }
        } else if (u.snap.state == ai::VoiceRecorder::State::Ended) {
            u.rec.reset();
        }
        redraw = true;
    }
    while (!u.jobs.empty() && u.jobs.front()->done.load(std::memory_order_acquire)) {
        const std::shared_ptr<AiVoiceUi::Job> job = u.jobs.front();
        u.jobs.pop_front();
        Recognized(s, u, job->answer);
        redraw = true;
    }
    if (u.test_job && u.test_job->done.load(std::memory_order_acquire)) {
        const std::shared_ptr<AiVoiceUi::TestJob> job = std::move(u.test_job);
        if (job->gen == u.gen) {
            const ai::VoiceAnswer& list = job->list;
            const ai::VoiceAnswer& run = job->run;
            u.listed = list.Ok();
            if (list.Ok()) u.models = list.models;
            if (list.Ok() && !job->model.empty()) u.model = job->model;
            const ai::VoiceAnswer& bad = !list.Ok() ? list : run;
            if (list.Ok() && list.models.empty()) {
                u.test = ui::AiVoiceTest::Bad;
                u.test_error = Text(I::AiVoiceErrNoModel);
            } else if (list.Ok() && run.Ok()) {
                u.test = ui::AiVoiceTest::Ok;
                u.test_ms = run.ms;
            } else {
                u.test = ui::AiVoiceTest::Bad;
                const std::wstring host = ai::ModelHost(Trim(EditText(u.url_edit)));
                if (bad.http_status == 0) u.test_error = Fill(Text(I::AiVoiceErrNet), L"{host}", host);
                else if (bad.http_status == 401) u.test_error = Text(I::AiVoiceErrKey);
                else {
                    // Cut the service's message in UTF-16, never inside a UTF-8
                    // sequence or a surrogate pair.
                    std::wstring msg = json::Wide(bad.error_message);
                    if (msg.size() > 120) {
                        size_t n = 119;
                        if (IS_HIGH_SURROGATE(msg[n - 1])) --n;
                        msg = msg.substr(0, n) + L"\u2026";
                    }
                    u.test_error = Fill(Fill(Text(I::AiVoiceErrHttp), L"{code}", std::to_wstring(bad.http_status)), L"{msg}",
                                        msg);
                }
            }
        }
        redraw = true;
    }
    if (!u.retiring.empty()) {
        const uint64_t now = Now();
        AiChat* c = s.aiAssistant.chat.get();
        for (auto it = u.retiring.begin(); it != u.retiring.end();) {
            if (now - it->second < kGoneMs) { ++it; continue; }
            if (c) {
                const uint32_t id = it->first;
                c->blocks.erase(std::remove_if(c->blocks.begin(), c->blocks.end(),
                                               [id](const ui::AiBlockView& b) { return b.id == id; }),
                                c->blocks.end());
            }
            it = u.retiring.erase(it);
            redraw = true;
        }
    }
    if (!u.rec && u.jobs.empty() && !u.test_job && u.retiring.empty() && s.hwnd) {
        KillTimer(s.hwnd, kVoiceTimer);
        u.timer_on = false;
    }
    if (redraw) Redraw(s);
}

void CALLBACK TimerProc(HWND hwnd, UINT, UINT_PTR id, DWORD) {
    auto* s = reinterpret_cast<AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!s || !s->aiAssistant.voice) {
        KillTimer(hwnd, id);
        return;
    }
    Tick(*s, *s->aiAssistant.voice);
}

void MicClick(AppState& s, AiVoiceUi& u) {
    if (u.rec) {
        StopRec(u);
        Redraw(s);
        return;
    }
    if (!Connected(u)) {
        NeedCard(s, u);
        return;
    }
    const std::wstring test_wav = TestWav(s);
    u.mics = ai::ListVoiceMics(&u.default_mic);
    const ai::MicAccess access = ai::ReadMicAccess();
    if (test_wav.empty() && (u.mics.empty() || access != ai::MicAccess::Allowed)) {
        PrecheckCard(s, u, access);
        return;
    }
    RetireCards(s, u);
    ai::VoiceRecorder::Options options;
    options.mic_id = json::Wide(u.prefs.mic);
    options.pause_ms = u.prefs.pause * 1000;
    options.test_wav = test_wav;
    u.rec = ai::VoiceRecorder::Start(std::move(options));
    u.snap = {};
    u.failed = false;
    u.shake_at = 0;
    u.host = ai::ModelHost(json::Wide(u.prefs.base_url));
    EnsureTimer(s, u);
    Redraw(s);
}

// ---- The connection card ------------------------------------------------

bool Typed(const AiVoiceUi& u) { return u.key_len > 0; }

std::wstring FieldUrl(const AiVoiceUi& u) {
    std::wstring url = Trim(EditText(u.url_edit));
    while (!url.empty() && url.back() == L'/') url.pop_back();
    if (!url.empty() && url.find(L"://") == std::wstring::npos) url = L"https://" + url;
    return url;
}

void ReadKey(AiVoiceUi& u) {
    std::wstring key = Trim(EditText(u.key_edit));
    u.key_len = static_cast<int>(key.size());
    u.key_tail = key.size() > 4 ? key.substr(key.size() - 4) : std::wstring();
    Wipe(key);
}

void ResetTest(AppState& s, AiVoiceUi& u) {
    ++u.gen;
    u.test_job.reset();
    u.test = ui::AiVoiceTest::None;
    u.test_error.clear();
    u.test_due = 0;
    if (s.hwnd) KillTimer(s.hwnd, kVoiceTestTimer);
}

void StartTest(AppState& s, AiVoiceUi& u) {
    ai::VoiceEndpoint endpoint;
    endpoint.base_url = FieldUrl(u);
    if (Typed(u)) endpoint.key = Trim(EditText(u.key_edit));
    else if (u.manage) (void)ai::LoadVoiceKey(endpoint.key);
    if (endpoint.key.empty() || endpoint.base_url.empty()) return;
    ResetTest(s, u);
    u.test = ui::AiVoiceTest::Running;
    auto job = std::make_shared<AiVoiceUi::TestJob>();
    job->gen = u.gen;
    u.test_job = job;
    const std::string service = Preset(u.service).id;
    const std::string chosen = u.model;
    std::thread([job, endpoint = std::move(endpoint), service, chosen]() mutable {
        ai::VoiceEndpoint copy = endpoint;   // the list call wipes its key
        job->list = ai::ListVoiceModels(std::move(copy));
        if (job->list.Ok() && !job->list.models.empty()) {
            const auto& m = job->list.models;
            job->model = std::find(m.begin(), m.end(), chosen) != m.end() ? chosen : ai::PickVoiceModel(service, m);
            endpoint.model = job->model;
            std::vector<int16_t> clip = ai::VoiceTestClip();
            job->run = ai::TranscribeVoice(std::move(endpoint), ai::EncodeVoiceWav(clip));
        } else {
            Wipe(endpoint.key);
        }
        job->done.store(true, std::memory_order_release);
    }).detach();
    EnsureTimer(s, u);
    Redraw(s);
}

void RunTestIfDue(AppState& s, AiVoiceUi& u) {
    if (!u.test_due || Now() < u.test_due) return;
    u.test_due = 0;
    if (s.hwnd) KillTimer(s.hwnd, kVoiceTestTimer);
    if (u.card_open) StartTest(s, u);
}

void CALLBACK TestTimerProc(HWND hwnd, UINT, UINT_PTR id, DWORD) {
    auto* st = reinterpret_cast<AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!st || !st->aiAssistant.voice) {
        KillTimer(hwnd, id);
        return;
    }
    RunTestIfDue(*st, *st->aiAssistant.voice);
}

void ArmTest(AppState& s, AiVoiceUi& u) {
    ResetTest(s, u);
    const bool key = Typed(u) ? u.key_len >= 8 : u.manage;
    if (!key || FieldUrl(u).empty() || !s.hwnd) return;
    u.test_due = Now() + 600;
    SetTimer(s.hwnd, kVoiceTestTimer, 600, TestTimerProc);
}

void KeyEdited(AppState& s, AiVoiceUi& u) {
    const int old_len = u.key_len;
    const std::wstring old_tail = u.key_tail;
    ReadKey(u);
    if (old_len == u.key_len && old_tail == u.key_tail) return;
    if ((old_len == 0) != (u.key_len == 0) && u.key_edit) InvalidateRect(u.key_edit, nullptr, TRUE);
    ArmTest(s, u);
    Redraw(s);
}

void HideEdit(AppState& s, HWND edit, bool& shown) {
    if (!edit || !shown) return;
    const bool had_focus = GetFocus() == edit;
    ShowWindow(edit, SW_HIDE);
    shown = false;
    if (had_focus && s.hwnd) SetFocus(s.hwnd);
}

void CloseCard(AppState& s, AiVoiceUi& u) {
    if (!u.card_open) return;
    u.card_open = false;
    u.card_closed_at = Now();
    ResetTest(s, u);
    HideEdit(s, u.key_edit, u.key_shown);
    HideEdit(s, u.url_edit, u.url_shown);
    if (u.key_edit) SetWindowTextW(u.key_edit, L"");
    u.key_len = 0;
    u.key_tail.clear();
    u.key_focus_pending = u.url_focus_pending = false;
    s.aiAssistant.focus_pending = true;
    Redraw(s);
}

void Save(AppState& s, AiVoiceUi& u) {
    if (u.test != ui::AiVoiceTest::Ok || u.model.empty()) return;
    if (Typed(u)) {
        std::wstring key = Trim(EditText(u.key_edit));
        const bool ok = ai::SaveVoiceKey(key);
        Wipe(key);
        if (!ok) {
            u.test = ui::AiVoiceTest::Bad;
            u.test_error = Text(I::AiVoiceErrSave);
            Redraw(s);
            return;
        }
    }
    u.key_saved = ai::HasVoiceKey();
    const bool added = !u.manage || !Connected(u);
    u.prefs.service = Preset(u.service).id;
    u.prefs.base_url = json::Utf8(FieldUrl(u));
    u.prefs.model = u.model;
    SavePrefs(u);
    CloseCard(s, u);
    RetireCards(s, u);
    if (added) Note(s, Fill(Text(I::AiVoiceAdded), L"{name}", ConnName(u)));
    Redraw(s);
}

void Remove(AppState& s, AiVoiceUi& u) {
    StopRec(u);
    (void)ai::ClearVoiceKey();
    u.key_saved = false;
    u.prefs.service.clear();
    u.prefs.base_url.clear();
    u.prefs.model.clear();
    SavePrefs(u);
    CloseCard(s, u);
    Note(s, Text(I::AiVoiceRemoved));
}

void PaintPlaceholder(AppState& s, AiVoiceUi& u, HWND hwnd) {
    HDC hdc = GetDC(hwnd);
    if (!hdc) return;
    HideCaret(hwnd);
    RECT r{};
    SendMessageW(hwnd, EM_GETRECT, 0, reinterpret_cast<LPARAM>(&r));
    HFONT old = static_cast<HFONT>(SelectObject(hdc, u.font));
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, ToColorRef(s.renderer.AiPanel().PlaceholderColor()));
    const std::wstring text = hwnd == u.key_edit ? Text(I::AiVoiceKeyPlaceholder) : std::wstring(L"https://");
    DrawTextW(hdc, text.c_str(), static_cast<int>(text.size()), &r, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(hdc, old);
    ShowCaret(hwnd);
    ReleaseDC(hwnd, hdc);
}

LRESULT CALLBACK VoiceEditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
    AppState& s = *reinterpret_cast<AppState*>(ref);
    AiVoiceUi* up = Peek(s);
    if (!up) return DefSubclassProc(hwnd, msg, wp, lp);
    AiVoiceUi& u = *up;
    switch (msg) {
    case WM_GETDLGCODE:
        return DefSubclassProc(hwnd, msg, wp, lp) | DLGC_WANTALLKEYS;
    case WM_KEYDOWN:
        if (wp == VK_RETURN) {
            Save(s, u);
            return 0;
        }
        if (wp == VK_ESCAPE) {
            CloseCard(s, u);
            return 0;
        }
        if (wp == VK_TAB) {
            HWND next = hwnd == u.key_edit ? u.url_edit : u.key_edit;
            if (next && IsWindowVisible(next)) SetFocus(next);
            Redraw(s);
            return 0;
        }
        break;
    case WM_CHAR:
        if (wp == L'\r' || wp == L'\n' || wp == 27 || wp == L'\t') return 0;
        if (wp == 1) {
            SendMessageW(hwnd, EM_SETSEL, 0, -1);
            return 0;
        }
        break;
    case WM_SETFOCUS:
    case WM_KILLFOCUS: {
        const LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
        Redraw(s);
        return r;
    }
    case WM_PAINT: {
        const LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
        if (GetWindowTextLengthW(hwnd) == 0) PaintPlaceholder(s, u, hwnd);
        return r;
    }
    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, VoiceEditProc, kVoiceSubclass);
        break;
    default:
        break;
    }
    const LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
    switch (msg) {
    case WM_CHAR: case WM_KEYDOWN: case WM_PASTE: case WM_CUT: case WM_CLEAR: case WM_UNDO:
    case EM_UNDO: case WM_SETTEXT: case EM_REPLACESEL:
        if (hwnd == u.key_edit) {
            KeyEdited(s, u);
        } else if (msg != WM_KEYDOWN || wp == VK_DELETE) {
            if (GetWindowTextLengthW(hwnd) <= 1) InvalidateRect(hwnd, nullptr, TRUE);   // placeholder
            if (msg != WM_SETTEXT) ArmTest(s, u);   // a typed address: test it again
            Redraw(s);
        }
        break;
    default:
        break;
    }
    return r;
}

void EnsureFont(AppState& s, AiVoiceUi& u) {
    const float font_scale = ui::typography::UiFontScale();
    if (u.font && u.font_scale == s.scale * font_scale) return;
    if (u.font) DeleteObject(u.font);
    // .vc-i: 12.5 px in the panel's mono face (the drawn text is the same).
    const int height = -static_cast<int>(std::lround(12.5f * s.scale * font_scale));
    u.font = CreateFontW(height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                         CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Cascadia Mono");
    if (!u.font)
        u.font = CreateFontW(height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    u.font_scale = s.scale * font_scale;
    u.line_px = 0;
    for (HWND e : {u.key_edit, u.url_edit}) {
        if (!e || !u.font) continue;
        SendMessageW(e, WM_SETFONT, reinterpret_cast<WPARAM>(u.font), TRUE);
        SendMessageW(e, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
    }
    if (u.font && s.hwnd) {
        HDC hdc = GetDC(s.hwnd);
        HFONT old = static_cast<HFONT>(SelectObject(hdc, u.font));
        TEXTMETRICW tm{};
        GetTextMetricsW(hdc, &tm);
        SelectObject(hdc, old);
        ReleaseDC(s.hwnd, hdc);
        u.line_px = static_cast<int>(tm.tmHeight);
    }
}

HWND MakeEdit(AppState& s, bool password) {
    const DWORD style = WS_CHILD | WS_CLIPSIBLINGS | WS_TABSTOP | ES_AUTOHSCROLL | (password ? ES_PASSWORD : 0);
    HWND e = CreateWindowExW(WS_EX_LAYERED, L"EDIT", L"", style, 0, 0, 0, 0, s.hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!e) return nullptr;
    SetLayeredWindowAttributes(e, 0, 255, LWA_ALPHA);
    SetWindowTheme(e, L"", L"");
    SendMessageW(e, EM_LIMITTEXT, 512, 0);
    if (password) SendMessageW(e, EM_SETPASSWORDCHAR, kMaskChar, 0);
    SetWindowSubclass(e, VoiceEditProc, kVoiceSubclass, reinterpret_cast<DWORD_PTR>(&s));
    return e;
}

void EnsureEdits(AppState& s, AiVoiceUi& u) {
    if (!s.hwnd) return;
    if (!u.key_edit) u.key_edit = MakeEdit(s, true);
    if (!u.url_edit) u.url_edit = MakeEdit(s, false);
    u.font_scale = 0.0f;
    EnsureFont(s, u);
}

void Place(AppState& s, AiVoiceUi& u, HWND edit, bool& shown, int field, bool& focus_pending) {
    D2D1_RECT_F r{};
    if (!edit || !s.renderer.AiPanel().VoiceFieldRect(field, r)) {
        HideEdit(s, edit, shown);
        return;
    }
    const int line = (std::max)(u.line_px, 1);
    const int x = static_cast<int>(std::lround(r.left));
    const int w = (std::max)(1, static_cast<int>(std::lround(r.right - r.left)));
    const int y = static_cast<int>(std::lround((r.top + r.bottom) * 0.5f - static_cast<float>(line) * 0.5f));
    RECT cur{};
    GetWindowRect(edit, &cur);
    MapWindowPoints(nullptr, s.hwnd, reinterpret_cast<POINT*>(&cur), 2);
    const bool placed = cur.left == x && cur.top == y && cur.right - cur.left == w && cur.bottom - cur.top == line;
    if (!placed || !shown) {
        SetWindowPos(edit, HWND_TOP, x, y, w, line, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        shown = true;
        Redraw(s);
    }
    if (focus_pending) {
        focus_pending = false;
        if (GetForegroundWindow() == s.hwnd || GetForegroundWindow() == GetAncestor(s.hwnd, GA_ROOT)) {
            SetFocus(edit);
            const int n = GetWindowTextLengthW(edit);
            SendMessageW(edit, EM_SETSEL, n, n);
        }
    }
}

void PickService(AppState& s, AiVoiceUi& u, int index) {
    u.service = index;
    const ai::VoicePreset& p = Preset(index);
    if (u.url_edit) SetWindowTextW(u.url_edit, p.base_url ? p.base_url : L"");
    u.models.clear();
    u.listed = false;
    u.model.clear();
    if (!p.base_url || !*p.base_url) u.url_focus_pending = true;
    else if (!Typed(u) && !u.manage) u.key_focus_pending = true;
    ArmTest(s, u);
}

void OpenCard(AppState& s, AiVoiceUi& u, bool manage) {
    u.menu_open = false;
    u.menu_closed_at = Now();
    EnsureEdits(s, u);
    u.card_open = true;
    u.manage = manage && Connected(u);
    ResetTest(s, u);
    if (u.key_edit) SetWindowTextW(u.key_edit, L"");
    u.key_len = 0;
    u.key_tail.clear();
    u.saved_mask.clear();
    if (u.manage) {
        std::wstring key;
        if (ai::LoadVoiceKey(key)) u.saved_mask = ai::MaskModelKey(key);
        Wipe(key);
        u.service = PresetIndex(u.prefs.service);
        if (u.url_edit) SetWindowTextW(u.url_edit, json::Wide(u.prefs.base_url).c_str());
        u.model = u.prefs.model;
        u.models.clear();
        u.listed = false;
        StartTest(s, u);   // 已连接 · … again, and the list for the dropdown
    } else {
        u.service = 0;
        PickService(s, u, 0);
        u.key_focus_pending = true;
    }
    Redraw(s);
}

void PickModel(AppState& s, AiVoiceUi& u) {
    D2D1_RECT_F box{};
    if (!u.listed || u.models.empty() || !s.renderer.AiPanel().VoiceAnchor(P::VoiceModel, box) || !EnsureMenu(s)) return;
    std::vector<ui::FluentMenuItem> items;
    for (size_t i = 0; i < u.models.size() && i < 40; ++i) {
        ui::FluentMenuItem item;
        item.command = static_cast<int>(i) + 1;
        item.text = json::Wide(u.models[i]);
        item.checked = u.models[i] == u.model;
        items.push_back(std::move(item));
    }
    POINT c[] = {{static_cast<LONG>(std::lround(box.left)), static_cast<LONG>(std::lround(box.top))},
                 {static_cast<LONG>(std::lround(box.right)), static_cast<LONG>(std::lround(box.bottom))}};
    MapWindowPoints(s.hwnd, nullptr, c, 2);
    s.menu->SetTheme(s.darkMode, s.accentColor);
    const int cmd = s.menu->TrackDropdown({c[0].x, c[0].y, c[1].x, c[1].y}, std::move(items));
    if (cmd <= 0 || static_cast<size_t>(cmd) > u.models.size()) return;
    const std::string chosen = u.models[static_cast<size_t>(cmd) - 1];
    if (chosen == u.model) return;
    u.model = chosen;
    StartTest(s, u);   // the chosen one has to recognize too
}

void PickMic(AppState& s, AiVoiceUi& u) {
    D2D1_RECT_F box{};
    if (!s.renderer.AiPanel().VoiceAnchor(P::VoiceMicPick, box) || !EnsureMenu(s)) return;
    u.mics = ai::ListVoiceMics(&u.default_mic);
    std::vector<ui::FluentMenuItem> items;
    {
        ui::FluentMenuItem item;
        item.command = 1;
        item.text = Text(I::AiVoiceMicDefault);
        item.checked = u.prefs.mic.empty();
        item.separator_after = !u.mics.empty();
        items.push_back(std::move(item));
    }
    const std::wstring want = json::Wide(u.prefs.mic);
    for (size_t i = 0; i < u.mics.size(); ++i) {
        ui::FluentMenuItem item;
        item.command = static_cast<int>(i) + 2;
        item.text = u.mics[i].name;
        item.checked = !want.empty() && u.mics[i].id == want;
        items.push_back(std::move(item));
    }
    POINT c[] = {{static_cast<LONG>(std::lround(box.left)), static_cast<LONG>(std::lround(box.top))},
                 {static_cast<LONG>(std::lround(box.right)), static_cast<LONG>(std::lround(box.bottom))}};
    MapWindowPoints(s.hwnd, nullptr, c, 2);
    s.menu->SetTheme(s.darkMode, s.accentColor);
    const int cmd = s.menu->TrackDropdown({c[0].x, c[0].y, c[1].x, c[1].y}, std::move(items));
    if (cmd <= 0) return;
    const std::string mic = cmd == 1 || static_cast<size_t>(cmd - 2) >= u.mics.size()
                                ? std::string() : json::Utf8(u.mics[static_cast<size_t>(cmd - 2)].id);
    if (mic == u.prefs.mic) return;
    u.prefs.mic = mic;
    SavePrefs(u);
}

std::wstring Clock(uint32_t ms) {
    const uint32_t secs = ms / 1000;
    wchar_t text[16]{};
    swprintf_s(text, L"%u:%02u", secs / 60, secs % 60);
    return text;
}

} // namespace

void AiVoiceFill(AppState& s, ui::WindowViewModel& vm) {
    AiVoiceUi& u = Ui(s);
    ui::AiPanelViewModel& p = vm.ai_panel;
    ui::AiVoiceView& v = p.voice;
    v.available = true;
    const bool active = u.rec != nullptr;
    v.listening = active || !u.jobs.empty();
    v.menu_open = u.menu_open;
    v.shake_at = u.shake_at;
    if (active) {
        for (size_t i = 0; i < v.levels.size(); ++i) v.levels[i] = u.snap.levels[i];
    }
    v.time = Clock(u.snap.elapsed_ms);
    v.service = ConnName(u);
    v.hint = Text(u.prefs.pause == 0 ? I::AiVoiceHintManual : I::AiVoiceHint);
    v.recognizing = !u.jobs.empty();
    v.recognizing_text = Text(I::AiVoiceRecognizing);
    // The chip says it while Pulse 助手 is not working.
    if (v.listening && (!s.aiAssistant.chat || s.aiAssistant.chat->chip == ui::StatusAiState::Idle)) {
        vm.status.ai_state = ui::StatusAiState::Busy;
        vm.status.ai_text = Text(active ? I::AiVoiceListening : I::AiVoiceRecognizingChip);
    }
    if (u.menu_open) {
        auto m = std::make_shared<ui::AiVoiceMenuView>();
        const bool conn = Connected(u);
        m->heading = Text(I::AiVoiceHeading);
        m->service = conn ? ServiceName(u.prefs.service, json::Wide(u.prefs.base_url)) : Text(I::AiVoiceNoConn);
        m->service_sub = conn ? Fill(Text(I::AiVoiceConnected), L"{model}", ai::ShortVoiceModel(u.prefs.model))
                              : Text(I::AiVoiceNotConn);
        m->connected = conn;
        m->manage = Text(conn ? I::AiAsstManage : I::AiVoiceAdd);
        m->mic_label = Text(I::AiVoiceMicLabel);
        const std::wstring mic = MicName(u);
        m->mic_missing = mic.empty();
        m->mic_name = mic.empty() ? Text(I::AiVoiceMicNone) : mic;
        m->pause_heading = Text(I::AiVoicePauseHeading);
        m->pauses = {Text(I::AiVoicePause1), Text(I::AiVoicePause2), Text(I::AiVoicePauseManual)};
        m->pause = PauseIndex(u.prefs.pause);
        m->note = Text(I::AiVoiceMenuNote);
        v.menu = std::move(m);
    }
    if (u.card_open) {
        auto c = std::make_shared<ui::AiVoiceCardView>();
        c->title = Text(u.manage ? I::AiVoiceCardManage : I::AiVoiceCardAdd);
        c->sub = Text(I::AiVoiceCardSub);
        c->service_label = Text(I::AiVoiceService);
        c->services = {Text(I::AiVoiceSiliconFlow), std::wstring(L"OpenAI"), Text(I::AiVoiceCustom)};
        c->service = u.service;
        c->url_label = Text(I::AiVoiceUrl);
        c->url_text = EditText(u.url_edit);
        c->url_placeholder = L"https://";
        c->url_focused = u.url_edit && (GetFocus() == u.url_edit || u.url_focus_pending);
        c->url_edit = true;
        c->key_label = L"API key";
        c->key_note = Text(I::AiVoiceKeyNote);
        c->key_focused = u.key_edit && (GetFocus() == u.key_edit || u.key_focus_pending);
        // A stored key shows masked until the field is clicked.
        c->key_edit = c->key_focused || Typed(u) || u.saved_mask.empty();
        if (!c->key_edit) {
            c->key_text = u.saved_mask;
        } else if (Typed(u)) {
            const int dots = u.key_len > 4 ? (std::min)(u.key_len - 4, 24) : u.key_len;
            c->key_text.assign(static_cast<size_t>(dots), kMaskChar);
            c->key_text += u.key_tail;
        }
        c->key_placeholder = Text(I::AiVoiceKeyPlaceholder);
        c->model_label = Text(I::AiVoiceModel);
        if (u.listed && !u.model.empty()) c->model_note = Text(I::AiVoiceModelNote);
        c->model = json::Wide(u.model);
        c->model_placeholder = Text(I::AiVoiceModelPlaceholder);
        c->model_ready = u.listed && !u.models.empty();
        c->test = u.test;
        if (u.test == ui::AiVoiceTest::Running) {
            c->test_text = Text(I::AiVoiceTesting);
        } else if (u.test == ui::AiVoiceTest::Ok) {
            wchar_t secs[16]{};
            swprintf_s(secs, L"%.1f", static_cast<double>(u.test_ms) / 1000.0);
            c->test_text = Fill(Text(I::AiVoiceTestOk), L"{s}", secs);
        } else if (u.test == ui::AiVoiceTest::Bad) {
            c->test_text = u.test_error;
        }
        c->privacy = Text(I::AiVoicePrivacy);
        if (u.manage) c->remove = Text(I::AiVoiceRemove);
        c->cancel = Text(I::AiVoiceCancel);
        c->save = Text(I::AiVoiceSave);
        c->can_save = u.test == ui::AiVoiceTest::Ok && !u.model.empty();
        v.card = std::move(c);
    }
    if (u.shake_at && Now() - u.shake_at < 800) Redraw(s);   // the shake plays out
}

void AiVoiceOpenConnection(AppState& s) {
    AiVoiceUi& u = Ui(s);
    if (u.rec) return;
    Reload(u);
    OpenCard(s, u, Connected(u));
    Redraw(s);
}

bool AiVoiceActivate(AppState& s, const ui::AiPanelHit& hit) {
    AiVoiceUi& u = Ui(s);
    switch (hit.part) {
    case P::Mic:
        if (u.menu_open) AiVoiceCloseMenu(s);
        Reload(u);
        MicClick(s, u);
        return true;
    case P::MicMenu:
        if (u.menu_open) {
            AiVoiceCloseMenu(s);
        } else if (!u.rec) {
            Reload(u);
            u.mics = ai::ListVoiceMics(&u.default_mic);
            u.menu_open = true;
        }
        Redraw(s);
        return true;
    case P::VoiceMenu:
        return true;
    case P::VoiceManage:
        OpenCard(s, u, Connected(u));
        return true;
    case P::VoiceMicPick:
        PickMic(s, u);
        Redraw(s);
        return true;
    case P::VoicePause:
        if (hit.index >= 0 && hit.index < 3) {
            constexpr int kPause[] = {1, 2, 0};
            if (u.prefs.pause != kPause[hit.index]) {
                u.prefs.pause = kPause[hit.index];
                SavePrefs(u);
            }
        }
        Redraw(s);
        return true;
    case P::VoiceCard:
        return true;
    case P::VoiceClose:
    case P::VoiceCancel:
        CloseCard(s, u);
        return true;
    case P::VoiceService:
        if (hit.index >= 0 && hit.index < 3 && hit.index != u.service) PickService(s, u, hit.index);
        Redraw(s);
        return true;
    case P::VoiceUrlField:
        u.url_focus_pending = true;
        Redraw(s);
        return true;
    case P::VoiceKeyField:
        u.key_focus_pending = true;
        Redraw(s);
        return true;
    case P::VoiceModel:
        PickModel(s, u);
        Redraw(s);
        return true;
    case P::VoiceSave:
        Save(s, u);
        return true;
    case P::VoiceRemove:
        Remove(s, u);
        return true;
    default:
        return false;
    }
}

bool AiVoiceErrAct(AppState& s, ui::AiErrAct act, int index) {
    if (act != ui::AiErrAct::VoiceAdd && act != ui::AiErrAct::VoiceSettings && act != ui::AiErrAct::VoiceRetry &&
        act != ui::AiErrAct::VoiceManage)
        return false;
    AiVoiceUi& u = Ui(s);
    const AiChat& c = Chat(s);
    const size_t block = index < 0 ? c.blocks.size() : static_cast<size_t>(index / 4);
    const uint32_t id = block < c.blocks.size() ? c.blocks[block].id : 0;
    const auto card = std::find_if(u.cards.begin(), u.cards.end(), [id](const AiVoiceUi::Card& k) { return k.id == id; });
    switch (act) {
    case ui::AiErrAct::VoiceAdd:
        OpenCard(s, u, false);
        break;
    case ui::AiErrAct::VoiceManage:
        OpenCard(s, u, true);
        break;
    case ui::AiErrAct::VoiceSettings:
        if (card != u.cards.end() && !card->uri.empty())
            ShellExecuteW(s.hwnd, L"open", card->uri.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        break;
    case ui::AiErrAct::VoiceRetry:
        RetireCards(s, u);
        MicClick(s, u);
        break;
    default:
        break;
    }
    Redraw(s);
    return true;
}

bool AiVoiceCovers(const AppState& s) {
    const AiVoiceUi* u = Peek(s);
    if (!u) return false;
    const uint64_t now = Now();
    return u->menu_open || u->card_open || u->rec || !u->jobs.empty() || now - u->menu_closed_at < kMenuFadeMs ||
           now - u->card_closed_at < kCardFadeMs;
}

void AiVoiceRunDue(AppState& s) {
    AiVoiceUi* u = Peek(s);
    if (!u) return;
    RunTestIfDue(s, *u);
    if (u->timer_on && Now() - u->last_tick >= 40) Tick(s, *u);
}

bool AiVoiceBusy(const AppState& s) {
    const AiVoiceUi* u = Peek(s);
    return u && (u->rec || !u->jobs.empty());
}

bool AiVoiceMenuOpen(const AppState& s) {
    const AiVoiceUi* u = Peek(s);
    return u && u->menu_open;
}

void AiVoiceCloseMenu(AppState& s) {
    AiVoiceUi* u = Peek(s);
    if (!u || !u->menu_open) return;
    u->menu_open = false;
    u->menu_closed_at = Now();
    Redraw(s);
}

bool AiVoiceEscape(AppState& s) {
    AiVoiceUi* u = Peek(s);
    if (!u) return false;
    if (u->card_open) {
        CloseCard(s, *u);
        return true;
    }
    if (u->menu_open) {
        AiVoiceCloseMenu(s);
        return true;
    }
    if (u->rec) {
        StopRec(*u);
        Redraw(s);
        return true;
    }
    return false;
}

void AiVoiceStop(AppState& s) {
    AiVoiceUi* u = Peek(s);
    if (!u) return;
    StopRec(*u);
    if (u->menu_open) AiVoiceCloseMenu(s);
    if (u->card_open) CloseCard(s, *u);
}

void AiVoiceLayout(AppState& s) {
    AiVoiceUi* up = Peek(s);
    if (!up) return;
    AiVoiceUi& u = *up;
    if (!u.card_open || !s.aiAssistant.open) {
        HideEdit(s, u.key_edit, u.key_shown);
        HideEdit(s, u.url_edit, u.url_shown);
        if (u.card_open && !s.aiAssistant.open) CloseCard(s, u);
        // The composer comes back after the fades.
        const uint64_t now = Now();
        if (now - u.menu_closed_at < kMenuFadeMs || now - u.card_closed_at < kCardFadeMs) Redraw(s);
        return;
    }
    EnsureFont(s, u);
    Place(s, u, u.url_edit, u.url_shown, 0, u.url_focus_pending);
    Place(s, u, u.key_edit, u.key_shown, 1, u.key_focus_pending);
}

bool AiVoiceHasTip(ui::AiPanelPart part) { return part == P::Mic || part == P::MicMenu || part == P::VoiceClose; }

bool AiVoiceTip(const AppState& s, ui::AiPanelPart part, std::wstring& text) {
    const AiVoiceUi* u = Peek(s);
    switch (part) {
    case P::Mic: text = Text(I::AiVoiceMic); return !(u && u->menu_open);
    case P::MicMenu: text = Text(I::AiVoiceOptions); return !(u && u->menu_open);
    case P::VoiceClose: text = Text(I::AiVoiceCancel); return u && u->card_open;
    default: return false;
    }
}

bool AiVoiceIsField(ui::AiPanelPart part) { return part == P::VoiceUrlField || part == P::VoiceKeyField; }

HBRUSH AiVoiceCtlColor(AppState& s, HDC hdc, HWND edit) {
    AiVoiceUi* u = Peek(s);
    if (!u || !edit || (edit != u->key_edit && edit != u->url_edit)) return nullptr;
    const ui::AiPanelView& view = s.renderer.AiPanel();
    const COLORREF back = ToColorRef(view.BgColor());
    if (!u->brush || u->brush_color != back) {
        if (u->brush) DeleteObject(u->brush);
        u->brush = CreateSolidBrush(back);
        u->brush_color = back;
    }
    SetTextColor(hdc, ToColorRef(view.TextColor()));
    SetBkColor(hdc, back);
    SetBkMode(hdc, OPAQUE);
    return u->brush;
}

void AiVoiceDpiChanged(AppState& s) {
    AiVoiceUi* u = Peek(s);
    if (!u) return;
    u->font_scale = 0.0f;
    EnsureFont(s, *u);
    Redraw(s);
}

void AiVoiceDestroy(AppState& s) {
    AiVoiceUi* u = Peek(s);
    if (!u) return;
    if (u->rec) u->rec->Stop();
    u->rec.reset();
    u->jobs.clear();
    ++u->gen;
    u->test_job.reset();
    if (s.hwnd) {
        KillTimer(s.hwnd, kVoiceTimer);
        KillTimer(s.hwnd, kVoiceTestTimer);
    }
    u->timer_on = false;
    if (u->key_edit) {
        SetWindowTextW(u->key_edit, L"");
        DestroyWindow(u->key_edit);
    }
    if (u->url_edit) DestroyWindow(u->url_edit);
    u->key_edit = u->url_edit = nullptr;
    if (u->font) DeleteObject(u->font);
    u->font = nullptr;
    if (u->brush) DeleteObject(u->brush);
    u->brush = nullptr;
}

} // namespace pulse
