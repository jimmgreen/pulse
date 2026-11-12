#include <pulse/client.hpp>
#include <pulse/ui_mount.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <new>
#include <optional>
#include <string_view>

PULSE_PLUGIN_SDK_PREVIEW()

namespace {

#pragma pack(push, 8)
enum CompatFixtureMode : uint32_t {
    COMPAT_MODE_NORMAL_PROBE = 0u,
    COMPAT_MODE_EXTENDED_PLUGIN_SIZE = 1u,
    COMPAT_MODE_UNDERSIZED_PLUGIN_SIZE = 2u,
    COMPAT_MODE_WRONG_PLUGIN_VERSION = 3u,
    COMPAT_MODE_DEGRADATION_FALLBACK = 4u,
};

struct CompatSharedState {
    uint32_t mode;
    uint32_t observed_host_size;
    uint32_t observed_host_version;
    int32_t entry_calls;
    int32_t activate_calls;
    int32_t deactivate_calls;
    int32_t destroy_calls;
    PulseStatus ui_mount_status;
    PulseStatus future_commands_status;
    PulseStatus unknown_quantum_status;
    uint32_t ui_mount_nullopt;
    uint32_t future_commands_nullopt;
    uint32_t unknown_quantum_nullopt;
    PulseStatus commands_v1_status;
    PulseStatus tasks_v1_status;
    PulseStatus register_cmd_status;
    PulseStatus register_collect_cmd_status;
    uint64_t registered_command_id;
    int32_t fallback_cmd_invocations;
    uint64_t fallback_task_id;
    std::atomic<int32_t> fallback_task_runs;
    uint64_t collected_tasks;
};

struct CompatControlTableV1 {
    uint32_t size;
    uint32_t version;
    CompatSharedState* state;
};
#pragma pack(pop)

static_assert(sizeof(CompatControlTableV1) == 16);

constexpr std::string_view kCompatControlInterface = "test.compat.control";
constexpr uint32_t kCompatControlVersion = 1u;

template <class Table>
std::optional<Table> QueryOptional(const pulse::sdk::Host& host,
                                   std::string_view name,
                                   uint32_t version,
                                   PulseStatus* out_status = nullptr) noexcept {
    Table table{};
    const PulseStatus status = host.Query(name, version, table);
    if (out_status) {
        *out_status = status;
    }
    if (status != PULSE_OK) {
        return std::nullopt;
    }
    return table;
}

struct FixtureInstance {
    CompatSharedState* state = nullptr;
    pulse::sdk::CommandRegistration fallback_run_cmd;
    pulse::sdk::CommandRegistration fallback_collect_cmd;
    std::unique_ptr<pulse::sdk::TaskGroup> task_group;

    explicit FixtureInstance(CompatSharedState* shared) noexcept : state(shared) {}
};

PulseStatus PULSE_CALL Activate(void* raw) noexcept {
    auto* self = static_cast<FixtureInstance*>(raw);
    if (!self || !self->state) {
        return PULSE_ERROR_ARGUMENT;
    }
    ++self->state->activate_calls;
    return PULSE_OK;
}

void PULSE_CALL Deactivate(void* raw) noexcept {
    auto* self = static_cast<FixtureInstance*>(raw);
    if (!self || !self->state) {
        return;
    }
    ++self->state->deactivate_calls;
    self->fallback_run_cmd.Reset();
    self->fallback_collect_cmd.Reset();
    if (self->task_group) {
        self->state->collected_tasks += static_cast<uint64_t>(self->task_group->Collect());
    }
}

void PULSE_CALL Destroy(void* raw) noexcept {
    auto* self = static_cast<FixtureInstance*>(raw);
    if (!self) {
        return;
    }
    if (self->state) {
        ++self->state->destroy_calls;
        if (self->task_group) {
            self->state->collected_tasks += static_cast<uint64_t>(self->task_group->Collect());
        }
    }
    self->fallback_run_cmd.Reset();
    self->fallback_collect_cmd.Reset();
    self->task_group.reset();
    delete self;
}

PulseStatus ConfigureDegradationFallback(const pulse::sdk::Host& host, FixtureInstance& instance) noexcept {
    try {
        CompatSharedState& state = *instance.state;

        const auto ui_mount = QueryOptional<PulseUiMountV1>(
            host, PULSE_UI_MOUNT_INTERFACE, PULSE_UI_MOUNT_VERSION_1, &state.ui_mount_status);
        state.ui_mount_nullopt = ui_mount.has_value() ? 0u : 1u;

        const auto future_commands = QueryOptional<PulseCommandsV1>(
            host, PULSE_COMMANDS_INTERFACE_NAME, 99u, &state.future_commands_status);
        state.future_commands_nullopt = future_commands.has_value() ? 0u : 1u;

        const auto quantum = QueryOptional<PulseCommandsV1>(
            host, "pulse.future.quantum", 1u, &state.unknown_quantum_status);
        state.unknown_quantum_nullopt = quantum.has_value() ? 0u : 1u;

        const auto commands_v1 = QueryOptional<PulseCommandsV1>(
            host, PULSE_COMMANDS_INTERFACE_NAME, PULSE_COMMANDS_VERSION_1, &state.commands_v1_status);
        const auto tasks_v1 = QueryOptional<PulseTasksV1>(
            host, PULSE_TASKS_INTERFACE_NAME, PULSE_TASKS_VERSION_1, &state.tasks_v1_status);
        if (!commands_v1.has_value() || !tasks_v1.has_value()) {
            return PULSE_ERROR_INITIALIZATION;
        }

        instance.task_group = std::make_unique<pulse::sdk::TaskGroup>(*tasks_v1);

        PulseCommandDescriptor run_desc{};
        run_desc.size = sizeof(run_desc);
        run_desc.id = pulse::sdk::View("compat.fallback.run");
        run_desc.title = pulse::sdk::View("Run Fallback Command");
        run_desc.icon = PULSE_COMMAND_ICON_DEFAULT;
        run_desc.kinds = PULSE_COMMAND_FILES | PULSE_COMMAND_DIRECTORIES;
        run_desc.min_selection = 0;
        run_desc.max_selection = 64;

        FixtureInstance* ptr = &instance;
        state.register_cmd_status = instance.fallback_run_cmd.Register(
            *commands_v1,
            run_desc,
            [ptr](PulseCommandId, const PulseContextSelectionSnapshot& selection) {
                if (!ptr || !ptr->state || !ptr->task_group) {
                    return;
                }
                if (selection.entry_count == 1) {
                    ++ptr->state->fallback_cmd_invocations;
                }
                PulseTaskId task_id = 0;
                const PulseStatus start_status = ptr->task_group->Start(
                    "Fallback Degradation Task",
                    "compat.degradation",
                    [ptr](const PulseTaskContext& ctx) -> PulseTaskState {
                        PulseTaskProgress progress{};
                        progress.size = sizeof(progress);
                        progress.percent = 100;
                        progress.completed_items = 1;
                        progress.total_items = 1;
                        progress.current_item = pulse::sdk::View("fallback-item.dat");
                        if (ctx.report) {
                            ctx.report(ctx.context, &progress);
                        }
                        ptr->state->fallback_task_runs.fetch_add(1, std::memory_order_acq_rel);
                        return PULSE_TASK_COMPLETED;
                    },
                    {},
                    &task_id);
                if (start_status == PULSE_OK) {
                    ptr->state->fallback_task_id = task_id;
                }
            });
        state.registered_command_id = instance.fallback_run_cmd.Id();
        if (state.register_cmd_status != PULSE_OK) {
            return state.register_cmd_status;
        }

        PulseCommandDescriptor collect_desc{};
        collect_desc.size = sizeof(collect_desc);
        collect_desc.id = pulse::sdk::View("compat.fallback.collect");
        collect_desc.title = pulse::sdk::View("Collect Fallback Tasks");
        collect_desc.icon = PULSE_COMMAND_ICON_DEFAULT;
        collect_desc.kinds = PULSE_COMMAND_FILES | PULSE_COMMAND_DIRECTORIES;
        collect_desc.min_selection = 0;
        collect_desc.max_selection = 64;
        state.register_collect_cmd_status = instance.fallback_collect_cmd.Register(
            *commands_v1,
            collect_desc,
            [ptr](PulseCommandId, const PulseContextSelectionSnapshot&) {
                if (ptr && ptr->state && ptr->task_group) {
                    ptr->state->collected_tasks += static_cast<uint64_t>(ptr->task_group->Collect());
                }
            });
        return state.register_collect_cmd_status;
    } catch (...) {
        return PULSE_ERROR_INITIALIZATION;
    }
}

} // namespace

