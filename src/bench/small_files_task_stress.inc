namespace {
struct SmallFilesTaskStress {
    struct Task { uint64_t id; bool light; bool cancelled; };
    std::filesystem::path root;
    std::vector<Task> tasks;
    bool prepared = false;
    std::string preparation_error;
    static std::string Payload(unsigned index) { return "parallel-fixture-" + std::to_string(index) + std::string(4096, 'x'); }

    void Prepare() {
        try {
            for (const auto* name : {L"ui-a", L"ui-b", L"parallel-source", L"parallel-destination",
                    L"dependency", L"bulk-source", L"queued-a", L"queued-b"})
                std::filesystem::create_directory(root / name);
            // KeepBoth adds a suffix exceeding NTFS's component limit. The valid
            // existing name passes admission, then the real create fails on the worker.
            std::filesystem::create_directory(fs::NormalizePath((root / std::wstring(255, L'n')).wstring()));
            const auto write = [&](const std::filesystem::path& file, unsigned index) {
                std::ofstream stream(file, std::ios::binary);
                stream << Payload(index);
                if (!stream) throw std::runtime_error("stress fixture write failed");
            };
            for (unsigned i = 0; i < 64; ++i) {
                write(root / L"ui-a" / (std::to_wstring(i) + L".txt"), i);
                write(root / L"ui-b" / (std::to_wstring(i) + L".txt"), i);
            }
            for (unsigned i = 0; i < 256; ++i)
                write(root / L"bulk-source" / (std::to_wstring(i) + L".txt"), i);
            for (unsigned i = 0; i < 6; ++i) {
                write(root / L"parallel-source" / (L"copy-" + std::to_wstring(i) + L".txt"), i);
                write(root / L"parallel-source" / (L"rename-" + std::to_wstring(i) + L".txt"), i);
                write(root / L"parallel-source" / (L"delete-" + std::to_wstring(i) + L".txt"), i);
            }
            prepared = true;
        } catch (const std::exception& error) { preparation_error = error.what(); }
    }

    int Submit(AppState& state, std::ofstream& report) {
        int failures = 0;
        const auto submit = [&](ops::OpRequest request, bool light, bool cancelled = false) {
            const auto id = state.ops.Submit(std::move(request));
            const bool retained = id && state.ops.RetainTask(id);
            report << (retained ? "[PASS] " : "[FAIL] ") << "TASK_STRESS admitted retained task=" << id << '\n';
            failures += !retained;
            tasks.push_back({id, light, cancelled});
            return id;
        };
        for (const auto* destination : {L"queued-a", L"queued-b"}) {
            ops::OpRequest request;
            request.type = ops::OpType::Copy;
            request.sources = {fs::NormalizePath((root / L"bulk-source").wstring())};
            request.dest_dir = fs::NormalizePath((root / destination).wstring());
            submit(std::move(request), false);
        }
        for (unsigned i = 0; i < 6; ++i) {
            const auto suffix = std::to_wstring(i) + L".txt";
            ops::OpRequest copy;
            copy.type = ops::OpType::Copy;
            copy.sources = {fs::NormalizePath((root / L"parallel-source" / (L"copy-" + suffix)).wstring())};
            copy.dest_dir = fs::NormalizePath((root / L"parallel-destination").wstring());
            submit(std::move(copy), true);
            ops::OpRequest rename;
            rename.type = ops::OpType::Rename;
            rename.sources = {fs::NormalizePath((root / L"parallel-source" / (L"rename-" + suffix)).wstring())};
            rename.new_name = L"renamed-" + suffix;
            submit(std::move(rename), true);
            ops::OpRequest remove;
            remove.type = ops::OpType::RealDelete;
            remove.sources = {fs::NormalizePath((root / L"parallel-source" / (L"delete-" + suffix)).wstring())};
            submit(std::move(remove), true);
            ops::OpRequest create;
            create.type = ops::OpType::CreateFolder;
            create.collision_policy = ops::CollisionPolicy::KeepBoth;
            create.sources = {fs::NormalizePath((root / L"parallel-destination" / (L"new-" + std::to_wstring(i))).wstring())};
            submit(std::move(create), true);
        }
        ops::OpRequest dependent_copy;
        dependent_copy.type = ops::OpType::Copy;
        dependent_copy.sources = {fs::NormalizePath((root / L"parallel-source" / L"copy-0.txt").wstring())};
        dependent_copy.dest_dir = fs::NormalizePath((root / L"dependency").wstring());
        submit(std::move(dependent_copy), true);
        ops::OpRequest dependent_delete;
        dependent_delete.type = ops::OpType::RealDelete;
        dependent_delete.sources = {fs::NormalizePath((root / L"dependency" / L"copy-0.txt").wstring())};
        submit(std::move(dependent_delete), true);
        ops::OpRequest cancelled;
        cancelled.type = ops::OpType::RealDelete;
        cancelled.sources = {fs::NormalizePath((root / L"destination" / L"many").wstring())};
        const auto cancelled_id = submit(std::move(cancelled), false, true);
        const bool accepted = state.ops.CancelTask(cancelled_id);
        report << (accepted ? "[PASS] " : "[FAIL] ") << "TASK_STRESS cancel queued overlapping directory delete\n";
        failures += !accepted;
        report << "TASK_STRESS total=" << tasks.size() << " independent_light=24 dependent_light=2 queued_bulk=2 cancelled_overlap=1\n";
        report.flush();
        return failures;
    }

    int CheckFinished(AppState& state, uint64_t heavy, std::ofstream& report) {
        int failures = 0;
        const bool finished = PumpUntil([&] {
            return std::all_of(tasks.begin(), tasks.end(), [&](const auto& task) {
                const auto status = state.ops.QueryTask(task.id);
                return status && !status->active && (status->phase == ops::OpPhase::Completed || status->phase == ops::OpPhase::Failed);
            });
        }, 180000);
        report << (finished ? "[PASS] " : "[FAIL] ") << "TASK_STRESS all admitted tasks reach terminal status\n";
        failures += !finished;
        const auto primary = state.ops.QueryTask(heavy);
        unsigned early = 0;
        for (const auto& task : tasks) {
            const auto status = state.ops.QueryTask(task.id);
            const bool ok = status && !status->active && (task.cancelled
                ? status->phase == ops::OpPhase::Failed && status->completed_items == 0 && status->last_error == l10n::Get(l10n::StringId::Canceled)
                : status->phase == ops::OpPhase::Completed);
            report << (ok ? "[PASS] " : "[FAIL] ") << "TASK_STRESS terminal task=" << task.id
                   << " phase=" << (status ? static_cast<int>(status->phase) : -1) << '\n';
            failures += !ok;
            if (task.light && status && primary && status->ended_tick < primary->ended_tick) ++early;
            state.ops.ReleaseTask(task.id);
        }
        report << "TASK_STRESS light_completed_before_primary=" << early << " of=26\n";
        failures += early != 26;
        report << (early == 26 ? "[PASS] " : "[FAIL] ") << "TASK_STRESS all lightweight tasks bypass unrelated queued bulk tasks\n";
        report.flush();
        return failures;
    }

    bool VerifyFiles() const {
        const auto equals = [&](const std::filesystem::path& path, unsigned i) {
            std::ifstream stream(path, std::ios::binary);
            return stream && std::string((std::istreambuf_iterator<char>(stream)), {}) == Payload(i);
        };
        for (unsigned i = 0; i < 6; ++i) {
            const auto suffix = std::to_wstring(i) + L".txt";
            if (!equals(root / L"parallel-destination" / (L"copy-" + suffix), i) ||
                !equals(root / L"parallel-source" / (L"renamed-" + suffix), i) ||
                std::filesystem::exists(root / L"parallel-source" / (L"rename-" + suffix)) ||
                std::filesystem::exists(root / L"parallel-source" / (L"delete-" + suffix)) ||
                !std::filesystem::is_directory(root / L"parallel-destination" / (L"new-" + std::to_wstring(i)))) return false;
        }
        for (const auto* destination : {L"queued-a", L"queued-b"}) {
            size_t count = 0;
            for (const auto& file : std::filesystem::directory_iterator(root / destination / L"bulk-source")) { (void)file; ++count; }
            if (count != 256) return false;
            for (unsigned i = 0; i < 256; ++i)
                if (!equals(root / destination / L"bulk-source" / (std::to_wstring(i) + L".txt"), i)) return false;
        }
        return !std::filesystem::exists(root / L"dependency" / L"copy-0.txt") &&
            std::filesystem::is_directory(root / L"destination" / L"many");
    }
};
}
