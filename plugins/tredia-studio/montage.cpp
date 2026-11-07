#include "montage.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/bmem.h>
#include <util/config-file.h>

#include <QApplication>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLabel>
#include <QMessageBox>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QPushButton>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>

#include <memory>
#include <thread>
#include <utility>

#ifdef BROWSER_AVAILABLE
#include <browser-panel.hpp>
#endif

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

const qint64 maxHeader = 64 * 1024;
const qint64 maxBody = 24 * 1024 * 1024;

/* The filters an export may use: cutting, framing, joining and mixing. Nothing that opens or writes
 * files (movie, amovie, sendcmd…), so a plan can only work on the files it names. */
const QSet<QString> allowedFilters = {
	"setpts", "asetpts", "split", "scale", "crop", "boxblur", "setsar", "color", "overlay", "fps", "format", "trim",
	"aresample", "aformat", "volume", "apad", "atrim", "anullsrc", "null", "anull", "concat", "afade", "adelay", "amix",
};
const QStringList videoExt = {"mp4", "mov", "mkv", "m4v", "webm", "flv", "ts"};
const QStringList imageExt = {"png", "jpg", "jpeg", "webp", "gif", "bmp"};
const QStringList audioExt = {"mp3", "m4a", "aac", "wav", "ogg", "opus", "flac"};
const QStringList remuxExt = {"mkv", "flv", "ts"};

QString kindOf(const QString &file)
{
	const QString ext = QFileInfo(file).suffix().toLower();
	if (videoExt.contains(ext))
		return QStringLiteral("video");
	if (imageExt.contains(ext))
		return QStringLiteral("image");
	if (audioExt.contains(ext))
		return QStringLiteral("audio");
	return QString();
}

QByteArray typeOf(const QString &file)
{
	static const QHash<QString, QByteArray> types = {
		{"mp4", "video/mp4"},   {"m4v", "video/mp4"},   {"mov", "video/quicktime"}, {"webm", "video/webm"},
		{"mkv", "video/x-matroska"}, {"flv", "video/x-flv"}, {"ts", "video/mp2t"}, {"png", "image/png"},
		{"jpg", "image/jpeg"},  {"jpeg", "image/jpeg"}, {"webp", "image/webp"},     {"gif", "image/gif"},
		{"bmp", "image/bmp"},   {"mp3", "audio/mpeg"},  {"m4a", "audio/mp4"},       {"aac", "audio/aac"},
		{"wav", "audio/wav"},   {"ogg", "audio/ogg"},   {"opus", "audio/ogg"},      {"flac", "audio/flac"},
	};
	return types.value(QFileInfo(file).suffix().toLower(), "application/octet-stream");
}

QByteArray reasonOf(int status)
{
	switch (status) {
	case 200:
		return "OK";
	case 206:
		return "Partial Content";
	case 400:
		return "Bad Request";
	case 403:
		return "Forbidden";
	case 404:
		return "Not Found";
	case 405:
		return "Method Not Allowed";
	case 409:
		return "Conflict";
	case 413:
		return "Payload Too Large";
	case 416:
		return "Range Not Satisfiable";
	case 502:
		return "Bad Gateway";
	default:
		return "Error";
	}
}

/* The filter names in a filter graph ("[0:v]scale=…,crop=…[s0];…"), skipping labels and quoted text. */
bool filtersAllowed(const QString &graph)
{
	QString seg;
	bool quote = false;
	bool label = false;
	QStringList segs;
	for (const QChar ch : graph) {
		if (quote) {
			if (ch == QLatin1Char('\''))
				quote = false;
			seg += ch;
			continue;
		}
		if (ch == QLatin1Char('\'')) {
			quote = true;
			seg += ch;
		} else if (ch == QLatin1Char('[')) {
			label = true;
		} else if (ch == QLatin1Char(']')) {
			label = false;
		} else if (label) {
			continue;
		} else if (ch == QLatin1Char(';') || ch == QLatin1Char(',')) {
			segs << seg;
			seg.clear();
		} else {
			seg += ch;
		}
	}
	segs << seg;
	for (const QString &s : segs) {
		const QString name = s.section(QLatin1Char('='), 0, 0).trimmed();
		if (!allowedFilters.contains(name))
			return false;
	}
	return !quote && !label;
}

QString safeTitle(const QString &title)
{
	QString t = title;
	t.remove(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|\\x00-\\x1f]")));
	t = t.simplified().left(60);
	return t.isEmpty() ? QStringLiteral("مونتاج") : t;
}

QString message(const char *text)
{
	return QString::fromUtf8(text);
}

} // namespace

/* ── One browser connection: reads HTTP/1.1 requests, answers them one after the other ────────── */

class TrediaBridgeConnection : public QObject {
public:
	TrediaBridgeConnection(QTcpSocket *s, TrediaMontage *o) : QObject(o), socket(s), owner(o)
	{
		s->setParent(this);
		connect(s, &QTcpSocket::readyRead, this, [this]() {
			if (!socket)
				return;
			buf += socket->readAll();
			parse();
		});
		connect(s, &QTcpSocket::bytesWritten, this, [this](qint64) { pump(); });
		connect(s, &QTcpSocket::disconnected, this, [this]() {
			if (file) {
				file->close();
				delete file;
				file = nullptr;
			}
			deleteLater();
		});
	}

	void reply(int status, const QByteArray &type, const QByteArray &body, const QList<QPair<QByteArray, QByteArray>> &extra = {})
	{
		if (!socket)
			return;
		QByteArray h = "HTTP/1.1 " + QByteArray::number(status) + " " + reasonOf(status) + "\r\n";
		h += "Content-Type: " + type + "\r\n";
		h += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
		h += "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n";
		for (const auto &e : extra)
			h += e.first + ": " + e.second + "\r\n";
		h += closeAfter ? "Connection: close\r\n\r\n" : "Connection: keep-alive\r\n\r\n";
		socket->write(h);
		if (!headOnly)
			socket->write(body);
		done();
	}

	void json(int status, const QJsonObject &o) { reply(status, "application/json; charset=utf-8", QJsonDocument(o).toJson(QJsonDocument::Compact)); }
	void error(int status, const QString &text) { json(status, QJsonObject{{"error", text}}); }

