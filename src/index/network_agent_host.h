#pragma once

namespace pulse::index {

// Runs the per-user UNC indexing mode of Pulse.Index.exe. This must remain a
// separate process from the SYSTEM service so SMB uses the interactive user's
// credentials, but it shares the same executable and release artifact.
int RunNetworkAgent();

} // namespace pulse::index
