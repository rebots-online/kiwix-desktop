#include "downloadmanagement.h"

#include "kiwixapp.h"
#include "kiwixconfirmbox.h"

#include <QStorageInfo>
#include <QThread>

////////////////////////////////////////////////////////////////////////////////
// DowloadState
////////////////////////////////////////////////////////////////////////////////

namespace
{

QString convertToUnits(double bytes)
{
    QStringList units = {"bytes", "KB", "MB", "GB", "TB", "PB", "EB"};
    int unitIndex = 0;
    while (bytes >= 1024 && unitIndex < units.size()) {
        bytes /= 1024;
        unitIndex++;
    }

    const auto preciseBytes = QString::number(bytes, 'g', 3);
    return preciseBytes + " " + units[unitIndex];
}

} // unnamed namespace

void DownloadState::update(const DownloadInfo& downloadInfos)
{
    double percent = downloadInfos["completedLength"].toDouble() / downloadInfos["totalLength"].toDouble();
    percent *= 100;
    percent = QString::number(percent, 'g', 3).toDouble();
    auto completedLength = convertToUnits(downloadInfos["completedLength"].toDouble());
    auto downloadSpeed = convertToUnits(downloadInfos["downloadSpeed"].toDouble()) + "/s";
    const bool paused = downloadInfos["status"] == "paused";
    *this = {percent, completedLength, downloadSpeed, paused};
}

////////////////////////////////////////////////////////////////////////////////
// DowloadManager
////////////////////////////////////////////////////////////////////////////////

DownloadManager::DownloadManager(const Library* lib, kiwix::Downloader *downloader)
    : mp_library(lib)
    , mp_downloader(downloader)
{
    restoreDownloads();
}

DownloadManager::~DownloadManager()
{
    if ( mp_downloadUpdaterThread )
    {
        QThread* t = mp_downloadUpdaterThread;
        mp_downloadUpdaterThread = nullptr; // tell the thread to terminate
        t->wait();
    }
}

bool DownloadManager::downloadingFunctionalityAvailable() const
{
    return mp_downloader != nullptr;
}

void DownloadManager::startDownloadUpdaterThread()
{
    // so that DownloadInfo can be copied across threads
    qRegisterMetaType<DownloadInfo>("DownloadInfo");

    mp_downloadUpdaterThread = QThread::create([=]() {
       while ( mp_downloadUpdaterThread != nullptr ) {
            updateDownloads();
            QThread::msleep(1000);
        }
    });
    mp_downloadUpdaterThread->start();
}

void DownloadManager::restoreDownloads()
{
    for ( const auto& bookId : mp_library->getBookIds() ) {
        const kiwix::Book& book = mp_library->getBookById(bookId);
        if ( ! book.getDownloadId().empty() ) {
            const auto newDownload = std::make_shared<DownloadState>();
            newDownload->paused = true;
            m_downloads.set(bookId, newDownload);
        }
    }
}

void DownloadManager::updateDownloads()
{
    DownloadInfo downloadInfo;
    for ( const auto& bookId : m_downloads.keys() ) {
        try {
            downloadInfo = getDownloadInfo(bookId);
        } catch ( ... ) {
            emit downloadDisappeared(bookId);
            continue;
        }

        emit downloadUpdated(bookId, downloadInfo);
    }
}

namespace
{

QString downloadStatus2QString(kiwix::Download::StatusResult status)
{
    switch(status){
    case kiwix::Download::K_ACTIVE:   return "active";
    case kiwix::Download::K_WAITING:  return "waiting";
    case kiwix::Download::K_PAUSED:   return "paused";
    case kiwix::Download::K_ERROR:    return "error";
    case kiwix::Download::K_COMPLETE: return "completed";
    case kiwix::Download::K_REMOVED:  return "removed";
    default:                          return "unknown";
    }
}

} // unnamed namespace

DownloadInfo DownloadManager::getDownloadInfo(QString bookId) const
{
    auto& b = mp_library->getBookById(bookId);
    const auto d = mp_downloader->getDownload(b.getDownloadId());
    d->updateStatus(true);

    return {
             { "status"          , downloadStatus2QString(d->getStatus())   },
             { "completedLength" , QString::number(d->getCompletedLength()) },
             { "totalLength"     , QString::number(d->getTotalLength())     },
             { "downloadSpeed"   , QString::number(d->getDownloadSpeed())   },
             { "path"            , QString::fromStdString(d->getPath())     }
    };
}

