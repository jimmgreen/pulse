#include "../ui/preview_handler_pan.h"
#include <cstdio>

namespace pulse::ui {

struct PreviewHandlerPanTest {
    struct NativeCanvas { int clicks = 0, cancellations = 0; bool pressed = false; };
    static LRESULT CALLBACK CanvasProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        auto* canvas = reinterpret_cast<NativeCanvas*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (canvas) {
            if (msg == WM_LBUTTONDOWN) { canvas->pressed = true; SetCapture(hwnd); return 0; }
            if (msg == WM_LBUTTONUP) {
                if (canvas->pressed) ++canvas->clicks;
                canvas->pressed = false;
                if (GetCapture() == hwnd) ReleaseCapture();
                return 0;
            }
            if (msg == WM_CANCELMODE) {
                ++canvas->cancellations;
                canvas->pressed = false;
                if (GetCapture() == hwnd) ReleaseCapture();
                return 0;
            }
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    static int Run() {
        int failures = 0;
        auto check = [&](bool ok, const char* name) {
            std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
            if (!ok) ++failures;
        };
        WNDCLASSW wc{};
        wc.lpfnWndProc = CanvasProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"PulsePreviewPanTest";
        RegisterClassW(&wc);
        HWND host = CreateWindowW(wc.lpszClassName, L"", WS_POPUP,
            0, 0, 400, 300, nullptr, nullptr, wc.hInstance, nullptr);
        HWND child = CreateWindowW(wc.lpszClassName, L"", WS_CHILD | WS_HSCROLL | WS_VSCROLL,
            0, 0, 200, 200, host, nullptr, wc.hInstance, nullptr);
        if (!host || !child) {
            check(false, "create test host and provider HWNDs");
            if (host) DestroyWindow(host);
            return failures;
        }

        PreviewHandlerPan pan;
        pan.host_ = host;
        pan.input_ = std::make_shared<PreviewHandlerPan::InputState>();
        auto& input = *pan.input_;
        input.host = host;
        input.generation = 1;
        input.active = true;
        input.suppress_left_up = true;
        SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS, 0, 1000, 100, 500, 0};
        SetScrollInfo(child, SB_HORZ, &info, FALSE);
        SetScrollInfo(child, SB_VERT, &info, FALSE);
        POINT content_point{50, 50};
        ClientToScreen(child, &content_point);
        check(PreviewHandlerPan::CanGrabContent(host, child, content_point),
            "left grab starts inside document content");
        RECT child_client{};
        GetClientRect(child, &child_client);
        POINT scrollbar_point{child_client.right + 1, 50};
        ClientToScreen(child, &scrollbar_point);
        check(!PreviewHandlerPan::CanGrabContent(host, child, scrollbar_point),
            "native scrollbars retain left-button interaction");
        HWND button = CreateWindowW(L"BUTTON", L"Action", WS_CHILD,
            0, 0, 80, 24, child, nullptr, wc.hInstance, nullptr);
        POINT button_point{10, 10};
        ClientToScreen(button, &button_point);
        check(button && !PreviewHandlerPan::CanGrabContent(host, button, button_point),
            "preview buttons retain left-button interaction");
        DestroyWindow(button);
        check(!PreviewHandlerPan::CanGrabContent(host, GetDesktopWindow(), content_point),
            "left grabs outside the preview are not intercepted");
        NativeCanvas canvas;
        SetWindowLongPtrW(child, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&canvas));
        input.active = false;
        input.suppress_left_up = false;
        const POINT click{50, 50};
        PreviewHandlerPan::TrackPress(input, host, child, click);
        SendMessageW(child, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(50, 50));
        check(!PreviewHandlerPan::ConsumeMouse(input, WM_MOUSEMOVE, POINT{51, 51}) &&
              !input.active && GetCapture() == child,
            "minor pointer jitter remains a native click without taking capture");
        const bool swallowed_click = PreviewHandlerPan::ConsumeMouse(input, WM_LBUTTONUP, POINT{51, 51});
        if (!swallowed_click) SendMessageW(child, WM_LBUTTONUP, 0, MAKELPARAM(51, 51));
        check(!swallowed_click && canvas.clicks == 1 && !pan.dragging_,
            "custom PDF-style page button receives a complete native click");
        PreviewHandlerPan::TrackPress(input, host, child, click);
        SendMessageW(child, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(50, 50));
        PreviewHandlerPan::ConsumeMouse(input, WM_MOUSEMOVE,
            POINT{50 + GetSystemMetrics(SM_CXDRAG) + 20, 50});
        check(input.active && input.suppress_left_up,
            "crossing the system drag threshold starts panning");
        MSG message{};
        if (PeekMessageW(&message, child, WM_CANCELMODE, WM_CANCELMODE, PM_REMOVE))
            DispatchMessageW(&message);
        check(canvas.cancellations == 1 && !canvas.pressed,
            "taking over a drag cancels the provider's original press");
        if (PeekMessageW(&message, host, PreviewHandlerPan::kBegin, PreviewHandlerPan::kBegin, PM_REMOVE))
            pan.HandleMessage(message.message, message.wParam, message.lParam);
        check(pan.dragging_ && GetCapture() == host,
            "threshold-triggered drag captures host using original content target");
        const bool swallowed_drag = PreviewHandlerPan::ConsumeMouse(input, WM_LBUTTONUP, click);
        if (!swallowed_drag) SendMessageW(child, WM_LBUTTONUP, 0, MAKELPARAM(50, 50));
        check(swallowed_drag && canvas.clicks == 1,
            "drag release never activates a PDF-style page button");
        while (PeekMessageW(&message, host, PreviewHandlerPan::kBegin, PreviewHandlerPan::kEnd, PM_REMOVE))
            pan.HandleMessage(message.message, message.wParam, message.lParam);
        check(!pan.dragging_, "drag release clears capture after threshold gesture");
        PreviewHandlerPan::TrackPress(input, host, child, click);
        pan.Disable();
        PreviewHandlerPan::ConsumeMouse(input, WM_MOUSEMOVE, POINT{200, 200});
        check(!input.active && !input.pending,
            "hiding or switching the preview cancels an unstarted gesture");
        pan.host_ = host;
        input.host = host;

