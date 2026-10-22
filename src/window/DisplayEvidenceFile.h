#pragma once
#include <windows.h>
#include <string>

namespace display_diagnostics {
// Return the Win32 error explicitly; CloseHandle must not obscure it.
inline DWORD AppendEvidenceRecord(const wchar_t* path, const std::string& line) {
    const std::string record = line + "\r\n";
    HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return GetLastError();
    const LARGE_INTEGER end{};
    DWORD written = 0;
    DWORD error = ERROR_SUCCESS;
    if (!SetFilePointerEx(file, end, nullptr, FILE_END) ||
        !WriteFile(file, record.data(), static_cast<DWORD>(record.size()), &written, nullptr))
        error = GetLastError();
    else if (written != record.size()) error = ERROR_WRITE_FAULT;
    else if (!FlushFileBuffers(file)) error = GetLastError();
    CloseHandle(file);
    return error;
}
}
