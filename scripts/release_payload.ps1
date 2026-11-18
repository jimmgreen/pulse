# Shared production executables for installer payload, signing and symbol packaging.
$PulseReleaseExecutables = @('pulse.exe', 'pulse_launcher.exe', 'Pulse.AI.exe', 'Pulse.Content.exe', 'Pulse.Document.exe', 'Pulse.Index.exe',
    'Pulse.Preview.exe', 'pulse_shell.exe', 'pulse_integration.exe', 'pulse_elevated.exe', 'pulse_hotkey_broker.exe')
$PulseReleaseRuntimeDlls = @('lumatext.dll', 'pdfium.dll', 'pulse_start_pin.dll')
$PulseReleasePayload = $PulseReleaseExecutables + $PulseReleaseRuntimeDlls
# Content helpers ship with the app so same-version local builds use their
# matching binaries. Separate enhancement-pack release assets remain available.
$PulseContentPackExecutables = @('Pulse.Content.exe', 'Pulse.Document.exe')
# The AI enhancement pack (the MCP server AI apps connect to), released the same
# way: the pulse_ai_pack target writes <build>/ai-pack, key "ai".
$PulseAiPackExecutables = @('Pulse.AI.exe')
$PulsePackExecutables = $PulseContentPackExecutables + $PulseAiPackExecutables
$PulseSignedExecutables = @($PulsePackExecutables + $PulseReleaseExecutables | Select-Object -Unique)
