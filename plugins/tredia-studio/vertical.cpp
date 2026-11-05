#include "vertical.hpp"

#include <obs-module.h>
#include <util/bmem.h>
#include <util/platform.h>

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QSaveFile>
#include <QTimer>

#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

const char *const canvasName = "Tredia Vertical";
/* A twin scene keeps the UUID of its scene; a twin item keeps the id of its item in that scene. */
const char *const twinKey = "tredia_main_scene";
const char *const itemKey = "tredia_main_item";
const char *const sceneSignals[] = {"item_add", "item_remove", "reorder", "refresh"};

QString settingsFile()
{
	char *path = obs_module_config_path("vertical.json");
	QString result = QString::fromUtf8(path);
	bfree(path);
	return result;
}

/* The vertical canvas from the main one: 1080×1920 to draw on, sent at 720×1280 (or 1080×1920 when
 * the main stream is 1080p or more), at the main frame rate. */
void verticalVideoInfo(struct obs_video_info &ovi)
{
	const bool tall = ovi.output_height >= 1080;
	ovi.base_width = 1080;
	ovi.base_height = 1920;
	ovi.output_width = tall ? 1080 : 720;
	ovi.output_height = tall ? 1920 : 1280;
}

QString privateString(obs_source_t *source, const char *key)
{
	obs_data_t *data = obs_source_get_private_settings(source);
	const QString value = data ? QString::fromUtf8(obs_data_get_string(data, key)) : QString();
	obs_data_release(data);
	return value;
}

int64_t mainItemId(obs_sceneitem_t *item)
{
	obs_data_t *data = obs_sceneitem_get_private_settings(item);
	const int64_t id = data ? obs_data_get_int(data, itemKey) : 0;
	obs_data_release(data);
	return id;
}

/* A scene's items, bottom to top, each with a reference the caller releases. */
std::vector<obs_sceneitem_t *> itemsOf(obs_scene_t *scene)
{
	std::vector<obs_sceneitem_t *> items;
	if (!scene)
		return items;
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			obs_sceneitem_addref(item);
			static_cast<std::vector<obs_sceneitem_t *> *>(param)->push_back(item);
			return true;
		},
		&items);
	return items;
}

void releaseItems(std::vector<obs_sceneitem_t *> &items)
{
	for (obs_sceneitem_t *item : items)
		obs_sceneitem_release(item);
	items.clear();
}

/* The canvas's scenes, each with a reference the caller releases. */
std::vector<obs_source_t *> canvasScenes(obs_canvas_t *canvas)
{
	std::vector<obs_source_t *> scenes;
	if (!canvas)
		return scenes;
	obs_canvas_enum_scenes(
		canvas,
		[](void *param, obs_source_t *source) {
			static_cast<std::vector<obs_source_t *> *>(param)->push_back(obs_source_get_ref(source));
			return true;
		},
		&scenes);
	return scenes;
}

void releaseSources(std::vector<obs_source_t *> &sources)
{
	for (obs_source_t *source : sources)
		obs_source_release(source);
	sources.clear();
}

/* Puts an item in a rectangle of the canvas (pixels), its picture scaled to the rectangle. */
void setRect(obs_sceneitem_t *item, float x, float y, float w, float h, enum obs_bounds_type type, bool crop)
{
	struct obs_transform_info info = {};
	obs_sceneitem_get_info2(item, &info);
	info.pos.x = x;
	info.pos.y = y;
	info.rot = 0.0f;
	info.scale.x = 1.0f;
	info.scale.y = 1.0f;
	info.alignment = OBS_ALIGN_LEFT | OBS_ALIGN_TOP;
	info.bounds_type = type;
	info.bounds_alignment = OBS_ALIGN_CENTER;
	info.bounds.x = w;
	info.bounds.y = h;
	info.crop_to_bounds = crop;
	obs_sceneitem_set_info2(item, &info);
}

