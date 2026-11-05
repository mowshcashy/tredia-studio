#pragma once

#include <obs.h>

#include <QList>
#include <QObject>
#include <QString>

/* One place to stream to straight from this PC (in addition to the main stream in Settings → Stream). */
struct LocalTarget {
	QString id;
	QString name;
	QString server;
	QString key;
	bool enabled = true;
	/* Gets the vertical picture (9:16) instead of the main one. */
	bool vertical = false;
};

/* "From this PC" multistreaming: extra RTMP outputs that reuse the main stream's encoders, so the picture
 * is encoded once, but every destination gets its own full copy over this PC's upload. Destinations set
 * to the vertical picture share one more video encoder, on the vertical canvas. */
class MultiOutput : public QObject {
	Q_OBJECT

public:
	explicit MultiOutput(QObject *parent = nullptr);
	~MultiOutput() override;

	QList<LocalTarget> targets() const { return list; }
	void setTargets(const QList<LocalTarget> &targets);

	/* The platforms ticked on Tredia, for this stream only (never saved: their keys come fresh from
	 * Tredia each time you go live). */
	void setSiteTargets(const QList<LocalTarget> &targets) { siteList = targets; }

	/* Call when the main stream has started / is stopping. `verticalVideo`: the vertical canvas's video
	 * (vertical destinations are skipped without it). `withOwn`: also the destinations added here by
	 * hand. Returns how many extra outputs started. */
	int startAll(QString &error, video_t *verticalVideo = nullptr, bool withOwn = true);
	void stopAll();
	int running() const { return static_cast<int>(live.size()); }

signals:
	void statusChanged(const QString &id, const QString &status);

private:
	struct Running {
		QString id;
		obs_output_t *output = nullptr;
		obs_service_t *service = nullptr;
		struct StopContext *context = nullptr;
	};

	QList<LocalTarget> list;
	QList<LocalTarget> siteList;
	QList<Running> live;
	obs_encoder_t *verticalEncoder = nullptr;

	void load();
	void save() const;
	bool prepareVerticalEncoder(obs_encoder_t *mainVideo, video_t *verticalVideo);
	static void onStop(void *data, calldata_t *cd);
};
