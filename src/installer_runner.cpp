#include "installer_runner.h"

#include "archive_installer.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QDateTime>
#include <QThread>

#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace freetp {

namespace {

constexpr int kInnoInstallTimeoutMs = 3600000;
constexpr int kAddonInstallTimeoutMs = 900000;
constexpr int kInnoLogSuccessGraceMs = 15000;

QStringList desktopRoots()
{
    QStringList roots;
    const QString userDesktop =
        QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    if (!userDesktop.isEmpty())
        roots.append(userDesktop);

#if defined(Q_OS_WIN)
    const QByteArray publicProfile = qgetenv("PUBLIC");
    if (!publicProfile.isEmpty())
        roots.append(QString::fromLocal8Bit(publicProfile) + QStringLiteral("/Desktop"));
#endif

    return roots;
}

QByteArray utf16LeBytes(const QString& text)
{
    QByteArray bytes;
    bytes.reserve(text.size() * 2);
    for (const QChar ch : text) {
        const ushort code = ch.unicode();
        bytes.append(char(code & 0xFF));
        bytes.append(char((code >> 8) & 0xFF));
    }
    return bytes;
}

bool shortcutDataContains(const QByteArray& data, const QString& needle)
{
    if (needle.isEmpty())
        return false;
    if (data.contains(needle.toUtf8()))
        return true;
    if (data.contains(utf16LeBytes(needle)))
        return true;
    const QString native = QDir::toNativeSeparators(needle);
    return data.contains(utf16LeBytes(native)) || data.contains(native.toUtf8());
}

// FreeTP installers drop a promo desktop shortcut ("Игры По Сети" → FreeTP.Org.url).
// Keep the real game shortcut; only remove that promo.
bool isFreetpPromoShortcut(const QString& shortcutPath)
{
    const QString baseName = QFileInfo(shortcutPath).completeBaseName().toLower();
    if (baseName.contains(QStringLiteral("игры по сети"))
        || baseName.contains(QStringLiteral("freetp.org"))
        || baseName == QStringLiteral("freetp")) {
        return true;
    }

    QFile file(shortcutPath);
    if (!file.open(QIODevice::ReadOnly))
        return false;

    const QByteArray data = file.readAll();
    static const QStringList kPromoNeedles = {
        QStringLiteral("FreeTP.Org.url"),
        QStringLiteral("FreeTP.Org"),
        QStringLiteral("freetp.org"),
        QStringLiteral("freetp.org.url"),
    };
    for (const QString& needle : kPromoNeedles) {
        if (shortcutDataContains(data, needle))
            return true;
    }
    return false;
}

QString innoPathArg(const QString& flag, const QString& path)
{
    return flag + QDir::toNativeSeparators(path);
}

QString tailOfInstallLog(const QString& logPath, int maxLines = 8)
{
    QFile file(logPath);
    if (!file.open(QIODevice::ReadOnly))
        return {};

    const QString text = QString::fromLocal8Bit(file.readAll());
    const QStringList lines = text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    if (lines.isEmpty())
        return {};

    const int start = qMax(0, lines.size() - maxLines);
    return lines.mid(start).join(QLatin1Char('\n'));
}

bool innoLogIndicatesFinished(const QString& logPath)
{
    QFile file(logPath);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QByteArray text = file.readAll();
    return text.contains("Log closed.") || text.contains("Deinitializing Setup.")
           || text.contains("Installation process succeeded.");
}

void clearTargetDirectory(const QString& targetPath, QString* errorOut)
{
    QDir targetDir(targetPath);
    if (!targetDir.exists())
        return;

    if (targetDir.removeRecursively())
        return;

    const QString backupPath =
        targetPath + QStringLiteral(".old-")
        + QString::number(QDateTime::currentMSecsSinceEpoch());
    if (QDir().rename(targetPath, backupPath))
        return;

    if (errorOut)
        *errorOut = QStringLiteral("Не удалось очистить папку установки");
}

QString waitForGameExecutable(const QString& targetPath)
{
    for (int attempt = 0; attempt < 20; ++attempt) {
        const QString exe = findGameExecutable(targetPath);
        if (!exe.isEmpty())
            return exe;
        QThread::msleep(500);
    }
    return {};
}

QString longestCommonDirPrefix(const QStringList& paths)
{
    if (paths.isEmpty())
        return {};

    QStringList parts = QDir::fromNativeSeparators(paths.constFirst()).split(QLatin1Char('/'));
    for (int i = 1; i < paths.size(); ++i) {
        const QStringList other =
            QDir::fromNativeSeparators(paths.at(i)).split(QLatin1Char('/'));
        int common = 0;
        while (common < parts.size() && common < other.size()
               && parts.at(common).compare(other.at(common), Qt::CaseInsensitive) == 0)
            ++common;
        parts = parts.mid(0, common);
        if (parts.isEmpty())
            return {};
    }
    if (parts.size() <= 1)
        return {};
    return parts.join(QLatin1Char('/'));
}

QString actualInstallRootFromLog(const QString& logPath, const QString& expectedTarget)
{
    QFile file(logPath);
    if (!file.open(QIODevice::ReadOnly))
        return {};

    const QString text = QString::fromLocal8Bit(file.readAll());
    static const QRegularExpression destRe(
        QStringLiteral(R"(Dest filename:\s*(.+)$)"),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::MultilineOption);

    QStringList destFiles;
    auto it = destRe.globalMatch(text);
    while (it.hasNext()) {
        const QString path = QDir::fromNativeSeparators(it.next().captured(1).trimmed());
        if (path.isEmpty())
            continue;
        const QString lower = path.toLower();
        if (lower.endsWith(QStringLiteral(".lnk")) || lower.endsWith(QStringLiteral(".url"))
            || lower.contains(QStringLiteral("/desktop/")))
            continue;
        destFiles.append(path);
    }
    if (destFiles.isEmpty())
        return {};

    QStringList dirs;
    dirs.reserve(destFiles.size());
    for (const QString& filePath : destFiles)
        dirs.append(QFileInfo(filePath).absolutePath());

    const QString common = longestCommonDirPrefix(dirs);
    if (common.isEmpty())
        return {};

    const QString cleanExpected = QDir::cleanPath(expectedTarget);
    const QString cleanActual = QDir::cleanPath(common);
    if (cleanActual.compare(cleanExpected, Qt::CaseInsensitive) == 0)
        return {};
    if (cleanActual.startsWith(cleanExpected + QLatin1Char('/'), Qt::CaseInsensitive))
        return {};

    // Prefer the directory that actually contains a game executable.
    QString probe = cleanActual;
    for (int depth = 0; depth < 4 && !probe.isEmpty(); ++depth) {
        if (!findGameExecutable(probe).isEmpty())
            return probe;
        const QFileInfo info(probe);
        if (info.isRoot())
            break;
        probe = info.absolutePath();
    }
    return cleanActual;
}

QStringList steamCommonRoots()
{
    QStringList roots;
    auto addRoot = [&roots](const QString& steamRoot) {
        if (steamRoot.trimmed().isEmpty())
            return;
        const QString common =
            QDir(steamRoot).absoluteFilePath(QStringLiteral("steamapps/common"));
        if (QDir(common).exists() && !roots.contains(common, Qt::CaseInsensitive))
            roots.append(common);
    };

#if defined(Q_OS_WIN)
    const QSettings steamUser(QStringLiteral(R"(HKEY_CURRENT_USER\Software\Valve\Steam)"),
                              QSettings::NativeFormat);
    addRoot(steamUser.value(QStringLiteral("SteamPath")).toString());

    const QSettings steamMachine(
        QStringLiteral(R"(HKEY_LOCAL_MACHINE\SOFTWARE\WOW6432Node\Valve\Steam)"),
        QSettings::NativeFormat);
    addRoot(steamMachine.value(QStringLiteral("InstallPath")).toString());
#endif

    static const QStringList kFallbacks = {
        QStringLiteral("C:/Program Files (x86)/Steam"),
        QStringLiteral("C:/Program Files/Steam"),
        QStringLiteral("C:/Steam"),
        QStringLiteral("D:/Steam"),
        QStringLiteral("D:/Media/Steam"),
        QStringLiteral("E:/Steam"),
        QStringLiteral("X:/Steam"),
    };
    for (const QString& fallback : kFallbacks)
        addRoot(fallback);

    // libraryfolders.vdf — extra Steam libraries on other drives.
    for (const QString& common : QStringList(roots)) {
        const QString steamRoot = QFileInfo(QFileInfo(common).absolutePath()).absolutePath();
        const QString vdfPath =
            QDir(steamRoot).absoluteFilePath(QStringLiteral("steamapps/libraryfolders.vdf"));
        QFile vdf(vdfPath);
        if (!vdf.open(QIODevice::ReadOnly))
            continue;
        const QString text = QString::fromUtf8(vdf.readAll());
        static const QRegularExpression pathRe(
            QStringLiteral("\"path\"\\s+\"([^\"]+)\""),
            QRegularExpression::CaseInsensitiveOption);
        auto match = pathRe.globalMatch(text);
        while (match.hasNext()) {
            QString libPath = match.next().captured(1);
            libPath.replace(QLatin1Char('\\'), QLatin1Char('/'));
            addRoot(libPath);
        }
    }

    return roots;
}

QString findMisplacedInstallNearSteam(const QString& targetPath)
{
    const QString targetName = QFileInfo(targetPath).fileName().trimmed();
    if (targetName.isEmpty())
        return {};

    const QString needle = targetName.toLower().remove(QStringLiteral("freetp-")).replace(
        QLatin1Char('-'), QLatin1Char(' '));

    QString bestPath;
    qint64 bestMtime = 0;
    const qint64 now = QDateTime::currentSecsSinceEpoch();

    for (const QString& common : steamCommonRoots()) {
        QDir dir(common);
        const auto entries = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QFileInfo& entry : entries) {
            const QString name = entry.fileName().toLower();
            const QString compactName = QString(name).remove(QLatin1Char(' '));
            const QString compactNeedle = QString(needle).remove(QLatin1Char(' '));
            const bool nameMatch =
                name.contains(needle) || needle.contains(name) || compactName == compactNeedle;
            if (!nameMatch)
                continue;
            if (findGameExecutable(entry.absoluteFilePath()).isEmpty())
                continue;
            const qint64 mtime = entry.lastModified().toSecsSinceEpoch();
            if (now - mtime > 6 * 3600)
                continue;
            if (mtime >= bestMtime) {
                bestMtime = mtime;
                bestPath = entry.absoluteFilePath();
            }
        }
    }
    return bestPath;
}