/* One of Tredia's own overlays (chat, alerts, viewer count): its kind, from the page's address. */
QString trediaOverlay(obs_source_t *source)
{
	const char *id = obs_source_get_unversioned_id(source);
	if (!id || strcmp(id, "browser_source") != 0)
		return QString();
	obs_data_t *settings = obs_source_get_settings(source);
	const QString url = QString::fromUtf8(obs_data_get_string(settings, "url"));
	obs_data_release(settings);
	if (!url.contains(QStringLiteral("/overlay/")))
		return QString();
	if (url.contains(QStringLiteral("/alerts")))
		return QStringLiteral("alerts");
	if (url.contains(QStringLiteral("/chat")))
		return QStringLiteral("chat");
	if (url.contains(QStringLiteral("/viewers")))
		return QStringLiteral("viewers");
	return QString();
}

} // namespace

TrediaVertical::TrediaVertical(QObject *parent) : QObject(parent)
{
	load();
}

TrediaVertical::~TrediaVertical()
{
	unwatchAll();
	if (verticalCanvas) {
		obs_canvas_release(verticalCanvas);
		verticalCanvas = nullptr;
	}
}

void TrediaVertical::load()
{
	QFile file(settingsFile());
	if (!file.open(QIODevice::ReadOnly))
		return;
	const QJsonObject o = QJsonDocument::fromJson(file.readAll()).object();
	on = o.value(QStringLiteral("enabled")).toBool(false);
	const QString mode = o.value(QStringLiteral("view")).toString();
	if (mode == QStringLiteral("h") || mode == QStringLiteral("both") || mode == QStringLiteral("v"))
		viewMode = mode;
}

void TrediaVertical::save() const
{
	char *dir = obs_module_config_path("");
	os_mkdirs(dir);
	bfree(dir);
	QJsonObject o;
	o.insert(QStringLiteral("enabled"), on);
	o.insert(QStringLiteral("view"), viewMode);
	QSaveFile file(settingsFile());
	if (file.open(QIODevice::WriteOnly)) {
		file.write(QJsonDocument(o).toJson());
		file.commit();
	}
}

void TrediaVertical::setEnabled(bool enabled)
{
	if (enabled == on)
		return;
	on = enabled;
	save();
	if (ready) {
		if (on) {
			ensureCanvas();
			resizeVideo();
			reconcileAll();
		}
		syncProgram();
	}
	emit changed();
}

void TrediaVertical::setView(const QString &mode)
{
	if (mode == viewMode)
		return;
	if (mode != QStringLiteral("h") && mode != QStringLiteral("both") && mode != QStringLiteral("v"))
		return;
	viewMode = mode;
	save();
	emit changed();
}

video_t *TrediaVertical::video() const
{
	obs_canvas_t *c = canvas();
	return c ? obs_canvas_get_video(c) : nullptr;
}

obs_source_t *TrediaVertical::programTwin() const
{
	if (!canvas())
		return nullptr;
	obs_source_t *program = obs_frontend_get_current_scene();
	obs_source_t *twin = program ? findTwin(program) : nullptr;
	obs_source_release(program);
	return twin;
}

void TrediaVertical::baseSize(uint32_t &width, uint32_t &height) const
{
	width = 1080;
	height = 1920;
	struct obs_video_info ovi = {};
	if (verticalCanvas && obs_canvas_get_video_info(verticalCanvas, &ovi) && ovi.base_width && ovi.base_height) {
		width = ovi.base_width;
		height = ovi.base_height;
	}
}

void TrediaVertical::prepare()
{
	if (on && ready)
		resizeVideo();
}

void TrediaVertical::handleEvent(enum obs_frontend_event event)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
		attach();
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP:
	case OBS_FRONTEND_EVENT_EXIT:
		detach();
		break;
	case OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED:
		queueReconcile();
		break;
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
	case OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED:
	case OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED:
		syncProgram();
		break;
	default:
		break;
	}
}

/* A scene collection is loaded: find its vertical canvas (OBS restores it with the collection, without
 * a picture size), size it, and bring the twins in line with the scenes. */
void TrediaVertical::attach()
{
	ready = true;
	if (!verticalCanvas) {
		struct obs_frontend_canvas_list list = {};
		obs_frontend_get_canvases(&list);
		for (size_t i = 0; i < list.canvases.num; i++) {
			obs_canvas_t *c = list.canvases.array[i];
			const char *name = obs_canvas_get_name(c);
			if (name && strcmp(name, canvasName) == 0 && !obs_canvas_removed(c)) {
				verticalCanvas = obs_canvas_get_ref(c);
				break;
			}
		}
		obs_frontend_canvas_list_free(&list);
	}
	if (on)
		ensureCanvas();
	if (verticalCanvas) {
		resizeVideo();
		reconcileAll();
	}
	syncProgram();
	emit changed();
}