namespace
{

void throwDownloadUnavailableError()
{
    throw KiwixAppError(gt("download-unavailable"),
                        gt("download-unavailable-text"));
}

void checkThatBookCanBeSaved(const kiwix::Book& book, QString targetDir)
{
    const QFileInfo targetDirInfo(targetDir);
    if ( !targetDirInfo.isDir() ) {
        throw KiwixAppError(gt("download-storage-error"),
                            gt("download-dir-missing"));
    }

    // XXX: This may lie under Windows
    // XXX: (see https://doc.qt.io/qt-5/qfile.html#platform-specific-issues)
    if ( !targetDirInfo.isWritable() ) {
        throw KiwixAppError(gt("download-storage-error"),
                            gt("download-dir-not-writable"));
    }

    QStorageInfo storage(targetDir);
    auto bytesAvailable = storage.bytesAvailable();
    if (bytesAvailable == -1 || book.getSize() > (unsigned long long) bytesAvailable) {
        throw KiwixAppError(gt("download-storage-error"),
                            gt("download-storage-error-text"));
    }
}

} // unnamed namespace


std::string DownloadManager::startDownload(const kiwix::Book& book, const QString& downloadDirPath)
{
    if ( ! DownloadManager::downloadingFunctionalityAvailable() )
        throwDownloadUnavailableError();

    checkThatBookCanBeSaved(book, downloadDirPath);

    typedef std::vector<std::pair<std::string, std::string>> DownloadOptions;

    const std::string& url = book.getUrl();
    const QString bookId = QString::fromStdString(book.getId());
    const DownloadOptions downloadOptions{{"dir", downloadDirPath.toStdString()}};

    std::string downloadId;
    try {
        const auto d = mp_downloader->startDownload(url, downloadOptions);
        downloadId = d->getDid();
    } catch (std::exception& e) {
        throwDownloadUnavailableError();
    }
    m_downloads.set(bookId, std::make_shared<DownloadState>());
    return downloadId;
}

void DownloadManager::pauseDownload(const QString& bookId)
{
    const auto downloadId = mp_library->getBookById(bookId).getDownloadId();
    if ( downloadId.empty() ) {
        // Completion of the download has been detected (and its id was reset)
        // before the pause-download action was triggered (most likely through
        // the context menu which can stay open for an arbitrarily long time,
        // or, unlikely, through the ⏸ button during the last milliseconds of
        // the download progress).
        return;
    }

    auto download = mp_downloader->getDownload(downloadId);
    if (download->getStatus() == kiwix::Download::K_ACTIVE) {
        try {
            download->pauseDownload();
        } catch (const kiwix::AriaError&) {
            // Download has completed before the pause request was handled.
            // Most likely the download was already complete at the time
            // when ContentManager::pauseBook() started executing, but its
            // completion was not yet detected (and/or handled) by the download
            // updater thread.
        }
    }
}

void DownloadManager::resumeDownload(const QString& bookId)
{
    auto& b = mp_library->getBookById(bookId);
    auto download = mp_downloader->getDownload(b.getDownloadId());
    if (download->getStatus() == kiwix::Download::K_PAUSED) {
        download->resumeDownload();
    }
}

bool DownloadManager::cancelDownload(const QString& bookId)
{
    const auto downloadId = mp_library->getBookById(bookId).getDownloadId();
    if ( downloadId.empty() ) {
        // Completion of the download has been detected (and its id was reset)
        // before the confirmation to cancel the download was granted.
        return false;
    }

    auto download = mp_downloader->getDownload(downloadId);
    try {
        download->cancelDownload();
        return true;
    } catch (const kiwix::AriaError&) {
        // Download has completed before the cancel request was handled.
        // Most likely the download was already complete at the time
        // when ContentManager::reallyCancelBook() started executing, but
        // its completion was not yet detected (and/or handled) by the
        // download updater thread (letting the code pass past the empty
        // downloadId check above).
        return false;
    }
}

void DownloadManager::removeDownload(QString bookId)
{
    m_downloads.remove(bookId);
}
