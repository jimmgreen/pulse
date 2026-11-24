// service_text.cpp — Display-time localization of text produced by Pulse's
// helper processes (index service, network agent, pulse_shell, preview host).
//
// Those processes do not know the signed-in user's UI language, so they report
// Simplified Chinese as the canonical protocol text. The UI converts it here:
// zh-CN unchanged, zh-TW via the Traditional converter, English via the tables
// below. Text is translated per " · " segment; patterns keep their captures
// (counts, drive letters, paths, system messages) verbatim. Anything unknown is
// shown as reported. tools/l10n/check_service_text.py keeps the tables in sync
// with the helper sources.
#include "localization.h"
#include "display_path.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::l10n {
namespace {

struct ServicePhrase {
    const wchar_t* zh;
    const wchar_t* en;
};

// Whole segments.
constexpr ServicePhrase kServiceExact[] = {
    // index client (in-process view of the service connection)
    {L"索引未连接", L"Index not connected"},
    {L"请选择独立的索引文件夹，不能与原位置互相包含，也不能使用链接目录。选择磁盘根目录时会自动使用其中的 Index 文件夹。",
     L"Choose a separate index folder. It can't contain or be inside the current location, and it can't be a "
     L"linked folder. If you choose a drive root, its Index folder is used."},
    {L"正在等待索引服务…", L"Waiting for the index service…"},
    {L"找不到 Pulse.Index.exe", L"Pulse.Index.exe not found"},
    {L"索引进程版本过旧，请重启 Pulse 和索引服务以启用实时搜索",
     L"The index service is out of date. Restart Pulse and the index service to enable live search"},
    {L"索引连接已断开，正在重新连接…", L"Index connection lost, reconnecting…"},
    {L"已取消索引迁移，原位置保持不变。", L"Index move canceled. The original location is unchanged."},
    {L"另一项索引设置正在处理，请完成后重试。",
     L"Another index setting is being applied. Try again when it finishes."},
    {L"目标磁盘空间不足，索引未迁移。请释放空间后重试。",
     L"Not enough space on the target drive, so the index was not moved. Free up space and try again."},
    {L"目标位置已有另一份索引。请选择空目录，避免覆盖已有数据。",
     L"The target already contains another index. Choose an empty folder so existing data is not overwritten."},
    {L"请选择独立的索引文件夹，不能与原位置互相包含，也不能使用链接目录。选择磁盘根目录时会自动使用其中的 Index 文件夹。",
     L"Choose a separate index folder that does not contain, and is not inside, the current location, and is not a linked folder. "
     L"Choosing a drive root uses the Index folder on it."},
    {L"索引已切换到新位置，但部分旧文件被占用，未能清理。新索引可以正常使用。",
     L"The index moved to the new location, but some old files are in use and could not be removed. The new index works normally."},
    {L"索引服务未能恢复。原索引文件已保留，请重新启动索引服务后重试。",
     L"The index service could not be restored. The original index files were kept. Restart the index service and try again."},
    {L"索引迁移未完成，原索引已保留。请检查目标目录的权限和磁盘连接后重试。",
     L"The index move did not finish and the original index was kept. Check the target folder's permissions and the drive connection, then try again."},

    // index configuration
    {L"无法定位 ProgramData 索引目录", L"Could not locate the ProgramData index folder"},
    {L"无法读取索引配置", L"Could not read the index configuration"},
    {L"索引配置为空或损坏", L"The index configuration is empty or damaged"},
    {L"保存索引配置失败", L"Could not save the index configuration"},
    {L"无效的卷标识", L"Invalid volume ID"},
    {L"索引路径不能为空", L"The index path cannot be empty"},
    {L"无法创建索引目录", L"Could not create the index folder"},
    {L"排除项必须是本地文件夹路径", L"Exclusions must be local folder paths"},
    {L"离线", L"Offline"},
    {L"非 NTFS", L"Not NTFS"},
    {L"不支持", L"Unsupported"},
    {L"等待索引", L"Waiting to index"},
    {L"已排除", L"Excluded"},

    // index engine
    {L"索引配置已更改，准备重建…", L"Index settings changed, preparing to rebuild…"},
    {L"已就绪", L"Ready"},
    {L"正在重新核对", L"Reconciling"},
    {L"仍可搜索", L"Still searchable"},
    {L"正在建立索引", L"Building index"},
    {L"正在建立索引…", L"Building index…"},
    {L"索引失败", L"Indexing failed"},
    {L"无法扫描该磁盘的目录内容", L"Could not scan the folders on this drive"},
    {L"无法读取该磁盘的文件变更记录", L"Could not read the change journal of this drive"},
    {L"索引更新暂时无法保存，正在等待重试；请检查磁盘空间和权限",
     L"Index updates cannot be saved right now and will be retried. Check disk space and permissions"},
    {L"正在完成", L"Finishing"},
    {L"实时更新", L"Live updates"},
    {L"正在监视", L"Monitoring"},
    {L"索引可用，但保存失败，请检查索引目录权限及磁盘空间",
     L"The index is usable but could not be saved. Check the index folder's permissions and disk space"},
    {L"索引正在后台建立…", L"Building the index in the background…"},
    {L"部分磁盘未实时更新", L"Some drives are not updated live"},

    // index migration and shards
    {L"索引迁移失败：请使用不含旧索引的独立目录，检查路径、权限及剩余空间。原索引已保留。",
     L"The index move failed. Use a separate folder without an old index and check the path, permissions and free space. The original index was kept."},
    {L"旧索引清理记录无效，文件已保留。", L"The old index cleanup record is invalid. The files were kept."},
    {L"新索引已迁移，但部分旧索引文件无法清理。",
     L"The index was moved, but some old index files could not be removed."},
    {L"新索引已迁移，但部分旧索引文件或空目录无法清理。",
     L"The index was moved, but some old index files or empty folders could not be removed."},
    {L"无法读取分片清单", L"Could not read the shard manifest"},
    {L"分片清单损坏", L"The shard manifest is damaged"},
    {L"分片清单参数无效", L"Invalid shard manifest parameters"},
    {L"无法创建分片清单临时文件", L"Could not create a temporary shard manifest"},
    {L"无法发布分片清单", L"Could not publish the shard manifest"},
    {L"没有可用的 V9 分片", L"No V9 shards are available"},
    {L"V9 base 校验失败", L"V9 base verification failed"},
    {L"无法发布 V9 base", L"Could not publish the V9 base"},

    // network index and agent
    {L"网络索引未配置", L"Network index not configured"},
    {L"网络索引代理运行中", L"Network index agent running"},
    {L"无法定位当前用户配置目录", L"Could not locate the current user's settings folder"},
    {L"网络索引配置不是有效的 UTF-8 文件", L"The network index configuration is not a valid UTF-8 file"},
    {L"网络索引配置已损坏", L"The network index configuration is damaged"},
    {L"WSL 中的文件不能加入网络索引", L"Files in WSL can't be added to the network index"},
    {L"无法将网络索引配置编码为 UTF-8", L"Could not encode the network index configuration as UTF-8"},
    {L"等待服务器校验", L"Waiting for server verification"},
    {L"等待扫描", L"Waiting to scan"},
    {L"请选择服务器共享中的文件夹（UNC 路径）", L"Choose a folder on a server share (UNC path)"},
    {L"等待重新扫描", L"Waiting to rescan"},
    {L"正在扫描服务器文件夹", L"Scanning server folder"},
    {L"正在扫描", L"Scanning"},
    {L"实时监视", L"Watching live"},
    {L"已同步", L"Synced"},
    {L"同步失败", L"Sync failed"},
    {L"继续使用旧索引", L"Using the previous index"},
    {L"服务器离线", L"Server offline"},
    {L"已保留索引", L"Index kept"},
    {L"无法访问", L"Not accessible"},

    // pulse_shell (shell host) and its client
    {L"没有权限在此位置新建", L"You don't have permission to create items here"},
    {L"目标文件夹不存在", L"The target folder doesn't exist"},
    {L"已存在同名项目", L"An item with the same name already exists"},
    {L"名称无效", L"Invalid name"},
    {L"新建失败", L"Could not create the item"},
    {L"无法确定回收站卷", L"Could not determine the Recycle Bin drive"},
    {L"无法确定当前用户的回收站", L"Could not determine the current user's Recycle Bin"},
    {L"无法打开当前用户的回收站", L"Could not open the current user's Recycle Bin"},
    {L"还原目标已存在", L"The restore target already exists"},
    {L"还原失败", L"Restore failed"},
    {L"回收站中未找到该项", L"The item was not found in the Recycle Bin"},
    {L"部分项目未能还原", L"Some items could not be restored"},
    {L"找不到 pulse_shell.exe（需与 Pulse 放在同一目录）",
     L"pulse_shell.exe not found (it must be in the same folder as Pulse)"},
    {L"pulse_shell.exe 管道忙碌", L"pulse_shell.exe pipe is busy"},
    {L"pulse_shell.exe 启动后立即退出", L"pulse_shell.exe exited right after starting"},
    {L"pulse_shell.exe 未响应管道", L"pulse_shell.exe did not respond on its pipe"},

    // preview host property labels
    {L"尺寸", L"Dimensions"},
    {L"拍摄时间", L"Date taken"},
    {L"相机", L"Camera"},
    {L"时长", L"Duration"},
    {L"分辨率", L"Resolution"},
    {L"帧率", L"Frame rate"},
    {L"编码格式", L"Codec"},
    {L"标题", L"Title"},
    {L"艺术家", L"Artist"},
    {L"专辑", L"Album"},
    {L"比特率", L"Bit rate"},
    {L"采样率", L"Sample rate"},
    {L"作者", L"Author"},
    {L"页数", L"Pages"},
    {L"镜头", L"Lens"},
    {L"参数", L"Exposure"},
    {L"焦距", L"Focal length"},
    {L"位深", L"Bit depth"},
    {L"声道", L"Channels"},
    {L"流派", L"Genre"},
    {L"年份", L"Year"},

    // index service, network agent and Recycle Bin verbs
    {L"索引位置里有不属于 Pulse 的文件或链接，为保护隐私，索引服务不会使用它。请改选一个空文件夹。",
     L"The index folder contains files or links that aren't Pulse's. To protect your privacy the index "
     L"service won't use them. Choose an empty folder."},
    {L"索引配置读取失败，禁止覆盖",
     L"Could not read the index configuration; overwriting it is blocked"},
    {L"索引目录权限不安全，请使用空的独立目录。原索引配置已保留。",
     L"The index folder's permissions are unsafe. Use an empty standalone folder. The previous index "
     L"configuration was kept."},
    {L"未知的系统文件夹分组", L"Unknown system folder group"},
    {L"索引增量记录不兼容或已损坏，正在重建…",
     L"The incremental index log is incompatible or damaged; rebuilding…"},
    {L"索引扫描未完成，保留上次结果，请稍后重试",
     L"The index scan didn't finish. The previous results were kept; try again later"},
    {L"索引更新尚未保存，需要重新扫描；请检查索引目录权限及磁盘空间",
     L"Index updates weren't saved and a rescan is needed. Check the index folder's permissions and disk space"},
    {L"网络索引配置包含无效的服务器文件夹",
     L"The network index configuration contains an invalid server folder"},
    {L"子目录扫描未完成，等待重新扫描",
     L"Subfolder scan didn't finish; waiting to rescan"},
    {L"无法读取网络索引配置", L"Could not read the network index configuration"},
    {L"监视已启动", L"Watching started"},
    {L"等待校验", L"Waiting for verification"},
    {L"无法读取所选回收站条目", L"Could not read the selected Recycle Bin item"},
    {L"还原失败（目标可能已存在）",
     L"Restore failed (the destination may already exist)"},
    {L"无法唯一确定回收站版本，请在回收站中选择具体条目",
     L"Couldn't identify the Recycle Bin version; select a specific item in the Recycle Bin"},

    // pulse.exe --mcp-stdio (ai_launcher.cpp), written to the AI client's log
    {L"未安装 AI 增强包。请在 Pulse 的“设置 › 增强包”中安装后重试。",
     L"The AI pack is not installed. Install it in Pulse under Settings › Enhancement packs, then try again."},
    {L"AI 增强包需要更新。请在 Pulse 的“设置 › 增强包”中更新后重试。",
     L"The AI pack needs an update. Update it in Pulse under Settings › Enhancement packs, then try again."},
    {L"AI 增强包已损坏。请在 Pulse 的“设置 › 增强包”中修复后重试。",
     L"The AI pack is damaged. Repair it in Pulse under Settings › Enhancement packs, then try again."},
    {L"这个版本的 Pulse 没有可用的 AI 增强包。", L"No AI pack is available for this version of Pulse."},
    {L"无法启动 Pulse.AI.exe", L"Could not start Pulse.AI.exe"},
};

// Segments with captures: {n} = digits/commas only, {} = any non-empty text.
// More specific patterns come first.
constexpr ServicePhrase kServicePatterns[] = {
    {L"已索引 {n} 项，正在重建…", L"{n} items indexed, rebuilding…"},
    {L"已索引 {n} 项", L"{n} items indexed"},
    {L"已加载 {n} 项", L"{n} items loaded"},
    {L"正在索引 {} 项", L"Indexing {} items"},
    {L"{}: 的变更跟踪失效，正在重建索引…", L"Change tracking for {}: was lost, rebuilding the index…"},
    {L"正在扫描 {}…", L"Scanning {}…"},
    {L"正在重新核对 {} 的索引…", L"Reconciling the index for {}…"},
    {L"{} 的索引已核对完成", L"Index for {} reconciled"},
    {L"正在重建 {} 的索引…", L"Rebuilding the index for {}…"},
    {L"{} 的索引已重建完成", L"Index for {} rebuilt"},
    {L"{} 的变更跟踪暂不可用，稍后重试", L"Change tracking for {} is unavailable, retrying later"},
    {L"保存索引配置失败（{n}）：{}", L"Could not save the index configuration ({n}): {}"},
    {L"保存索引配置失败（{n}）", L"Could not save the index configuration ({n})"},
    {L"无法创建索引目录（{n}）：{}", L"Could not create the index folder ({n}): {}"},
    {L"无法创建索引目录（{n}）", L"Could not create the index folder ({n})"},
    {L"无法启动 pulse_shell.exe（错误 {n}）", L"Could not start pulse_shell.exe (error {n})"},
    {L"错误 {n}：{}", L"Error {n}: {}"},
    {L"错误 {n}", L"Error {n}"},
    {L"{n} 项", L"{n} items"},
};

constexpr std::wstring_view kSeparator = L" \u00B7 ";

bool HasHan(std::wstring_view text) {
    for (const wchar_t c : text)
        if ((c >= 0x3400 && c <= 0x9FFF) || (c >= 0xF900 && c <= 0xFAFF)) return true;
    return false;
}

bool IsCount(std::wstring_view text) {
    if (text.empty()) return false;
    for (const wchar_t c : text)
        if (!((c >= L'0' && c <= L'9') || c == L',')) return false;
    return true;
}

// Splits "a{n}b{}c" into literals {a, b, c} and placeholder kinds {count, any}.
void SplitPattern(std::wstring_view pattern, std::vector<std::wstring_view>& literals,
                  std::vector<bool>& counts) {
    literals.clear();
    counts.clear();
    size_t start = 0;
    for (;;) {
        const size_t open = pattern.find(L'{', start);
        if (open == std::wstring_view::npos) break;
        const size_t close = pattern.find(L'}', open);
        if (close == std::wstring_view::npos) break;
        literals.push_back(pattern.substr(start, open - start));
        counts.push_back(pattern.substr(open, close - open + 1) == L"{n}");
        start = close + 1;
    }
    literals.push_back(pattern.substr(start));
}

bool MatchPattern(std::wstring_view text, std::wstring_view pattern,
                  std::vector<std::wstring_view>& captures) {
    std::vector<std::wstring_view> literals;
    std::vector<bool> counts;
    SplitPattern(pattern, literals, counts);
    captures.clear();
    if (counts.empty()) return false;
    if (!text.starts_with(literals.front())) return false;
    const std::wstring_view tail = literals.back();
    if (text.size() < literals.front().size() + tail.size() || !text.ends_with(tail)) return false;
    size_t pos = literals.front().size();
    const size_t end = text.size() - tail.size();
    for (size_t i = 0; i < counts.size(); ++i) {
        size_t capture_end = end;
        if (i + 1 < counts.size()) {
            const std::wstring_view next = literals[i + 1];
            capture_end = text.find(next, pos + 1);
            if (capture_end == std::wstring_view::npos || capture_end > end) return false;
        }
        if (capture_end <= pos) return false;
        const std::wstring_view capture = text.substr(pos, capture_end - pos);
        if (counts[i] && !IsCount(capture)) return false;
        captures.push_back(capture);
        pos = capture_end + (i + 1 < counts.size() ? literals[i + 1].size() : 0);
    }
    return true;
}

std::wstring Fill(std::wstring_view localized, const std::vector<std::wstring_view>& captures) {
    std::wstring out;
    size_t start = 0, index = 0;
    for (;;) {
        const size_t open = localized.find(L'{', start);
        const size_t close = open == std::wstring_view::npos ? open : localized.find(L'}', open);
        if (open == std::wstring_view::npos || close == std::wstring_view::npos) break;
        out.append(localized.substr(start, open - start));
        if (index < captures.size()) out.append(captures[index++]);
        start = close + 1;
    }
    out.append(localized.substr(start));
    return out;
}

// Localized form of one table entry: English text, or the Traditional form of
// the Chinese template (placeholders are ASCII and survive the conversion).
std::wstring Target(const ServicePhrase& phrase, bool traditional) {
    return traditional ? HantText(phrase.zh) : std::wstring(phrase.en);
}

std::optional<std::wstring> TranslateKnown(std::wstring_view segment, bool traditional) {
    for (const auto& phrase : kServiceExact)
        if (segment == phrase.zh) return Target(phrase, traditional);
    std::vector<std::wstring_view> captures;
    for (const auto& phrase : kServicePatterns)
        if (MatchPattern(segment, phrase.zh, captures)) return Fill(Target(phrase, traditional), captures);
    return std::nullopt;
}

std::wstring TranslateSegment(std::wstring_view segment, bool traditional) {
    if (!HasHan(segment)) return std::wstring(segment);
    if (auto known = TranslateKnown(segment, traditional)) return *known;
    // Unknown text: Traditional conversion keeps working as before; English keeps
    // the reported text rather than guessing.
    return traditional ? HantText(segment) : std::wstring(segment);
}

} // namespace

