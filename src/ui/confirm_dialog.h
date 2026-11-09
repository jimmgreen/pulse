#pragma once

#include <d2d1.h>
#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace pulse::ui {

// The icon tile and its colour. Auto picks Danger for `danger` specs and
// Question otherwise.
enum class ConfirmTone { Auto, Info, Question, Warning, Danger };

enum class ConfirmChoice { Cancel, Confirm, Secondary };

struct ConfirmDialogSpec {
    std::wstring title;
    std::wstring message;
    std::wstring confirm_text;
    std::wstring cancel_text;
    bool danger = false;
    // Optional middle button, e.g. "No" in a Yes / No / Cancel question.
    std::wstring secondary_text;
    // Optional list shown in a card under the message (one row per item).
    std::vector<std::wstring> items;
    // Optional: how many items `items` stands for when only the first ones
    // are listed (0: items.size()). The card then ends with "and N more".
    size_t items_total = 0;
    std::wstring item_glyph;
    // Optional: one glyph per listed item (a folder among files); an empty
    // entry, or a row past the end, uses item_glyph.
    std::vector<std::wstring> item_glyphs;
    // Optional: the item rows become check boxes, one flag per item (at most
    // kConfirmMaxItemRows items, no items_total). Clicks and Space toggle them
    // and the dialog writes the ticks back here; the confirm and secondary
    // buttons need at least one tick.
    std::vector<bool>* item_checks = nullptr;
    // Optional small print under the card.
    std::wstring note;
    ConfirmTone tone = ConfirmTone::Auto;
    std::wstring glyph;  // overrides the tone's icon
    // The button Enter presses at first. Destructive questions start on
    // Cancel, like MB_DEFBUTTON2, so Enter alone never confirms them.
    ConfirmChoice default_choice = ConfirmChoice::Confirm;
    // Information only: just the confirm button (Esc / close still dismiss).
    bool single_button = false;
    // Optional: the confirm button must be pressed twice (a permanent delete
    // asked for an AI app). The first press only turns its text into
    // confirm_again_text and the note into confirm_again_note.
    std::wstring confirm_again_text;
    std::wstring confirm_again_note;
    // Optional: called with the dialog once it shows and with null when it
    // goes, so a question asked for someone else (an AI client) can be
    // closed from outside with WM_CLOSE, which answers Cancel.
    std::function<void(HWND)> on_open;
    HANDLE cancel_event = nullptr; // borrowed; nested picker disconnect cancellation
};

ConfirmChoice ShowConfirmDialogEx(HWND owner, const ConfirmDialogSpec& spec, bool dark,
                                  D2D1_COLOR_F accent);

// True when the user picked the confirm button.
bool ShowConfirmDialog(HWND owner, const ConfirmDialogSpec& spec, bool dark,
                       D2D1_COLOR_F accent);

// Captures the existing native confirmation surface without changing its state.
// Only a live confirmation window on the calling UI thread is accepted.
// Live controls on an opaque host-theme surface; DWM acrylic is not captured.
bool SaveConfirmDialogFrame(HWND window, const std::wstring& path);

} // namespace pulse::ui