QString recoverMisplacedInnoInstall(const QString& targetPath, const QString& logPath,
                                    QString* errorOut)
{
    QString actual = actualInstallRootFromLog(logPath, targetPath);
    if (actual.isEmpty())
        actual = findMisplacedInstallNearSteam(targetPath);
    if (actual.isEmpty())
        return {};

    if (QDir::cleanPath(actual).compare(QDir::cleanPath(targetPath), Qt::CaseInsensitive) == 0)
        return targetPath;

    if (!relocateInstalledContent(actual, targetPath, errorOut))
        return {};
    return QDir(targetPath).absolutePath();
}

#if defined(Q_OS_WIN)
QString quoteWindowsArg(const QString& text)
{
    if (text.isEmpty())
        return QStringLiteral("\"\"");
    if (!text.contains(QLatin1Char(' ')) && !text.contains(QLatin1Char('\t'))
        && !text.contains(QLatin1Char('"')))
        return text;

    QString escaped;
    escaped.reserve(text.size() + 4);
    escaped += QLatin1Char('"');
    int backslashes = 0;
    for (const QChar ch : text) {
        if (ch == QLatin1Char('\\')) {
            ++backslashes;
            continue;
        }
        if (ch == QLatin1Char('"')) {
            escaped += QString(backslashes * 2 + 1, QLatin1Char('\\'));
            backslashes = 0;
            escaped += QLatin1Char('"');
            continue;
        }
        if (backslashes > 0) {
            escaped += QString(backslashes, QLatin1Char('\\'));
            backslashes = 0;
        }
        escaped += ch;
    }
    if (backslashes > 0)
        escaped += QString(backslashes * 2, QLatin1Char('\\'));
    escaped += QLatin1Char('"');
    return escaped;
}

