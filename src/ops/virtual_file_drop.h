#pragma once

#include <shlobj.h>
#include <string>
#include <vector>

namespace pulse::ops {

// Called on the source's OLE apartment. Files is an already validated,
// ordered snapshot of flat filenames and advertised sizes. A true return
// means the copy was queued, not that Shell finished writing it.
bool QueueVirtualFileDrop(std::wstring destination, IDataObject* source,
                          std::vector<FILEDESCRIPTORW> files);

} // namespace pulse::ops
