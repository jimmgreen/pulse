// protocol.h — Pulse UI process <-> pulse_shell.exe wire protocol (named pipe).
//
// Transport: named pipe `\\.\pipe\pulse_shell_<ui-pid>`, byte mode, blocking I/O.
// The UI process (client) creates the pipe name from its own PID and passes the
// same PID to pulse_shell.exe as argv[1]; the shell host is the pipe server.
//
// Framing: fixed 16-byte header + payload.
//   struct MsgHeader { u32 magic; u32 type; u32 request_id; u32 payload_size; }
// Payload is a packed sequence of fields (little-endian, no alignment):
//   string  := u32 wchar_count (excluding terminator) + wchar data
//   u32     := 4 bytes
//
// Message flow:
//   client -> host : REQ_DELETE_RECYCLE / REQ_REALDELETE /
//                    REQ_RENAME / REQ_NEW_FOLDER / REQ_NEW_FILE /
//                    REQ_CANCEL / REQ_PING / REQ_SHUTDOWN / REQ_RESTORE_RECYCLE /
//                    REQ_CTX_QUERY / REQ_CTX_INVOKE / REQ_CTX_CLOSE /
//                    REQ_SHELL_VERB / REQ_SHELL_TRANSFER
//   host -> client : RSP_PROGRESS (0..N) then exactly one RSP_DONE per request,
//                    RSP_PONG for REQ_PING, RSP_CTX_ITEMS for REQ_CTX_QUERY.
//
// Context-menu sessions (Explorer verbs): REQ_CTX_QUERY builds the third-party
// IContextMenu on a dedicated STA thread inside the host; its request id is the
// session id. The IContextMenu/HMENU stay alive on that thread until
// REQ_CTX_INVOKE (host closes the session after invoking, answering RSP_DONE)
// or REQ_CTX_CLOSE (no response). Sessions self-expire after 120s.
//
// Cancellation: REQ_CANCEL carries the target request id in the header's
// request_id field; the host sets an atomic flag that the progress sink checks
// in every callback, aborting the running IFileOperation.
//
// Payload layouts (all strings UTF-16, wchar_count first):
//   REQ_DELETE_RECYCLE /
//   REQ_REALDELETE /
//   REQ_RESTORE_RECYCLE  : count(u32) + paths(string * count)
//   REQ_RENAME               : path(string) + new_name(string)  (name only, not a path)
//   REQ_NEW_FOLDER /
//   REQ_NEW_FILE             : path(string)  (full path of the item to create)
//   REQ_CANCEL / REQ_PING /
//   REQ_SHUTDOWN             : empty
//   REQ_CTX_QUERY            : owner_hwnd(u32) + flags(u32, CTXF_*) +
//                              paths(string array) + disabled_clsids(string array)
//   REQ_CTX_INVOKE           : session_id(u32) + item_id(u32) +
//                              verb(string) + text(string)
//   REQ_CTX_CLOSE            : session_id(u32)
//   REQ_SHELL_VERB           : owner_hwnd(u32) + flags(u32, SVF_*) + verb(string) +
//                              paths(string array). Runs one of Explorer's own
//                              verbs (empty = the default verb; "paste" with
//                              SVF_BACKGROUND pastes the clipboard into paths[0];
//                              "drop:copy" / "drop:move" with SVF_BACKGROUND drop
//                              paths[1..] on the folder paths[0])
//                              on shell parsing names, e.g. phone items. RSP_DONE.
//   REQ_SHELL_TRANSFER       : op(u32, STO_*) + flags(u32, STF_*) + dest(string) +
//                              sources(string array) + names(string array).
//                              One IFileOperation on shell parsing names or
//                              paths (phones: MTP), run on its own STA thread so
//                              a long copy never holds other requests. COPY /
//                              MOVE: sources into the folder dest, names[i] the
//                              new leaf name of sources[i] (empty = keep it).
//                              NEW_FOLDER / NEW_FILE: names[0] made in dest,
//                              no sources. DELETE: sources deleted for good
//                              (dest empty). RSP_PROGRESS (percent of the whole
//                              operation), then RSP_DONE. Never retried after
//                              a host restart: a half-done copy is not re-run.
//   RSP_PROGRESS             : percent(f32) + current_item(string) +
//                              items_done(u32) + total_items(u32)
//   RSP_DONE                 : result_hresult(u32) + cancelled(u32) + error_text(string)
//   RSP_PONG                 : empty
//   RSP_CTX_ITEMS            : session_id(u32) + flags(u32, CTX_ITEMS_*) +
//                              count(u32) + per item:
//                              item_id(u32) + flags(u32, CTX_ITEM_*) +
//                              verb(string) + text(string) +
//                              clsid(string) + handler(string) +
//                              slow_clsids(string array, optional; final payload
//                              only: the one handler provably hung on its own
//                              at the stuck limit, see collect_hung) +
//                              handlers(optional; count(u32) + per handler in
//                              menu order: clsid(string) + flags(u32,
//                              CTX_HANDLER_*) + elapsed_ms(u32) + rows(u32)) +
//                              icons(optional, after handlers; count(u32) +
//                              per icon: row(u32, index into this payload's
//                              items) + width(u32) + height(u32) +
//                              premultiplied BGRA bytes, see menu_icon.h)
#pragma once
#include "../common/menu_icon.h"

