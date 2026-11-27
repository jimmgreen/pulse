#include "host/video/video_ui_mailbox.h"

#include <cstdio>
#include <cstdlib>
#include <string>

using pulse::desktop::video::VideoUiMailbox;

namespace {
void Check(bool condition, const char* message) {
    if (!condition) {
        std::printf("[FAIL] %s\n", message);
        std::exit(1);
    }
}
}

int main() {
    VideoUiMailbox<std::string> mailbox;
    mailbox.Open();
    unsigned wakes = 0;
    auto post = [&] { ++wakes; return true; };
    for (uint64_t seq = 1; seq <= 100000; ++seq) {
        Check(mailbox.PushFrame(std::to_string(seq), 1, seq), "accept increasing sequence");
        mailbox.TryWake(post);
        Check(mailbox.PendingCount() == 1, "burst retains exactly one frame");
    }
    Check(wakes == 1 && mailbox.WakePending(), "burst posts exactly one wake");
    auto batch = mailbox.TakeBatch();
    Check(batch.size() == 1 && batch.front() == "100000", "burst drains newest frame");
    Check(mailbox.TakeBatch().empty(), "duplicate wake does not repeat a frame");
    std::puts("[PASS] 100000-frame burst: one slot, one wake, newest frame, no duplicate draw");

    Check(!mailbox.PushFrame("duplicate", 1, 100000), "reject consumed sequence");
    Check(!mailbox.PushFrame("old-sequence", 1, 99999), "reject old sequence");
    Check(mailbox.PushFrame("new-generation", 2, 1), "new generation resets sequence order");
    Check(!mailbox.PushFrame("old-generation", 1, 100001), "reject old generation");
    Check(mailbox.TakeBatch().front() == "new-generation", "stale frame cannot overwrite newest");
    std::puts("[PASS] stale generation and stale/duplicate sequence rejection survives drains");

    mailbox.PushControl("swap");
    mailbox.PushFrame("discarded-frame", 2, 2);
    mailbox.PushControl("freeze");
    mailbox.PushFrame("newest-frame", 2, 3);
    mailbox.PushControl("error");
    mailbox.TryWake(post);
    batch = mailbox.TakeBatch();
    Check(batch == std::deque<std::string>{"swap", "freeze", "newest-frame", "error"},
          "control FIFO and latest frame position retained");
    std::puts("[PASS] control FIFO survives coalescing and frame keeps notification order");

    // The consumer holds the detached batch while a producer posts more work.
    const unsigned before = wakes;
    mailbox.PushFrame("during-render", 2, 4);
    mailbox.TryWake(post);
    mailbox.PushControl("finish-during-render");
    mailbox.TryWake(post);
    Check(wakes == before + 1, "work arriving during drain gets exactly one next wake");
    Check(batch.back() == "error", "producer does not mutate detached batch");
    batch = mailbox.TakeBatch();
    Check(batch == std::deque<std::string>{"during-render", "finish-during-render"},
          "next drain retains all work posted during render");
    std::puts("[PASS] arrivals during detached-batch rendering schedule one following wake");

    mailbox.PushControl("retained-on-failed-post");
    unsigned failed_posts = 0;
    mailbox.TryWake([&] { ++failed_posts; return false; });
    Check(failed_posts == 1 && !mailbox.WakePending() && mailbox.PendingCount() == 1,
          "failed post retains controls and clears wake state");
    mailbox.TryWake(post);
    Check(mailbox.WakePending(), "timer/next-notification retry rearms wake");
    Check(mailbox.TakeBatch().front() == "retained-on-failed-post", "retry delivers retained control");
    std::puts("[PASS] failed post retains work and a later retry recovers wake");

    mailbox.PushControl("discard-on-shutdown");
    mailbox.PushFrame("discard-frame-on-shutdown", 2, 5);
    mailbox.TryWake(post);
    mailbox.Close();
    mailbox.PushControl("late-control");
    Check(!mailbox.PushFrame("late-frame", 3, 1), "closed mailbox rejects late frame");
    Check(!mailbox.TryWake(post) && !mailbox.WakePending() && mailbox.TakeBatch().empty(),
          "shutdown clears resources and suppresses late producers");
    mailbox.Open();
    Check(mailbox.PushFrame("restart", 1, 1), "reinitialize resets generation and sequence");
    mailbox.TryWake(post);
    Check(mailbox.TakeBatch().front() == "restart", "reinitialize delivers new work");
    std::puts("[PASS] shutdown drops pending work, rejects late producers, and permits reinitialization");

    {
        VideoUiMailbox<std::string> lifecycle;
        lifecycle.Open();
        lifecycle.PushFrame("before-freeze", 7, 90);
        lifecycle.PushControl("frozen");
        Check(lifecycle.TakeBatch() == std::deque<std::string>{"before-freeze", "frozen"},
              "last old frame is consumed before freeze control");
        Check(lifecycle.PushFrame("resumed-frame", 7, 91),
              "same-generation resume accepts continued sequence");
        lifecycle.PushControl("reopened-swap");
        Check(lifecycle.TakeBatch() == std::deque<std::string>{"resumed-frame", "reopened-swap"},
              "reopen preserves player first-frame-before-swap callback order");
        Check(!lifecycle.PushFrame("late-before-freeze", 7, 90),
              "resume does not re-admit pre-freeze frames");
        lifecycle.PushControl("stopped");
        lifecycle.PushControl("new-open-swap");
        Check(lifecycle.PushFrame("new-open-frame", 9, 1),
              "close/open new generation permits sequence restart without reopening mailbox");
        Check(lifecycle.TakeBatch() == std::deque<std::string>{"stopped", "new-open-swap", "new-open-frame"},
              "stop/open controls and new generation frame retain order");
        Check(!lifecycle.PushFrame("late-old-open", 7, 92),
              "old generation remains stale after stop/open");
        std::puts("[PASS] same-generation freeze/reopen continues sequence; stop/open starts a new generation");
    }
    return 0;
}