void TrediaVertical::detach()
{
	ready = false;
	unwatchAll();
	if (verticalCanvas) {
		obs_canvas_set_channel(verticalCanvas, 0, nullptr);
		obs_canvas_release(verticalCanvas);
		verticalCanvas = nullptr;
	}
	emit changed();
}

void TrediaVertical::ensureCanvas()
{
	if (verticalCanvas || !ready)
		return;
	struct obs_video_info ovi = {};
	if (!obs_get_video_info(&ovi))
		return;
	verticalVideoInfo(ovi);
	/* Its sources activate when shown, and it keeps its scenes; the sound stays the main mix's. */
	verticalCanvas = obs_frontend_add_canvas(canvasName, &ovi, ACTIVATE | SCENE_REF);
}

/* The canvas follows the main video's frame rate and size class (never while it's being sent). */
void TrediaVertical::resizeVideo()
{
	if (!verticalCanvas)
		return;
	struct obs_video_info want = {};
	if (!obs_get_video_info(&want))
		return;
	verticalVideoInfo(want);
	struct obs_video_info have = {};
	const bool same = obs_canvas_has_video(verticalCanvas) && obs_canvas_get_video_info(verticalCanvas, &have) &&
			  have.base_width == want.base_width && have.base_height == want.base_height &&
			  have.output_width == want.output_width && have.output_height == want.output_height &&
			  have.fps_num == want.fps_num && have.fps_den == want.fps_den;
	if (same || obs_canvas_video_active(verticalCanvas))
		return;
	obs_canvas_reset_video(verticalCanvas, &want);
}

/* Every scene gets a twin (same name); a twin whose scene is gone goes too. */
void TrediaVertical::reconcileAll()
{
	if (!verticalCanvas || !ready)
		return;
	struct obs_frontend_source_list mains = {};
	obs_frontend_get_scenes(&mains);
	std::vector<obs_source_t *> twins = canvasScenes(verticalCanvas);
	std::set<obs_source_t *> used;

	for (size_t i = 0; i < mains.sources.num; i++) {
		obs_source_t *mainScene = mains.sources.array[i];
		const QString uuid = QString::fromUtf8(obs_source_get_uuid(mainScene));
		obs_source_t *twin = nullptr;
		for (obs_source_t *t : twins) {
			if (!used.count(t) && privateString(t, twinKey) == uuid) {
				twin = obs_source_get_ref(t);
				break;
			}
		}
		if (!twin) {
			obs_scene_t *created = obs_canvas_scene_create(verticalCanvas, obs_source_get_name(mainScene));
			if (!created)
				continue;
			twin = obs_source_get_ref(obs_scene_get_source(created));
			obs_scene_release(created);
			obs_data_t *data = obs_source_get_private_settings(twin);
			obs_data_set_string(data, twinKey, obs_source_get_uuid(mainScene));
			obs_data_release(data);
		} else if (strcmp(obs_source_get_name(twin), obs_source_get_name(mainScene)) != 0) {
			obs_source_set_name(twin, obs_source_get_name(mainScene));
		}
		used.insert(twin);
		watch(mainScene);
		reconcileScene(mainScene, twin);
		obs_source_release(twin);
	}

	for (obs_source_t *t : twins)
		if (!used.count(t))
			obs_source_remove(t);
	releaseSources(twins);
	obs_frontend_source_list_free(&mains);
}

/* A twin holds the scene's sources in the same order: a new one is placed where Tredia puts it, a gone
 * one goes; groups stay in the horizontal picture only. */
