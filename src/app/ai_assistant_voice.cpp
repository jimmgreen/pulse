// ai_assistant_voice.cpp - 语音输入: listening, the ordered recognition queue,
// the voice cards in the conversation and the 语音设置 menu. The connection
// itself is set up in Settings > AI 助手 (ai_assistant_settings.cpp).
#include "ai_assistant_voice.h"

#include "ai_assistant.h"
#include "ai_assistant_chat.h"
#include "ai_assistant_settings.h"
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
#include "../ui/ui_motion.h"
#include "../ui/ui_renderer.h"
#include "../ui/ui_view_model.h"

#include <shellapi.h>
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
};

namespace {

namespace ai = app::ai;
using I = l10n::StringId;
using P = ui::AiPanelPart;
constexpr UINT_PTR kVoiceTimer = 0x4157;       // listening, recognition, the test
constexpr uint64_t kMenuFadeMs = 240;
constexpr uint64_t kGoneMs = 260;

std::wstring Text(I id) { return l10n::Get(id).c_str(); }

std::wstring Fill(std::wstring text, std::wstring_view key, const std::wstring& value) {
    if (const size_t at = text.find(key); at != std::wstring::npos) text.replace(at, key.size(), value);
    return text;
}

void Redraw(AppState& s) {
    if (s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
}

uint64_t Now() { return ui::motion::NowMs(); }

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

// Settings (here or in another Pulse window) may have changed the connection
// since this one read ai.json: read it again when the user reaches for the
// mic, never mid-recording.
void Reload(AiVoiceUi& u) {
    if (u.rec) return;
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
    if (!u.rec && u.jobs.empty() && u.retiring.empty() && s.hwnd) {
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

// The service, key and model are set up in Settings > AI 助手.
void OpenVoiceSettings(AppState& s, AiVoiceUi& u) {
    if (u.menu_open) {
        u.menu_open = false;
        u.menu_closed_at = Now();
    }
    ShowAssistantVoiceSettings(s);
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
        m->manage = Text(I::AiVoiceSettings);
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
    if (u.shake_at && Now() - u.shake_at < 800) Redraw(s);   // the shake plays out
}

void AiVoicePrefsChanged(AppState& s) {
    AiVoiceUi* u = Peek(s);
    if (!u) return;   // read at first use
    Reload(*u);
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
        OpenVoiceSettings(s, u);
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
    case ui::AiErrAct::VoiceManage:
        OpenVoiceSettings(s, u);
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
    return u->menu_open || u->rec || !u->jobs.empty() || now - u->menu_closed_at < kMenuFadeMs;
}

void AiVoiceRunDue(AppState& s) {
    AiVoiceUi* u = Peek(s);
    if (!u) return;
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
}

void AiVoiceLayout(AppState& s) {
    const AiVoiceUi* u = Peek(s);
    // The composer comes back after the menu's fade.
    if (u && Now() - u->menu_closed_at < kMenuFadeMs) Redraw(s);
}

bool AiVoiceHasTip(ui::AiPanelPart part) { return part == P::Mic || part == P::MicMenu; }

bool AiVoiceTip(const AppState& s, ui::AiPanelPart part, std::wstring& text) {
    const AiVoiceUi* u = Peek(s);
    switch (part) {
    case P::Mic: text = Text(I::AiVoiceMic); return !(u && u->menu_open);
    case P::MicMenu: text = Text(I::AiVoiceOptions); return !(u && u->menu_open);
    default: return false;
    }
}

void AiVoiceDestroy(AppState& s) {
    AiVoiceUi* u = Peek(s);
    if (!u) return;
    if (u->rec) u->rec->Stop();
    u->rec.reset();
    u->jobs.clear();
    if (s.hwnd) KillTimer(s.hwnd, kVoiceTimer);
    u->timer_on = false;
}

} // namespace pulse
