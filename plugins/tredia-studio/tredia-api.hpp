#pragma once

#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QTimer>

#include <functional>

/* Talks to tredia.cloud: device sign-in, the account, live settings, uploads. Everything runs on Qt's
 * event loop (no threads); answers come back as callbacks on the UI thread. */
class TrediaApi : public QObject {
	Q_OBJECT

public:
	using Done = std::function<void(const QJsonObject &data, int status, const QString &error)>;

	explicit TrediaApi(QObject *parent = nullptr);

	QString baseUrl() const { return base; }
	bool signedIn() const { return !token.isEmpty(); }
	QString userName() const { return name; }

	/* Device sign-in: emits linkCode, then signedInChanged (or linkFailed). */
	void startLink();
	void cancelLink();
	void signOut();

	void get(const QString &path, Done done);
	/* timeoutMs: how long the answer may take (the montage assistant's AI takes longer than the rest). */
	void post(const QString &path, const QJsonObject &body, Done done, int timeoutMs = 30000);

	/* Sends a recording in 8 MB pieces, then turns it into a draft post. done(postUrl, error). */
	void uploadRecording(const QString &filePath, const QString &title, std::function<void(qint64, qint64)> progress,
			     std::function<void(const QString &, const QString &)> done);
	bool uploading() const { return uploadActive; }

signals:
	void signedInChanged();
	void linkCode(const QString &userCode, const QString &verifyUrl);
	void linkFailed(const QString &reason);

private:
	QNetworkAccessManager net;
	QString base;
	QString token;
	QString name;

	QString deviceCode;
	QTimer pollTimer;
	QTimer linkDeadline;
	bool uploadActive = false;

	QNetworkRequest request(const QString &path) const;
	void handle(class QNetworkReply *reply, Done done);
	void poll();
	void loadAccount();
	void saveAccount();
};