void TrediaVertical::reconcileScene(obs_source_t *mainSource, obs_source_t *twinSource) const
{
	obs_scene_t *mainScene = obs_scene_from_source(mainSource);
	obs_scene_t *twinScene = obs_scene_from_source(twinSource);
	if (!mainScene || !twinScene)
		return;
	std::vector<obs_sceneitem_t *> mainItems = itemsOf(mainScene);
	std::vector<obs_sceneitem_t *> twinItems = itemsOf(twinScene);

	std::set<int64_t> wanted;
	for (obs_sceneitem_t *m : mainItems)
		if (!obs_sceneitem_is_group(m))
			wanted.insert(obs_sceneitem_get_id(m));

	std::map<int64_t, obs_sceneitem_t *> byMain;
	for (obs_sceneitem_t *t : twinItems) {
		const int64_t id = mainItemId(t);
		if (!id || !wanted.count(id) || byMain.count(id))
			obs_sceneitem_remove(t);
		else
			byMain[id] = t;
	}

	std::vector<obs_sceneitem_t *> ordered;
	for (obs_sceneitem_t *m : mainItems) {
		if (obs_sceneitem_is_group(m))
			continue;
		const int64_t id = obs_sceneitem_get_id(m);
		obs_sceneitem_t *t = nullptr;
		auto found = byMain.find(id);
		if (found != byMain.end()) {
			t = found->second;
		} else {
			t = obs_scene_add(twinScene, obs_sceneitem_get_source(m));
			if (!t)
				continue;
			obs_data_t *data = obs_sceneitem_get_private_settings(t);
			obs_data_set_int(data, itemKey, id);
			obs_data_release(data);
			placeDefault(t);
			obs_sceneitem_set_visible(t, obs_sceneitem_visible(m));
		}
		ordered.push_back(t);
	}
	for (size_t i = 0; i < ordered.size(); i++)
		obs_sceneitem_set_order_position(ordered[i], static_cast<int>(i));

	releaseItems(mainItems);
	releaseItems(twinItems);
}

obs_source_t *TrediaVertical::findTwin(obs_source_t *mainScene) const
{
	if (!verticalCanvas || !mainScene)
		return nullptr;
	const QString uuid = QString::fromUtf8(obs_source_get_uuid(mainScene));
	std::vector<obs_source_t *> twins = canvasScenes(verticalCanvas);
	obs_source_t *found = nullptr;
	for (obs_source_t *t : twins)
		if (!found && privateString(t, twinKey) == uuid)
			found = obs_source_get_ref(t);
	releaseSources(twins);
	return found;
}

/* The vertical picture shows the twin of the scene on air (nothing while it's off). */
void TrediaVertical::syncProgram()
{
	if (!verticalCanvas)
		return;
	obs_source_t *twin = (on && ready) ? programTwin() : nullptr;
	obs_canvas_set_channel(verticalCanvas, 0, twin);
	obs_source_release(twin);
}

void TrediaVertical::watch(obs_source_t *mainScene)
{
	/* Scenes that are gone first: a new scene can take a gone one's address. */
	for (int i = static_cast<int>(watched.size()) - 1; i >= 0; i--) {
		if (obs_weak_source_expired(watched[i])) {
			obs_weak_source_release(watched[i]);
			watched.removeAt(i);
		}
	}
	for (obs_weak_source_t *w : watched)
		if (obs_weak_source_references_source(w, mainScene))
			return;
	signal_handler_t *handler = obs_source_get_signal_handler(mainScene);
	for (const char *name : sceneSignals)
		signal_handler_connect(handler, name, &TrediaVertical::onSceneSignal, this);
	watched.append(obs_source_get_weak_source(mainScene));
}

void TrediaVertical::unwatchAll()
{
	for (obs_weak_source_t *w : watched) {
		if (obs_source_t *s = obs_weak_source_get_source(w)) {
			signal_handler_t *handler = obs_source_get_signal_handler(s);
			for (const char *name : sceneSignals)
				signal_handler_disconnect(handler, name, &TrediaVertical::onSceneSignal, this);
			obs_source_release(s);
		}
		obs_weak_source_release(w);
	}
	watched.clear();
}

/* Scene signals come on whichever thread changed the scene: the work is done on the UI thread, once
 * for a burst of changes. */
void TrediaVertical::onSceneSignal(void *data, calldata_t *)
{
	auto *self = static_cast<TrediaVertical *>(data);
	QMetaObject::invokeMethod(self, [self]() { self->queueReconcile(); }, Qt::QueuedConnection);
}

