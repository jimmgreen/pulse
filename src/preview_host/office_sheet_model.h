// office_sheet_model.h - first worksheet of legacy Excel / WPS spreadsheets.
#pragma once

#include "office_model.h"
#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>

namespace pulse::preview {

// Excel 97-2003 and WPS spreadsheets (.xls/.et: BIFF8 "Workbook" stream in a
// compound file; Excel 5/95 "Book" streams read too). The first worksheet in
// BOUNDSHEET order, starting at its first non-empty cell, up to rows x cols:
// shared strings (SST with CONTINUE), labels, numbers, RK / MULRK, booleans,
// errors and cached formula results. Numbers show as stored (%.15g), like the
// .xlsx reader. False (with *error) for encrypted or damaged files.
bool ReadXlsModel(const std::wstring& path, int rows, int cols, SheetModel& model, std::wstring* error);

struct XlsWorkbookInfo {
    std::vector<std::wstring> sheet_names;
    bool truncated = false;
    uint32_t bytes_read = 0;
};

bool ReadXlsSheet(const std::wstring& path, int rows, int cols, SheetModel& model,
                  std::wstring* error, uint32_t sheet_index, XlsWorkbookInfo& workbook);
bool MakeXlsTable(const std::wstring& path, std::wstring& payload, uint32_t& bytes_read,
                  uint32_t sheet_index = 0);

// Either format, by content (ZIP -> .xlsx reader, compound file -> BIFF).
bool ReadSheetModel(const std::wstring& path, int rows, int cols, SheetModel& model, std::wstring* error);

} // namespace pulse::preview
