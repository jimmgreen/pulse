namespace {
int MenuCacheReuseTests() {
    using namespace pulse;
    auto pixels = [](uint8_t color, uint32_t width = 2, uint32_t height = 2) {
        auto p = std::make_shared<common::MenuIconPixels>();
        p->width = width; p->height = height;
        p->bgra.assign(static_cast<size_t>(width) * height * 4, color);
        return common::MenuIconRef(p);
    };
    app::ContextMenuPrefs prefs; prefs.persist = false;
    app::ContextMenuController controller;
    uint32_t session = 0;
    app::ContextMenuController::ShellOperations ops;
    ops.query = [&](auto, HWND, bool, bool, auto) { return ++session; };
    controller.SetShellOperations(std::move(ops));
    auto start = [&] {
        controller.ExecuteShellCommand(0, {}, {});
        controller.StartQuery(prefs, nullptr, {L"C:\\fixture\\sample.cachetest"},
            false, L".cachetest", false, {}, {});
    };
    start();
    auto row = Row(10, L"{CACHE}", L"open", L"Vendor action");
    row.icon = pixels(20); const auto original = row.icon;
    auto child = Row(11, L"{CACHE}", L"nested", L"Nested action", false, true);
    child.depth = 2; child.icon = pixels(40); const auto original_child = child.icon;
    controller.CompleteComQuery(session, {row, child}, 10, false);
    start();
    Check(session == 2 && controller.com_predicted(), "warm layout does not reuse stale host session");
    row.id = 90; row.enabled = false; row.icon = pixels(20);
    child.id = 91; child.icon = pixels(40);
    controller.CompleteComQuery(session, {row, child}, 20, false);
    Check(controller.com_items()[0].icon == original && controller.com_items()[1].icon == original_child,
          "new session reuses identical root and nested native pixel buffers");
    Check(controller.com_items()[0].id == 90 && !controller.com_items()[0].enabled &&
          controller.com_items()[1].id == 91, "icon reuse preserves current IDs and enabled state");
    start();
    Check(controller.com_items()[0].icon == original, "next predicted frame retains canonical pixels");
    row.id = 100; row.icon = pixels(21); const auto recolored = row.icon;
    controller.CompleteComQuery(session, {row}, 30, false);
    Check(controller.com_items()[0].icon == recolored && recolored != original,
          "changed icon color is not replaced by cached pixels");
    start(); row.icon = pixels(21, 1, 4); const auto resized = row.icon;
    controller.CompleteComQuery(session, {row}, 40, false);
    Check(controller.com_items()[0].icon == resized, "same byte count with different dimensions stays changed");
    start(); row.icon.reset();
    controller.CompleteComQuery(session, {row}, 50, false);
    Check(!controller.com_items()[0].icon, "removed provider icon is not restored from cache");
    start(); row.icon = pixels(55); const auto partial_icon = row.icon;
    controller.CompleteComQuery(session, {row}, 60, true);
    row.icon = pixels(55);
    controller.CompleteComQuery(session, {row}, 70, false);
    Check(controller.com_items()[0].icon == partial_icon, "partial to final reply reuses unchanged live pixels");

    app::StaticVerb verb; verb.verb = L"vendor"; verb.display = L"Vendor";
    verb.icon_pixels = pixels(70);
    app::StaticVerb nested; nested.verb = L"nested"; nested.display = L"Nested";
    nested.icon_pixels = pixels(80); verb.children.push_back(nested);
    const auto gen = controller.cache_generation();
    Check(controller.CompleteStaticVerbs(L".cachetest", {verb}, gen), "first static icon delivery refreshes display");
    verb.icon_pixels = pixels(70); verb.children[0].icon_pixels = pixels(80);
    Check(!controller.CompleteStaticVerbs(L".cachetest", {verb}, gen),
          "identical static root and child pixels do not trigger redundant refresh");
    verb.children[0].icon_pixels = pixels(81);
    Check(controller.CompleteStaticVerbs(L".cachetest", {verb}, gen), "changed nested static icon triggers refresh");
    verb.children[0].icon_pixels.reset();
    Check(controller.CompleteStaticVerbs(L".cachetest", {verb}, gen), "removed static icon triggers refresh");
    controller.InvalidateCaches();
    Check(!controller.CompleteStaticVerbs(L".cachetest", {verb}, gen), "stale generation cannot repopulate static pixels");
    const auto old_session = session;
    controller.CompleteComQuery(old_session, {row}, 80, false);
    Check(controller.ComLayoutSnapshot().empty(), "pre-invalidation live answer cannot repopulate new cache");
    controller.StartQuery(prefs, nullptr, {L"C:\\fixture\\sample.cachetest"},
        false, L".cachetest", false, {}, {});
    Check(session == old_session + 1 && !controller.com_predicted(),
          "registry invalidation renews same-path live session instead of reusing it");
    controller.Close();
    controller.StartQuery(prefs, nullptr, {L"C:\\fixture\\pending.cachetest"},
        false, L".cachetest", false, {}, {});
    controller.InvalidateCaches();
    const auto draining = session;
    controller.Close();
    controller.CompleteComQuery(draining, {row}, 90, false);
    Check(controller.ComLayoutSnapshot().empty(), "pre-invalidation draining reply cannot restore removed layout");
    start();
    controller.InvalidateCaches();
    controller.CompleteComQuery(session, {row}, 100, true, {Handler(L"{CACHE}", true, 1, 1)});
    Check(controller.ComLayoutSnapshot().empty(), "stale partial handler shard does not refill invalidated cache");
    controller.CompleteComQuery(session, {row}, 110, false, {Handler(L"{CACHE}", true, 1, 1)});
    Check(controller.ComLayoutSnapshot().empty(), "stale final handler shard does not refill invalidated cache");
    start();
    controller.CompleteComQuery(session, {row}, 120, false);
    Check(!controller.ComLayoutSnapshot().empty(), "current generation repopulates cache normally");
    std::printf("%s\n", failures ? "FAILED" : "ALL PASSED");
    return failures ? 1 : 0;
}
}
