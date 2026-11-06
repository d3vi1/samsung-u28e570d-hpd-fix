#pragma once
#include "core.hpp"
#include <memory>

namespace monitorfw {
// Fresh run directory; per-monitor recovery markers live outside it in LocalAppData.
std::unique_ptr<IArtifactStore> makeWindowsArtifactStore(const std::wstring& runDirectory, bool mockScope = false);
Bytes readLocalFile(const std::wstring& path, std::uint32_t maximumBytes = 8 * 1024 * 1024);
// Offline bundle validation. An empty artifactDirectory uses the manifest's
// directory. All referenced files are reopened and validated, never trusted from
// manifest metadata alone. Ordinary local files/directories only; no reparse.
BackupEvidence loadLocalBackupEvidence(const std::wstring& manifestPath,
    const std::wstring& artifactDirectory = std::wstring());
std::string narrowUtf8(const std::wstring& text);
std::wstring widenUtf8(const std::string& text);
}