#include <windows.h>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace pulse::ipc {

constexpr uint32_t kMagic = 0x53504C50; // 'PLPS'
constexpr size_t kMaxPayload = 16 * 1024 * 1024;

enum MsgType : uint32_t {
    REQ_DELETE_RECYCLE = 3,
    REQ_REALDELETE = 4,
    REQ_RENAME = 5,
    REQ_CANCEL = 6,
    REQ_PING = 7,
    REQ_SHUTDOWN = 8,
    REQ_NEW_FOLDER = 9,
    REQ_NEW_FILE = 10,
    REQ_RESTORE_RECYCLE = 11,
    REQ_CTX_QUERY = 12,
    REQ_CTX_INVOKE = 13,
    REQ_CTX_CLOSE = 14,
    REQ_SHELL_VERB = 15,
    REQ_SHELL_TRANSFER = 16,
    RSP_PROGRESS = 100,
    RSP_DONE = 101,
    RSP_PONG = 102,
    RSP_CTX_ITEMS = 103,
};

// REQ_SHELL_VERB flags.
enum ShellVerbFlags : uint32_t {
    SVF_BACKGROUND = 1,  // paths[0] is a folder; the verb is its background verb
};

// REQ_SHELL_TRANSFER operations and flags.
enum ShellTransferOp : uint32_t {
    STO_COPY = 0,
    STO_MOVE = 1,
    STO_NEW_FOLDER = 2,
    STO_NEW_FILE = 3,
    STO_DELETE = 4,
};
enum ShellTransferFlags : uint32_t {
    STF_RENAME_ON_COLLISION = 1,  // the shell names a colliding copy "a (2).jpg"
};

// REQ_CTX_QUERY flags.
enum CtxQueryFlags : uint32_t {
    CTXF_EXTENDED = 1,    // Shift held: include extended verbs
    CTXF_BACKGROUND = 2,  // paths[0] is a folder; build its background menu
};

// RSP_CTX_ITEMS are preorder rows. Bits 8..15 preserve submenu depth.
// Legacy CHILD rows without depth are immediate children (depth 1).
enum CtxItemFlags : uint32_t {
    CTX_ITEM_ENABLED = 1,
    CTX_ITEM_SEPARATOR_AFTER = 2,
    CTX_ITEM_HAS_CHILDREN = 4,  // submenu header; item_id is not invokable
    CTX_ITEM_CHILD = 8,         // belongs to the nearest preceding header
};
inline constexpr uint32_t CTX_ITEM_DEPTH_SHIFT = 8;
inline uint32_t PackCtxItemDepth(uint8_t depth) { return uint32_t(depth) << CTX_ITEM_DEPTH_SHIFT; }
inline uint8_t UnpackCtxItemDepth(uint32_t flags) {
    const auto depth = static_cast<uint8_t>((flags >> CTX_ITEM_DEPTH_SHIFT) & 0xFF);
    return depth ? depth : (flags & CTX_ITEM_CHILD) ? 1 : 0;
}
// Bits 16..31 of the per-item flags carry the row's access key (UTF-16 code
// unit of the raw label's "&X", upper-cased; 0 = none). Older readers only
// test the low bits above, so the field is backward compatible both ways.
constexpr uint32_t CTX_ITEM_MNEMONIC_SHIFT = 16;
inline uint32_t PackCtxItemMnemonic(wchar_t key) {
    return static_cast<uint32_t>(static_cast<uint16_t>(key)) << CTX_ITEM_MNEMONIC_SHIFT;
}
inline wchar_t UnpackCtxItemMnemonic(uint32_t flags) {
    return static_cast<wchar_t>(flags >> CTX_ITEM_MNEMONIC_SHIFT);
}

// RSP_CTX_ITEMS message flags (after session_id).
enum CtxItemsMsgFlags : uint32_t {
    CTX_ITEMS_PARTIAL = 1,  // more handlers still running; keep the session pending
};

