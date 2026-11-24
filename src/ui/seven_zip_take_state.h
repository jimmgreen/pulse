#pragma once
namespace pulse::ui {
enum class SevenZipTakeOwner { Off, Pulse, Other, Free, Unknown };
inline SevenZipTakeOwner DecodeSevenZipTakeOwner(char value) {
    switch (value) {
    case 'p': return SevenZipTakeOwner::Pulse;
    case 'o': return SevenZipTakeOwner::Other;
    case 'f': return SevenZipTakeOwner::Free;
    case 'x': return SevenZipTakeOwner::Off;
    default: return SevenZipTakeOwner::Unknown;
    }
}
inline bool SevenZipNeedsConfirmation(SevenZipTakeOwner owner) {
    return owner != SevenZipTakeOwner::Off && owner != SevenZipTakeOwner::Pulse;
}
}