	/* A file, or the byte range the player asked for. */
	void sendFile(const QString &path, const QByteArray &type, const QByteArray &range)
	{
		auto *f = new QFile(path);
		if (!f->open(QIODevice::ReadOnly)) {
			delete f;
			error(404, message("الملف غير موجود."));
			return;
		}
		const qint64 size = f->size();
		qint64 start = 0;
		qint64 end = size - 1;
		bool partial = false;
		static const QRegularExpression re(QStringLiteral("^bytes=(\\d*)-(\\d*)$"));
		const QRegularExpressionMatch m = re.match(QString::fromLatin1(range));
		if (m.hasMatch() && size > 0) {
			partial = true;
			const QString a = m.captured(1);
			const QString b = m.captured(2);
			if (a.isEmpty()) {
				start = qMax<qint64>(0, size - b.toLongLong());
			} else {
				start = a.toLongLong();
				if (!b.isEmpty())
					end = qMin(b.toLongLong(), size - 1);
			}
			if (start > end || start >= size) {
				delete f;
				reply(416, "text/plain", QByteArray(), {{"Content-Range", "bytes */" + QByteArray::number(size)}});
				return;
			}
		}
		QByteArray h = "HTTP/1.1 " + QByteArray(partial ? "206 Partial Content" : "200 OK") + "\r\n";
		h += "Content-Type: " + type + "\r\n";
		h += "Content-Length: " + QByteArray::number(size > 0 ? end - start + 1 : 0) + "\r\n";
		h += "Accept-Ranges: bytes\r\nCache-Control: no-store\r\n";
		if (partial)
			h += "Content-Range: bytes " + QByteArray::number(start) + "-" + QByteArray::number(end) + "/" + QByteArray::number(size) + "\r\n";
		h += closeAfter ? "Connection: close\r\n\r\n" : "Connection: keep-alive\r\n\r\n";
		socket->write(h);
		if (headOnly || size <= 0) {
			delete f;
			done();
			return;
		}
		f->seek(start);
		file = f;
		left = end - start + 1;
		pump();
	}

	/* Answered later (a dialog, the site, a file being looked at): the socket may be gone by then. */
	bool alive() const { return !socket.isNull() && socket->state() == QAbstractSocket::ConnectedState; }

private:
	QPointer<QTcpSocket> socket;
	QPointer<TrediaMontage> owner;
	QByteArray buf;
	bool busy = false;
	bool closeAfter = false;
	bool headOnly = false;
	QFile *file = nullptr;
	qint64 left = 0;

	void parse()
	{
		if (busy || !socket || !owner)
			return;
		const qsizetype end = buf.indexOf("\r\n\r\n");
		if (end < 0) {
			if (buf.size() > maxHeader)
				socket->abort();
			return;
		}
		const QList<QByteArray> lines = buf.left(end).split('\n');
		const QList<QByteArray> first = lines.value(0).trimmed().split(' ');
		if (first.size() < 3) {
			socket->abort();
			return;
		}
		QHash<QByteArray, QByteArray> headers;
		for (qsizetype i = 1; i < lines.size(); i++) {
			const QByteArray l = lines[i].trimmed();
			const qsizetype c = l.indexOf(':');
			if (c > 0)
				headers.insert(l.left(c).trimmed().toLower(), l.mid(c + 1).trimmed());
		}
		const qint64 length = headers.value("content-length", "0").toLongLong();
		if (length < 0 || length > maxBody) {
			socket->abort();
			return;
		}
		if (buf.size() < end + 4 + length)
			return;

		TrediaMontage::Request r;
		r.method = first[0];
		r.headers = headers;
		r.body = buf.mid(end + 4, length);
		buf.remove(0, end + 4 + length);
		const QUrl url(QStringLiteral("http://local") + QString::fromUtf8(first[1]));
		r.path = url.path(QUrl::FullyDecoded);
		r.query = url.query(QUrl::FullyEncoded);
		for (const auto &item : QUrlQuery(url).queryItems(QUrl::FullyDecoded))
			r.params.insert(item.first, item.second);
		closeAfter = headers.value("connection").toLower() == "close";
		headOnly = r.method == "HEAD";
		busy = true;
		owner->handle(this, r);
	}

	void pump()
	{
		while (file && socket && left > 0 && socket->bytesToWrite() < 4 * 1024 * 1024) {
			const QByteArray chunk = file->read(qMin<qint64>(left, 512 * 1024));
			if (chunk.isEmpty()) {
				left = 0;
				break;
			}
			socket->write(chunk);
			left -= chunk.size();
		}
		if (file && left <= 0) {
			file->close();
			delete file;
			file = nullptr;
			done();
		}
	}

	void done()
	{
		busy = false;
		if (closeAfter && socket) {
			socket->disconnectFromHost();
			return;
		}
		QTimer::singleShot(0, this, [this]() { parse(); });
	}
};

/* ── The window ──────────────────────────────────────────────────────────────────────────────── */

TrediaMontageWindow::TrediaMontageWindow(TrediaMontage *o, QCef *c) : QWidget(nullptr, Qt::Window), owner(o), cef(c)
{
	setAttribute(Qt::WA_DeleteOnClose);
	setObjectName("trediaMontageWindow");
	setWindowTitle(QStringLiteral("المونتاج · تريديا ستوديو"));
	setMinimumSize(980, 620);
	column = new QVBoxLayout(this);
	column->setContentsMargins(0, 0, 0, 0);
	status = new QLabel(QStringLiteral("نفتح المونتاج…"));
	status->setAlignment(Qt::AlignCenter);
	status->setWordWrap(true);
	column->addWidget(status);
}

void TrediaMontageWindow::load(const QString &url)
{
	wanted = url;
#ifdef BROWSER_AVAILABLE
	if (page) {
		page->setURL(url.toStdString());
		return;
	}
	if (!cef) {
		status->setText(QStringLiteral("المتصفح المدمج غير متوفر في هذي النسخة، فما نقدر نفتح المونتاج."));
		return;
	}
	if (cef->init_browser()) {
		create();
		return;
	}
	/* The built-in browser starts the first time something needs it: wait for it off the UI thread. */
	status->setText(QStringLiteral("نجهّز المتصفح المدمج…"));
	QPointer<TrediaMontageWindow> self(this);
	QCef *c = cef;
	std::thread([c, self]() {
		c->wait_for_browser_init();
		QMetaObject::invokeMethod(
			qApp,
			[self]() {
				if (self)
					self->create();
			},
			Qt::QueuedConnection);
	}).detach();
#else
	status->setText(QStringLiteral("المتصفح المدمج غير متوفر في هذي النسخة، فما نقدر نفتح المونتاج."));
#endif
}