void TrediaVertical::queueReconcile()
{
	if (reconcileQueued)
		return;
	reconcileQueued = true;
	QTimer::singleShot(0, this, [this]() {
		reconcileQueued = false;
		if (!ready)
			return;
		reconcileAll();
		syncProgram();
	});
}

/* Where Tredia's own studio puts a new source in the vertical picture. */
void TrediaVertical::placeDefault(obs_sceneitem_t *item) const
{
	if (!item)
		return;
	uint32_t cw = 0, ch = 0;
	baseSize(cw, ch);
	const float W = static_cast<float>(cw);
	const float H = static_cast<float>(ch);
	const float canvasAspect = W / H;

	obs_source_t *source = obs_sceneitem_get_source(item);
	const char *rawId = obs_source_get_unversioned_id(source);
	const std::string id = rawId ? rawId : "";
	float sw = static_cast<float>(obs_source_get_width(source));
	float sh = static_cast<float>(obs_source_get_height(source));
	if (sw <= 0.0f || sh <= 0.0f) {
		/* Not drawn yet (a page still loading): its set size, else 16:9. */
		obs_data_t *settings = obs_source_get_settings(source);
		sw = static_cast<float>(obs_data_get_int(settings, "width"));
		sh = static_cast<float>(obs_data_get_int(settings, "height"));
		obs_data_release(settings);
	}
	const float aspect = (sw > 0.0f && sh > 0.0f) ? sw / sh : 16.0f / 9.0f;

	const QString overlay = trediaOverlay(source);
	if (overlay == QStringLiteral("alerts")) {
		setRect(item, 0.06f * W, 0.08f * H, 0.88f * W, 0.16f * H, OBS_BOUNDS_SCALE_INNER, false);
		return;
	}
	if (overlay == QStringLiteral("chat")) {
		setRect(item, 0.04f * W, 0.62f * H, 0.6f * W, 0.3f * H, OBS_BOUNDS_SCALE_INNER, false);
		return;
	}
	if (overlay == QStringLiteral("viewers")) {
		setRect(item, 0.62f * W, 0.03f * H, 0.34f * W, 0.035f * H, OBS_BOUNDS_SCALE_INNER, false);
		return;
	}
	if (id == "color_source") {
		setRect(item, 0.0f, 0.0f, W, H, OBS_BOUNDS_STRETCH, false);
		return;
	}
	if (id == "dshow_input" || id == "av_capture_input" || id == "v4l2_input") {
		/* A camera: 60% wide, in the lower half. */
		const float w = 0.6f;
		const float h = std::min(0.4f, w * canvasAspect / aspect);
		setRect(item, (1.0f - w) / 2.0f * W, 0.58f * H, w * W, h * H, OBS_BOUNDS_SCALE_INNER, false);
		return;
	}
	if (id == "game_capture" || id == "window_capture" || id == "monitor_capture" || id == "screen_capture" ||
	    id == "ffmpeg_source" || id == "vlc_source" || id == "browser_source" || id == "scene" ||
	    id == "xcomposite_input" || id == "pipewire-screen-capture-source" || id == "display_capture") {
		/* A game, a screen, a video, a page: the whole picture, fitting inside. */
		setRect(item, 0.0f, 0.0f, W, H, OBS_BOUNDS_SCALE_INNER, false);
		return;
	}
	/* Anything else (an image, text): 80% wide, in the middle. */
	const float w = 0.8f;
	const float h = std::min(0.9f, w * canvasAspect / aspect);
	setRect(item, (1.0f - w) / 2.0f * W, (1.0f - h) / 2.0f * H, w * W, h * H, OBS_BOUNDS_SCALE_INNER, false);
}

void TrediaVertical::fill(obs_sceneitem_t *item, bool cover) const
{
	if (!item)
		return;
	uint32_t cw = 0, ch = 0;
	baseSize(cw, ch);
	setRect(item, 0.0f, 0.0f, static_cast<float>(cw), static_cast<float>(ch),
		cover ? OBS_BOUNDS_SCALE_OUTER : OBS_BOUNDS_SCALE_INNER, cover);
}
