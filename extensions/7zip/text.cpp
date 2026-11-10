#include "text.h"
#include <windows.h>
#include <array>

namespace text {
namespace {
struct Row { Id id; const char* zh; const char* tw; const char* en; };
constexpr Row kRows[] = {
    {Id::CmdSmart, "智能解压", "智慧解壓縮", "Smart extract"},
    {Id::CmdFolder, "解压到同名文件夹", "解壓縮到同名資料夾", "Extract to folder"},
    {Id::CmdTo, "解压到…", "解壓縮到…", "Extract to…"},
    {Id::CmdTest, "测试压缩包", "測試壓縮檔", "Test archive"},
    {Id::CmdAdd, "添加到压缩包…", "加入壓縮檔…", "Add to archive…"},
    {Id::CmdAdd7z, "压缩为 7z", "壓縮為 7z", "Compress to 7z"},
    {Id::CmdAddZip, "压缩为 zip", "壓縮為 zip", "Compress to zip"},
    {Id::CmdHash, "计算校验值", "計算校驗值", "Compute checksums"},
    {Id::TaskExtract, "解压 {name}", "解壓縮 {name}", "Extracting {name}"},
    {Id::TaskTest, "测试 {name}", "測試 {name}", "Testing {name}"},
    {Id::TaskCompress, "压缩为 {name}", "壓縮為 {name}", "Compressing to {name}"},
    {Id::TaskHash, "计算校验值", "計算校驗值", "Computing checksums"},
    {Id::TaskType, "7-Zip", "7-Zip", "7-Zip"},
    {Id::PasswordTitle, "需要密码", "需要密碼", "Password required"},
    {Id::PasswordMessage, "“{name}”已加密，请输入密码。", "「{name}」已加密，請輸入密碼。", "\"{name}\" is encrypted. Enter its password."},
    {Id::PasswordRetry, "密码不正确，请重新输入“{name}”的密码。", "密碼不正確，請重新輸入「{name}」的密碼。", "Wrong password. Enter the password for \"{name}\" again."},
    {Id::PasswordLabel, "密码", "密碼", "Password"},
    {Id::Ok, "确定", "確定", "OK"},
    {Id::Cancel, "取消", "取消", "Cancel"},
    {Id::AddTitle, "添加到压缩包", "加入壓縮檔", "Add to archive"},
    {Id::AddName, "压缩包名称", "壓縮檔名稱", "Archive name"},
    {Id::AddFormat, "格式", "格式", "Format"},
    {Id::AddLevel, "压缩等级", "壓縮等級", "Compression level"},
    {Id::AddVolume, "分卷大小", "分割大小", "Split into volumes"},
    {Id::AddPassword, "密码（留空则不加密）", "密碼（留空則不加密）", "Password (leave empty for none)"},
    {Id::AddConfirm, "再次输入密码", "再次輸入密碼", "Confirm password"},
    {Id::AddEncryptNames, "同时加密文件名", "同時加密檔案名稱", "Encrypt file names too"},
    {Id::AddZipMethod, "ZIP 加密方式", "ZIP 加密方式", "ZIP encryption"},
    {Id::AddSolid, "固实压缩（体积更小，单个文件解压稍慢）", "固實壓縮（檔案更小，單獨解壓縮稍慢）", "Solid archive (smaller, slower to extract single files)"},
    {Id::AddStart, "压缩", "壓縮", "Compress"},
    {Id::AddMismatch, "两次输入的密码不一致。", "兩次輸入的密碼不一致。", "The passwords do not match."},
    {Id::AddBadName, "名称不能为空，也不能包含 \\ / : * ? \" < > |", "名稱不能為空，也不能包含 \\ / : * ? \" < > |", "The name cannot be empty or contain \\ / : * ? \" < > |"},
    {Id::AddEncryptSection, "加密", "加密", "Encryption"},
    {Id::Level0, "仅存储", "僅儲存", "Store"},
    {Id::Level1, "最快", "最快", "Fastest"},
    {Id::Level3, "快速", "快速", "Fast"},
    {Id::Level5, "标准", "標準", "Normal"},
    {Id::Level7, "最大", "最大", "Maximum"},
    {Id::Level9, "极限", "極限", "Ultra"},
    {Id::VolumeNone, "不分卷", "不分割", "No split"},
    {Id::ZipAes, "AES-256（推荐）", "AES-256（建議）", "AES-256 (recommended)"},
    {Id::ZipCrypto, "ZipCrypto（兼容旧软件）", "ZipCrypto（相容舊軟體）", "ZipCrypto (old software)"},
    {Id::HashTitle, "校验值", "校驗值", "Checksums"},
    {Id::HashCopy, "复制并关闭", "複製並關閉", "Copy and close"},
    {Id::Close, "关闭", "關閉", "Close"},
    {Id::ErrWrongPassword, "密码不正确。", "密碼不正確。", "Wrong password."},
    {Id::ErrNoPassword, "未输入密码，已取消。", "未輸入密碼，已取消。", "No password entered; cancelled."},
    {Id::ErrUnsupported, "不是 7-Zip 能打开的压缩包。", "不是 7-Zip 能開啟的壓縮檔。", "Not an archive 7-Zip can open."},
    {Id::ErrDamaged, "压缩包已损坏：", "壓縮檔已損毀：", "The archive is damaged: "},
    {Id::ErrIo, "读写文件失败：", "讀寫檔案失敗：", "File error: "},
    {Id::ErrStage, "无法在目标位置准备临时文件夹。", "無法在目標位置準備暫存資料夾。", "Cannot prepare a staging folder at the destination."},
    {Id::ErrCommit, "无法把结果交给 Pulse 发布。", "無法把結果交給 Pulse 發佈。", "Cannot hand the result to Pulse."},
    {Id::ErrLinks, "已跳过 {name} 个链接（不会解压符号链接或硬链接）。", "已略過 {name} 個連結（不會解壓縮符號連結或硬連結）。", "Skipped {name} link(s): symbolic and hard links are not extracted."},
    {Id::ErrSkipped, "已跳过 {name} 个链接或连接点（不会跟随）。", "已略過 {name} 個連結或連接點（不會跟隨）。", "Skipped {name} link(s) or junction(s); they are not followed."},
    {Id::WaitingHost, "等待确认并写入文件…", "等待確認並寫入檔案…", "Waiting for confirmation and writing files…"},
    {Id::TestOk, "测试通过，没有发现错误。", "測試通過，沒有發現錯誤。", "Test passed; no errors found."},
    {Id::SettingsTitle, "7-Zip", "7-Zip", "7-Zip"},
    {Id::SettingsFormat, "默认压缩格式", "預設壓縮格式", "Default format"},
    {Id::SettingsLevel, "默认压缩等级", "預設壓縮等級", "Default level"},
    {Id::SettingsDestination, "“解压到…”默认位置", "「解壓縮到…」預設位置", "Default location for \"Extract to…\""},
    {Id::SettingsDestinationHint, "留空则使用压缩包所在文件夹", "留空則使用壓縮檔所在資料夾", "Empty uses the archive's folder"},
    {Id::SettingsBrowse, "浏览…", "瀏覽…", "Browse…"},
    {Id::SettingsSave, "保存", "儲存", "Save"},
    {Id::SettingsReload, "重新载入", "重新載入", "Reload"},
    {Id::SettingsNote, "智能：压缩包里只有一个文件夹或文件时直接放出来，否则放进同名文件夹。写入经过 Pulse 的文件操作，可撤销。", "智慧：壓縮檔裡只有一個資料夾或檔案時直接放出來，否則放進同名資料夾。寫入經過 Pulse 的檔案作業，可復原。", "Smart: an archive with one folder or file is extracted as is; otherwise into a folder named after it. Writes go through Pulse file operations and can be undone."},
    {Id::SettingsLoading, "正在载入…", "正在載入…", "Loading…"},
    {Id::SettingsSaved, "已保存。", "已儲存。", "Saved."},
    {Id::SettingsFailed, "设置未能读取或保存，请重试。", "設定未能讀取或儲存，請重試。", "Settings could not be loaded or saved. Try again."},
    {Id::SettingsBadPath, "请填写本机磁盘上的完整路径，或留空。", "請填寫本機磁碟上的完整路徑，或留空。", "Use a full path on a local drive, or leave it empty."},
    {Id::SettingsEngine, "引擎：7-Zip 26.00 · 支持 {name} 种格式", "引擎：7-Zip 26.00 · 支援 {name} 種格式", "Engine: 7-Zip 26.00 · {name} formats"},
    {Id::SettingsMode, "右键“解压”时", "右鍵「解壓縮」時", "Extract does"},
    {Id::ModeSmart, "智能解压", "智慧解壓縮", "Smart extract"},
    {Id::ModeFolder, "总是放进同名文件夹", "一律放進同名資料夾", "Always into a folder named after it"},
    {Id::ModeHere, "直接解压到当前位置", "直接解壓縮到目前位置", "Straight into the location"},
    {Id::SettingsLocation, "解压到", "解壓縮到", "Extract into"},
    {Id::LocationBeside, "压缩包所在文件夹", "壓縮檔所在資料夾", "The archive's folder"},
    {Id::LocationFixed, "固定文件夹", "固定資料夾", "A fixed folder"},
    {Id::SettingsExtractSection, "解压", "解壓縮", "Extracting"},
    {Id::SettingsCompressSection, "压缩", "壓縮", "Compressing"},
    {Id::SettingsCompressNote, "右键“快速压缩”直接按这里的格式和等级生成压缩包，不再弹窗；需要密码或分卷时用“添加到压缩包…”。", "右鍵「快速壓縮」直接按這裡的格式和等級產生壓縮檔，不再彈窗；需要密碼或分割時用「加入壓縮檔…」。", "Quick compress uses this format and level with no dialog; use Add to archive… for passwords or volumes."},
    {Id::SettingsPasswordSection, "密码", "密碼", "Passwords"},
    {Id::SettingsRemember, "记住本次输入过的密码，解压时先自动尝试（只在内存中，退出 Pulse 即清除）", "記住本次輸入過的密碼，解壓縮時先自動嘗試（只在記憶體中，結束 Pulse 即清除）", "Remember passwords entered this session and try them first (memory only, cleared when Pulse exits)"},
    {Id::CmdHere, "解压到此处", "解壓縮到此處", "Extract here"},
    {Id::CmdFixed, "解压到固定文件夹", "解壓縮到固定資料夾", "Extract to the fixed folder"},
    {Id::CmdQuick, "快速压缩为 {name}", "快速壓縮為 {name}", "Quick compress to {name}"},
    {Id::SourceMore, "{name} 和另外 {count} 项", "{name} 和另外 {count} 項", "{name} and {count} more"},
    {Id::SourceSaveTo, "保存到 {name}", "儲存到 {name}", "Saves to {name}"},
    {Id::Fmt7z, "压缩率最高，可加密文件名。需要 7-Zip 或 Pulse 打开。", "壓縮率最高，可加密檔案名稱。需要 7-Zip 或 Pulse 開啟。", "Smallest archives; can hide file names. Opens in 7-Zip or Pulse."},
    {Id::FmtZip, "兼容性最好，Windows、macOS 和手机都能直接打开。", "相容性最好，Windows、macOS 和手機都能直接開啟。", "Most compatible: Windows, macOS and phones open it directly."},
    {Id::FmtTar, "只打包不压缩，保留 Linux 权限信息。", "只封裝不壓縮，保留 Linux 權限資訊。", "Bundles without compressing; keeps Linux permissions."},
    {Id::FmtTarGz, "Linux 和 macOS 上最常见，不支持加密。", "Linux 和 macOS 上最常見，不支援加密。", "The usual choice on Linux and macOS; no encryption."},
    {Id::FmtTarXz, "比 tar.gz 更小，压缩较慢，不支持加密。", "比 tar.gz 更小，壓縮較慢，不支援加密。", "Smaller than tar.gz, slower to make; no encryption."},
    {Id::FmtTarBz2, "较老的 Linux 发行格式，不支持加密。", "較舊的 Linux 發行格式，不支援加密。", "An older Linux format; no encryption."},
    {Id::LevelTar, "tar 不压缩", "tar 不壓縮", "tar does not compress"},
    {Id::ProtectTitle, "设置密码", "設定密碼", "Password protect"},
    {Id::ProtectOff, "不加密", "不加密", "Not encrypted"},
    {Id::Protect7z, "使用 AES-256 加密", "使用 AES-256 加密", "Encrypted with AES-256"},
    {Id::ProtectZip, "在下方选择加密方式", "在下方選擇加密方式", "Choose the encryption below"},
    {Id::AddMore, "更多选项", "更多選項", "More options"},
    {Id::VolumeDesc, "大文件拆成多个 .001、.002… 便于传输", "大檔案拆成多個 .001、.002… 方便傳輸", "Splits into .001, .002… for easier transfer"},
    {Id::SolidTitle, "固实压缩", "固實壓縮", "Solid archive"},
    {Id::SolidDesc, "体积更小，单独取出某个文件会稍慢", "檔案更小，單獨取出某個檔案會稍慢", "Smaller; taking out a single file is slower"},
    {Id::PasswordHint, "这个压缩包已加密，输入密码后继续解压。", "這個壓縮檔已加密，輸入密碼後繼續解壓縮。", "This archive is encrypted. Enter its password to continue."},
    {Id::PasswordWrong, "密码不正确，请重新输入。", "密碼不正確，請重新輸入。", "Wrong password. Try again."},
    {Id::PasswordMemory, "本次运行期间会记住，同一密码的其他压缩包自动解开。可在扩展设置里关闭。", "本次執行期間會記住，同一密碼的其他壓縮檔自動解開。可在擴充設定裡關閉。", "Remembered until Pulse exits, so other archives with it open by themselves. You can turn this off in the extension settings."},
    {Id::PasswordStart, "解压", "解壓縮", "Extract"},
    {Id::HashVerify, "核对", "核對", "Verify"},
    {Id::HashVerifyHint, "粘贴网站公布的值，自动比对", "貼上網站公布的值，自動比對", "Paste the published value to compare"},
    {Id::HashVerifyPlaceholder, "粘贴 CRC32、SHA-1 或 SHA-256", "貼上 CRC32、SHA-1 或 SHA-256", "Paste a CRC32, SHA-1 or SHA-256"},
    {Id::HashMatch, "✓ 与 {name} 一致", "✓ 與 {name} 一致", "✓ Matches {name}"},
    {Id::HashNoMatch, "✗ 与这些值都不一致", "✗ 與這些值都不一致", "✗ Matches none of these values"},
    {Id::HashCopyAll, "复制全部", "複製全部", "Copy all"},
    {Id::HashBytes, "{name} 字节", "{name} 位元組", "{name} bytes"},
    {Id::HeroTitle, "7-Zip 压缩与解压", "7-Zip 壓縮與解壓縮", "7-Zip compression"},
    {Id::HeroDetail, "Pulse 官方 · 7-Zip 26.00 引擎，支持 {name} 种格式", "Pulse 官方 · 7-Zip 26.00 引擎，支援 {name} 種格式", "Pulse official · 7-Zip 26.00 engine, {name} formats"},
    {Id::AutoSaved, "改动会自动保存", "變更會自動儲存", "Changes save automatically"},
    {Id::ModeTitle, "右键“解压”的方式", "右鍵「解壓縮」的方式", "What Extract does"},
    {Id::ModeDesc, "决定菜单里第一项叫什么、做什么", "決定選單第一項叫什麼、做什麼", "Names and sets the first menu command"},
    {Id::MenuShows, "右键菜单显示：{name}", "右鍵選單顯示：{name}", "Menu shows: {name}"},
    {Id::ModeSmartDesc, "只有一个文件夹或文件时直接放出来，否则放进同名文件夹", "只有一個資料夾或檔案時直接放出來，否則放進同名資料夾", "One folder or file comes out as is; otherwise into a folder named after the archive"},
    {Id::ModeFolderDesc, "菜单显示为“解压到同名文件夹”", "選單顯示為「解壓縮到同名資料夾」", "The menu reads \"Extract to folder\""},
    {Id::ModeHereDesc, "菜单显示为“解压到此处”", "選單顯示為「解壓縮到此處」", "The menu reads \"Extract here\""},
    {Id::LocationTitle, "解压到", "解壓縮到", "Extract into"},
    {Id::LocationFixedDesc, "所有压缩包都解压到同一个文件夹", "所有壓縮檔都解壓縮到同一個資料夾", "Every archive goes to the same folder"},
    {Id::QuickFormatTitle, "快速压缩格式", "快速壓縮格式", "Quick compress format"},
    {Id::QuickFormatDesc, "右键“快速压缩为 {name}”直接生成，不弹窗", "右鍵「快速壓縮為 {name}」直接產生，不彈窗", "\"Quick compress to {name}\" makes it with no dialog"},
    {Id::QuickLevelTitle, "快速压缩等级", "快速壓縮等級", "Quick compress level"},
    {Id::QuickLevelDesc, "等级越高越小，也越慢", "等級越高越小，也越慢", "Higher is smaller and slower"},
    {Id::RememberTitle, "记住本次输入的密码", "記住本次輸入的密碼", "Remember passwords this session"},
    {Id::RememberDesc, "解压时先自动尝试，只保存在内存中，退出 Pulse 即清除", "解壓縮時先自動嘗試，只保存在記憶體中，結束 Pulse 即清除", "Tried first when extracting; kept in memory only and cleared when Pulse exits"},
    {Id::RarNote, "RAR 只能解压：7-Zip 不提供 RAR 压缩。", "RAR 只能解壓縮：7-Zip 不提供 RAR 壓縮。", "RAR can be extracted only: 7-Zip cannot create RAR archives."},
};
int Language() {
    static const int language = [] {
        const LANGID id = GetUserDefaultUILanguage();
        if (PRIMARYLANGID(id) != LANG_CHINESE) return 2;
        const auto sub = SUBLANGID(id);
        return sub == SUBLANG_CHINESE_TRADITIONAL || sub == SUBLANG_CHINESE_HONGKONG || sub == SUBLANG_CHINESE_MACAU ? 1 : 0;
    }();
    return language;
}
} // namespace

const char* Get(Id id) {
    for (const auto& row : kRows)
        if (row.id == id) return Language() == 0 ? row.zh : Language() == 1 ? row.tw : row.en;
    return "";
}
std::string Format(Id id, const std::string& name, const std::string& count) {
    std::string value = Format(id, name);
    const auto at = value.find("{count}");
    if (at != std::string::npos) value.replace(at, 7, count);
    return value;
}
std::string Format(Id id, const std::string& name) {
    std::string value = Get(id);
    const auto at = value.find("{name}");
    if (at != std::string::npos) value.replace(at, 6, name);
    return value;
}
} // namespace text
