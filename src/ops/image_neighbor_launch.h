#pragma once
#include <windows.h>
#include <string>

namespace pulse::ops {
enum class ImageNeighborLaunchStage { Initialize, File, Query, Owner, Launch, Complete, PhotosProtocol = 7 };
struct ImageNeighborLaunchResult {
    bool launched = false;
    HRESULT error = E_FAIL;
    ImageNeighborLaunchStage stage = ImageNeighborLaunchStage::Initialize;
    bool query_prepared = false;
    bool shallow = false;
    bool ordered_by_name = false;
    unsigned filter_count = 0;
    bool photos_protocol_attempted = false;
    HRESULT photos_protocol_error = S_OK;
};
// Blocking helpers for an existing background worker, never the UI thread.
ImageNeighborLaunchResult LaunchImageWithNeighbors(const std::wstring& path, HWND owner) noexcept;
// Exercises exactly the production preparation path without launching any app.
ImageNeighborLaunchResult PrepareImageNeighborLaunch(const std::wstring& path, HWND owner) noexcept;
}
