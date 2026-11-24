// ai_context_memory_test.cpp - Tests for Batch 5: context compaction and local habit memory.
#include "../app/ai_context_memory.h"
#include "../common/json_value.h"

#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <string>

namespace {

int g_pass = 0;
int g_fail = 0;

void Check(bool ok, const char* name) {
    if (ok) ++g_pass;
    else {
        ++g_fail;
        std::fprintf(stderr, "[FAIL] %s\n", name);
    }
}

pulse::json::Value MakeMsg(const char* role, const std::string& content) {
    pulse::json::Value m = pulse::json::Value::Object();
    m.set("role", role);
    m.set("content", content);
    return m;
}

} // namespace

int main() {
    using namespace pulse::app::ai;
    namespace json = pulse::json;
    namespace fs = std::filesystem;

    // 1. Token estimation
    {
        const json::Value ascii = MakeMsg("user", std::string(400, 'a'));
        const size_t t_ascii = EstimateMessageTokens(ascii);
        Check(t_ascii >= 100 && t_ascii <= 120, "EstimateMessageTokens ASCII ~108");

        json::Value multimodal = json::Value::Object();
        multimodal.set("role", "user");
        json::Value parts = json::Value::Array();
        json::Value p1 = json::Value::Object();
        p1.set("type", "text");
        p1.set("text", "look");
        json::Value p2 = json::Value::Object();
        p2.set("type", "image_url");
        json::Value iu = json::Value::Object();
        iu.set("url", "data:image/jpeg;base64," + std::string(100000, 'A'));
        p2.set("image_url", std::move(iu));
        parts.push(std::move(p1));
        parts.push(std::move(p2));
        multimodal.set("content", std::move(parts));
        const size_t t_img = EstimateMessageTokens(multimodal);
        Check(t_img >= 750 && t_img <= 900, "EstimateMessageTokens counts image as ~765 tokens not base64 bytes");
    }

    // 2. Stage 1 compaction: strips old reasoning_content, shrinks old tool output, drops old base64 images,
    //    while preserving active-turn reasoning_content & active tool outputs intact!
    {
        json::Value hist = json::Value::Array();
        // Turn 1 (completed): user with image -> assistant with reasoning -> big tool -> assistant answer
        {
            json::Value u1 = json::Value::Object();
            u1.set("role", "user");
            json::Value parts = json::Value::Array();
            json::Value p1 = json::Value::Object();
            p1.set("type", "text");
            p1.set("text", "Check this screenshot");
            json::Value p2 = json::Value::Object();
            p2.set("type", "image_url");
            json::Value iu = json::Value::Object();
            iu.set("url", "data:image/jpeg;base64,AAAA");
            p2.set("image_url", std::move(iu));
            parts.push(std::move(p1));
            parts.push(std::move(p2));
            u1.set("content", std::move(parts));
            hist.push(std::move(u1));
        }
        {
            json::Value a1 = MakeMsg("assistant", "");
            a1.set("reasoning_content", "Old thinking from turn 1");
            hist.push(std::move(a1));
        }
        {
            json::Value t1 = MakeMsg("tool", std::string(4000, 'x'));
            t1.set("tool_call_id", "call_1");
            hist.push(std::move(t1));
        }
        hist.push(MakeMsg("assistant", "Turn 1 answer."));

        // Turn 2 (active): user -> assistant with active reasoning -> active large tool
        hist.push(MakeMsg("user", "Now check Downloads"));
        {
            json::Value a2 = MakeMsg("assistant", "");
            a2.set("reasoning_content", "Active turn 2 reasoning MUST stay");
            hist.push(std::move(a2));
        }
        {
            json::Value t2 = MakeMsg("tool", std::string(3000, 'y'));
            t2.set("tool_call_id", "call_2");
            hist.push(std::move(t2));
        }

        bool compacted = false;
        const json::Value out = CompactHistoryForRequest(hist, 48000, compacted);
        Check(compacted, "Stage 1 sets compacted=true");
        Check(out.size() == hist.size(), "Stage 1 keeps message count when under budget");
        Check(out.at(0).find("content")->is_string() &&
                  out.at(0).get_string("content").find("omitted from context") != std::string::npos,
              "Stage 1 flattens old image_url");
        Check(out.at(1).find("reasoning_content") == nullptr, "Stage 1 strips completed-turn reasoning_content");
        Check(out.at(2).get_string("content").size() < 1200 &&
                  out.at(2).get_string("content").find("earlier tool output compacted") != std::string::npos,
              "Stage 1 compacts old large tool payload");
        Check(out.at(5).get_string("reasoning_content") == "Active turn 2 reasoning MUST stay",
              "Stage 1 preserves active-turn reasoning_content");
        Check(out.at(6).get_string("content").size() == 3000, "Stage 1 preserves active-turn tool payload");
    }

    // 3. Stage 2 sliding digest when exceeding token budget
    {
        json::Value hist = json::Value::Array();
        for (int i = 0; i < 10; ++i) {
            hist.push(MakeMsg("user", "Turn " + std::to_string(i) + " request: " + std::string(600, 'u')));
            hist.push(MakeMsg("assistant", "Turn " + std::to_string(i) + " reply: " + std::string(600, 'a')));
        }
        hist.push(MakeMsg("user", "Latest question about current folder"));

        const size_t before = EstimateHistoryTokens(hist);
        bool compacted = false;
        const json::Value out = CompactHistoryForRequest(hist, 1200, compacted);
        const size_t after = EstimateHistoryTokens(out);
        Check(compacted, "Stage 2 sets compacted=true");
        Check(after < before, "Stage 2 reduces token estimate significantly");
        Check(out.size() < hist.size(), "Stage 2 folds older messages");
        Check(out.at(0).get_string("content").find("Pulse earlier conversation summary") != std::string::npos,
              "Stage 2 emits structured summary message");
        Check(out.at(out.size() - 1).get_string("content") == "Latest question about current folder",
              "Stage 2 preserves latest user turn verbatim");
    }

    // 4. Local habit memory CRUD + dedup + prompt formatting in isolated temp dir
    wchar_t tmp_buf[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmp_buf);
    const fs::path dir = fs::path(tmp_buf) / (L"pulse_ai_memory_test_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    {
        Check(LoadMemories(dir.wstring()).empty(), "Fresh memory dir is empty");
        MemoryItem m1, m2, dup;
        Check(AddMemory("\xE5\x8F\x91\xE7\xA5\xA8\xE9\xBB\x98\xE8\xAE\xA4\xE5\xBD\x92\xE6\xA1\xA3\xE5\x88\xB0 D:\\Finance\\2026",
                        m1, dir.wstring()),
              "AddMemory UTF-8 Chinese habit");
        Check(AddMemory("Prefer yyyyMMdd photo naming", m2, dir.wstring()), "AddMemory English habit");
        Check(AddMemory("prefer yyyymmdd photo naming", dup, dir.wstring()), "AddMemory dedup case-insensitive");
        Check(dup.id == m2.id, "AddMemory dedup returns existing id");

        const auto loaded = LoadMemories(dir.wstring());
        Check(loaded.size() == 2, "LoadMemories persists 2 distinct memories");

        const std::string prompt = FormatMemoriesForPrompt(loaded);
        Check(prompt.find("D:\\Finance\\2026") != std::string::npos &&
                  prompt.find("Prefer yyyyMMdd photo naming") != std::string::npos,
              "FormatMemoriesForPrompt includes all memories");

        Check(RemoveMemory(m1.id, dir.wstring()) == 1, "RemoveMemory by id");
        Check(LoadMemories(dir.wstring()).size() == 1, "1 memory remains after remove by id");

        Check(RemoveMemory("photo", dir.wstring()) == 1, "RemoveMemory by keyword substring");
        Check(LoadMemories(dir.wstring()).empty(), "0 memories remain");

        Check(AddMemory("Keep screenshots in Pictures\\Shots", m1, dir.wstring()), "Re-add before clear");
        Check(ClearMemories(dir.wstring()) && LoadMemories(dir.wstring()).empty(), "ClearMemories empties store");
    }

    fs::remove_all(dir, ec);
    std::printf("[SUMMARY] ai_context_memory_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