bool runInnoProcessWatchingLog(const QString& program, const QStringList& arguments, int timeoutMs,
                               const QString& logPath, QString* errorOut,
                               const QString& workingDirectory)
{
    if (!QFileInfo::exists(program)) {
        if (errorOut)
            *errorOut = QStringLiteral("Установщик не найден");
        return false;
    }

    QStringList parts;
    for (const QString& argument : arguments)
        parts << quoteWindowsArg(argument);
    const QString parameters = parts.join(QLatin1Char(' '));
    const QString nativeProgram = QDir::toNativeSeparators(program);
    const QString nativeWorkDir =
        workingDirectory.isEmpty() ? QString() : QDir::toNativeSeparators(workingDirectory);

    SHELLEXECUTEINFOW executeInfo{};
    executeInfo.cbSize = sizeof(executeInfo);
    executeInfo.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOZONECHECKS;
    executeInfo.lpVerb = L"open";
    executeInfo.lpFile = reinterpret_cast<LPCWSTR>(nativeProgram.utf16());
    executeInfo.lpParameters =
        parameters.isEmpty() ? nullptr : reinterpret_cast<LPCWSTR>(parameters.utf16());
    executeInfo.lpDirectory =
        nativeWorkDir.isEmpty() ? nullptr : reinterpret_cast<LPCWSTR>(nativeWorkDir.utf16());
    executeInfo.nShow = SW_HIDE;

    if (!ShellExecuteExW(&executeInfo) || !executeInfo.hProcess) {
        if (errorOut)
            *errorOut = QStringLiteral("Не удалось запустить установщик");
        return false;
    }

    const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + timeoutMs;
    qint64 successSince = 0;
    for (;;) {
        const DWORD waitResult = WaitForSingleObject(executeInfo.hProcess, 1000);
        if (waitResult == WAIT_OBJECT_0)
            break;

        if (QDateTime::currentMSecsSinceEpoch() >= deadline) {
            TerminateProcess(executeInfo.hProcess, 1);
            CloseHandle(executeInfo.hProcess);
            if (errorOut)
                *errorOut = QStringLiteral("Таймаут установщика");
            return false;
        }

        if (innoLogIndicatesFinished(logPath)) {
            if (successSince == 0)
                successSince = QDateTime::currentMSecsSinceEpoch();
            else if (QDateTime::currentMSecsSinceEpoch() - successSince >= kInnoLogSuccessGraceMs) {
                // FreeTP Inno often hangs after "Log closed" (shortcuts / post-run).
                TerminateProcess(executeInfo.hProcess, 0);
                break;
            }
        }
    }

    DWORD exitCode = 1;
    GetExitCodeProcess(executeInfo.hProcess, &exitCode);
    CloseHandle(executeInfo.hProcess);

    if (exitCode != 0 && !innoLogIndicatesFinished(logPath)) {
        if (errorOut)
            *errorOut = QStringLiteral("Установщик завершился с кодом %1").arg(exitCode);
        return false;
    }
    return true;
}
#endif