void TrediaMontageWindow::create()
{
#ifdef BROWSER_AVAILABLE
	if (page || !cef)
		return;
	page = cef->create_widget(this, wanted.toStdString(), nullptr);
	if (!page) {
		status->setText(QStringLiteral("ما قدرنا نفتح المتصفح المدمج."));
		return;
	}
	status->hide();
	column->addWidget(page);
#endif
}

void TrediaMontageWindow::closeEvent(QCloseEvent *event)
{
	if (owner && owner->exporting()) {
		QMessageBox box(QMessageBox::Question, QStringLiteral("المونتاج"),
				QStringLiteral("التصدير لسى شغّال. يكمل وأنت مسكّر النافذة، أو توقفه؟"), QMessageBox::NoButton, this);
		QPushButton *keep = box.addButton(QStringLiteral("خلّه يكمل"), QMessageBox::AcceptRole);
		QPushButton *stop = box.addButton(QStringLiteral("أوقف التصدير"), QMessageBox::DestructiveRole);
		box.addButton(QStringLiteral("رجوع"), QMessageBox::RejectRole);
		box.exec();
		if (box.clickedButton() == stop)
			owner->cancelExports();
		else if (box.clickedButton() != keep) {
			event->ignore();
			return;
		}
	}
#ifdef BROWSER_AVAILABLE
	if (page)
		page->closeBrowser();
#endif
	event->accept();
}

/* ── The controller ──────────────────────────────────────────────────────────────────────────── */

TrediaMontage::TrediaMontage(QObject *parent) : QObject(parent)
{
	/* TREDIA_URL points a developer's build at a local Tredia, as for the rest of the app. */
	const QString env = qEnvironmentVariable("TREDIA_URL");
	site = env.isEmpty() ? QStringLiteral("https://tredia.cloud") : env;
	while (site.endsWith(QLatin1Char('/')))
		site.chop(1);
	QByteArray raw(16, Qt::Uninitialized);
	for (char &c : raw)
		c = static_cast<char>(QRandomGenerator::system()->bounded(256));
	key = raw.toHex();
	connect(&server, &QTcpServer::newConnection, this, [this]() {
		while (QTcpSocket *s = server.nextPendingConnection())
			new TrediaBridgeConnection(s, this);
	});
	loadRegistry();

	/* Which ffmpeg ships with the program decides how the filter graph is passed (-/filter_complex
	 * from ffmpeg 7). */
	auto *p = new QProcess(this);
	connect(p, &QProcess::finished, this, [this, p](int, QProcess::ExitStatus) {
		static const QRegularExpression re(QStringLiteral("ffmpeg version n?(\\d+)\\."));
		const QRegularExpressionMatch m = re.match(QString::fromUtf8(p->readAllStandardOutput()));
		ffmpegMajor = m.hasMatch() ? m.captured(1).toInt() : 0;
		p->deleteLater();
	});
	connect(p, &QProcess::errorOccurred, p, [p](QProcess::ProcessError e) {
		if (e == QProcess::FailedToStart)
			p->deleteLater();
	});
	p->start(ffmpeg(), {QStringLiteral("-hide_banner"), QStringLiteral("-version")});

	/* PNGs of exports from earlier days aren't needed. */
	const QDateTime old = QDateTime::currentDateTime().addDays(-1);
	for (const QFileInfo &f : QDir(dir("montage/layers")).entryInfoList({QStringLiteral("*.png")}, QDir::Files))
		if (f.lastModified() < old)
			QFile::remove(f.absoluteFilePath());
}

TrediaMontage::~TrediaMontage()
{
	/* (The browser's QCef helper stays: OBS's own one isn't deleted either, and the browser may still
	 * be closing.) */
	shutdown();
}

void TrediaMontage::shutdown()
{
	cancelExports();
	if (window)
		window->close();
	server.close();
}

bool TrediaMontage::listen()
{
	return server.isListening() || server.listen(QHostAddress::LocalHost, 0);
}

QString TrediaMontage::pageUrl() const
{
	return QStringLiteral("http://127.0.0.1:%1/montage/desktop?k=%2&theme=%3")
		.arg(server.serverPort())
		.arg(QString::fromLatin1(key))
		.arg(obs_frontend_is_theme_dark() ? QStringLiteral("dark") : QStringLiteral("light"));
}

void TrediaMontage::open(const QString &file)
{
	if (!listen()) {
		QMessageBox::warning(nullptr, QStringLiteral("المونتاج"), QStringLiteral("ما قدرنا نشغّل المونتاج على هذا الجهاز."));
		return;
	}
	if (!file.isEmpty() && QFileInfo(file).isFile() && kindOf(file) == QStringLiteral("video")) {
		pendingOpen = idFor(file);
		startProbe(pendingOpen);
	}
#ifdef BROWSER_AVAILABLE
	if (!cef)
		cef = obs_browser_init_panel();
#endif
	if (!window) {
		window = new TrediaMontageWindow(this, cef);
		window->resize(1440, 900);
		window->show();
		window->load(pageUrl());
		return;
	}
	window->showNormal();
	window->raise();
	window->activateWindow();
	if (!file.isEmpty())
		window->load(pageUrl());
}

QString TrediaMontage::ffmpeg() const
{
	const QString shipped = QCoreApplication::applicationDirPath() + QStringLiteral("/tredia-ffmpeg.exe");
	return QFileInfo::exists(shipped) ? shipped : QStringLiteral("ffmpeg");
}

QString TrediaMontage::dir(const char *sub) const
{
	char *p = obs_module_config_path(sub);
	const QString path = QString::fromUtf8(p);
	bfree(p);
	QDir().mkpath(path);
	return path;
}

QString TrediaMontage::recordingFolder() const
{
	QString path;
	if (config_t *cfg = obs_frontend_get_profile_config()) {
		const char *mode = config_get_string(cfg, "Output", "Mode");
		if (mode && qstrcmp(mode, "Advanced") == 0) {
			const char *type = config_get_string(cfg, "AdvOut", "RecType");
			path = QString::fromUtf8(config_get_string(cfg, "AdvOut", type && qstrcmp(type, "FFmpeg") == 0 ? "FFFilePath" : "RecFilePath"));
		} else {
			path = QString::fromUtf8(config_get_string(cfg, "SimpleOutput", "FilePath"));
		}
	}
	if (path.isEmpty() || !QDir(path).exists())
		path = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
	return QDir::toNativeSeparators(path);
}

