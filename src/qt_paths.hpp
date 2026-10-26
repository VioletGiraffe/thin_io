#pragma once

// QString paths for thin_io's calls. Header-only: the library itself builds without Qt.

#include "filesystem_types.hpp"

#include "compiler/compiler_warnings_control.h" // cpp-template-utils

DISABLE_COMPILER_WARNINGS
#include <QFile>
#include <QString>
RESTORE_COMPILER_WARNINGS

namespace thin_io {

// For a path-taking call: pass .data()
#ifdef _WIN32
[[nodiscard]] inline std::wstring native_path(const QString& path)
{
	return path.toStdWString();
}
#else
[[nodiscard]] inline QByteArray native_path(const QString& path)
{
	return QFile::encodeName(path);
}
#endif

// Lossy for a POSIX name that is not valid in the locale's encoding
[[nodiscard]] inline QString to_qstring(const native_string& name)
{
#ifdef _WIN32
	return QString::fromStdWString(name);
#else
	return QFile::decodeName(QByteArray::fromRawData(name.data(), static_cast<qsizetype>(name.size())));
#endif
}

} // namespace thin_io
