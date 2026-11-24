#pragma once

namespace pulse {
struct AppState;
namespace app { class OperationRefreshPlan; }
namespace ops { struct CompletedOperation; struct FinishedTask; }
void ApplyOperationIntentCompletion(AppState& state, const ops::CompletedOperation& completed,
                                    app::OperationRefreshPlan* refresh_plan = nullptr);
void FinishOperationIntents(AppState& state, const ops::FinishedTask& finished,
                            app::OperationRefreshPlan* refresh_plan = nullptr);
}