QStringList TrediaMontage::recordings() const
{
	QStringList filters;
	for (const QString &e : videoExt)
		filters << QStringLiteral("*.") + e;
	QStringList out;
	for (const QFileInfo &f : QDir(recordingFolder()).entryInfoList(filters, QDir::Files, QDir::Time)) {
		out << f.absoluteFilePath();
		if (out.size() >= 30)
			break;
	}
	char *last = obs_frontend_get_last_recording();
	const QString lastFile = QString::fromUtf8(last);
	bfree(last);
	if (!lastFile.isEmpty() && QFileInfo(lastFile).isFile() && !out.contains(QFileInfo(lastFile).absoluteFilePath()))
		out.prepend(QFileInfo(lastFile).absoluteFilePath());
	return out;
}

QString TrediaMontage::idFor(const QString &file)
{
	const QString abs = QFileInfo(file).absoluteFilePath();
	const QByteArray hash = QCryptographicHash::hash(abs.toLower().toUtf8(), QCryptographicHash::Sha1).toHex();
	const QString id = QStringLiteral("f") + QString::fromLatin1(hash.left(15));
	if (registry.value(id) != abs) {
		registry.insert(id, abs);
		saveRegistry();
	}
	return id;
}

void TrediaMontage::loadRegistry()
{
	QFile f(dir("montage") + QStringLiteral("/files.json"));
	if (!f.open(QIODevice::ReadOnly))
		return;
	const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
	for (auto it = o.begin(); it != o.end(); ++it)
		registry.insert(it.key(), it.value().toString());
}

void TrediaMontage::saveRegistry() const
{
	QJsonObject o;
	for (auto it = registry.begin(); it != registry.end(); ++it)
		if (QFileInfo::exists(it.value()))
			o.insert(it.key(), it.value());
	QSaveFile f(dir("montage") + QStringLiteral("/files.json"));
	if (f.open(QIODevice::WriteOnly)) {
		f.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
		f.commit();
	}
}

/* ── Looking at files: ffmpeg -i (length, picture size as shown, sound) ───────────────────────── */

void TrediaMontage::startProbe(const QString &id)
{
	const QString file = registry.value(id);
	const QFileInfo info(file);
	Probe &p = probes[id];
	const qint64 stamp = info.size() ^ info.lastModified().toMSecsSinceEpoch();
	if (p.stamp != stamp) {
		p.done = false;
		p.stamp = stamp;
	}
	if (p.done || p.running)
		return;
	if (!info.isFile() || kindOf(file) == QStringLiteral("image")) {
		p.done = true;
		return;
	}
	p.running = true;
	auto *proc = new QProcess(this);
	connect(proc, &QProcess::finished, this, [this, proc, id](int, QProcess::ExitStatus) {
		parseProbe(id, QString::fromUtf8(proc->readAllStandardError()));
		proc->deleteLater();
	});
	connect(proc, &QProcess::errorOccurred, this, [this, proc, id](QProcess::ProcessError e) {
		if (e != QProcess::FailedToStart)
			return;
		parseProbe(id, QString());
		proc->deleteLater();
	});
	proc->start(ffmpeg(), {QStringLiteral("-hide_banner"), QStringLiteral("-nostdin"), QStringLiteral("-i"), file});
}

void TrediaMontage::parseProbe(const QString &id, const QString &text)
{
	Probe &p = probes[id];
	p.running = false;
	p.done = true;
	p.video = false;
	p.audio = false;
	p.width = 0;
	p.height = 0;
	p.duration = 0;
	static const QRegularExpression durRe(QStringLiteral("Duration: (\\d+):(\\d+):(\\d+(?:\\.\\d+)?)"));
	static const QRegularExpression sizeRe(QStringLiteral(", (\\d{2,5})x(\\d{2,5})[ ,\\[]"));
	static const QRegularExpression sarRe(QStringLiteral("\\[SAR (\\d+):(\\d+) DAR"));
	static const QRegularExpression rotRe(QStringLiteral("rotation of (-?\\d+(?:\\.\\d+)?) degrees"));
	static const QRegularExpression rot2Re(QStringLiteral("rotate\\s*:\\s*(-?\\d+)"));
	static const QRegularExpression audioRe(QStringLiteral("Stream #\\d+:\\d+.*: Audio:"));
	const QRegularExpressionMatch d = durRe.match(text);
	if (d.hasMatch())
		p.duration = d.captured(1).toDouble() * 3600 + d.captured(2).toDouble() * 60 + d.captured(3).toDouble();
	for (const QString &raw : text.split(QLatin1Char('\n'))) {
		const QString line = raw.trimmed(); /* (Windows ends lines with \r\n) */
		if (!line.contains(QStringLiteral("Stream #")) || !line.contains(QStringLiteral(": Video:")) || line.contains(QStringLiteral("attached pic")))
			continue;
		const QRegularExpressionMatch s = sizeRe.match(line + QLatin1Char(' '));
		if (s.hasMatch()) {
			p.video = true;
			p.width = s.captured(1).toInt();
			p.height = s.captured(2).toInt();
			const QRegularExpressionMatch sar = sarRe.match(line);
			if (sar.hasMatch() && sar.captured(1).toInt() > 0 && sar.captured(2).toInt() > 0)
				p.width = qRound(double(p.width) * sar.captured(1).toInt() / sar.captured(2).toInt());
		}
		break;
	}
	QRegularExpressionMatch r = rotRe.match(text);
	if (!r.hasMatch())
		r = rot2Re.match(text);
	if (r.hasMatch() && qAbs(qRound(r.captured(1).toDouble())) % 180 == 90)
		std::swap(p.width, p.height);
	p.audio = audioRe.match(text).hasMatch();
	const bool video = p.video;
	const QList<std::function<void()>> waiting = p.waiting;
	p.waiting.clear();
	/* (p may move once the callbacks look at other files: nothing below touches it.) */
	for (const auto &fn : waiting)
		fn();
	/* The player can't open every container: MKV, FLV and TS recordings get an MP4 copy. */
	if (video && remuxExt.contains(QFileInfo(registry.value(id)).suffix().toLower()))
		startRemux(id);
}

