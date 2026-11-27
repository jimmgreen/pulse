// FolderPickerWindow's inline-details controller. Enumeration stays on PickerLoader.
    bool TreeEligible() const {
        return !new_folder_draft_ && view_mode_ == ViewMode::Details && !order_.group_by && options_.search.empty() &&
            !current_.empty() && IsPickerDiskPath(current_) && !InArchive();
    }
    std::wstring TreeSelectionName(const PickerEntry& entry) const {
        if (SamePickerPath(PickerParent(entry.path), current_)) return entry.name;
        auto root = path::StripExtendedPathPrefix(current_);
        auto full = path::StripExtendedPathPrefix(entry.path);
        while (!root.empty() && (root.back() == L'\\' || root.back() == L'/')) root.pop_back();
        if (!root.empty() && full.size() > root.size() && _wcsnicmp(full.c_str(), root.c_str(), root.size()) == 0 &&
            (full[root.size()] == L'\\' || full[root.size()] == L'/')) return full.substr(root.size() + 1);
        return entry.path;
    }
    bool TreePlaceholder(int index) const {
        const auto* row = tree_rows_ && !new_folder_draft_ ? tree_rows_->At(index) : nullptr;
        return row && row->kind != TreeRowKind::Entry;
    }
    void ClearTree() {
        tree_loader_.reset(); tree_pending_.clear(); tree_active_ = 0;
        tree_.Clear(); tree_rows_.reset(); ++tree_serial_;
    }
    void RebuildTree() {
        EndMarquee(true); CancelPlacesDrag(); DismissHover();
        std::vector<std::wstring> selected;
        for (int i : selected_set_) if (i >= 0 && static_cast<size_t>(i) < entries_.size() && !TreePlaceholder(i))
            selected.push_back(entries_[i].path);
        const std::wstring caret = SelectedEntry() ? SelectedEntry()->path : std::wstring();
        auto view = tree_.Build();
        entries_ = std::move(view.entries); snapshot_ = std::move(view.snapshot); tree_rows_ = std::move(view.rows);
        selected_set_.clear(); selected_ = anchor_ = -1;
        for (size_t i = 0; i < entries_.size(); ++i) {
            if (TreePlaceholder(static_cast<int>(i))) continue;
            if (SamePickerPath(entries_[i].path, caret)) selected_ = static_cast<int>(i);
            if (std::any_of(selected.begin(), selected.end(), [&](const auto& p) { return SamePickerPath(p, entries_[i].path); }))
                selected_set_.insert(static_cast<int>(i));
        }
        anchor_ = selected_;
        if (!rename_path_.empty()) {
            rename_index_ = -1;
            for (size_t i = 0; i < entries_.size(); ++i)
                if (!TreePlaceholder(static_cast<int>(i)) && SamePickerPath(entries_[i].path, rename_path_)) { rename_index_ = static_cast<int>(i); break; }
            if (rename_index_ < 0) EndRename(false);
            else PlaceRenameEdit();
        }
        if (filename_from_selection_) {
            if (mode_ == PickerMode::Save && selected_set_.empty() && !selected.empty()) SetFilename(L"");
            SyncSelectedNames();
        }
        UpdateChosen(); ClampScroll(); Invalidate();
    }
    void StartNextTreeLoad() {
        if (tree_active_ || !TreeEligible() || done_) return;
        while (!tree_pending_.empty()) {
            const auto path = std::move(tree_pending_.front()); tree_pending_.pop_front();
            if (!tree_.NeedsLoad(path)) continue;
            if (!tree_loader_) tree_loader_ = std::make_unique<PickerLoader>(hwnd_, kTreeListingMessage);
            tree_active_ = ++tree_serial_; tree_active_path_ = path;
            tree_.BeginLoad(path, tree_active_, motion::NowMs());
            auto options = options_;
            // Main details-tree children inherit the visible parent's sort, not a
            // different per-folder view. Rebind the folder for tag/path keys.
            options.order_entries = [order = order_, path](auto& entries, const auto& rows) {
                app::OrderPickerEntries(entries, rows, path, order);
            };
            tree_loader_->Load(tree_active_, path, mode_, std::move(options));
            break;
        }
    }
    void ToggleTree(int index) {
        if (!TreeEligible() || waiting_ || validating_ || operating_ || !rename_path_.empty() ||
            index < 0 || static_cast<size_t>(index) >= entries_.size() || !tree_rows_) return;
        const auto* row = tree_rows_->At(index);
        if (!row || row->kind != TreeRowKind::Entry || !row->expandable) return;
        const auto path = entries_[index].path;
        if (!tree_.Toggle(path)) return;
        if (tree_.Expanded(path)) tree_pending_.push_back(path);
        else {
            const auto parent = ListOrderKey(path), active = ListOrderKey(tree_active_path_);
            if (active == parent || (active.size() > parent.size() && active.compare(0, parent.size(), parent) == 0 && active[parent.size()] == L'\\')) {
                // Cancel a blocked read of this subtree without disturbing a sibling read.
                tree_loader_.reset(); tree_active_ = 0;
            }
        }
        StartNextTreeLoad(); RebuildTree();
    }
    void OnTreeListing(std::unique_ptr<PickerListing> listing) {
        if (!listing || listing->generation != tree_active_ || !TreeEligible()) return;
        tree_active_ = 0;
        if (spec_.shell_item_filter && !listing->error) {
            const auto generation = generation_;
            unsigned checks = 0; ULONGLONG next_yield = GetTickCount64() + 8;
            const auto current = [&] { return ContinueShellFilterBatch(generation, checks, next_yield); };
            const auto filtered = FilterPickerShellListing(*listing,
                [&](const auto& path) { return NotifyEmbedded(PickerDialogEvent::IncludeItem, path); }, current);
            if (done_) return;
            if (filtered != S_OK || !current()) {
                // A host callback can cancel validation without navigating. Finish
                // this token as failed; navigation/collapse is rejected by Complete.
                listing->error = ERROR_CANCELLED; listing->entries.clear(); listing->snapshot.reset();
            }
        }
        if (tree_.Complete(std::move(listing))) RebuildTree();
        StartNextTreeLoad();
    }
    bool TreeMouse(const HitTestResult& hit, float x, float y) {
        if (new_folder_draft_ || !tree_rows_ || (hit.region != HitTestResult::Row && hit.region != HitTestResult::TreeSticky)) return false;
        const int index = hit.index;
        const auto* row = tree_rows_->At(index);
        if (!row) return false;
        if (row->kind == TreeRowKind::More) {
            const int action = renderer_.TreeMoreActionAt(vm_.pane, PaneBounds(), index, x, y);
            if (action == 2) OpenPathInPulse(entries_[index].path, false);
            else if (action >= 0) { tree_.ShowMore(entries_[index].path, action == 1); RebuildTree(); }
            return true;
        }
        if (row->kind != TreeRowKind::Entry) return true;
        if (renderer_.TreeChevronAt(vm_.pane, PaneBounds(), x, y) == index) {
            FocusList(); ToggleTree(index); return true;
        }
        if (hit.region == HitTestResult::TreeSticky) { Select(index); return true; }
        return false;
    }
    bool TreeKey(WPARAM key) {
        if (focus_ != kPickList || !tree_rows_ || selected_set_.size() > 1 ||
            KeyDown(VK_CONTROL) || KeyDown(VK_SHIFT) || KeyDown(VK_MENU)) return false;
        const auto* row = tree_rows_->At(selected_);
        if (!row || row->kind != TreeRowKind::Entry) return false;
        if (key == VK_RIGHT && row->expandable) {
            if (!row->expanded) ToggleTree(selected_);
            else if (const auto* child = tree_rows_->At(selected_ + 1);
                child && child->depth > row->depth && child->kind == TreeRowKind::Entry) Select(selected_ + 1);
            return true;
        }
        if (key == VK_LEFT) {
            if (row->expanded) { ToggleTree(selected_); return true; }
            const int parent = tree_rows_->ParentRow(selected_);
            if (parent >= 0) { Select(parent); return true; }
        }
        return false;
    }
    int SkipTreePlaceholder(int index, int direction) const {
        const int count = static_cast<int>(entries_.size());
        if (!count) return -1;
        index = std::clamp(index, 0, count - 1);
        int next = index;
        while (next >= 0 && next < count && TreePlaceholder(next)) next += direction;
        if (next >= 0 && next < count) return next;
        next = index;
        while (next >= 0 && next < count && TreePlaceholder(next)) next -= direction;
        return next >= 0 && next < count ? next : -1;
    }
