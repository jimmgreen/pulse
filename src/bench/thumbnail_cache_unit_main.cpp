#include <cstring>
bool RunThumbnailCacheTests();
bool RunThumbnailOrientationZoomRegression();
bool RunThumbnailRenameRegression();
bool RunBoundedImageRetryRegression();
bool RunThumbnailRetryRegression();
bool RunThumbnailDownloadRetryRegression();
bool RunThumbnailLaneWakeRegression();
bool RunEmptyThumbnailDownloadRegression();
bool RunThumbnailEditorSaveRegression();
bool RunThumbnailInterestRegression();
bool RunThumbnailVolumeRegression();
bool RunThumbnailMemoryRegression();
bool RunThumbnailQueueRegression();
bool RunThumbnailIdleRegression(bool baseline);
bool RunThumbnailUploadRegression();
bool RunPsdProgressiveRegression(const char* fixture);
bool RunThumbnailInlineWarmRegression();
int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--empty-download") == 0) return RunEmptyThumbnailDownloadRegression() ? 0 : 1;
    if (argc == 2 && std::strcmp(argv[1], "--lane-wake") == 0) return RunThumbnailLaneWakeRegression() ? 0 : 1;
    if (argc == 2 && std::strcmp(argv[1], "--orientation-zoom") == 0) return RunThumbnailOrientationZoomRegression() ? 0 : 1;
    if (argc == 2 && std::strcmp(argv[1], "--rename") == 0) return RunThumbnailRenameRegression() ? 0 : 1;
    if (argc == 2 && std::strcmp(argv[1], "--bounded-image-retry") == 0) return RunBoundedImageRetryRegression() ? 0 : 1;
    if (argc == 2 && std::strcmp(argv[1], "--editor-save") == 0) return RunThumbnailEditorSaveRegression() ? 0 : 1;
    if (argc == 2 && std::strcmp(argv[1], "--download-retry") == 0) return RunThumbnailDownloadRetryRegression() ? 0 : 1;
    if (argc >= 2 && std::strcmp(argv[1], "--psd-progressive") == 0)
        return RunPsdProgressiveRegression(argc >= 3 ? argv[2] : nullptr) ? 0 : 1;
    if (argc == 2 && std::strcmp(argv[1], "--retry") == 0) return RunThumbnailRetryRegression() ? 0 : 1;
    if (argc == 2 && std::strcmp(argv[1], "--interests") == 0) return RunThumbnailInterestRegression() ? 0 : 1;
    if (argc == 2 && std::strcmp(argv[1], "--upload") == 0) return RunThumbnailUploadRegression() ? 0 : 1;
    if (argc == 2 && std::strcmp(argv[1], "--idle") == 0) return RunThumbnailIdleRegression(false) ? 0 : 1;
    if (argc == 2 && std::strcmp(argv[1], "--idle-baseline") == 0) return RunThumbnailIdleRegression(true) ? 0 : 1;
    if (argc == 2 && std::strcmp(argv[1], "--queue") == 0) return RunThumbnailQueueRegression() ? 0 : 1;
    if (argc == 2 && std::strcmp(argv[1], "--memory") == 0) return RunThumbnailMemoryRegression() ? 0 : 1;
    if (argc == 2 && std::strcmp(argv[1], "--volume") == 0) return RunThumbnailVolumeRegression() ? 0 : 1;
    if (argc == 2 && std::strcmp(argv[1], "--inline-warm") == 0) return RunThumbnailInlineWarmRegression() ? 0 : 1;
    return RunThumbnailCacheTests() ? 0 : 1;
}