void TrediaMontage::whenProbed(const QStringList &ids, std::function<void()> done, int timeoutMs)
{
	auto left = std::make_shared<int>(0);
	auto fired = std::make_shared<bool>(false);
	auto fire = [done, fired]() {
		if (*fired)
			return;
		*fired = true;
		done();
	};
	for (const QString &id : ids) {
		startProbe(id);
		Probe &p = probes[id];
		if (p.done)
			continue;
		++*left;
		p.waiting << [left, fire]() {
			if (--*left == 0)
				fire();
		};
	}
	if (*left == 0) {
		fire();
		return;
	}
	QTimer::singleShot(timeoutMs, this, fire);
}

void TrediaMontage::startRemux(const QString &id)
{
	const QString src = registry.value(id);
	const QString out = dir("montage/cache") + QStringLiteral("/") + id + QStringLiteral(".mp4");
	if (QFileInfo(out).exists() && QFileInfo(out).lastModified() >= QFileInfo(src).lastModified()) {
		remuxed.insert(id, out);
		return;
	}
	if (remuxing.contains(id))
		return;
	remuxing << id;
	auto *proc = new QProcess(this);
	connect(proc, &QProcess::finished, this, [this, proc, id, out](int code, QProcess::ExitStatus) {
		remuxing.removeAll(id);
		if (code == 0)
			remuxed.insert(id, out);
		else
			QFile::remove(out);
		proc->deleteLater();
	});
	proc->start(ffmpeg(), {QStringLiteral("-hide_banner"), QStringLiteral("-nostdin"), QStringLiteral("-y"), QStringLiteral("-loglevel"),
			       QStringLiteral("error"), QStringLiteral("-i"), src, QStringLiteral("-map"), QStringLiteral("0:v:0"), QStringLiteral("-map"),
			       QStringLiteral("0:a:0?"), QStringLiteral("-c"), QStringLiteral("copy"), QStringLiteral("-movflags"),
			       QStringLiteral("+faststart"), out});
}

QJsonObject TrediaMontage::mediaJson(const QString &id) const
{
	const QString file = registry.value(id);
	const QFileInfo info(file);
	QJsonObject o{{"id", id}, {"kind", kindOf(file)}, {"name", info.fileName()}, {"size", double(info.size())},
		      {"mtime", double(info.lastModified().toMSecsSinceEpoch())}};
	const Probe p = probes.value(id);
	if (p.done && p.duration > 0)
		o.insert("duration", p.duration);
	if (p.done && p.video) {
		o.insert("width", p.width);
		o.insert("height", p.height);
	}
	if (p.done && kindOf(file) != QStringLiteral("image"))
		o.insert("audio", p.audio);
	if (remuxing.contains(id))
		o.insert("pending", QStringLiteral("نجهّزه للمعاينة… لحظات"));
	return o;
}

/* ── Requests ────────────────────────────────────────────────────────────────────────────────── */

void TrediaMontage::handle(TrediaBridgeConnection *c, const Request &r)
{
	const QByteArray host = "127.0.0.1:" + QByteArray::number(server.serverPort());
	/* Only this program's own address: a web page that renames itself to 127.0.0.1 (DNS rebinding)
	 * still can't talk to the bridge. */
	if (r.headers.value("host") != host) {
		c->error(403, message("غير مسموح."));
		return;
	}
	if (!r.path.startsWith(QStringLiteral("/bridge/"))) {
		proxy(c, r);
		return;
	}
	const QByteArray given = r.params.value(QStringLiteral("k")).toLatin1();
	int diff = given.size() == key.size() ? 0 : 1;
	for (qsizetype i = 0; i < key.size() && i < given.size(); i++)
		diff |= given[i] ^ key[i];
	if (diff) {
		c->error(403, message("مفتاح غير صحيح. افتح المونتاج من تريديا ستوديو."));
		return;
	}
	if (r.method != "GET") {
		const QByteArray origin = r.headers.value("origin");
		if (!origin.isEmpty() && origin != "http://" + host) {
			c->error(403, message("غير مسموح."));
			return;
		}
	}
	const QStringList parts = r.path.mid(8).split(QLatin1Char('/'), Qt::SkipEmptyParts);
	const QString what = parts.value(0);
	const QString arg = parts.value(1);
	const bool get = r.method == "GET" || r.method == "HEAD";
	const bool post = r.method == "POST";
	if (what == QStringLiteral("state") && get)
		state(c);
	else if (what == QStringLiteral("probe") && get)
		probeOne(c, arg);
	else if (what == QStringLiteral("pick") && post)
		pick(c, r.params.value(QStringLiteral("kind")));
	else if (what == QStringLiteral("media") && get)
		media(c, arg, r.headers.value("range"));
	else if (what == QStringLiteral("projects"))
		project(c, r.method, arg, r.body);
	else if (what == QStringLiteral("layer") && post)
		layer(c, r.body);
	else if (what == QStringLiteral("export") && post)
		startExport(c, r.body);
	else if (what == QStringLiteral("job") && get)
		jobState(c, arg);
	else if (what == QStringLiteral("cancel") && post)
		cancel(c, arg);
	else if (what == QStringLiteral("reveal") && post)
		reveal(c, arg);
	else
		c->error(404, message("غير موجود."));
}

