#include "../ipc/elevated_transfer_transport.h"
#include <iostream>
#include <thread>

int main() {
    using namespace pulse::elevated;
    const auto name = L"\\\\.\\pipe\\PulseTransportReuse-" + std::to_wstring(GetCurrentProcessId());
    Handle server(CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 4096, 4096, 0, nullptr));
    Handle client(CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED, nullptr));
    if (!server || !client || !WaitPipeConnection(server.Get(), nullptr, 1000)) return 2;
    Nonce nonce{};
    bool sent = true;
    std::thread writer([&] {
        for (uint64_t i = 0; i < 1000; ++i) {
            Writer payload;
            if (i % 2) payload.Number(i);
            if (!WriteFrame(client.Get(), nullptr, nonce, Kind::Progress, i, payload, 2000)) { sent = false; break; }
        }
    });
    bool received = true;
    std::vector<unsigned char> payload;
    size_t growths = 0, capacity = 0;
    for (uint64_t i = 0; i < 1000; ++i) {
        Header header;
        if (!ReadFrame(server.Get(), nullptr, nonce, header, payload, {}, 2000)) { received = false; break; }
        received &= header.request == i && payload.size() == (i % 2 ? sizeof(uint64_t) : 0);
        if (i % 2) { Reader reader{payload}; received &= reader.Number<uint64_t>() == i && reader.Done(); }
        if (payload.capacity() != capacity) { ++growths; capacity = payload.capacity(); }
    }
    writer.join();
    const bool ok = sent && received && growths == 1;
    std::cout << (ok ? "[PASS] " : "[FAIL] ")
        << "1000 alternating empty/data frames preserve framing with one receive allocation\n";
    Header header;
    const auto start = GetTickCount64();
    const bool timeout = !ReadFrame(server.Get(), nullptr, nonce, header, payload, {}, 100) && GetLastError() == ERROR_TIMEOUT;
    std::cout << (timeout ? "[PASS] " : "[FAIL] ") << "idle receive cancels and drains at deadline\n";
    return ok && timeout && GetTickCount64() - start < 2000 ? 0 : 1;
}