// State of each handler the host queried for one RSP_CTX_ITEMS payload, so a
// partial answer tells "finished with no rows" from "not finished yet".
enum CtxHandlerFlags : uint32_t {
    CTX_HANDLER_DONE = 1,       // finished; its rows (maybe none) are in the payload
    CTX_HANDLER_DUPLICATE = 2,  // finished, rows dropped as copies of classic rows
};
struct CtxHandlerStatus {
    std::wstring clsid;
    uint32_t flags = 0;
    uint32_t elapsed_ms = 0;  // finished: its own query time; else time so far
    uint32_t rows = 0;        // rows it put in this payload
};
constexpr uint32_t kMaxCtxHandlerStatuses = 256;

struct MsgHeader {
    uint32_t magic = kMagic;
    uint32_t type = 0;
    uint32_t request_id = 0;
    uint32_t payload_size = 0;
};
static_assert(sizeof(MsgHeader) == 16);

// ---------------------------------------------------------------------------
// Payload writer/reader helpers (header-only, shared by both processes).
// ---------------------------------------------------------------------------
class PayloadWriter {
public:
    void PutU32(uint32_t v) {
        size_t off = buf_.size();
        buf_.resize(off + 4);
        std::memcpy(buf_.data() + off, &v, 4);
    }
    void PutU64(uint64_t v) {
        size_t off = buf_.size();
        buf_.resize(off + 8);
        std::memcpy(buf_.data() + off, &v, 8);
    }
    void PutF32(float v) {
        size_t off = buf_.size();
        buf_.resize(off + 4);
        std::memcpy(buf_.data() + off, &v, 4);
    }
    void PutString(const std::wstring& s) {
        PutU32((uint32_t)s.size());
        size_t off = buf_.size();
        buf_.resize(off + s.size() * sizeof(wchar_t));
        if (!s.empty()) std::memcpy(buf_.data() + off, s.data(), s.size() * sizeof(wchar_t));
    }
    void PutStringArray(const std::vector<std::wstring>& v) {
        PutU32((uint32_t)v.size());
        for (const auto& s : v) PutString(s);
    }
    void PutBytes(const uint8_t* bytes, size_t size) {
        size_t off = buf_.size();
        buf_.resize(off + size);
        if (size) std::memcpy(buf_.data() + off, bytes, size);
    }
    const std::vector<uint8_t>& data() const { return buf_; }
private:
    std::vector<uint8_t> buf_;
};

class PayloadReader {
public:
    PayloadReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}
    bool GetU32(uint32_t& v) {
        if (off_ + 4 > size_) return false;
        std::memcpy(&v, data_ + off_, 4);
        off_ += 4;
        return true;
    }
    bool GetU64(uint64_t& v) {
        if (off_ + 8 > size_) return false;
        std::memcpy(&v, data_ + off_, 8);
        off_ += 8;
        return true;
    }
    bool GetF32(float& v) {
        if (off_ + 4 > size_) return false;
        std::memcpy(&v, data_ + off_, 4);
        off_ += 4;
        return true;
    }
    bool GetString(std::wstring& s) {
        uint32_t n = 0;
        if (!GetU32(n)) return false;
        if ((uint64_t)n * sizeof(wchar_t) > size_ - off_) return false;
        s.assign(reinterpret_cast<const wchar_t*>(data_ + off_), n);
        off_ += (size_t)n * sizeof(wchar_t);
        return true;
    }
    bool GetStringArray(std::vector<std::wstring>& v) {
        uint32_t n = 0;
        if (!GetU32(n)) return false;
        v.clear();
        v.reserve(n);
        for (uint32_t i = 0; i < n; ++i) {
            std::wstring s;
            if (!GetString(s)) return false;
            v.push_back(std::move(s));
        }
        return true;
    }
    bool TryStringArray(std::vector<std::wstring>& v) {
        if (off_ >= size_) {
            v.clear();
            return true;
        }
        return GetStringArray(v);
    }
    bool GetBytes(std::vector<uint8_t>& v, size_t size) {
        if (size > size_ - off_) return false;
        v.assign(data_ + off_, data_ + off_ + size);
        off_ += size;
        return true;
    }
    size_t remaining() const { return off_ < size_ ? size_ - off_ : 0; }
private:
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
    size_t off_ = 0;
};

inline void PutCtxHandlerStatuses(PayloadWriter& w, const std::vector<CtxHandlerStatus>& v) {
    w.PutU32(static_cast<uint32_t>(v.size()));
    for (const auto& h : v) {
        w.PutString(h.clsid);
        w.PutU32(h.flags);
        w.PutU32(h.elapsed_ms);
        w.PutU32(h.rows);
    }
}