/* The editor page and its scripts, from the site (only what the page needs; the key stays here). */
void TrediaMontage::proxy(TrediaBridgeConnection *c, const Request &r)
{
	if (r.method != "GET" && r.method != "HEAD") {
		c->error(405, message("غير مسموح."));
		return;
	}
	static const QStringList prefixes = {"/montage/desktop", "/_next/", "/brand/", "/favicon.ico", "/icon.png", "/apple-icon.png", "/fonts/"};
	bool allowed = false;
	for (const QString &p : prefixes)
		allowed = allowed || r.path.startsWith(p);
	if (!allowed) {
		c->error(404, message("غير موجود."));
		return;
	}
	const bool immutable = r.path.startsWith(QStringLiteral("/_next/static/"));
	if (immutable && staticCache.contains(r.path)) {
		const auto hit = staticCache.value(r.path);
		c->reply(200, hit.first, hit.second);
		return;
	}
	QUrlQuery q(r.query);
	q.removeAllQueryItems(QStringLiteral("k"));
	QUrl url(site + r.path);
	url.setQuery(q);
	QNetworkRequest req(url);
	req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("TrediaStudio/1.0 (montage)"));
	req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
	for (const QByteArray h : {QByteArray("accept"), QByteArray("rsc"), QByteArray("next-router-state-tree"), QByteArray("next-router-prefetch"), QByteArray("next-url")})
		if (r.headers.contains(h))
			req.setRawHeader(h, r.headers.value(h));
	QNetworkReply *reply = net.get(req);
	QPointer<TrediaBridgeConnection> conn(c);
	const QString path = r.path;
	connect(reply, &QNetworkReply::finished, this, [this, reply, conn, path, immutable]() {
		reply->deleteLater();
		if (!conn || !conn->alive())
			return;
		const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		if (status == 0) {
			conn->reply(502, "text/html; charset=utf-8",
				    QStringLiteral("<!doctype html><html lang=\"ar\" dir=\"rtl\"><meta charset=\"utf-8\"><body style=\"font-family:Segoe UI,Tahoma,sans-serif;"
						   "background:#100822;color:#f5f1ff;display:grid;place-items:center;height:100vh;margin:0\"><p>ما قدرنا نفتح "
						   "المونتاج. تأكد إن الإنترنت شغال، وسكّر النافذة وافتحها من جديد.</p></body></html>")
					    .toUtf8());
			return;
		}
		const QByteArray body = reply->readAll();
		QByteArray type = reply->header(QNetworkRequest::ContentTypeHeader).toString().toUtf8();
		if (type.isEmpty())
			type = "application/octet-stream";
		if (immutable && status == 200 && staticCacheBytes + body.size() < 96 * 1024 * 1024) {
			staticCache.insert(path, {type, body});
			staticCacheBytes += body.size();
		}
		conn->reply(status, type, body);
	});
}

void TrediaMontage::state(TrediaBridgeConnection *c)
{
	QStringList ids;
	for (const QString &f : recordings())
		ids << idFor(f);
	QPointer<TrediaBridgeConnection> conn(c);
	whenProbed(
		ids,
		[this, conn, ids]() {
			if (!conn || !conn->alive())
				return;
			QJsonArray recs;
			for (const QString &id : ids) {
				const Probe p = probes.value(id);
				if (p.done && !p.video)
					continue; /* not a video after all */
				recs.append(mediaJson(id));
			}
			QJsonObject o{{"version", 1}, {"folder", recordingFolder()}, {"recordings", recs}, {"projects", projects()}};
			o.insert("open", pendingOpen.isEmpty() ? QJsonValue() : QJsonValue(pendingOpen));
			pendingOpen.clear();
			conn->json(200, o);
		},
		8000);
}

void TrediaMontage::probeOne(TrediaBridgeConnection *c, const QString &id)
{
	if (!registry.contains(id) || !QFileInfo::exists(registry.value(id))) {
		c->error(404, message("الملف غير موجود على جهازك."));
		return;
	}
	QPointer<TrediaBridgeConnection> conn(c);
	whenProbed(
		{id},
		[this, conn, id]() {
			if (conn && conn->alive())
				conn->json(200, mediaJson(id));
		},
		15000);
}

void TrediaMontage::pick(TrediaBridgeConnection *c, const QString &kind)
{
	QString filter;
	QString start;
	if (kind == QStringLiteral("video")) {
		filter = QStringLiteral("فيديو (*.mp4 *.mov *.mkv *.m4v *.webm *.flv *.ts)");
		start = recordingFolder();
	} else if (kind == QStringLiteral("image")) {
		filter = QStringLiteral("صور (*.png *.jpg *.jpeg *.webp *.gif *.bmp)");
		start = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
	} else if (kind == QStringLiteral("audio")) {
		filter = QStringLiteral("صوت (*.mp3 *.m4a *.aac *.wav *.ogg *.opus *.flac)");
		start = QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
	} else {
		c->error(400, message("نوع غير معروف."));
		return;
	}
	QPointer<TrediaBridgeConnection> conn(c);
	const QStringList files = QFileDialog::getOpenFileNames(window, QStringLiteral("اختر الملفات"), start, filter);
	QStringList ids;
	for (const QString &f : files)
		if (kindOf(f) == kind)
			ids << idFor(f);
	if (!conn)
		return;
	whenProbed(
		ids,
		[this, conn, ids]() {
			if (!conn || !conn->alive())
				return;
			QJsonArray media;
			for (const QString &id : ids)
				media.append(mediaJson(id));
			conn->json(200, QJsonObject{{"media", media}});
		},
		15000);
}

void TrediaMontage::media(TrediaBridgeConnection *c, const QString &id, const QByteArray &range)
{
	QString file = registry.value(id);
	if (file.isEmpty() || !QFileInfo(file).isFile()) {
		c->error(404, message("الملف غير موجود على جهازك."));
		return;
	}
	if (remuxed.contains(id) && QFileInfo::exists(remuxed.value(id)))
		file = remuxed.value(id);
	c->sendFile(file, typeOf(file), range);
}

void TrediaMontage::project(TrediaBridgeConnection *c, const QByteArray &method, const QString &id, const QByteArray &body)
{
	static const QRegularExpression ok(QStringLiteral("^[a-z0-9]{4,40}$"));
	if (!ok.match(id).hasMatch()) {
		c->error(400, message("مشروع غير صحيح."));
		return;
	}
	const QString path = dir("montage/projects") + QStringLiteral("/") + id + QStringLiteral(".json");
	if (method == "GET") {
		QFile f(path);
		if (!f.open(QIODevice::ReadOnly)) {
			c->error(404, message("المشروع غير موجود."));
			return;
		}
		c->reply(200, "application/json; charset=utf-8", f.readAll());
	} else if (method == "PUT") {
		if (body.size() > 4 * 1024 * 1024 || !QJsonDocument::fromJson(body).isObject()) {
			c->error(400, message("ما قدرنا نحفظ المشروع."));
			return;
		}
		QSaveFile f(path);
		if (!f.open(QIODevice::WriteOnly) || f.write(body) != body.size() || !f.commit()) {
			c->error(500, message("ما قدرنا نحفظ المشروع على جهازك."));
			return;
		}
		c->json(200, QJsonObject{{"ok", true}});
	} else if (method == "DELETE") {
		QFile::remove(path);
		c->json(200, QJsonObject{{"ok", true}});
	} else {
		c->error(405, message("غير مسموح."));
	}
}

