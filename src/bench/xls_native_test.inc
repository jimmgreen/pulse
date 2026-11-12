// Focused XLS reader and actual preview-host checks. Included by preview_test_main.cpp.
static int RunXlsModelTest(wchar_t** argv) {
        const std::wstring path = std::wstring(argv[2]) + L"\\model.xls";
        auto stream = BuildXlsStream(true);
        // Duplicate the worksheet and insert a second BOUNDSHEET into globals.
        const auto u16 = [&](size_t at) { return unsigned(stream[at]) | (unsigned(stream[at + 1]) << 8); };
        size_t bound = 0, first = 0;
        for (size_t at = 0; at + 4 <= stream.size(); at += 4 + u16(at + 2)) {
            if (u16(at) == 0x85) { bound = at; break; }
        }
        std::memcpy(&first, stream.data() + bound + 4, 4);
        const size_t size = 4 + u16(bound + 2);
        std::vector<unsigned char> descriptor(stream.begin() + bound, stream.begin() + bound + size);
        descriptor.back() = '2';
        std::vector<unsigned char> second(stream.begin() + first, stream.end());
        const uint32_t second_at = static_cast<uint32_t>(stream.size() + size);
        const uint32_t first_at = static_cast<uint32_t>(first + size);
        std::memcpy(stream.data() + bound + 4, &first_at, 4);
        std::memcpy(descriptor.data() + 4, &second_at, 4);
        stream.insert(stream.begin() + bound + size, descriptor.begin(), descriptor.end());
        stream.insert(stream.end(), second.begin(), second.end());
        Check(WriteBytes(path, BuildCompoundFile(L"Workbook", stream)), L"write isolated two-sheet BIFF fixture");
        WriteBytes(std::wstring(argv[2]) + L"\\multi.xls", BuildCompoundFile(L"Workbook", stream));
        for (uint32_t i : {0u, 1u, 0u}) {
            std::wstring payload; uint32_t bytes = 0;
            Check(preview::MakeXlsTable(path, payload, bytes, i), L"decode selected XLS worksheet");
            Check(payload.find(L"S\txls\tS1") != std::wstring::npos &&
                  payload.find(L"S\txls\tS2") != std::wstring::npos &&
                  payload.find(L"W\t2\t1\t" + std::to_wstring(i)) != std::wstring::npos,
                  L"sheet metadata and selected index retained");
            Check(payload.find(L"Qty-long") != std::wstring::npos &&
                  payload.find(L"shared") != std::wstring::npos &&
                  payload.find(L"12.5") != std::wstring::npos,
                  L"SST continuation, cached formula and numeric cells retained");
            Check(payload.find(L"R\t\t\t\t\n") != std::wstring::npos,
                  L"leading blank row preserves Excel coordinates");
        }
        std::wstring payload; uint32_t bytes = 0;
        Check(!preview::MakeXlsTable(path, payload, bytes, 2), L"invalid sheet index rejected");
        auto limited = stream;
        for (size_t at = first_at; at + 4 <= stream.size(); at += 4 + u16(at + 2)) {
            if (u16(at) == 0xFD) {
                limited[at + 4] = 0x70; limited[at + 5] = 0x17; // row 6000
                break;
            }
        }
        Check(WriteBytes(path, BuildCompoundFile(L"Workbook", limited)) &&
              preview::MakeXlsTable(path, payload, bytes) && payload.find(L"\t1\t\t0\n") != std::wstring::npos,
              L"out-of-limit rows explicitly mark table truncated");
        auto empty = stream;
        // Replace the first worksheet BOF's following record with EOF.
        const size_t first_cell = first_at + 4 + u16(first_at + 2);
        empty[first_cell] = 0x0A; empty[first_cell + 1] = 0;
        empty[first_cell + 2] = empty[first_cell + 3] = 0;
        Check(WriteBytes(path, BuildCompoundFile(L"Workbook", empty)) &&
              preview::MakeXlsTable(path, payload, bytes) && payload.find(L"S\txls\tS1\t0\t0") != std::wstring::npos,
              L"empty worksheet returns valid empty table");
        auto damaged = stream;
        damaged.resize(first_at + 4 + u16(first_at + 2));
        Check(WriteBytes(path, BuildCompoundFile(L"Workbook", damaged)) &&
              !preview::MakeXlsTable(path, payload, bytes), L"missing worksheet EOF rejected");
        // FILEPASS before globals EOF; reject rather than present empty content.
        stream[bound] = 0x2F; stream[bound + 1] = 0;
        Check(WriteBytes(path, BuildCompoundFile(L"Workbook", stream)) &&
              !preview::MakeXlsTable(path, payload, bytes), L"encrypted workbook rejected");
        Check(WriteBytes(path, {0, 1, 2, 3}) && !preview::MakeXlsTable(path, payload, bytes),
              L"damaged workbook rejected");
        std::printf("xls-model passed=%d failed=%d\n", passed, failed);
        return failed ? 1 : 0;
    }

static int RunXlsNativeTest(int argc, wchar_t** argv) {
        Host host;
        if (!host.Start()) return 2;
        Result result;
        const bool received = host.Request(argv[2], result, MAXDWORD, 1024,
            ipc::PreviewRequestKind::Content, ipc::kPreviewRequestFlagRichText);
        const bool ok = received && result.response.kind == ipc::PreviewContentKind::Table &&
            result.text.rfind(L"PULSETBL\t1\n", 0) == 0 &&
            result.text.find(L"\nR\t") != std::wstring::npos;
        if (argc == 4 && received && !result.text.empty()) {
            const std::wstring output = argv[3];
            std::vector<unsigned char> data(result.text.size() * sizeof(wchar_t));
            std::memcpy(data.data(), result.text.data(), data.size());
            WriteBytes(output, data);
        }
        std::printf("xls-native received=%d kind=%u chars=%zu content=%d\n", received,
            static_cast<unsigned>(result.response.kind), result.text.size(), ok);
        return ok ? 0 : 1;
    }
