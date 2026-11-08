#include "tredia-api.hpp"
#include "secure-store.hpp"

#include <obs-module.h>
#include <util/bmem.h>
#include <util/platform.h>

#include <QFile>
#include <QFileInfo>
#include <QHostInfo>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QUrl>

#include <memory>

namespace {

constexpr qint64 CHUNK_BYTES = 8 * 1024 * 1024; /* matches the server's UPLOAD_CHUNK_BYTES */

QString accountFile()
{
	char *path = obs_module_config_path("account.json");
	QString result = QString::fromUtf8(path);
	bfree(path);
	return result;
}

} // namespace

TrediaApi::TrediaApi(QObject *parent) : QObject(parent)
{
	/* TREDIA_URL lets a developer point the app at a local Tredia; everyone else uses tredia.cloud. */
	QString env = qEnvironmentVariable("TREDIA_URL");
	base = env.isEmpty() ? QStringLiteral("https://tredia.cloud") : env;
	while (base.endsWith('/'))
		base.chop(1);

	pollTimer.setSingleShot(false);
	connect(&pollTimer, &QTimer::timeout, this, &TrediaApi::poll);
	linkDeadline.setSingleShot(true);
	connect(&linkDeadline, &QTimer::timeout, this, [this]() {
		cancelLink();
		emit linkFailed(QStringLiteral("انتهت مهلة الرمز. اضغط «سجّل دخولك» من جديد."));
	});
	loadAccount();
}

QNetworkRequest TrediaApi::request(const QString &path) const
{
	QNetworkRequest req(QUrl(base + path));
	req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
	req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("TrediaStudio/1.0"));
	req.setTransferTimeout(30000);
	if (!token.isEmpty())
		req.setRawHeader("Authorization", "Bearer " + token.toUtf8());
	return req;
}

void TrediaApi::handle(QNetworkReply *reply, Done done)
{
	connect(reply, &QNetworkReply::finished, this, [this, reply, done]() {
		reply->deleteLater();
		const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		const QJsonObject data = QJsonDocument::fromJson(reply->readAll()).object();
		QString error;
		if (status == 0)
			error = QStringLiteral("تعذّر الاتصال بتريديا. تأكد من الإنترنت.");
		else if (status >= 400)
			error = data.value(QStringLiteral("error")).toString(QStringLiteral("صار خطأ، حاول مرة ثانية."));
		if (status == 401 && signedIn())
			signOut(); /* the sign-in was revoked from the website */
		if (done)
			done(data, status, error);
	});
}

void TrediaApi::get(const QString &path, Done done)
{
	handle(net.get(request(path)), std::move(done));
}

void TrediaApi::post(const QString &path, const QJsonObject &body, Done done, int timeoutMs)
{
	QNetworkRequest req = request(path);
	req.setTransferTimeout(timeoutMs);
	handle(net.post(req, QJsonDocument(body).toJson(QJsonDocument::Compact)), std::move(done));
}

/* ---- Device sign-in ------------------------------------------------------------------------- */

void TrediaApi::startLink()
{
	cancelLink();
	QJsonObject body;
	body.insert(QStringLiteral("deviceName"), QHostInfo::localHostName().left(60));
	post(QStringLiteral("/api/app/link"), body, [this](const QJsonObject &data, int, const QString &error) {
		if (!error.isEmpty()) {
			emit linkFailed(error);
			return;
		}
		deviceCode = data.value(QStringLiteral("deviceCode")).toString();
		const int interval = qMax(2, data.value(QStringLiteral("interval")).toInt(3));
		const int expiresIn = qMax(60, data.value(QStringLiteral("expiresIn")).toInt(600));
		pollTimer.start(interval * 1000);
		linkDeadline.start(expiresIn * 1000);
		emit linkCode(data.value(QStringLiteral("userCode")).toString(), data.value(QStringLiteral("verifyUrl")).toString());
	});
}

void TrediaApi::cancelLink()
{
	pollTimer.stop();
	linkDeadline.stop();
	deviceCode.clear();
}

void TrediaApi::poll()
{
	if (deviceCode.isEmpty())
		return;
	QJsonObject body;
	body.insert(QStringLiteral("deviceCode"), deviceCode);
	post(QStringLiteral("/api/app/link/poll"), body, [this](const QJsonObject &data, int, const QString &) {
		const QString status = data.value(QStringLiteral("status")).toString();
		if (status == QStringLiteral("approved")) {
			cancelLink();
			token = data.value(QStringLiteral("token")).toString();
			name = data.value(QStringLiteral("name")).toString();
			saveAccount();
			emit signedInChanged();
		} else if (status == QStringLiteral("denied")) {
			cancelLink();
			emit linkFailed(QStringLiteral("رُفض تسجيل الدخول من الموقع."));
		} else if (status == QStringLiteral("expired")) {
			cancelLink();
			emit linkFailed(QStringLiteral("انتهت مهلة الرمز. اضغط «سجّل دخولك» من جديد."));
		}
	});
}

