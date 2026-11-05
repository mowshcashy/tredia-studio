#pragma once

#include <obs.h>

#include <QColor>
#include <QPointer>
#include <QWidget>

#include <mutex>

class QLabel;
class TrediaVertical;

/* The vertical picture's preview, beside OBS's own, as in Tredia's own studio. Drag a source to move
 * it, the mouse wheel over it to make it bigger or smaller, and a right-click for the rest (fill the
 * picture, fit inside it, back to where it started, what shows). */
class TrediaVerticalPreview : public QWidget {
	Q_OBJECT
	Q_PROPERTY(QColor displayBackgroundColor READ backgroundColor WRITE setBackgroundColor DESIGNABLE true)

public:
	explicit TrediaVerticalPreview(TrediaVertical *vertical, QWidget *parent = nullptr);
	~TrediaVerticalPreview() override;

	QPaintEngine *paintEngine() const override { return nullptr; }
	QColor backgroundColor() const { return background; }
	void setBackgroundColor(const QColor &color);

protected:
	void showEvent(QShowEvent *event) override;
	void hideEvent(QHideEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;
	void mousePressEvent(QMouseEvent *event) override;
	void mouseMoveEvent(QMouseEvent *event) override;
	void mouseReleaseEvent(QMouseEvent *event) override;
	void wheelEvent(QWheelEvent *event) override;
	void contextMenuEvent(QContextMenuEvent *event) override;

private:
	QPointer<TrediaVertical> vertical;
	obs_display_t *display = nullptr;
	QColor background = QColor(10, 5, 21);
	/* Read by the graphics thread while it draws: both are references, swapped under the lock. */
	std::mutex lock;
	obs_canvas_t *drawCanvas = nullptr;
	obs_sceneitem_t *selected = nullptr;
	bool dragging = false;
	QPointF dragFrom;

	void createDisplay();
	void refresh();
	void select(obs_sceneitem_t *item);
	obs_sceneitem_t *selection();
	QPointF toCanvas(const QPointF &point) const;
	obs_sceneitem_t *itemAt(const QPointF &canvasPoint) const;
	static void draw(void *param, uint32_t cx, uint32_t cy);
};

/* The box beside OBS's preview: a caption over the vertical preview. The top bar's view buttons
 * (horizontal, both, vertical) decide what shows while the vertical picture is on. */
class TrediaVerticalBox : public QWidget {
	Q_OBJECT

public:
	TrediaVerticalBox(TrediaVertical *vertical, QWidget *obsPreview, QWidget *parent = nullptr);

protected:
	void resizeEvent(QResizeEvent *event) override;

private:
	QPointer<TrediaVertical> vertical;
	QPointer<QWidget> obsPreview;
	QLabel *caption;
	TrediaVerticalPreview *preview;

	void apply();
	void fitWidth();
};
