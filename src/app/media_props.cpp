#include "media_props.h"
#include <propkey.h>
#include <propsys.h>
#include <shobjidl.h>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <algorithm>
#include <atomic>
#include <unordered_map>

namespace pulse::app::media {
namespace {

std::wstring ShellPath(const std::wstring& path) {
    // The shell parses no \\?\ paths (tabs keep the prefix).
    if (path.rfind(L"\\\\?\\UNC\\", 0) == 0) return L"\\\\" + path.substr(8);
    if (path.rfind(L"\\\\?\\", 0) == 0) return path.substr(4);
    return path;
}

std::wstring Text(IPropertyStore* store, const PROPERTYKEY& key) {
    PROPVARIANT v;
    PropVariantInit(&v);
    std::wstring out;
    if (SUCCEEDED(store->GetValue(key, &v))) {
        if (v.vt == VT_LPWSTR && v.pwszVal) {
            out = v.pwszVal;
        } else if (v.vt == (VT_VECTOR | VT_LPWSTR)) {
            // Several artists: "A; B", like File Explorer.
            for (ULONG i = 0; i < v.calpwstr.cElems; ++i) {
                if (!v.calpwstr.pElems[i] || !*v.calpwstr.pElems[i]) continue;
                if (!out.empty()) out += L"; ";
                out += v.calpwstr.pElems[i];
            }
        }
    }
    PropVariantClear(&v);
    while (!out.empty() && (out.back() == L' ' || out.back() == L'\0')) out.pop_back();
    return out;
}

uint64_t Number(IPropertyStore* store, const PROPERTYKEY& key) {
    PROPVARIANT v;
    PropVariantInit(&v);
    uint64_t out = 0;
    if (SUCCEEDED(store->GetValue(key, &v))) {
        if (v.vt == VT_UI4) out = v.ulVal;
        else if (v.vt == VT_UI8) out = v.uhVal.QuadPart;
        else if (v.vt == VT_I4 && v.lVal > 0) out = static_cast<uint64_t>(v.lVal);
        else if (v.vt == VT_FILETIME)
            out = (static_cast<uint64_t>(v.filetime.dwHighDateTime) << 32) | v.filetime.dwLowDateTime;
    }
    PropVariantClear(&v);
    return out;
}

uint32_t Narrow(uint64_t v) noexcept { return v > 0xFFFFFFFFull ? 0u : static_cast<uint32_t>(v); }

constexpr DWORD kNeverOpen = FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_OPEN |
                             FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_DIRECTORY;
constexpr size_t kCacheLimit = 60000;

struct Cached {
    uint64_t mtime = 0, size = 0;
    std::shared_ptr<const Props> props;
};

// One reader for the process. Its thread is detached and lives as long as the
// process; the state it shares is never freed.
struct Shared {
    std::mutex m;
    std::condition_variable wake;
    std::deque<Job> queue;
    size_t busy = 0;             // jobs taken off the queue, not yet stored
    std::atomic<uint64_t> revision{0};
    bool started = false;
    std::unordered_map<std::wstring, Cached> cache;
};

Shared& State() {
    static Shared* shared = new Shared();
    return *shared;
}

bool Current(const Shared& s, const Job& job) {
    const auto it = s.cache.find(job.path);
    return it != s.cache.end() && it->second.mtime == job.mtime && it->second.size == job.size;
}

void Run() {
    Shared& s = State();
    // COM stays initialised for the life of the process (the thread never ends).
    (void)CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // Low CPU and I/O priority: the list must stay smooth while this reads.
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    for (;;) {
        Job job;
        {
            std::unique_lock lock(s.m);
            s.wake.wait(lock, [&] { return !s.queue.empty(); });
            job = std::move(s.queue.front());
            s.queue.pop_front();
            ++s.busy;
        }
        auto props = std::make_shared<Props>(ReadProps(job));
        {
            std::lock_guard lock(s.m);
            if (s.cache.size() >= kCacheLimit) s.cache.clear();   // rare; values are re-read on demand
            s.cache[job.path] = Cached{job.mtime, job.size, std::move(props)};
            --s.busy;
            s.revision.fetch_add(1, std::memory_order_release);
        }
    }
}

}  // namespace

std::wstring CameraLabel(const std::wstring& make, const std::wstring& model) {
    if (model.empty()) return make;
    if (make.empty()) return model;
    // Most makers repeat their name in the model ("Canon EOS R5", "NIKON Z 6").
    const size_t n = make.size();
    if (model.size() >= n && CompareStringOrdinal(model.c_str(), static_cast<int>(n), make.c_str(),
                                                  static_cast<int>(n), TRUE) == CSTR_EQUAL)
        return model;
    // "NIKON CORPORATION" + "NIKON Z 6": the first word is enough.
    const size_t space = make.find(L' ');
    if (space != std::wstring::npos && model.size() >= space &&
        CompareStringOrdinal(model.c_str(), static_cast<int>(space), make.c_str(),
                             static_cast<int>(space), TRUE) == CSTR_EQUAL)
        return model;
    return make + L" " + model;
}

Props ReadProps(const Job& job) {
    Props out;
    if (job.attrs & kNeverOpen) return out;
    IPropertyStore* store = nullptr;
    if (FAILED(SHGetPropertyStoreFromParsingName(ShellPath(job.path).c_str(), nullptr, GPS_DEFAULT,
                                                 IID_PPV_ARGS(&store))) || !store)
        return out;
    switch (job.kind) {
    case Kind::Photo:
        out.taken = Number(store, PKEY_Photo_DateTaken);
        out.width = Narrow(Number(store, PKEY_Image_HorizontalSize));
        out.height = Narrow(Number(store, PKEY_Image_VerticalSize));
        out.camera = CameraLabel(Text(store, PKEY_Photo_CameraManufacturer), Text(store, PKEY_Photo_CameraModel));
        break;
    case Kind::Music:
        out.title = Text(store, PKEY_Title);
        out.artist = Text(store, PKEY_Music_Artist);
        if (out.artist.empty()) out.artist = Text(store, PKEY_Music_AlbumArtist);
        out.album = Text(store, PKEY_Music_AlbumTitle);
        out.track = Narrow(Number(store, PKEY_Music_TrackNumber));
        out.duration = Number(store, PKEY_Media_Duration);
        break;
    case Kind::Video:
        out.duration = Number(store, PKEY_Media_Duration);
        out.width = Narrow(Number(store, PKEY_Video_FrameWidth));
        out.height = Narrow(Number(store, PKEY_Video_FrameHeight));
        out.fps_milli = Narrow(Number(store, PKEY_Video_FrameRate));
        break;
    default:
        break;
    }
    store->Release();
    return out;
}

uint64_t Revision() {
    return State().revision.load(std::memory_order_acquire);
}

void Request(const void* owner, std::vector<Job> jobs) {
    Shared& s = State();
    bool start = false;
    {
        std::lock_guard lock(s.m);
        // This owner's previous folder; then its new files go first.
        std::erase_if(s.queue, [owner](const Job& job) { return job.owner == owner; });
        std::deque<Job> fresh;
        for (Job& job : jobs) {
            if (job.kind == Kind::None || Current(s, job)) continue;
            job.owner = owner;
            fresh.push_back(std::move(job));
        }
        s.queue.insert(s.queue.begin(), std::make_move_iterator(fresh.begin()),
                       std::make_move_iterator(fresh.end()));
        start = !s.started && !s.queue.empty();
        if (start) s.started = true;
    }
    if (start) std::thread(Run).detach();
    s.wake.notify_one();
}

std::shared_ptr<const Props> Find(const std::wstring& path, uint64_t mtime, uint64_t size) {
    Shared& s = State();
    std::lock_guard lock(s.m);
    const auto it = s.cache.find(path);
    if (it == s.cache.end() || it->second.mtime != mtime || it->second.size != size) return nullptr;
    return it->second.props;
}

size_t Pending() {
    Shared& s = State();
    std::lock_guard lock(s.m);
    return s.queue.size() + s.busy;
}

}  // namespace pulse::app::media
