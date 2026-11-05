#include "multi-output.hpp"
#include "secure-store.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/bmem.h>
#include <util/platform.h>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QPointer>
#include <QSaveFile>

struct StopContext {
	QPointer<MultiOutput> owner;
	QString id;
};

namespace {

QString targetsFile()
{
	char *path = obs_module_config_path("local-targets.json");
	QString result = QString::fromUtf8(path);
	bfree(path);
	return result;
}

} // namespace

MultiOutput::MultiOutput(QObject *parent) : QObject(parent)
{
	load();
}

MultiOutput::~MultiOutput()
{
	stopAll();
}

void MultiOutput::setTargets(const QList<LocalTarget> &targets)
{
	list = targets;
	save();
}

int MultiOutput::startAll(QString &error)
{
	stopAll();
	obs_output_t *main = obs_frontend_get_streaming_output();
	if (!main) {
		error = QStringLiteral("البث الرئيسي ما بدأ.");
		return 0;
	}
	obs_encoder_t *video = obs_output_get_video_encoder(main);
	obs_encoder_t *audio = obs_output_get_audio_encoder(main, 0);
	obs_output_release(main);
	if (!video || !audio) {
		error = QStringLiteral("ما قدرنا نشارك ترميز البث الرئيسي.");
		return 0;
	}

	int started = 0;
	for (const LocalTarget &t : siteList + list) {
		if (!t.enabled || t.server.isEmpty())
			continue;

		obs_data_t *settings = obs_data_create();
		obs_data_set_string(settings, "server", t.server.toUtf8().constData());
		obs_data_set_string(settings, "key", t.key.toUtf8().constData());
		obs_service_t *service = obs_service_create_private("rtmp_custom", "tredia-local", settings);
		obs_data_release(settings);

		const QByteArray outputName = QStringLiteral("tredia-local-%1").arg(t.id).toUtf8();
		obs_output_t *output = obs_output_create("rtmp_output", outputName.constData(), nullptr, nullptr);
		if (!service || !output) {
			obs_service_release(service);
			obs_output_release(output);
			emit statusChanged(t.id, QStringLiteral("تعذّر التجهيز"));
			continue;
		}
		obs_output_set_video_encoder(output, video);
		obs_output_set_audio_encoder(output, audio, 0);
		obs_output_set_service(output, service);

		auto *context = new StopContext{this, t.id};
		signal_handler_connect(obs_output_get_signal_handler(output), "stop", &MultiOutput::onStop, context);

		if (obs_output_start(output)) {
			Running running;
			running.id = t.id;
			running.output = output;
			running.service = service;
			running.context = context;
			live.append(running);
			emit statusChanged(t.id, QStringLiteral("مباشر"));
			started += 1;
		} else {
			const char *why = obs_output_get_last_error(output);
			emit statusChanged(t.id, why ? QString::fromUtf8(why) : QStringLiteral("ما قدر يبدأ"));
			signal_handler_disconnect(obs_output_get_signal_handler(output), "stop", &MultiOutput::onStop, context);
			delete context;
			obs_output_release(output);
			obs_service_release(service);
		}
	}
	return started;
}

void MultiOutput::stopAll()
{
	for (Running &r : live) {
		signal_handler_disconnect(obs_output_get_signal_handler(r.output), "stop", &MultiOutput::onStop, r.context);
		obs_output_stop(r.output);
		obs_output_release(r.output);
		obs_service_release(r.service);
		delete r.context;
		emit statusChanged(r.id, QStringLiteral("متوقف"));
	}
	live.clear();
}

/* Runs on a libobs thread: hand the news to the UI thread. */
void MultiOutput::onStop(void *data, calldata_t *cd)
{
	auto *context = static_cast<StopContext *>(data);
	const long long code = calldata_int(cd, "code");
	QPointer<MultiOutput> owner = context->owner;
	const QString id = context->id;
	if (!owner)
		return;
	QMetaObject::invokeMethod(
		owner,
		[owner, id, code]() {
			if (owner)
				emit owner->statusChanged(id, code == OBS_OUTPUT_SUCCESS ? QStringLiteral("متوقف")
											 : QStringLiteral("انقطع الاتصال"));
		},
		Qt::QueuedConnection);
}

void MultiOutput::load()
{
	QFile file(targetsFile());
	if (!file.open(QIODevice::ReadOnly))
		return;
	const QJsonArray items = QJsonDocument::fromJson(file.readAll()).array();
	for (const QJsonValue &v : items) {
		const QJsonObject o = v.toObject();
		LocalTarget t;
		t.id = o.value(QStringLiteral("id")).toString();
		t.name = o.value(QStringLiteral("name")).toString();
		t.server = o.value(QStringLiteral("server")).toString();
		t.key = QString::fromUtf8(unprotectSecret(o.value(QStringLiteral("key")).toString().toUtf8()));
		t.enabled = o.value(QStringLiteral("enabled")).toBool(true);
		if (!t.id.isEmpty())
			list.append(t);
	}
}

void MultiOutput::save() const
{
	char *dir = obs_module_config_path("");
	os_mkdirs(dir);
	bfree(dir);
	QJsonArray items;
	for (const LocalTarget &t : list) {
		QJsonObject o;
		o.insert(QStringLiteral("id"), t.id);
		o.insert(QStringLiteral("name"), t.name);
		o.insert(QStringLiteral("server"), t.server);
		o.insert(QStringLiteral("key"), QString::fromUtf8(protectSecret(t.key.toUtf8())));
		o.insert(QStringLiteral("enabled"), t.enabled);
		items.append(o);
	}
	QSaveFile file(targetsFile());
	if (file.open(QIODevice::WriteOnly)) {
		file.write(QJsonDocument(items).toJson());
		file.commit();
	}
}