        check(PreviewHandlerPan::QueueBegin(input, host, POINT{100, 100}) &&
              !pan.dragging_ && GetCapture() != host &&
              PeekMessageW(&message, host, PreviewHandlerPan::kBegin,
                  PreviewHandlerPan::kBegin, PM_REMOVE) && message.wParam == input.generation,
            "pan takeover is queued asynchronously after drag detection");
        pan.Begin(child, POINT{100, 100});
        check(pan.dragging_ && GetCapture() == host, "left grab captures host");
        check(pan.horizontal_.thumb && pan.vertical_.thumb, "both native scroll axes discovered");

        const bool consumed_move = PreviewHandlerPan::ConsumeMouse(input, WM_MOUSEMOVE, POINT{130, 70});
        check(!consumed_move && pan.horizontal_.position == 500,
            "hook preserves pointer motion and does not synchronously scroll");
        const bool got_move = PeekMessageW(&message, host, PreviewHandlerPan::kMove,
            PreviewHandlerPan::kMove, PM_REMOVE) != FALSE;
        check(got_move, "hook posts move to preview worker");
        if (got_move) pan.HandleMessage(message.message, message.wParam, message.lParam);
        check(pan.horizontal_.position < 500 && pan.vertical_.position > 500,
            "worker moves content with the hand on both axes");
        check(!PreviewHandlerPan::ConsumeMouse(input, WM_MOUSEWHEEL, POINT{}) &&
              !PreviewHandlerPan::ConsumeMouse(input, WM_MOUSEHWHEEL, POINT{}),
            "native wheel browsing passes through during grab");
        check(!PreviewHandlerPan::ConsumeMouse(input, WM_MBUTTONDOWN, POINT{}) &&
              !PreviewHandlerPan::ConsumeMouse(input, WM_MBUTTONUP, POINT{}) && input.active,
            "middle button neither pans nor cancels an active left grab");
        const double still = pan.vertical_.position;
        pan.HandleMessage(WM_TIMER, PreviewHandlerPan::kCaptureTimer, 0);
        check(pan.vertical_.position == still, "stationary grab has no timed scrolling");
        pan.Scroll(pan.vertical_, true, -100000, POINT{130, -99930});
        check(pan.vertical_.position == pan.vertical_.maximum, "large drag clamps at boundary");