bool runInnoInstaller(const QString& setupPath, const QStringList& args, int timeoutMs,
                      const QString& logPath, QString* errorOut,
                      const arachnel::core::WindowsRunEnv& env)
{
#if defined(Q_OS_WIN)
    (void)env;
    return runInnoProcessWatchingLog(setupPath, args, timeoutMs, logPath, errorOut,
                                     QFileInfo(setupPath).absolutePath());
#else
    return runInstallProcess(setupPath, args, timeoutMs, errorOut,
                             QFileInfo(setupPath).absolutePath(), env);
#endif
}

} // namespace

QString findSetupExecutable(const QString& rootDir)
{
    QString bestPath;
    int bestDepth = 9999;

    QDirIterator it(rootDir, {QStringLiteral("*.exe")}, QDir::Files,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        const QFileInfo info(path);
        const QString lower = info.fileName().toLower();
        if (lower == QStringLiteral("unins000.exe")
            || lower == QStringLiteral("uninstall.exe"))
            continue;

        const bool isSetup = lower == QStringLiteral("setup.exe")
                             || lower.contains(QStringLiteral("setup"));
        if (!isSetup)
            continue;

        const QString relative = QDir(rootDir).relativeFilePath(path);
        const int depth = relative.count(QLatin1Char('/')) + relative.count(QLatin1Char('\\'));
        if (depth < bestDepth || (depth == bestDepth && lower == QStringLiteral("setup.exe"))) {
            bestDepth = depth;
            bestPath = path;
        }
    }

    return bestPath;
}

bool isInnoSetupExecutable(const QString& setupPath)
{
    QFile file(setupPath);
    if (!file.open(QIODevice::ReadOnly))
        return false;

    const QByteArray header = file.read(1024 * 1024);
    return header.contains("Inno Setup");
}

