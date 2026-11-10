// text.h - the extension's UI strings in 简体 / 繁體 / English, chosen from the
// Windows display language (the SDK does not expose Pulse's own language).
#pragma once
#include <string>

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
    ErrWrongPassword, ErrNoPassword, ErrUnsupported, ErrDamaged, ErrIo, ErrStage, ErrCommit, ErrLinks, ErrSkipped,
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
};
const char* Get(Id id);                       // UTF-8
std::string Format(Id id, const std::string& name);  // replaces {name}
std::string Format(Id id, const std::string& name, const std::string& count);  // {name} and {count}
} // namespace text