        PreviewHandlerPan::ConsumeMouse(input, WM_MOUSEMOVE, POINT{100, 100});
        check(PreviewHandlerPan::ConsumeMouse(input, WM_LBUTTONUP, POINT{}),
            "matching left release is consumed");
        const double released_position = pan.horizontal_.position;
        if (PeekMessageW(&message, host, PreviewHandlerPan::kMove,
                PreviewHandlerPan::kMove, PM_REMOVE))
            pan.HandleMessage(message.message, message.wParam, message.lParam);
        check(pan.horizontal_.position == released_position,
            "delayed move cannot replay scrolling after release");
        const bool got_end = PeekMessageW(&message, host, PreviewHandlerPan::kEnd,
            PreviewHandlerPan::kEnd, PM_REMOVE) != FALSE;
        if (got_end) pan.HandleMessage(message.message, message.wParam, message.lParam);
        check(got_end && !pan.dragging_ && GetCapture() != host,
            "asynchronous release ends grab and releases capture");
        pan.HandleMessage(PreviewHandlerPan::kBegin, input.generation, MAKELPARAM(100, 100));
        check(!pan.dragging_ && GetCapture() != host,
            "delayed press cannot begin a grab after release");

        input.active = true;
        input.suppress_left_up = true;
        pan.Begin(child, POINT{});
        pan.HandleMessage(WM_CANCELMODE, 0, 0);
        check(!pan.dragging_ && !input.active && GetCapture() != host,
            "cancellation clears gesture and capture");
        check(PreviewHandlerPan::ConsumeMouse(input, WM_LBUTTONUP, POINT{}),
            "release after cancellation cannot leak to provider");
        input.suppress_left_up = true;
        const UINT old_generation = input.generation.load();
        pan.Disable();
        pan.HandleMessage(PreviewHandlerPan::kMove, old_generation, MAKELPARAM(90, 90));
        check(!pan.dragging_ && input.generation != old_generation,
            "hide invalidates queued gesture messages");
        check(PreviewHandlerPan::ConsumeMouse(input, WM_LBUTTONUP, POINT{}),
            "release after hide cannot leak into another window");
        check(!PreviewHandlerPan::ConsumeMouse(input, WM_LBUTTONUP, POINT{}) &&
              !PreviewHandlerPan::ConsumeMouse(input, WM_LBUTTONDOWN, POINT{}),
            "unrelated left-button events pass through");

        check(!PreviewHandlerPan::ConsumeMouse(input, WM_MBUTTONDOWN, POINT{}) &&
              !PreviewHandlerPan::ConsumeMouse(input, WM_MBUTTONUP, POINT{}) && !input.active,
            "middle button cannot start preview panning");

        pan.host_ = host;
        info.nMax = 100000;
        SetScrollInfo(child, SB_VERT, &info, FALSE);
        auto fallback = pan.FindAxis(child, SB_VERT);
        check(!fallback.thumb && fallback.target == child,
            "large ranges avoid truncated native thumb positions");
        pan.Scroll(fallback, true, 48, POINT{100, 100});
        check(PeekMessageW(&message, child, WM_MOUSEWHEEL, WM_MOUSEWHEEL, PM_REMOVE) &&
              static_cast<SHORT>(HIWORD(message.wParam)) == WHEEL_DELTA,
            "custom-canvas vertical fallback follows drag displacement");
        pan.Scroll(fallback, false, 48, POINT{100, 100});
        check(PeekMessageW(&message, child, WM_MOUSEHWHEEL, WM_MOUSEHWHEEL, PM_REMOVE) &&
              static_cast<SHORT>(HIWORD(message.wParam)) == -WHEEL_DELTA,
            "custom-canvas horizontal fallback uses grab direction");
        check(PreviewGrabCursor(false) && PreviewGrabCursor(true) &&
              PreviewGrabCursor(false) != PreviewGrabCursor(true),
            "distinct open and closed hand cursors created");
        pan.Disable();
        DestroyWindow(host);
        return failures;
    }
};

} // namespace pulse::ui

int main() {
    return pulse::ui::PreviewHandlerPanTest::Run() == 0 ? 0 : 1;
}
