#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPair>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTcpServer>
#include <QWidget>

#include <functional>

class QLabel;
class QProcess;
class QVBoxLayout;
class QCefWidget;
struct QCef;
class TrediaApi;
class TrediaBridgeConnection;
class TrediaMontageWindow;

/* The montage editor inside Tredia Studio: Tredia's own editor page (the same one as on tredia.cloud)
 * in a window of the program, working on the recordings on this PC.
 *
 * The program runs a small web server on 127.0.0.1 only (a random port and a secret key per run). It
 * serves the editor page itself (fetched from tredia.cloud, so the editor updates with the site) and,
 * beside it under /bridge/, what the page needs from the PC: the recordings folder, the file picker,
 * projects saved as files, and the export, rendered with ffmpeg here (tredia-ffmpeg.exe ships with
 * the program). No file is uploaded (the editor's assistant sends Tredia the project's outline only,
 * with this program's sign-in). The page builds the export plan; the program runs it only after
 * checking it uses known files and nothing but cutting, framing and mixing filters. */
class TrediaMontage : public QObject {
	Q_OBJECT

public:
	explicit TrediaMontage(QObject *parent = nullptr);
	~TrediaMontage() override;

	/* Shows the editor; with a file (the recording that just stopped), starts a project with it. */
	void open(const QString &file = QString());
	/* OBS is closing: stop exports and close the window. */
	void shutdown();
	bool exporting() const;
	void cancelExports();
	/* The signed-in Tredia account, for the editor's assistant (the page itself has no sign-in). */
	void setApi(TrediaApi *client);

	struct Request {
		QByteArray method;
		QString path;
		QString query; /* as sent, still encoded */
		QHash<QString, QString> params;
		QHash<QByteArray, QByteArray> headers;
		QByteArray body;
	};
	void handle(TrediaBridgeConnection *c, const Request &r);

private:
	struct Probe {
		bool done = false;
		bool running = false;
		bool video = false;
		bool audio = false;
		double duration = 0;
		int width = 0;
		int height = 0;
		qint64 stamp = 0; /* size and time of the file it was made for */
		QList<std::function<void()>> waiting;
	};
	struct Job {
		QString id;
		QString status = QStringLiteral("rendering");
		double progress = 0;
		double seconds = 1;
		QString error;
		QString output;
		QString graphFile;
		QStringList inputArgs;
		QStringList layerFiles;
		int fps = 30;
		bool nvenc = true;
		QPointer<QProcess> process;
		QString stderrTail;
	};

	QTcpServer server;
	QByteArray key;
	QString site;
	QNetworkAccessManager net;
	QHash<QString, QPair<QByteArray, QByteArray>> staticCache; /* /_next/static/… → type, bytes */
	qint64 staticCacheBytes = 0;
	QPointer<TrediaMontageWindow> window;
	QCef *cef = nullptr;
	QString pendingOpen;
	QHash<QString, QString> registry; /* media id → file on this PC */
	QHash<QString, Probe> probes;
	QHash<QString, Job *> jobs;
	QHash<QString, QString> remuxed; /* media id → MP4 copy for the player (MKV, FLV, TS recordings) */
	QStringList remuxing;
	int ffmpegMajor = 0;
	QPointer<TrediaApi> api;

	bool listen();
	QString pageUrl() const;
	QString ffmpeg() const;
	QString dir(const char *sub) const;
	QString recordingFolder() const;
	QStringList recordings() const;
	QString idFor(const QString &file);
	void loadRegistry();
	void saveRegistry() const;

	void startProbe(const QString &id);
	void whenProbed(const QStringList &ids, std::function<void()> done, int timeoutMs);
	void parseProbe(const QString &id, const QString &text);
	void startRemux(const QString &id);
	QJsonObject mediaJson(const QString &id) const;

	void proxy(TrediaBridgeConnection *c, const Request &r);
	void state(TrediaBridgeConnection *c);
	void probeOne(TrediaBridgeConnection *c, const QString &id);
	void pick(TrediaBridgeConnection *c, const QString &kind);
	void media(TrediaBridgeConnection *c, const QString &id, const QByteArray &range);
	void project(TrediaBridgeConnection *c, const QByteArray &method, const QString &id, const QByteArray &body);
	QJsonArray projects() const;
	void layer(TrediaBridgeConnection *c, const QByteArray &body);
	void startExport(TrediaBridgeConnection *c, const QByteArray &body);
	void run(Job *job);
	void finish(Job *job, int exitCode);
	void jobState(TrediaBridgeConnection *c, const QString &id);
	void cancel(TrediaBridgeConnection *c, const QString &id);
	void reveal(TrediaBridgeConnection *c, const QString &id);
	void assistant(TrediaBridgeConnection *c, const QByteArray &body);
	void hear(TrediaBridgeConnection *c, const QByteArray &body);

	friend class TrediaMontageWindow;
};

/* The window the editor page shows in (OBS's built-in browser). */
class TrediaMontageWindow : public QWidget {
	Q_OBJECT

public:
	TrediaMontageWindow(TrediaMontage *owner, QCef *cef);
	void load(const QString &url);

protected:
	void closeEvent(QCloseEvent *event) override;

private:
	QPointer<TrediaMontage> owner;
	QCef *cef;
	QVBoxLayout *column;
	QLabel *status;
	QCefWidget *page = nullptr;
	QString wanted;

	void create();
};