QJsonArray TrediaMontage::projects() const
{
	QJsonArray out;
	for (const QFileInfo &f : QDir(dir("montage/projects")).entryInfoList({QStringLiteral("*.json")}, QDir::Files, QDir::Time)) {
		QFile file(f.absoluteFilePath());
		if (!file.open(QIODevice::ReadOnly))
			continue;
		const QString title = QJsonDocument::fromJson(file.readAll()).object().value(QStringLiteral("title")).toString();
		out.append(QJsonObject{{"id", f.completeBaseName()},
				       {"title", title.isEmpty() ? QStringLiteral("مشروع") : title},
				       {"updated", double(f.lastModified().toMSecsSinceEpoch())}});
		if (out.size() >= 60)
			break;
	}
	return out;
}

void TrediaMontage::layer(TrediaBridgeConnection *c, const QByteArray &body)
{
	if (body.size() < 8 || body.size() > 16 * 1024 * 1024 || !body.startsWith("\x89PNG")) {
		c->error(400, message("صورة غير صحيحة."));
		return;
	}
	QByteArray raw(16, Qt::Uninitialized);
	for (char &ch : raw)
		ch = static_cast<char>(QRandomGenerator::global()->bounded(256));
	const QString id = QString::fromLatin1(raw.toHex());
	QSaveFile f(dir("montage/layers") + QStringLiteral("/") + id + QStringLiteral(".png"));
	if (!f.open(QIODevice::WriteOnly) || f.write(body) != body.size() || !f.commit()) {
		c->error(500, message("ما قدرنا نحفظ الصورة على جهازك."));
		return;
	}
	c->json(200, QJsonObject{{"id", id}});
}

/* ── Exports ─────────────────────────────────────────────────────────────────────────────────── */

bool TrediaMontage::exporting() const
{
	for (const Job *j : jobs)
		if (j->status == QStringLiteral("rendering"))
			return true;
	return false;
}

void TrediaMontage::cancelExports()
{
	for (Job *j : jobs) {
		if (j->status != QStringLiteral("rendering"))
			continue;
		j->status = QStringLiteral("canceled");
		if (j->process)
			j->process->kill();
	}
}