QString installInnoSetup(const QString& setupPath, const QString& targetPath, QString* errorOut,
                         const arachnel::core::WindowsRunEnv& env)
{
    if (!QFileInfo::exists(setupPath)) {
        if (errorOut)
            *errorOut = QStringLiteral("Установщик не найден");
        return {};
    }

    clearTargetDirectory(targetPath, errorOut);
    if (errorOut && !errorOut->isEmpty())
        return {};

    if (!QDir().mkpath(targetPath)) {
        if (errorOut)
            *errorOut = QStringLiteral("Не удалось создать папку установки");
        return {};
    }

    const QString logPath = QDir(targetPath).absoluteFilePath(QStringLiteral("install.log"));
    const QStringList args = {
        QStringLiteral("/VERYSILENT"),
        QStringLiteral("/SUPPRESSMSGBOXES"),
        QStringLiteral("/NORESTART"),
        QStringLiteral("/SP-"),
        innoPathArg(QStringLiteral("/DIR="), targetPath),
        innoPathArg(QStringLiteral("/LOG="), logPath),
    };

    if (!runInnoInstaller(setupPath, args, kInnoInstallTimeoutMs, logPath, errorOut, env)) {
        // Even on process failure, FreeTP may have unpacked into Steam\common.
        QString recoverError;
        if (!recoverMisplacedInnoInstall(targetPath, logPath, &recoverError).isEmpty()
            && !waitForGameExecutable(targetPath).isEmpty()) {
            return QDir(targetPath).absolutePath();
        }
        if (errorOut) {
            const QString logTail = tailOfInstallLog(logPath);
            if (!logTail.isEmpty())
                *errorOut += QStringLiteral("\n") + logTail;
        }
        return {};
    }

    QString exe = waitForGameExecutable(targetPath);
    if (exe.isEmpty()) {
        QString recoverError;
        if (recoverMisplacedInnoInstall(targetPath, logPath, &recoverError).isEmpty()) {
            if (errorOut) {
                const QString logTail = tailOfInstallLog(logPath);
                *errorOut = QStringLiteral("Игра не найдена после установки Inno Setup");
                if (!recoverError.isEmpty())
                    *errorOut += QStringLiteral("\n") + recoverError;
                if (!logTail.isEmpty())
                    *errorOut += QStringLiteral("\n") + logTail;
            }
            return {};
        }
        exe = waitForGameExecutable(targetPath);
        if (exe.isEmpty()) {
            if (errorOut)
                *errorOut = QStringLiteral("Игра найдена вне библиотеки, но перенос не удался");
            return {};
        }
    }

    return QFileInfo(exe).absolutePath();
}

QString installInnoOverlay(const QString& setupPath, const QString& targetPath, QString* errorOut,
                           const arachnel::core::WindowsRunEnv& env)
{
    if (!QFileInfo::exists(setupPath)) {
        if (errorOut)
            *errorOut = QStringLiteral("Установщик не найден");
        return {};
    }

    if (!QDir().mkpath(targetPath)) {
        if (errorOut)
            *errorOut = QStringLiteral("Не удалось создать папку игры");
        return {};
    }

    const QString logPath = QDir(targetPath).absoluteFilePath(QStringLiteral("addon-install.log"));
    const QStringList args = {
        QStringLiteral("/VERYSILENT"),
        QStringLiteral("/SUPPRESSMSGBOXES"),
        QStringLiteral("/NORESTART"),
        QStringLiteral("/SP-"),
        innoPathArg(QStringLiteral("/DIR="), targetPath),
        innoPathArg(QStringLiteral("/LOG="), logPath),
    };

    if (!runInnoInstaller(setupPath, args, kAddonInstallTimeoutMs, logPath, errorOut, env)) {
        if (errorOut) {
            const QString logTail = tailOfInstallLog(logPath);
            if (!logTail.isEmpty())
                *errorOut += QStringLiteral("\n") + logTail;
            *errorOut += QStringLiteral("\nЛог: %1").arg(logPath);
        }
        return {};
    }

    return targetPath;
}

void cleanupInnoSideEffects(const QString& installPath)
{
    if (installPath.isEmpty())
        return;

    QDirIterator urls(installPath, {QStringLiteral("*.url")}, QDir::Files,
                      QDirIterator::Subdirectories);
    while (urls.hasNext()) {
        const QString urlPath = urls.next();
        const QString lower = QFileInfo(urlPath).fileName().toLower();
        if (lower.contains(QStringLiteral("freetp")))
            QFile::remove(urlPath);
    }

    for (const QString& desktop : desktopRoots()) {
        QDirIterator shortcuts(desktop, {QStringLiteral("*.lnk")}, QDir::Files);
        while (shortcuts.hasNext()) {
            const QString shortcutPath = shortcuts.next();
            if (isFreetpPromoShortcut(shortcutPath))
                QFile::remove(shortcutPath);
        }
    }
}

} // namespace freetp
