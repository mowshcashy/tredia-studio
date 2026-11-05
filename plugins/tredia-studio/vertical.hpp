#pragma once

#include <obs-frontend-api.h>
#include <obs.h>

#include <QList>
#include <QObject>
#include <QString>

/* Tredia Studio's vertical picture (9:16, for TikTok, Reels and Shorts), as in Tredia's own studio:
 * a second OBS canvas where every scene has a twin holding the same sources, arranged for a tall
 * picture. Switching scenes switches both; a source added to or removed from a scene is added to or
 * removed from its twin; each picture keeps its own arrangement and what shows in it. The canvas
 * and its scenes are saved with the scene collection (OBS does that for extra canvases). */
class TrediaVertical : public QObject {
	Q_OBJECT

public:
	explicit TrediaVertical(QObject *parent = nullptr);
	~TrediaVertical() override;

	/* The "العمودي" switch. Off: nothing is drawn or sent; the arrangement is kept for next time. */
	bool enabled() const { return on; }
	void setEnabled(bool enabled);

	/* Which previews show: "h", "both" or "v". */
	QString view() const { return viewMode; }
	void setView(const QString &mode);

	/* The vertical canvas while it's on (no reference added), else null. */
	obs_canvas_t *canvas() const { return on && ready ? verticalCanvas : nullptr; }
	/* Its video, for an encoder, or null. */
	video_t *video() const;
	/* The twin of the scene on air (a new reference), or null. */
	obs_source_t *programTwin() const;
	/* The canvas's size (1080×1920 unless set otherwise). */
	void baseSize(uint32_t &width, uint32_t &height) const;
	/* Before going live: the canvas follows the main video's frame rate and size. */
	void prepare();

	void handleEvent(enum obs_frontend_event event);

	/* Where Tredia puts a source in the vertical picture at first. */
	void placeDefault(obs_sceneitem_t *twinItem) const;
	/* Fills the whole picture (cropping what overflows) or fits inside it. */
	void fill(obs_sceneitem_t *twinItem, bool cover) const;

signals:
	void changed();

private:
	obs_canvas_t *verticalCanvas = nullptr; /* a strong reference */
	bool on = false;
	bool ready = false; /* false while a scene collection is changing */
	bool reconcileQueued = false;
	QString viewMode = QStringLiteral("both");
	QList<obs_weak_source_t *> watched; /* main scenes whose signals are connected */

	void load();
	void save() const;
	void attach();
	void detach();
	void ensureCanvas();
	void resizeVideo();
	void reconcileAll();
	void reconcileScene(obs_source_t *mainScene, obs_source_t *twinScene) const;
	obs_source_t *findTwin(obs_source_t *mainScene) const;
	void syncProgram();
	void watch(obs_source_t *mainScene);
	void unwatchAll();
	void queueReconcile();
	static void onSceneSignal(void *data, calldata_t *cd);
};