std::wstring ServiceText(std::wstring_view text) {
    const Language language = effective_language();
    if (language == Language::ZhCN || !HasHan(text)) return std::wstring(text);
    const bool traditional = language == Language::ZhTW;
    if (auto whole = TranslateKnown(text, traditional)) return *whole;
    std::wstring out;
    size_t start = 0;
    for (;;) {
        const size_t separator = text.find(kSeparator, start);
        out += TranslateSegment(text.substr(start, separator == std::wstring_view::npos
                                                       ? std::wstring_view::npos
                                                       : separator - start),
                                traditional);
        if (separator == std::wstring_view::npos) break;
        out += kSeparator;
        start = separator + kSeparator.size();
    }
    return out;
}

bool IsKnownServiceText(std::wstring_view text) {
    if (!HasHan(text)) return true;
    if (TranslateKnown(text, false)) return true;
    size_t start = 0;
    for (;;) {
        const size_t separator = text.find(kSeparator, start);
        const std::wstring_view segment = text.substr(
            start, separator == std::wstring_view::npos ? std::wstring_view::npos : separator - start);
        if (HasHan(segment) && !TranslateKnown(segment, false)) return false;
        if (separator == std::wstring_view::npos) return true;
        start = separator + kSeparator.size();
    }
}

std::wstring ServiceErrorText(std::wstring_view text) {
    // "message | item path": only a known message is localized. Present any
    // embedded file paths as DOS/UNC without altering the stored error.
    const size_t bar = text.find(L" | ");
    const std::wstring_view message = text.substr(0, bar);
    std::wstring out = IsKnownServiceText(message) ? ServiceText(message) : std::wstring(message);
    if (bar != std::wstring_view::npos) out += text.substr(bar);
    return path::FriendlyPathText(out);
}

} // namespace pulse::l10n
