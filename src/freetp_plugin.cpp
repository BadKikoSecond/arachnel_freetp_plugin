#include "freetp_plugin.h"

#include "archive_installer.h"
#include "installer_runner.h"
#include "linux_fix_launch.h"

#include "catalog_parser.h"
#include "install_heuristics.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>
#include <QUrl>

namespace freetp {

namespace {

constexpr auto kSourceId = "freetp";
constexpr qint64 kCatalogCacheTtlMs = 5 * 60 * 1000;

QString readManifestString(const QString& rootPath, const QString& key)
{
    QFile file(rootPath + QStringLiteral("/plugin.json"));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QJsonObject obj = QJsonDocument::fromJson(file.readAll()).object();
    return obj.value(key).toString().trimmed();
}

QByteArray fetchCatalogUrl(const QUrl& url)
{
    if (!url.isValid() || url.host().isEmpty())
        return {};

    QNetworkAccessManager nam;
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Arachnel-FreeTP/1"));

    QNetworkReply* reply = nam.get(request);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    QByteArray body;
    if (reply->error() == QNetworkReply::NoError)
        body = reply->readAll();
    reply->deleteLater();
    return body;
}

QString writableCatalogCachePath()
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (base.isEmpty())
        return {};
    return base + QStringLiteral("/plugin-catalog-cache/freetp/games-arachnel.json");
}

bool catalogCacheIsFresh(const QString& path)
{
    const QFileInfo info(path);
    if (!info.exists() || info.size() <= 0)
        return false;
    const qint64 ageMs = info.lastModified().msecsTo(QDateTime::currentDateTime());
    return ageMs >= 0 && ageMs < kCatalogCacheTtlMs;
}

QByteArray readFileBytes(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

bool writeCatalogCache(const QString& path, const QByteArray& payload)
{
    if (path.isEmpty() || payload.isEmpty())
        return false;
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return false;
    QFile cache(path);
    if (!cache.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return cache.write(payload) == payload.size();
}

QByteArray readCatalogBytes(const QString& rootPath)
{
    const QString cachePath = writableCatalogCachePath();

    // Fresh AppData cache (5 minutes) — do not pin a JSON inside the plugin folder.
    if (catalogCacheIsFresh(cachePath)) {
        const QByteArray cached = readFileBytes(cachePath);
        if (!cached.isEmpty())
            return cached;
    }

    const QString catalogUrl = readManifestString(rootPath, QStringLiteral("catalogUrl"));
    const QByteArray remote = fetchCatalogUrl(QUrl(catalogUrl));
    if (!remote.isEmpty()) {
        writeCatalogCache(cachePath, remote);
        return remote;
    }

    // Offline: prefer stale AppData cache, then optional bundled snapshot.
    const QByteArray stale = readFileBytes(cachePath);
    if (!stale.isEmpty())
        return stale;

    return readFileBytes(rootPath + QStringLiteral("/games-arachnel.json"));
}

bool shouldUseInnoInstaller(const QString& contentRoot,
                            const arachnel::core::InstallContext& ctx)
{
    if (ctx.installKind == arachnel::core::InstallKind::Installer)
        return true;

    const QString setupExe = findSetupExecutable(contentRoot);
    return !setupExe.isEmpty() && isInnoSetupExecutable(setupExe);
}

} // namespace

arachnel::core::WindowsRunEnv windowsRunEnvFromInstallContext(
    const arachnel::core::InstallContext& ctx)
{
    arachnel::core::WindowsRunEnv env;
    env.protonExecutable = ctx.protonExecutable;
    env.compatDataPath = ctx.compatDataPath;
    env.steamCompatClientPath = ctx.steamCompatClientPath;
    return env;
}

arachnel::core::WindowsRunEnv windowsRunEnvFromAddonContext(
    const arachnel::core::AddonInstallContext& ctx)
{
    arachnel::core::WindowsRunEnv env;
    env.protonExecutable = ctx.protonExecutable;
    env.compatDataPath = ctx.compatDataPath;
    env.steamCompatClientPath = ctx.steamCompatClientPath;
    return env;
}

FreetpPlugin::FreetpPlugin(QString rootPath)
    : m_rootPath(std::move(rootPath))
{
}

QString FreetpPlugin::id() const
{
    return QString::fromLatin1(kSourceId);
}

QString FreetpPlugin::name() const
{
    return QStringLiteral("FreeTP");
}

QString FreetpPlugin::description() const
{
    return QStringLiteral("Russian site similar to online-fix.me");
}

QString FreetpPlugin::version() const
{
    const QString fromManifest = readManifestString(m_rootPath, QStringLiteral("version"));
    return !fromManifest.isEmpty() ? fromManifest : QStringLiteral("1.0.0");
}

QStringList FreetpPlugin::capabilities() const
{
    return {QStringLiteral("search"), QStringLiteral("install"), QStringLiteral("update"),
            QStringLiteral("launch")};
}

void FreetpPlugin::resetCatalogCache()
{
    m_catalogLoaded = false;
    m_catalogLoadedAt = {};
    m_catalog.clear();
}

void FreetpPlugin::ensureCatalogLoaded() const
{
    if (m_catalogLoaded && m_catalogLoadedAt.isValid()) {
        const qint64 ageMs = m_catalogLoadedAt.msecsTo(QDateTime::currentDateTime());
        if (ageMs >= 0 && ageMs < kCatalogCacheTtlMs)
            return;
    }

    const QByteArray payload = readCatalogBytes(m_rootPath);
    m_catalog.clear();
    if (!payload.isEmpty())
        m_catalog = arachnel::core::parseCatalogFeed(payload, id());
    m_catalogLoaded = true;
    m_catalogLoadedAt = QDateTime::currentDateTime();
}

QVector<arachnel::core::CatalogEntry> FreetpPlugin::catalog() const
{
    ensureCatalogLoaded();
    return m_catalog;
}

QVector<arachnel::core::CatalogEntry> FreetpPlugin::search(const QString& query) const
{
    ensureCatalogLoaded();
    const QString needle = query.trimmed().toLower();
    if (needle.isEmpty())
        return m_catalog;

    QVector<arachnel::core::CatalogEntry> filtered;
    filtered.reserve(m_catalog.size());
    for (const auto& entry : m_catalog) {
        if (entry.title.toLower().contains(needle))
            filtered.append(entry);
    }
    return filtered;
}

std::optional<arachnel::core::CatalogEntry> FreetpPlugin::entryById(
    const QString& entryId) const
{
    ensureCatalogLoaded();
    for (const auto& entry : m_catalog) {
        if (entry.id == entryId)
            return entry;
    }
    return std::nullopt;
}

arachnel::core::InstallAnalysis FreetpPlugin::analyzeFileNames(
    const QStringList& fileNames) const
{
    bool hasFtpChunk = false;
    for (const QString& path : fileNames) {
        if (QFileInfo(path).fileName().toLower().endsWith(QStringLiteral(".ftp")))
            hasFtpChunk = true;
    }

    if (hasFtpChunk) {
        return arachnel::core::makeInstallAnalysis(arachnel::core::InstallKind::Installer,
                                                   QStringLiteral("freetp-chunked"), 95,
                                                   QStringLiteral("FreeTP multi-part installer"),
                                                   true);
    }

    arachnel::core::InstallAnalysis result = arachnel::core::analyzeTorrentFileNames(fileNames);
    if (result.confidence >= 40)
        result.canInstall = true;
    return result;
}

arachnel::core::InstallAnalysis FreetpPlugin::analyzeDownload(
    const arachnel::core::InstallContext& ctx) const
{
    const QString contentRoot = findDownloadContentRoot(ctx.downloadPath);
    if (!contentRoot.isEmpty()) {
        QDirIterator ftpIt(contentRoot, QDir::Files, QDirIterator::Subdirectories);
        while (ftpIt.hasNext()) {
            if (ftpIt.next().endsWith(QStringLiteral(".ftp"), Qt::CaseInsensitive)) {
                return arachnel::core::makeInstallAnalysis(
                    arachnel::core::InstallKind::Installer, QStringLiteral("freetp-chunked"), 95,
                    QStringLiteral("FreeTP multi-part installer"), true);
            }
        }
    }

    if (!contentRoot.isEmpty() && shouldUseInnoInstaller(contentRoot, ctx)) {
        return arachnel::core::makeInstallAnalysis(arachnel::core::InstallKind::Installer,
                                                   QStringLiteral("inno-setup"), 98,
                                                   QStringLiteral("Inno Setup installer"), true);
    }

    arachnel::core::InstallAnalysis result =
        arachnel::core::analyzeDownloadPath(ctx.downloadPath);
    if (result.confidence >= 40)
        result.canInstall = true;
    return result;
}

arachnel::core::InstallResult FreetpPlugin::installFromDownload(
    const arachnel::core::InstallContext& ctx) const
{
    arachnel::core::InstallResult result;

    const QString contentRoot = findDownloadContentRoot(ctx.downloadPath);
    if (contentRoot.isEmpty() || !QDir(contentRoot).exists()) {
        result.success = false;
        result.error = QStringLiteral("Файлы загрузки не найдены");
        return result;
    }

    QString error;
    QString installPath;
    const arachnel::core::WindowsRunEnv runEnv = windowsRunEnvFromInstallContext(ctx);

    if (shouldUseInnoInstaller(contentRoot, ctx)) {
        const QString setupExe = findSetupExecutable(contentRoot);
        if (setupExe.isEmpty()) {
            result.success = false;
            result.error = QStringLiteral("Inno Setup не найден в загрузке");
            return result;
        }

        installPath = installInnoSetup(setupExe, ctx.targetPath, &error, runEnv);
        if (installPath.isEmpty()) {
            result.success = false;
            result.error = error.isEmpty() ? QStringLiteral("Ошибка тихой установки Inno Setup")
                                           : error;
            return result;
        }

        cleanupInnoSideEffects(installPath);
    } else {
        installPath = installPortableFromDownload(contentRoot, ctx.targetPath, &error);
        if (installPath.isEmpty()) {
            result.success = false;
            result.error = error.isEmpty() ? QStringLiteral("Ошибка portable-установки") : error;
            return result;
        }
    }

    result.success = true;
    result.installPath = installPath;
#if defined(Q_OS_LINUX)
    prepareLinuxFixInstall(installPath, m_rootPath);
#endif
    return result;
}

arachnel::core::InstallResult FreetpPlugin::installAddonFromDownload(
    const arachnel::core::AddonInstallContext& ctx) const
{
    arachnel::core::InstallResult result;
    if (ctx.gameInstallPath.isEmpty() || !QDir(ctx.gameInstallPath).exists()) {
        result.error = QStringLiteral("Сначала установите игру");
        return result;
    }
    if (ctx.downloadPath.isEmpty() || !QFileInfo::exists(ctx.downloadPath)) {
        result.error = QStringLiteral("Файлы дополнения не найдены");
        return result;
    }

    QString error;
    const arachnel::core::WindowsRunEnv runEnv = windowsRunEnvFromAddonContext(ctx);
    const QFileInfo artifact(ctx.downloadPath);
    if (artifact.isFile()) {
        const QString suffix = artifact.suffix().toLower();
        if (suffix == QStringLiteral("exe")) {
            if (installInnoOverlay(ctx.downloadPath, ctx.gameInstallPath, &error, runEnv).isEmpty()) {
                result.error = error.isEmpty() ? QStringLiteral("Ошибка установки фикса") : error;
                return result;
            }
            cleanupInnoSideEffects(ctx.gameInstallPath);
            result.success = true;
            result.installPath = ctx.gameInstallPath;
#if defined(Q_OS_LINUX)
            prepareLinuxFixInstall(ctx.gameInstallPath, m_rootPath);
#endif
            return result;
        }
    }

    const QString contentRoot = artifact.isDir() ? findDownloadContentRoot(ctx.downloadPath)
                                                 : artifact.absolutePath();
    const QString setupExe = findSetupExecutable(contentRoot);
    if (!setupExe.isEmpty() && isInnoSetupExecutable(setupExe)) {
        if (installInnoOverlay(setupExe, ctx.gameInstallPath, &error, runEnv).isEmpty()) {
            result.error = error.isEmpty() ? QStringLiteral("Ошибка установки фикса") : error;
            return result;
        }
        cleanupInnoSideEffects(ctx.gameInstallPath);
        result.success = true;
        result.installPath = ctx.gameInstallPath;
#if defined(Q_OS_LINUX)
        prepareLinuxFixInstall(ctx.gameInstallPath, m_rootPath);
#endif
        return result;
    }

    if (!installAddonOverlay(ctx.downloadPath, ctx.gameInstallPath, &error)) {
        result.error = error.isEmpty() ? QStringLiteral("Ошибка установки дополнения") : error;
        return result;
    }

    result.success = true;
    result.installPath = ctx.gameInstallPath;
#if defined(Q_OS_LINUX)
    prepareLinuxFixInstall(ctx.gameInstallPath, m_rootPath);
#endif
    return result;
}

std::optional<QString> FreetpPlugin::detectUpdate(const arachnel::core::LibraryGame& local,
                                                  const arachnel::core::CatalogEntry& remote) const
{
    if (remote.uploadDate.isEmpty() || local.uploadDate.isEmpty())
        return std::nullopt;

    const QDateTime remoteDate = QDateTime::fromString(remote.uploadDate, Qt::ISODate);
    const QDateTime localDate = QDateTime::fromString(local.uploadDate, Qt::ISODate);
    if (remoteDate.isValid() && localDate.isValid()) {
        if (remoteDate > localDate)
            return remote.uploadDate;
        return std::nullopt;
    }

    if (remote.uploadDate > local.uploadDate)
        return remote.uploadDate;
    return std::nullopt;
}

arachnel::core::LaunchInfo FreetpPlugin::launchInfo(const arachnel::core::LibraryGame& local) const
{
    arachnel::core::LaunchInfo info;
    if (local.installPath.isEmpty())
        return info;

    const QString exe = linuxFixLaunchEnabled() ? findBestGameExecutable(local.installPath)
                                                : findGameExecutable(local.installPath);
    if (exe.isEmpty())
        return info;

    info.executable = exe;
    info.workingDirectory = QFileInfo(exe).absolutePath();

#if defined(Q_OS_LINUX)
    LinuxFixLaunchOptions options;
    applyLinuxFixLaunchInfo(local.installPath, m_rootPath, options, &info);
#endif

    return info;
}

} // namespace freetp