void TrediaMontage::startExport(TrediaBridgeConnection *c, const QByteArray &body)
{
	if (obs_frontend_streaming_active()) {
		c->error(409, message("وقّف البث قبل التصدير، عشان ما يتأثر بثك."));
		return;
	}
	if (exporting()) {
		c->error(409, message("فيه تصدير شغّال الحين. انتظره يخلص."));
		return;
	}
	const QJsonObject root = QJsonDocument::fromJson(body).object();
	const QJsonObject plan = root.value(QStringLiteral("plan")).toObject();
	const QString graph = plan.value(QStringLiteral("graph")).toString();
	const QJsonArray inputs = plan.value(QStringLiteral("inputs")).toArray();
	const int width = plan.value(QStringLiteral("width")).toInt();
	const int height = plan.value(QStringLiteral("height")).toInt();
	const int fps = plan.value(QStringLiteral("fps")).toInt();
	const double seconds = plan.value(QStringLiteral("duration")).toDouble();
	if (graph.isEmpty() || graph.size() > 400000 || inputs.isEmpty() || inputs.size() > 300 || width < 16 || width > 8192 || height < 16 ||
	    height > 8192 || fps < 1 || fps > 120 || !(seconds > 0) || seconds > 6 * 3600 || !filtersAllowed(graph)) {
		c->error(400, message("خطة التصدير غير صحيحة."));
		return;
	}

	static const QRegularExpression layerId(QStringLiteral("^[a-f0-9]{32}$"));
	const QString layers = dir("montage/layers");
	auto *job = new Job;
	for (const QJsonValue &v : inputs) {
		const QJsonObject i = v.toObject();
		const QString src = i.value(QStringLiteral("src")).toString();
		const QString kind = i.value(QStringLiteral("kind")).toString();
		const double t = i.value(QStringLiteral("t")).toDouble();
		const double ss = i.value(QStringLiteral("ss")).toDouble(0);
		QString file;
		if (layerId.match(src).hasMatch()) {
			file = layers + QStringLiteral("/") + src + QStringLiteral(".png");
			job->layerFiles << file;
		} else {
			file = registry.value(src);
		}
		if (file.isEmpty() || !QFileInfo(file).isFile() || !(t > 0) || t > 6 * 3600 || ss < 0 || ss > 48 * 3600 ||
		    (kind != QStringLiteral("video") && kind != QStringLiteral("still") && kind != QStringLiteral("audio"))) {
			delete job;
			c->error(400, message("ملف في المشروع ما عاد موجود على جهازك. احذفه من المشروع."));
			return;
		}
		if (kind == QStringLiteral("still"))
			job->inputArgs << QStringLiteral("-loop") << QStringLiteral("1") << QStringLiteral("-framerate") << QString::number(fps);
		else
			job->inputArgs << QStringLiteral("-ss") << QString::number(ss, 'f', 3);
		job->inputArgs << QStringLiteral("-t") << QString::number(t, 'f', 3) << QStringLiteral("-i") << QDir::toNativeSeparators(file);
	}

	QByteArray raw(8, Qt::Uninitialized);
	for (char &ch : raw)
		ch = static_cast<char>(QRandomGenerator::global()->bounded(256));
	job->id = QString::fromLatin1(raw.toHex());
	job->graphFile = dir("montage/jobs") + QStringLiteral("/") + job->id + QStringLiteral(".txt");
	QSaveFile g(job->graphFile);
	if (!g.open(QIODevice::WriteOnly) || g.write(graph.toUtf8()) < 0 || !g.commit()) {
		delete job;
		c->error(500, message("ما قدرنا نجهّز التصدير على جهازك."));
		return;
	}
	const QString folder = recordingFolder();
	QDir().mkpath(folder);
	job->output = QDir::toNativeSeparators(folder + QStringLiteral("/") + safeTitle(root.value(QStringLiteral("title")).toString()) + QStringLiteral(" ") +
					      QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd hh-mm-ss")) + QStringLiteral(".mp4"));
	job->seconds = seconds;
	job->fps = fps;
	jobs.insert(job->id, job);
	run(job);
	c->json(200, QJsonObject{{"job", job->id}});
}

void TrediaMontage::run(Job *job)
{
	QStringList args = {QStringLiteral("-hide_banner"), QStringLiteral("-nostdin"), QStringLiteral("-y"), QStringLiteral("-loglevel"), QStringLiteral("error"),
			    QStringLiteral("-progress"), QStringLiteral("pipe:1"), QStringLiteral("-nostats")};
	args << job->inputArgs;
	/* ffmpeg 7 reads an option's value from a file with -/; older ones had -filter_complex_script. */
	args << (ffmpegMajor >= 7 || ffmpegMajor == 0 ? QStringLiteral("-/filter_complex") : QStringLiteral("-filter_complex_script")) << job->graphFile;
	args << QStringLiteral("-map") << QStringLiteral("[vout]") << QStringLiteral("-map") << QStringLiteral("[aout]");
	if (job->nvenc)
		args << QStringLiteral("-c:v") << QStringLiteral("h264_nvenc") << QStringLiteral("-preset") << QStringLiteral("p5") << QStringLiteral("-tune")
		     << QStringLiteral("hq") << QStringLiteral("-rc") << QStringLiteral("vbr") << QStringLiteral("-cq") << QStringLiteral("20")
		     << QStringLiteral("-b:v") << QStringLiteral("0") << QStringLiteral("-profile:v") << QStringLiteral("high");
	else
		args << QStringLiteral("-c:v") << QStringLiteral("libx264") << QStringLiteral("-preset") << QStringLiteral("veryfast") << QStringLiteral("-crf")
		     << QStringLiteral("20");
	args << QStringLiteral("-pix_fmt") << QStringLiteral("yuv420p") << QStringLiteral("-r") << QString::number(job->fps) << QStringLiteral("-c:a")
	     << QStringLiteral("aac") << QStringLiteral("-b:a") << QStringLiteral("192k") << QStringLiteral("-ar") << QStringLiteral("48000")
	     << QStringLiteral("-t") << QString::number(job->seconds, 'f', 3) << QStringLiteral("-movflags") << QStringLiteral("+faststart") << job->output;

	auto *proc = new QProcess(this);
	job->process = proc;
	job->stderrTail.clear();
	connect(proc, &QProcess::readyReadStandardOutput, this, [proc, job]() {
		static const QRegularExpression re(QStringLiteral("out_time_(?:us|ms)=(\\d+)"));
		auto it = re.globalMatch(QString::fromUtf8(proc->readAllStandardOutput()));
		while (it.hasNext())
			job->progress = qMin(0.99, it.next().captured(1).toDouble() / 1e6 / job->seconds);
	});
	connect(proc, &QProcess::readyReadStandardError, this, [proc, job]() { job->stderrTail = (job->stderrTail + QString::fromUtf8(proc->readAllStandardError())).right(3000); });
	connect(proc, &QProcess::started, this, [proc]() {
#ifdef _WIN32
		/* Below normal: a game or a stream on this PC keeps the CPU first. */
		if (HANDLE h = OpenProcess(PROCESS_SET_INFORMATION, FALSE, static_cast<DWORD>(proc->processId()))) {
			SetPriorityClass(h, BELOW_NORMAL_PRIORITY_CLASS);
			CloseHandle(h);
		}
#else
		(void)proc;
#endif
	});
	connect(proc, &QProcess::finished, this, [this, job, proc](int code, QProcess::ExitStatus) {
		proc->deleteLater();
		finish(job, code);
	});
	connect(proc, &QProcess::errorOccurred, this, [this, job, proc](QProcess::ProcessError e) {
		if (e != QProcess::FailedToStart)
			return;
		proc->deleteLater();
		job->stderrTail = message("أداة التصدير ناقصة. ثبّت تريديا ستوديو من جديد.");
		job->nvenc = false;
		finish(job, -1);
	});
	proc->start(ffmpeg(), args);
}

void TrediaMontage::finish(Job *job, int exitCode)
{
	if (job->status == QStringLiteral("canceled")) {
		QFile::remove(job->output);
		return;
	}
	if (exitCode != 0 && job->nvenc) {
		/* No NVIDIA card (or its encoder is busy): the same export on the CPU. */
		job->nvenc = false;
		QFile::remove(job->output);
		job->progress = 0;
		run(job);
		return;
	}
	QFile::remove(job->graphFile);
	for (const QString &f : job->layerFiles)
		QFile::remove(f);
	if (exitCode == 0 && QFileInfo(job->output).size() > 0) {
		job->status = QStringLiteral("done");
		job->progress = 1;
		if (!window) {
			/* The window was closed while it rendered: say where the video went. */
			auto *box = new QMessageBox(QMessageBox::Information, QStringLiteral("المونتاج"),
						    QStringLiteral("انصدّر الفيديو:\n%1").arg(job->output), QMessageBox::Ok,
						    static_cast<QWidget *>(obs_frontend_get_main_window()));
			box->setAttribute(Qt::WA_DeleteOnClose);
			box->open();
		}
		return;
	}
	QFile::remove(job->output);
	job->status = QStringLiteral("failed");
	const QString last = job->stderrTail.trimmed().section(QLatin1Char('\n'), -1).trimmed();
	job->error = QStringLiteral("تعذّر التصدير") + (last.isEmpty() ? QString() : QStringLiteral(": ") + last.left(300));
}

void TrediaMontage::jobState(TrediaBridgeConnection *c, const QString &id)
{
	const Job *j = jobs.value(id);
	if (!j) {
		c->error(404, message("التصدير غير موجود."));
		return;
	}
	QJsonObject o{{"status", j->status}, {"progress", j->progress}, {"output", j->output}};
	if (!j->error.isEmpty())
		o.insert("error", j->error);
	c->json(200, o);
}

void TrediaMontage::cancel(TrediaBridgeConnection *c, const QString &id)
{
	if (Job *j = jobs.value(id); j && j->status == QStringLiteral("rendering")) {
		j->status = QStringLiteral("canceled");
		if (j->process)
			j->process->kill();
	}
	c->json(200, QJsonObject{{"ok", true}});
}

void TrediaMontage::reveal(TrediaBridgeConnection *c, const QString &id)
{
	const Job *j = jobs.value(id);
	if (!j || !QFileInfo::exists(j->output)) {
		c->error(404, message("الملف غير موجود."));
		return;
	}
#ifdef _WIN32
	(void)QProcess::startDetached(QStringLiteral("explorer.exe"), {QStringLiteral("/select,"), QDir::toNativeSeparators(j->output)});
#endif
	c->json(200, QJsonObject{{"ok", true}});
}