extern "C" __declspec(dllexport) PulseStatus PULSE_CALL PulsePluginEntryV1(const PulseHostV1* host,
                                                                           PulsePluginV1* out) {
    if (!host || !out || host->size < sizeof(PulseHostV1) || host->version != PULSE_ABI_VERSION_1 ||
        !host->query_interface) {
        return PULSE_ERROR_ABI;
    }

    const pulse::sdk::Host sdk_host(*host);
    CompatControlTableV1 control{};
    const PulseStatus control_status = sdk_host.Query(kCompatControlInterface, kCompatControlVersion, control);
    if (control_status != PULSE_OK || control.size < sizeof(CompatControlTableV1) ||
        control.version != kCompatControlVersion || !control.state) {
        return PULSE_ERROR_INITIALIZATION;
    }

    CompatSharedState& state = *control.state;
    ++state.entry_calls;
    state.observed_host_size = host->size;
    state.observed_host_version = host->version;

    auto* instance = new (std::nothrow) FixtureInstance(&state);
    if (!instance) {
        return PULSE_ERROR_INITIALIZATION;
    }

    switch (state.mode) {
    case COMPAT_MODE_NORMAL_PROBE:
        *out = {sizeof(PulsePluginV1), PULSE_ABI_VERSION_1, instance, &Activate, &Deactivate, &Destroy};
        return PULSE_OK;

    case COMPAT_MODE_EXTENDED_PLUGIN_SIZE:
        *out = {static_cast<uint32_t>(sizeof(PulsePluginV1) + 24u), PULSE_ABI_VERSION_1, instance,
                &Activate, &Deactivate, &Destroy};
        return PULSE_OK;

    case COMPAT_MODE_UNDERSIZED_PLUGIN_SIZE:
        *out = {static_cast<uint32_t>(sizeof(PulsePluginV1) - 1u), PULSE_ABI_VERSION_1, instance,
                &Activate, &Deactivate, &Destroy};
        return PULSE_OK;

    case COMPAT_MODE_WRONG_PLUGIN_VERSION:
        *out = {sizeof(PulsePluginV1), 2u, instance, &Activate, &Deactivate, &Destroy};
        return PULSE_OK;

    case COMPAT_MODE_DEGRADATION_FALLBACK: {
        const PulseStatus cfg_status = ConfigureDegradationFallback(sdk_host, *instance);
        if (cfg_status != PULSE_OK) {
            Destroy(instance);
            *out = {};
            return cfg_status;
        }
        *out = {sizeof(PulsePluginV1), PULSE_ABI_VERSION_1, instance, &Activate, &Deactivate, &Destroy};
        return PULSE_OK;
    }

    default:
        Destroy(instance);
        *out = {};
        return PULSE_ERROR_ARGUMENT;
    }
}
