// text.h - the extension's UI strings in 简体 / 繁體 / English, following Pulse's
// effective host UI language when available and falling back to OS display language.
#pragma once
#include <string>
#include <string_view>

namespace text {
enum class Id {
    CmdSmart, CmdFolder, CmdTo, CmdTest, CmdAdd, CmdAdd7z, CmdAddZip, CmdHash,
    TaskExtract, TaskTest, TaskCompress, TaskHash, TaskType,
    PasswordTitle, PasswordMessage, PasswordRetry, PasswordLabel, Ok, Cancel,
    AddTitle, AddName, AddFormat, AddLevel, AddVolume, AddPassword, AddConfirm, AddEncryptNames, AddZipMethod,
    AddSolid, AddStart, AddMismatch, AddBadName, AddEncryptSection,
    Level0, Level1, Level3, Level5, Level7, Level9,
    VolumeNone, ZipAes, ZipCrypto,
    HashTitle, HashCopy, Close,
    ErrWrongPassword, ErrNoPassword, ErrUnsupported, ErrDamaged, ErrIo, ErrStage, ErrCommit, ErrLinks, ErrSkipped, ErrUnsafePath,
    WaitingHost, TestOk,
    SettingsTitle, SettingsFormat, SettingsLevel, SettingsDestination, SettingsDestinationHint, SettingsBrowse,
    SettingsSave, SettingsReload, SettingsNote, SettingsLoading, SettingsSaved, SettingsFailed, SettingsBadPath, SettingsEngine,
    SettingsMode, ModeSmart, ModeFolder, ModeHere, SettingsLocation, LocationBeside, LocationFixed,
    SettingsExtractSection, SettingsCompressSection, SettingsCompressNote, SettingsPasswordSection, SettingsRemember,
    CmdHere, CmdFixed, CmdQuick,
    SourceMore, SourceSaveTo, Fmt7z, FmtZip, FmtTar, FmtTarGz, FmtTarXz, FmtTarBz2, LevelTar,
    ProtectTitle, ProtectOff, Protect7z, ProtectZip, AddMore, VolumeDesc, SolidTitle, SolidDesc,
    PasswordHint, PasswordWrong, PasswordMemory, PasswordStart,
    HashVerify, HashVerifyHint, HashVerifyPlaceholder, HashMatch, HashNoMatch, HashCopyAll, HashBytes,
    HeroTitle, HeroDetail, AutoSaved, ModeTitle, ModeDesc, MenuShows, ModeSmartDesc, ModeFolderDesc, ModeHereDesc,
    LocationTitle, LocationFixedDesc, QuickFormatTitle, QuickFormatDesc, QuickLevelTitle, QuickLevelDesc,
    RememberTitle, RememberDesc, RarNote,
    MenuSection, PlacementTitle, PlacementDesc, PlacementTop, PlacementTopDesc, PlacementSub, PlacementSubDesc,
    CommandsTitle, CommandsDesc, CmdExtractSwitch, CmdQuickSwitch, HintSmart, HintFolder, HintHere, HintFixed, HintFixedHere, HintQuick, HintTar,
    DialogCaption, StepUnpack, StepWrite, NoteUnpack, NoteWrite, NoteCompress, SafeUnchanged, SafeNoArchive,
    DetailWrongPassword, DetailDamaged, ActionRetry, ActionPassword, RememberSwitch, RememberSwitchDesc,
    ResultExtracted, ResultCompressed, ResultItems, ResultTook,
    TestOkDetail, StatFiles, StatUnpacked, StatRatio, StatFormat, StatTime, StatCount, StatRatioValue, StatSeconds, Done,
    ErrWorkerCrashed, ErrWorkerTimedOut, ErrWorkerMemory, DetailWorkerIsolated,
    TakeFailed, TypeArchive, TypeBundle, TypeCab, VerbPreview, AppDescription,
    CmdBrowseArchive, CmdBrowseDetail, TabGeneral, TabFormats, FormatsRwTitle, FormatsRoTitle,
    SidePreviewTitle, SidePreviewDetail, VfsUnavailable,
    TakeSection, TakeTitle, TakeDesc, TakeBusy, TakeWinSettings, TakeConfirmEach, TakeAskSet, TakeAskChange, DblTitle, DblDesc, DblOpen, DblOpenDesc, TakeStatusExtract, TakeStatusOpen, TakeStatusOther, TakeStatusExcluded, TakeStatusExcludedWho, TakeStatusOff, TakeStatusOffWho, TakeStatusFree, TakeStatusUnknown, TakeOffHint, TakeSummaryAsk, TakeSummaryDone,
    Recommended, MenuDetailSmart, MenuDetailFolder, MenuDetailHere, MenuDetailFixed, MenuDetailQuick, MenuSettings, PreviewArchive, PreviewFile,
};
const char* Get(Id id);                       // UTF-8
enum class MenuLanguage { ZhCN, ZhTW, EnUS };
void SetHostLanguage(std::string_view language_tag) noexcept;
MenuLanguage CurrentLanguage() noexcept;
const char* Get(Id id, MenuLanguage language);
std::string Format(Id id, const std::string& name);  // replaces {name}
std::string Format(Id id, const std::string& name, const std::string& count);  // {name} and {count}
} // namespace text
