#include "../ops/transfer_stage_timing.h"
#include <cstdio>
int main() {
    pulse::ops::TransferStageTiming t(100);
    if (t.PreparationMs(125) != 25 || t.ExecutionWallMs(125) != 0 || t.copy_callback_seen) return 1;
    t.EndPreparation(130);
    t.EndPreparation(140);
    t.ObserveCopyCallback(150, 0);
    if (t.copy_callback_seen) return 2;
    t.ObserveCopyCallback(160, 1);
    t.ObserveCopyCallback(180, 200);
    if (t.PreparationMs(200) != 30 || t.ExecutionWallMs(200) != 70 || t.FirstCopyCallbackMs() != 60) return 3;
    pulse::ops::TransferStageTiming empty(0);
    empty.EndPreparation(0);
    if (!empty.preparation_complete || empty.copy_callback_seen || empty.FirstCopyCallbackMs() != 0) return 4;
    std::puts("[PASS] preparation, execution wall time, first positive callback, absent callback and zero timestamp");
}
