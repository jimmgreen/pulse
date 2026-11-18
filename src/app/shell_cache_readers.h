#pragma once
#include <algorithm>
#include <atomic>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

namespace pulse::app {
// UI-thread admission and shutdown; workers only post results, never wait for UI.
// Reap finished readers during admission and join all remaining readers before
// the last window releases localization and other process-owned resources.
class ShellCacheReaders {
  public:
    ~ShellCacheReaders() { Stop(); }
    template <class Work> void Start(Work work) {
        if (stopping_)
            return;
        std::erase_if(readers_, [](const Reader &reader) { return reader.done->load(); });
        auto done = std::make_shared<std::atomic<bool>>(false);
        std::jthread thread([work = std::move(work), done](std::stop_token stop) {
            work(stop);
            done->store(true);
        });
        readers_.push_back({done, std::move(thread)});
    }
    void Stop() {
        stopping_ = true;
        for (auto &reader : readers_)
            reader.thread.request_stop();
        readers_.clear(); // jthread joins while every process dependency is still alive.
    }

  private:
    struct Reader {
        std::shared_ptr<std::atomic<bool>> done;
        std::jthread thread;
    };
    std::vector<Reader> readers_;
    bool stopping_ = false;
};
} // namespace pulse::app
