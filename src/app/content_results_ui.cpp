#include "app_internal.h"
#include "content_results_ui.h"
#include "search_query.h"
#include "../common/localization.h"
#include <mutex>
#include <optional>

namespace pulse {
struct ContentSelectionAction {
    std::weak_ptr<index::ContentResultStore> store;
    std::vector<int> indices;
    bool all=false;
    bool pattern=false;
    bool focused_only=false;
    size_t count=0;
    int focus=-1;
    uint64_t selection=0;
    std::function<void(AppState&)> action;
    std::mutex mutex;
    std::optional<index::ContentResultStore::Selection> result;
};
struct ContentSizeSummary {
    std::weak_ptr<index::ContentResultStore> store;
    uint64_t selection=0;
    size_t count=0;
    uint64_t bytes=0;
    std::atomic<bool> done{false};
    DWORD error=0;
};
std::optional<uint64_t> ContentSelectionSize(app::Tab& tab) {
    auto job=tab.content_size_summary;
    if(!job || job->store.lock()!=tab.content_results || job->selection!=tab.selection_revision || job->count!=tab.EntryCount()) {
        job=std::make_shared<ContentSizeSummary>(); job->store=tab.content_results;
        job->selection=tab.selection_revision; job->count=tab.EntryCount(); tab.content_size_summary=job;
        tab.content_results->Sum(tab.all_selected ? std::vector<int>{}:tab.SelectedIndices(),tab.all_selected,job->count,
            [weak=std::weak_ptr<ContentSizeSummary>(job)](uint64_t bytes,DWORD error) {
                if(auto current=weak.lock()) {current->bytes=bytes;current->error=error;current->done=true;}
            });
    }
    return job->done && !job->error ? std::optional<uint64_t>(job->bytes):std::nullopt;
}
struct ContentSelectionRestore {
    std::weak_ptr<index::ContentResultStore> store;
    uint64_t selection=0;
    int index=-1;
    std::atomic<bool> done{false};
};
namespace {
void ContentTitle(AppState& s, app::Tab& tab) {
    std::wstring kind,raw; app::ParsePulsePath(tab.current_path,&kind,&raw);
    std::wstring name=app::SearchDisplayNeedle(raw);
    if(kind==L"saved-search") {
        const auto i=static_cast<size_t>(wcstoull(raw.c_str(),nullptr,10));
        if(i<s.savedSearches.items().size()) name=s.savedSearches.items()[i].name;
    }
    wchar_t text[1024]{};
    const auto label = tab.content_results->Sorting() ? l10n::StringId::ContentSortingFormat :
        tab.search_content_active ? l10n::StringId::ContentFoundFormat :
        tab.content_count_final && !tab.content_results->Error() ? l10n::StringId::ContentMatchesFormat : l10n::StringId::ContentPartialFormat;
    swprintf_s(text,l10n::Get(label).c_str(),name.c_str(),tab.search_total);
    tab.virtual_title=text;
    if(tab.search_content_active) {
        wchar_t scanned[96]{};
        swprintf_s(scanned,l10n::Get(l10n::StringId::ScannedFilesFormat).c_str(),
            static_cast<unsigned long long>(tab.content_scanned_files));
        tab.virtual_title+=scanned;
    }
}
}
bool RefreshContentResults(AppState& s) {
    bool changed=false;
    if (const auto* active=ActiveTab(s); active && active->search_content_active && !active->content_total_files) {
        RECT status{}; GetClientRect(s.hwnd,&status);
        status.top=std::max<LONG>(0,status.bottom-static_cast<LONG>(32*s.scale));
        InvalidateRect(s.hwnd,&status,FALSE);
    }
    ForEachPane(s,[&](app::Pane& pane) {
        auto* tab=pane.ActiveTab(); if(!tab || !tab->content_results) return;
        auto& store=*tab->content_results;
        if(tab->content_filter!=tab->filter_text) {
            tab->content_filter=tab->filter_text;
            tab->ClearSelection(); tab->scroll_y=0;
            store.SetFilter(app::ContentFilter(tab->filter_text,s.places));
        }
        if(tab->content_revision!=store.Revision() || tab->search_total!=store.Count()) {
            // Live deltas re-sort the store off the UI thread, so index-based
            // selection would now name other files. Re-find the focused file.
            if(!tab->search_content_active && tab->selected_index>=0 && tab->search_preserve_selection.empty() &&
               tab->content_focus_selection==tab->selection_revision &&
               tab->content_focus_revision==tab->content_revision && !tab->content_focus_path.empty()) {
                tab->search_preserve_selection=tab->content_focus_path;
                tab->ClearSelection();
            }
            tab->content_revision=store.Revision();
            tab->search_total=store.Count(); tab->file_count=store.Count(); tab->directory_count=0;
            tab->loading=tab->search_content_active && !store.Count();
            tab->view_row_cache.reset(); tab->view_filter_map.reset();

            if(store.Error()) {
                tab->banner_title=l10n::Get(l10n::StringId::SearchIncomplete);
                tab->banner_message=l10n::Get(l10n::StringId::ErrorCodeFormat);
                wchar_t error[128]{}; swprintf_s(error,tab->banner_message.c_str(),store.Error()); tab->banner_message=error;
            }
            changed=true;
        }
        const auto old_title=tab->virtual_title;
        ContentTitle(s,*tab);
        changed |= old_title!=tab->virtual_title;
        if(!tab->search_content_active && !store.Filtering() && !store.Sorting() && !tab->search_preserve_selection.empty()) {
            auto job=std::make_shared<ContentSelectionRestore>();
            job->store=tab->content_results; job->selection=tab->selection_revision;
            tab->content_selection_restore=job;
            store.FindPath(std::move(tab->search_preserve_selection),[weak=std::weak_ptr<ContentSelectionRestore>(job)](int index) {
                if(auto current=weak.lock()) {current->index=index;current->done=true;}
            });
            tab->search_preserve_selection.clear();
        }
        if(auto job=tab->content_selection_restore; job && job->done) {
            if(job->store.lock()==tab->content_results && job->selection==tab->selection_revision && job->index>=0) {
                tab->SelectOnly(job->index);
                store.Prefetch(static_cast<size_t>(job->index));
                if(&pane==s.pane) EnsureRowVisible(s,*tab,job->index);
                changed=true;
            }
            tab->content_selection_restore.reset();
        }
        if(tab->selected_index>=0 && tab->content_revision==store.Revision() &&
           (tab->content_focus_selection!=tab->selection_revision || tab->content_focus_revision!=tab->content_revision)) {
            index::ContentResultStore::Row row;
            if(store.Get(static_cast<size_t>(tab->selected_index),row) && store.Revision()==tab->content_revision) {
                tab->content_focus_path=row.entry.full_path;
                tab->content_focus_selection=tab->selection_revision;
                tab->content_focus_revision=tab->content_revision;
            }
        }
        // Rendering/hit testing can ask for any page (including thumb jumps).
        // A small neighborhood is prefetched without walking earlier pages.
        if(&pane==s.pane && store.Count()) {
            const auto first=static_cast<size_t>(std::max(0.0f,tab->scroll_y)/std::max(1.0f,s.renderer.RowHeight()));
            store.Prefetch(std::min(first,store.Count()-1));
            store.Prefetch(std::min(first+index::ContentResultStore::kPageSize,store.Count()-1));
            if(first>=index::ContentResultStore::kPageSize) store.Prefetch(first-index::ContentResultStore::kPageSize);
        }
    });
    if(s.contentSelectionAction && s.contentSelectionAction->pattern) CompleteContentSelection(s);
    if(changed) InvalidateRect(s.hwnd,nullptr,FALSE);
    return changed;
}
void CancelContentSelection(AppState& s,const app::Tab& tab) {
    if(s.contentSelectionAction && s.contentSelectionAction->store.lock()==tab.content_results)
        s.contentSelectionAction.reset();
}
bool DeferContentSelection(AppState& s,std::function<void(AppState&)> action,bool focused_only) {
    auto* tab=ActiveTab(s);
    if(!tab || !tab->content_results || tab->content_action_ready || !tab->SelectedCount()) return false;
    if(s.contentSelectionAction) return true;
    auto job=std::make_shared<ContentSelectionAction>();
    job->store=tab->content_results; job->all=tab->all_selected && !focused_only; job->focused_only=focused_only;
    if(focused_only) job->indices={tab->selected_index};
    else if(!job->all) job->indices=tab->SelectedIndices();
    job->selection=tab->selection_revision;
    job->count=tab->EntryCount(); job->focus=tab->selected_index; job->action=std::move(action);
    s.contentSelectionAction=job; InvalidateRect(s.hwnd,nullptr,FALSE);
    const HWND hwnd=s.hwnd;
    tab->content_results->Resolve(job->indices,job->all,job->count,
        [weak=std::weak_ptr<ContentSelectionAction>(job),hwnd](auto result) mutable {
            if(auto current=weak.lock()) {
                { std::lock_guard lock(current->mutex); current->result=std::move(result); }
                PostMessageW(hwnd,WM_CONTENT_SELECTION,0,0);
            }
        });
    return true;
}
void SelectContentPattern(AppState& s,const std::wstring& pattern) {
    auto* tab=ActiveTab(s);if(!tab || !tab->content_results) return;
    auto job=std::make_shared<ContentSelectionAction>();job->pattern=true;job->store=tab->content_results;
    tab->filter_text.clear();RefreshContentResults(s);
    job->selection=tab->selection_revision; s.contentSelectionAction=job; InvalidateRect(s.hwnd,nullptr,FALSE);
    tab->content_results->Match(app::ContentFilter(pattern,s.places),[weak=std::weak_ptr<ContentSelectionAction>(job),hwnd=s.hwnd](auto result) {
        if(auto current=weak.lock()) {
            {std::lock_guard lock(current->mutex);current->result=std::move(result);}
            PostMessageW(hwnd,WM_CONTENT_SELECTION,0,0);
        }
    });
}
void CompleteContentSelection(AppState& s) {
    auto job=s.contentSelectionAction; if(!job) return;
    if(job->pattern) {
        auto store=job->store.lock();
        if(store && store->Filtering()) return;
    }
    std::optional<index::ContentResultStore::Selection> result;
    { std::lock_guard lock(job->mutex); result=std::move(job->result); }
    if(!result) return;
    s.contentSelectionAction.reset(); InvalidateRect(s.hwnd,nullptr,FALSE);
    auto* tab=ActiveTab(s); auto store=job->store.lock();
    if(job->pattern) {
        if(tab && store && tab->content_results==store && tab->selection_revision==job->selection && !result->error) {
            tab->SelectIndices(result->matches);
            if(tab->selected_index>=0) EnsureRowVisible(s,*tab,tab->selected_index);
            InvalidateRect(s.hwnd,nullptr,FALSE);
        }
        return;
    }
    if(!tab || !store || tab->content_results!=store || tab->selection_revision!=job->selection || (!job->focused_only && tab->all_selected!=job->all) ||
        (!job->focused_only && !job->all && tab->SelectedIndices()!=job->indices) || tab->selected_index!=job->focus) return;
    if(result->error) {
        tab->banner_title=l10n::Get(l10n::StringId::SearchIncomplete);
        wchar_t error[128]{}; swprintf_s(error,l10n::Get(l10n::StringId::ErrorCodeFormat).c_str(),result->error);
        tab->banner_message=error; InvalidateRect(s.hwnd,nullptr,FALSE); return;
    }
    for(auto& row:result->rows) tab->content_action_rows.emplace(row.first,std::move(row.second));
    tab->content_action_count=job->count; tab->content_action_ready=true;
    job->action(s);
    // A command may have closed/replaced the originating tab.
    ForEachPane(s,[&](app::Pane& pane) {
        auto* current=pane.ActiveTab();
        if(current==tab) { current->content_action_rows.clear(); current->content_action_ready=false; }
    });
}
} // namespace pulse
