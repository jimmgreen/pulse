    static bool RenameRegression() {
        bool ok = true;
        const auto check = [&](bool value, const char* name) {
            std::printf("[%s] %s\n", value ? "PASS" : "FAIL", name); ok &= value;
        };
        ThumbnailCache cache;
        const std::wstring from = L"C:\\isolated\\before.jpeg", to = L"C:\\isolated\\002.jpeg";
        const auto request = [&](const std::wstring& path, uint32_t pixels, uint64_t modified = 7) {
            ThumbnailCache::Request req;
            req.id = cache.next_id_++; req.epoch = cache.epoch_.load();
            req.path = path; req.pixels = pixels; req.modified = modified; req.size = 100;
            req.key = cache.Key(path, pixels, modified, 100);
            req.identity = cache.Key(path, 0, modified, 100); return req;
        };
        const auto bitmap = [](uint32_t edge) {
            ThumbnailCache::Item item; item.kind = ipc::PreviewContentKind::Bitmap;
            item.w = item.h = edge; item.stride = edge * 4;
            item.pixels.assign(size_t(edge) * edge * 4, 0xff); item.cost = item.pixels.size(); return item;
        };
        const auto old = request(from, 256), small_request = request(from, 64), late = request(to, 256);
        cache.StoreResult(old, bitmap(256)); cache.StoreResult(small_request, bitmap(64));
        ThumbnailCache::Item failed; failed.failed = true;
        cache.StoreResult(late, std::move(failed)); cache.pending_.insert(late.key);
        cache.RenameImage(from, to, 7, 100);
        const auto good = [&](const std::wstring& path, uint32_t edge) {
            const auto it = cache.items_.find(cache.Key(path, edge, 7, 100));
            return it != cache.items_.end() && !it->second.failed && it->second.w == edge &&
                it->second.pixels.size() == size_t(edge) * edge * 4;
        };
        check(good(to, 256) && good(to, 64), "rename preserves full-quality pixels at every cached size over negative destination");
        ThumbnailCache::Item late_failure; late_failure.failed = true;
        cache.StoreResult(late, std::move(late_failure));
        check(good(to, 256), "late pre-rename failure cannot turn transferred thumbnail white");
        check(!cache.StoreResult(late, bitmap(32)), "late low-resolution response is rejected for persistent storage too");
        check(good(to, 256), "late pre-rename low-resolution response cannot blur transferred thumbnail");
        check(!cache.pending_.contains(late.key), "late request releases pending state");
        cache.RenameImage(to, from, 7, 100);
        check(good(from, 256), "rename back restores original thumbnail without folder navigation");
        cache.RenameImage(from, L"C:\\isolated\\case.JPEG", 7, 100);
        check(good(L"C:\\isolated\\case.JPEG", 256), "extension case change preserves artwork");
        cache.RenameImage(L"C:\\isolated\\case.JPEG", to, 8, 100);
        check(!good(to, 256), "changed timestamp does not transfer stale content");
        cache.RenameImage(L"C:\\isolated\\case.JPEG", L"C:\\isolated\\changed.png", 7, 100);
        check(!good(L"C:\\isolated\\changed.png", 256), "changed extension is decoded anew");
        size_t bytes = 0; for (const auto& [key, item] : cache.items_) bytes += item.cost;
        check(bytes == cache.cache_bytes_ && cache.lru_.size() == cache.items_.size(), "rename maintains cache byte and LRU accounting");
        const auto after = request(L"C:\\isolated\\case.JPEG", 256);
        cache.StoreResult(after, bitmap(256));
        check(cache.items_.at(after.key).renamed_before_request == 0, "new requests remain eligible after rename");
        fs::DirEntry before; before.name = L"old.jpeg"; before.size = 100;
        before.mtime.dwLowDateTime = 7; before.ctime.dwLowDateTime = 3;
        auto after_entry = before; after_entry.name = L"new.jpeg";
        fs::DirNotifyEvent event; event.action = FILE_ACTION_RENAMED_NEW_NAME;
        event.old_name = before.name; event.name = after_entry.name;
        unsigned transfers = 0;
        const auto transfer = [&](const std::wstring&, const std::wstring&, uint64_t, uint64_t) { ++transfers; };
        app::TransferRenamedThumbnails(L"C:\\fixture", {before}, {after_entry}, {event}, transfer);
        check(transfers == 1, "notification transfers unchanged file metadata");
        after_entry.size++;
        app::TransferRenamedThumbnails(L"C:\\fixture", {before}, {after_entry}, {event}, transfer);
        after_entry.size--; after_entry.ctime.dwLowDateTime++;
        app::TransferRenamedThumbnails(L"C:\\fixture", {before}, {after_entry}, {event}, transfer);
        after_entry.ctime = before.ctime; after_entry.is_dir = true;
        app::TransferRenamedThumbnails(L"C:\\fixture", {before}, {after_entry}, {event}, transfer);
        check(transfers == 1, "changed contents, replacement identity and folders are not transferred");
        after_entry.is_dir = false; after_entry.name = L"final.jpeg";
        auto second = event; second.old_name = event.name; second.name = after_entry.name;
        app::TransferRenamedThumbnails(L"C:\\fixture", {before}, {after_entry}, {event, second}, transfer);
        check(transfers == 2, "coalesced rapid rename chain transfers directly to final filename");
        return EditorSaveRegression(true) && ok;
    }
