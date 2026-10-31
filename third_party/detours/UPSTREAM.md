# Microsoft Detours

Pinned upstream release: v4.0.1 (release commit e4bfd6b).
Source: https://github.com/microsoft/Detours/releases/tag/v4.0.1
Archive: https://codeload.github.com/microsoft/Detours/zip/refs/tags/v4.0.1
License: MIT, see LICENSE.md. Source files are unmodified.
Archive SHA-256: 5ab84eb08fb9befeb16ffd04ca283731b1e0e1e53b1947ce7868d4b9654e43fc

Only src/ and upstream license/readme are vendored. Pulse builds the static library through CMake; upstream scripts are not run. This dependency enables explicit same-permission process launch and standard API interception; it does not authorize system-wide attachment or bypass process protections.

Build compatibility: this third-party target alone uses C++14 and legacy MSVC permissive mode, matching upstream cleanup-jump conventions. Pulse targets retain C++20, /permissive- and /W4.
