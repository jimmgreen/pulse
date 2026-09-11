#pragma once
#include <windows.h>

namespace pulse::ui {

inline HCURSOR PreviewGrabCursor(bool dragging) {
    struct Cursors {
        HCURSOR open = Create(false);
        HCURSOR closed = Create(true);

        ~Cursors() {
            if (open) DestroyCursor(open);
            if (closed) DestroyCursor(closed);
        }

        static HCURSOR Create(bool closed) {
            const POINT open_hand[] = {
                {9, 27}, {5, 20}, {2, 16}, {2, 13}, {4, 12}, {8, 16},
                {7, 7}, {8, 5}, {10, 5}, {12, 14}, {12, 3}, {14, 2},
                {16, 3}, {16, 13}, {18, 5}, {20, 5}, {21, 7}, {20, 15},
                {23, 10}, {25, 10}, {26, 12}, {23, 23}, {20, 27}
            };
            const POINT closed_hand[] = {
                {9, 27}, {6, 23}, {3, 18}, {3, 15}, {5, 14}, {8, 17},
                {7, 11}, {8, 9}, {11, 9}, {12, 11}, {12, 8}, {15, 7},
                {17, 9}, {20, 8}, {22, 10}, {24, 10}, {26, 12},
                {25, 22}, {21, 27}
            };
            HDC dc = CreateCompatibleDC(nullptr);
            HBITMAP mask = CreateBitmap(32, 32, 1, 1, nullptr);
            HBITMAP color = CreateBitmap(32, 32, 1, 1, nullptr);
            if (!dc || !mask || !color) {
                if (dc) DeleteDC(dc);
                if (mask) DeleteObject(mask);
                if (color) DeleteObject(color);
                return nullptr;
            }
            HGDIOBJ previous = SelectObject(dc, mask);
            RECT bounds{0, 0, 32, 32};
            FillRect(dc, &bounds, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
            SelectObject(dc, GetStockObject(BLACK_PEN));
            SelectObject(dc, GetStockObject(BLACK_BRUSH));
            const POINT* points = closed ? closed_hand : open_hand;
            const int count = closed ? ARRAYSIZE(closed_hand) : ARRAYSIZE(open_hand);
            Polygon(dc, points, count);
            SelectObject(dc, color);
            FillRect(dc, &bounds, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            SelectObject(dc, GetStockObject(WHITE_BRUSH));
            Polygon(dc, points, count);
            if (closed) {
                MoveToEx(dc, 12, 12, nullptr); LineTo(dc, 12, 17);
                MoveToEx(dc, 17, 11, nullptr); LineTo(dc, 17, 16);
                MoveToEx(dc, 21, 12, nullptr); LineTo(dc, 21, 17);
            }
            SelectObject(dc, previous);
            DeleteDC(dc);
            // Monochrome cursors use a double-height AND/XOR mask.
            BYTE and_bits[128]{}, xor_bits[128]{}, cursor_bits[256]{};
            GetBitmapBits(mask, sizeof(and_bits), and_bits);
            GetBitmapBits(color, sizeof(xor_bits), xor_bits);
            for (int i = 0; i < 128; ++i) {
                cursor_bits[i] = and_bits[i];
                cursor_bits[i + 128] = xor_bits[i];
            }
            DeleteObject(mask);
            DeleteObject(color);
            HBITMAP combined = CreateBitmap(32, 64, 1, 1, cursor_bits);
            ICONINFO info{};
            info.xHotspot = 13;
            info.yHotspot = 16;
            info.hbmMask = combined;
            HCURSOR result = combined ? static_cast<HCURSOR>(CreateIconIndirect(&info)) : nullptr;
            if (combined) DeleteObject(combined);
            return result;
        }
    };
    static const Cursors cursors;
    HCURSOR cursor = dragging ? cursors.closed : cursors.open;
    return cursor ? cursor : LoadCursorW(nullptr, IDC_SIZEALL);
}

} // namespace pulse::ui