void TrediaApi::signOut()
{
	cancelLink();
	token.clear();
	name.clear();
	QFile::remove(accountFile());
	emit signedInChanged();
}

void TrediaApi::loadAccount()
{
	QFile file(accountFile());
	if (!file.open(QIODevice::ReadOnly))
		return;
	const QJsonObject data = QJsonDocument::fromJson(file.readAll()).object();
	token = QString::fromUtf8(unprotectSecret(data.value(QStringLiteral("token")).toString().toUtf8()));
	name = data.value(QStringLiteral("name")).toString();
}

void TrediaApi::saveAccount()
{
	char *dir = obs_module_config_path("");
	os_mkdirs(dir);
	bfree(dir);
	QJsonObject data;
	data.insert(QStringLiteral("name"), name);
	data.insert(QStringLiteral("token"), QString::fromUtf8(protectSecret(token.toUtf8())));
	QSaveFile file(accountFile());
	if (file.open(QIODevice::WriteOnly)) {
		file.write(QJsonDocument(data).toJson());
		file.commit();
	}
}

/* ---- Uploading a recording ------------------------------------------------------------------ */

struct UploadState {
	std::unique_ptr<QFile> file;
	QString uploadId;
	QString fileName;
	QString title;
	qint64 total = 0;
	qint64 offset = 0;
	int retries = 0;
	std::function<void(qint64, qint64)> progress;
	std::function<void(const QString &, const QString &)> done;
};

void TrediaApi::uploadRecording(const QString &filePath, const QString &title, std::function<void(qint64, qint64)> progress,
				std::function<void(const QString &, const QString &)> done)
{
	if (uploadActive) {
		done(QString(), QStringLiteral("فيه رفع شغال، انتظره يخلص."));
		return;
	}
	auto state = std::make_shared<UploadState>();
	state->file = std::make_unique<QFile>(filePath);
	if (!state->file->open(QIODevice::ReadOnly)) {
		done(QString(), QStringLiteral("ما قدرنا نفتح ملف التسجيل."));
		return;
	}
	state->fileName = QFileInfo(filePath).fileName();
	state->title = title;
	state->total = state->file->size();
	state->progress = std::move(progress);
	state->done = std::move(done);
	uploadActive = true;

	auto finish = [this, state](const QString &url, const QString &error) {
		uploadActive = false;
		state->file->close();
		state->done(url, error);
	};

	auto sendNext = std::make_shared<std::function<void()>>();
	*sendNext = [this, state, sendNext, finish]() {
		state->file->seek(state->offset);
		const QByteArray chunk = state->file->read(qMin(CHUNK_BYTES, state->total - state->offset));
		QNetworkRequest req = request(QStringLiteral("/api/app/uploads"));
		req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/octet-stream"));
		req.setTransferTimeout(120000);
		req.setRawHeader("x-offset", QByteArray::number(state->offset));
		req.setRawHeader("x-total", QByteArray::number(state->total));
		req.setRawHeader("x-file-name", QUrl::toPercentEncoding(state->fileName));
		if (!state->uploadId.isEmpty())
			req.setRawHeader("x-upload-id", state->uploadId.toUtf8());

		handle(net.post(req, chunk), [this, state, sendNext, finish](const QJsonObject &data, int status, const QString &error) {
			if (status == 409) { /* the server has a different amount: resume from there */
				state->offset = static_cast<qint64>(data.value(QStringLiteral("received")).toDouble());
				(*sendNext)();
				return;
			}
			if (!error.isEmpty()) {
				if (status == 0 && state->retries < 5) { /* network hiccup: try this piece again */
					state->retries += 1;
					QTimer::singleShot(3000 * state->retries, this, [sendNext]() { (*sendNext)(); });
					return;
				}
				finish(QString(), error);
				return;
			}
			state->retries = 0;
			state->uploadId = data.value(QStringLiteral("id")).toString();
			state->offset = static_cast<qint64>(data.value(QStringLiteral("received")).toDouble());
			if (state->progress)
				state->progress(state->offset, state->total);

			const QString key = data.value(QStringLiteral("key")).toString();
			if (key.isEmpty()) {
				(*sendNext)();
				return;
			}
			/* All there: make it a draft post in the library. */
			QJsonObject body;
			body.insert(QStringLiteral("key"), key);
			body.insert(QStringLiteral("title"), state->title);
			body.insert(QStringLiteral("name"), state->fileName);
			post(QStringLiteral("/api/app/recordings"), body, [finish](const QJsonObject &result, int, const QString &err) {
				finish(result.value(QStringLiteral("url")).toString(), err);
			});
		});
	};
	(*sendNext)();
}
