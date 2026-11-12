// Focused Office native-preview protocol probes; included after fixture helpers.
static int WriteOfficeNativeFixtures(const std::wstring& directory) {
    const auto path = [&](const wchar_t* name) { return directory + L"\\" + name; };
    bool ok = WriteBytes(path(L"sample.doc"), BuildWord97Doc(false)) &&
        WriteBytes(path(L"encrypted.doc"), BuildWord97Doc(true)) &&
        WriteBytes(path(L"sample.ppt"), BuildCompoundFile(L"PowerPoint Document", BuildPptStream(false))) &&
        WriteBytes(path(L"damaged.doc"), {1, 2, 3}) && WriteBytes(path(L"damaged.ppt"), {1, 2, 3});
    std::string rtf = "{\\rtf1\\ansi\\uc1 ";
    for (int i = 0; i < 100; ++i) rtf += "Paragraph-" + std::to_string(i) + " \\u20013?\\u25991?\\par ";
    rtf += "EndOfDocument}";
    const std::vector<unsigned char> bytes(rtf.begin(), rtf.end());
    ok = ok && WriteBytes(path(L"sample.rtf"), bytes) && WriteBytes(path(L"disguised.doc"), bytes);
    printf("office fixtures created=%d\n", ok);
    return ok ? 0 : 1;
}

static int RunOfficeNativeProbe(int argc, wchar_t** argv) {
    Host host;
    if (!host.Start()) return 2;
    Result result;
    const bool received = host.Request(argv[2], result, MAXDWORD, 1024,
        ipc::PreviewRequestKind::Content, ipc::kPreviewRequestFlagRichText);
    const bool ok = received && result.response.kind == ipc::PreviewContentKind::Markdown &&
        result.text.rfind(L"PULSEMD\t1\n", 0) == 0 && result.text.find(L"M\t") != std::wstring::npos;
    if (argc >= 4 && !result.text.empty()) {
        std::vector<unsigned char> bytes(result.text.size() * sizeof(wchar_t));
        std::memcpy(bytes.data(), result.text.data(), bytes.size());
        if (!WriteBytes(argv[3], bytes)) return 2;
    }
    printf("office-native received=%d kind=%u chars=%zu native=%d\n", received,
        static_cast<unsigned>(result.response.kind), result.text.size(), ok);
    return ok ? 0 : 1;
}