// Optional trailing field: absent (older host) reads as an empty list; a
// damaged one is rejected without touching the rows read before it.
inline bool TryCtxHandlerStatuses(PayloadReader& r, std::vector<CtxHandlerStatus>& v) {
    v.clear();
    if (r.remaining() == 0) return true;
    uint32_t n = 0;
    if (!r.GetU32(n) || n > kMaxCtxHandlerStatuses) return false;
    std::vector<CtxHandlerStatus> out;
    out.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        CtxHandlerStatus h;
        if (!r.GetString(h.clsid) || !r.GetU32(h.flags) || !r.GetU32(h.elapsed_ms) ||
            !r.GetU32(h.rows))
            return false;
        out.push_back(std::move(h));
    }
    v = std::move(out);
    return true;
}

// Optional trailing field after the handler statuses: the app icons Explorer
// shows next to handler rows (MIIM_BITMAP pixels). Item is CtxItemOut on the
// host and CtxMenuItem on the client; both carry `common::MenuIconRef icon`.
constexpr uint32_t kMaxCtxItemIcons = 512;

template <typename Item>
inline void PutCtxItemIcons(PayloadWriter& w, const std::vector<Item>& items) {
    uint32_t count = 0;
    for (const auto& it : items)
        if (it.icon && it.icon->Valid()) ++count;
    w.PutU32(count);
    for (size_t i = 0; i < items.size(); ++i) {
        const auto& icon = items[i].icon;
        if (!icon || !icon->Valid()) continue;
        w.PutU32(static_cast<uint32_t>(i));
        w.PutU32(icon->width);
        w.PutU32(icon->height);
        w.PutBytes(icon->bgra.data(), icon->bgra.size());
    }
}

// Absent (older host) leaves the rows without icons; a damaged field stops
// there, rows read before it keep theirs.
template <typename Item>
inline bool TryCtxItemIcons(PayloadReader& r, std::vector<Item>& items) {
    if (r.remaining() == 0) return true;
    uint32_t n = 0;
    if (!r.GetU32(n) || n > kMaxCtxItemIcons) return false;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t row = 0, width = 0, height = 0;
        if (!r.GetU32(row) || !r.GetU32(width) || !r.GetU32(height)) return false;
        if (width == 0 || height == 0 || width > common::kMaxMenuIconEdge ||
            height > common::kMaxMenuIconEdge)
            return false;
        auto pixels = std::make_shared<common::MenuIconPixels>();
        pixels->width = width;
        pixels->height = height;
        if (!r.GetBytes(pixels->bgra, static_cast<size_t>(width) * height * 4u)) return false;
        if (row < items.size()) items[row].icon = std::move(pixels);
    }
    return true;
}

inline std::wstring PipeNameFor(uint32_t ui_pid) {
    return L"\\\\.\\pipe\\pulse_shell_" + std::to_wstring(ui_pid);
}

// ---------------------------------------------------------------------------
// Blocking-style pipe I/O over an OVERLAPPED handle.
//
// Both ends read and write the same pipe handle from different threads
// concurrently. On a synchronous handle that serializes (a blocked ReadFile
// starves WriteFile), so the handle must be created with FILE_FLAG_OVERLAPPED
// and every call driven through an explicit OVERLAPPED + GetOverlappedResult.
// ---------------------------------------------------------------------------
inline bool PipeRead(HANDLE pipe, uint8_t* out, DWORD size) {
    DWORD left = size;
    while (left > 0) {
        OVERLAPPED ol{};
        ol.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ol.hEvent) return false;
        DWORD got = 0;
        BOOL ok = ReadFile(pipe, out, left, &got, &ol);
        if (!ok && GetLastError() == ERROR_IO_PENDING)
            ok = GetOverlappedResult(pipe, &ol, &got, TRUE);
        DWORD err = GetLastError();
        CloseHandle(ol.hEvent);
        if (!ok || got == 0) {
            SetLastError(err);
            return false;
        }
        out += got;
        left -= got;
    }
    return true;
}

inline bool PipeWrite(HANDLE pipe, const uint8_t* data, DWORD size) {
    DWORD left = size;
    while (left > 0) {
        OVERLAPPED ol{};
        ol.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ol.hEvent) return false;
        DWORD written = 0;
        BOOL ok = WriteFile(pipe, data, left, &written, &ol);
        if (!ok && GetLastError() == ERROR_IO_PENDING)
            ok = GetOverlappedResult(pipe, &ol, &written, TRUE);
        DWORD err = GetLastError();
        CloseHandle(ol.hEvent);
        if (!ok || written == 0) {
            SetLastError(err);
            return false;
        }
        data += written;
        left -= written;
    }
    return true;
}

} // namespace pulse::ipc
