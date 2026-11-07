#include "vertical-preview.hpp"
#include "vertical.hpp"

#include <graphics/graphics.h>
#include <graphics/matrix4.h>
#include <graphics/vec3.h>
#include <graphics/vec4.h>

#include <QAction>
#include <QContextMenuEvent>
#include <QHideEvent>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPolygonF>
#include <QResizeEvent>
#include <QShowEvent>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <vector>

namespace {

/* OBS's own packing of a display's background colour: alpha, blue, green, red. */
uint32_t packColor(const QColor &c)
{
	return (static_cast<uint32_t>(c.alpha()) << 24) | (static_cast<uint32_t>(c.blue()) << 16) |
	       (static_cast<uint32_t>(c.green()) << 8) | static_cast<uint32_t>(c.red());
}

void solidColor(float r, float g, float b)
{
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *color = gs_effect_get_param_by_name(solid, "color");
	struct vec4 value;
	vec4_set(&value, r, g, b, 1.0f);
	gs_effect_set_vec4(color, &value);
}

/* The picture's own black, under its sources (as Tredia's studio draws it). */
void drawBackdrop(float w, float h)
{
	solidColor(0.0f, 0.0f, 0.0f);
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	while (gs_effect_loop(solid, "Solid")) {
		gs_render_start(true);
		gs_vertex2f(0.0f, 0.0f);
		gs_vertex2f(w, 0.0f);
		gs_vertex2f(0.0f, h);
		gs_vertex2f(w, h);
		gs_render_stop(GS_TRISTRIP);
	}
}

/* The four corners of an item's box, in canvas pixels. */
void itemCorners(obs_sceneitem_t *item, struct vec3 points[4])
{
	struct matrix4 box;
	obs_sceneitem_get_box_transform(item, &box);
	const float corners[4][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
	for (int i = 0; i < 4; i++) {
		struct vec3 in;
		vec3_set(&in, corners[i][0], corners[i][1], 0.0f);
		vec3_transform(&points[i], &in, &box);
	}
}

/* The selected source's outline, in the studio's purple, one screen pixel wide. */
void drawOutline(obs_sceneitem_t *item, int x, int y, float scale, uint32_t cx, uint32_t cy)
{
	struct vec3 points[4];
	itemCorners(item, points);
	gs_viewport_push();
	gs_projection_push();
	gs_matrix_push();
	gs_matrix_identity();
	gs_ortho(0.0f, static_cast<float>(cx), 0.0f, static_cast<float>(cy), -100.0f, 100.0f);
	gs_set_viewport(0, 0, static_cast<int>(cx), static_cast<int>(cy));
	solidColor(0xB9 / 255.0f, 0xA0 / 255.0f, 0xF7 / 255.0f);
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	while (gs_effect_loop(solid, "Solid")) {
		gs_render_start(true);
		for (int i = 0; i <= 4; i++) {
			const struct vec3 &p = points[i % 4];
			gs_vertex2f(static_cast<float>(x) + p.x * scale, static_cast<float>(y) + p.y * scale);
		}
		gs_render_stop(GS_LINESTRIP);
	}
	gs_matrix_pop();
	gs_projection_pop();
	gs_viewport_pop();
}

/* The twin scene's items, top first, each with a reference the caller releases. */
std::vector<obs_sceneitem_t *> itemsTopFirst(obs_scene_t *scene)
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
	std::reverse(items.begin(), items.end());
	return items;
}

} // namespace

/* ---- The preview ---------------------------------------------------------------------------------- */

TrediaVerticalPreview::TrediaVerticalPreview(TrediaVertical *vertical_, QWidget *parent) : QWidget(parent), vertical(vertical_)
{
	setObjectName("trediaVerticalPreview");
	/* A native window OBS draws into, like OBS's own preview. */
	setAttribute(Qt::WA_PaintOnScreen);
	setAttribute(Qt::WA_StaticContents);
	setAttribute(Qt::WA_NoSystemBackground);
	setAttribute(Qt::WA_OpaquePaintEvent);
	setAttribute(Qt::WA_DontCreateNativeAncestors);
	setAttribute(Qt::WA_NativeWindow);
	setFocusPolicy(Qt::ClickFocus);
	setMinimumSize(90, 160);
	if (vertical_)
		connect(vertical_, &TrediaVertical::changed, this, &TrediaVerticalPreview::refresh);
	refresh();
}

TrediaVerticalPreview::~TrediaVerticalPreview()
{
	if (display) {
		obs_display_remove_draw_callback(display, &TrediaVerticalPreview::draw, this);
		obs_display_destroy(display);
		display = nullptr;
	}
	std::lock_guard<std::mutex> guard(lock);
	obs_canvas_release(drawCanvas);
	obs_sceneitem_release(selected);
	drawCanvas = nullptr;
	selected = nullptr;
}

void TrediaVerticalPreview::setBackgroundColor(const QColor &color)
{
	background = color;
	if (display)
		obs_display_set_background_color(display, packColor(color));
}

void TrediaVerticalPreview::createDisplay()
{
#ifdef _WIN32
	if (display || !isVisible() || width() <= 0 || height() <= 0)
		return;
	const qreal ratio = devicePixelRatioF();
	struct gs_init_data info = {};
	info.cx = static_cast<uint32_t>(width() * ratio);
	info.cy = static_cast<uint32_t>(height() * ratio);
	info.format = GS_BGRA;
	info.zsformat = GS_ZS_NONE;
	info.window.hwnd = reinterpret_cast<void *>(winId());
	display = obs_display_create(&info, packColor(background));
	if (display)
		obs_display_add_draw_callback(display, &TrediaVerticalPreview::draw, this);
#endif
}

void TrediaVerticalPreview::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);
	createDisplay();
	if (display)
		obs_display_set_enabled(display, true);
}

void TrediaVerticalPreview::hideEvent(QHideEvent *event)
{
	QWidget::hideEvent(event);
	/* Hidden: nothing is drawn for it. */
	if (display)
		obs_display_set_enabled(display, false);
}

void TrediaVerticalPreview::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	if (!display) {
		createDisplay();
		return;
	}
	const qreal ratio = devicePixelRatioF();
	obs_display_resize(display, static_cast<uint32_t>(width() * ratio), static_cast<uint32_t>(height() * ratio));
}

/* The vertical picture turned on or off, or another scene collection: draw the right canvas. */
void TrediaVerticalPreview::refresh()
{
	obs_canvas_t *next = nullptr;
	if (vertical) {
		if (obs_canvas_t *c = vertical->canvas())
			next = obs_canvas_get_ref(c);
	}
	obs_canvas_t *old = nullptr;
	{
		std::lock_guard<std::mutex> guard(lock);
		old = drawCanvas;
		drawCanvas = next;
	}
	obs_canvas_release(old);
	select(nullptr);
}

void TrediaVerticalPreview::select(obs_sceneitem_t *item)
{
	obs_sceneitem_t *old = nullptr;
	{
		std::lock_guard<std::mutex> guard(lock);
		if (item == selected)
			return;
		if (item)
			obs_sceneitem_addref(item);
		old = selected;
		selected = item;
	}
	obs_sceneitem_release(old);
}

/* The selected item with a reference the caller releases, or null. */
obs_sceneitem_t *TrediaVerticalPreview::selection()
{
	std::lock_guard<std::mutex> guard(lock);
	if (selected)
		obs_sceneitem_addref(selected);
	return selected;
}

/* A point on this widget, in the vertical canvas's pixels. */
QPointF TrediaVerticalPreview::toCanvas(const QPointF &point) const
{
	uint32_t bw = 1080, bh = 1920;
	if (vertical)
		vertical->baseSize(bw, bh);
	const double w = width();
	const double h = height();
	const double scale = std::min(w / bw, h / bh);
	if (scale <= 0.0)
		return QPointF();
	const double x0 = (w - bw * scale) / 2.0;
	const double y0 = (h - bh * scale) / 2.0;
	return QPointF((point.x() - x0) / scale, (point.y() - y0) / scale);
}

/* The topmost shown source under a point of the canvas (a reference the caller releases), or null. */
obs_sceneitem_t *TrediaVerticalPreview::itemAt(const QPointF &canvasPoint) const
{
	if (!vertical)
		return nullptr;
	obs_source_t *twin = vertical->programTwin();
	std::vector<obs_sceneitem_t *> items = itemsTopFirst(twin ? obs_scene_from_source(twin) : nullptr);
	obs_sceneitem_t *hit = nullptr;
	for (obs_sceneitem_t *item : items) {
		if (hit || !obs_sceneitem_visible(item))
			continue;
		struct vec3 points[4];
		itemCorners(item, points);
		QPolygonF quad;
		for (const struct vec3 &p : points)
			quad << QPointF(p.x, p.y);
		if (quad.containsPoint(canvasPoint, Qt::OddEvenFill)) {
			obs_sceneitem_addref(item);
			hit = item;
		}
	}
	for (obs_sceneitem_t *item : items)
		obs_sceneitem_release(item);
	obs_source_release(twin);
	return hit;
}

void TrediaVerticalPreview::mousePressEvent(QMouseEvent *event)
{
	if (event->button() != Qt::LeftButton) {
		QWidget::mousePressEvent(event);
		return;
	}
	const QPointF point = toCanvas(event->position());
	obs_sceneitem_t *item = itemAt(point);
	select(item);
	dragging = item != nullptr;
	dragFrom = point;
	obs_sceneitem_release(item);
}

/* Dragging moves the selected source. */
void TrediaVerticalPreview::mouseMoveEvent(QMouseEvent *event)
{
	if (!dragging)
		return;
	obs_sceneitem_t *item = selection();
	if (!item)
		return;
	const QPointF point = toCanvas(event->position());
	struct vec2 pos;
	obs_sceneitem_get_pos(item, &pos);
	pos.x += static_cast<float>(point.x() - dragFrom.x());
	pos.y += static_cast<float>(point.y() - dragFrom.y());
	obs_sceneitem_set_pos(item, &pos);
	dragFrom = point;
	obs_sceneitem_release(item);
}

void TrediaVerticalPreview::mouseReleaseEvent(QMouseEvent *event)
{
	dragging = false;
	QWidget::mouseReleaseEvent(event);
}

/* The wheel over a source makes it bigger or smaller, around the pointer. */
void TrediaVerticalPreview::wheelEvent(QWheelEvent *event)
{
	const QPointF point = toCanvas(event->position());
	obs_sceneitem_t *item = itemAt(point);
	if (!item || event->angleDelta().y() == 0) {
		obs_sceneitem_release(item);
		event->ignore();
		return;
	}
	const float f = event->angleDelta().y() > 0 ? 1.08f : 1.0f / 1.08f;
	const float px = static_cast<float>(point.x());
	const float py = static_cast<float>(point.y());
	struct obs_transform_info info = {};
	obs_sceneitem_get_info2(item, &info);
	if (info.bounds_type != OBS_BOUNDS_NONE) {
		info.bounds.x *= f;
		info.bounds.y *= f;
	} else {
		info.scale.x *= f;
		info.scale.y *= f;
	}
	info.pos.x = px + (info.pos.x - px) * f;
	info.pos.y = py + (info.pos.y - py) * f;
	obs_sceneitem_set_info2(item, &info);
	select(item);
	obs_sceneitem_release(item);
	event->accept();
}

/* A right-click: the source under the pointer (fill, fit, back to where it started, hide) and what
 * shows in the vertical picture. */
void TrediaVerticalPreview::contextMenuEvent(QContextMenuEvent *event)
{
	if (!vertical || !vertical->canvas())
		return;
	QPointer<TrediaVertical> v = vertical;
	QMenu menu(this);

	obs_sceneitem_t *item = itemAt(toCanvas(QPointF(event->pos())));
	if (item) {
		select(item);
		menu.addSection(QString::fromUtf8(obs_source_get_name(obs_sceneitem_get_source(item))));
		connect(menu.addAction(QStringLiteral("املأ الإطار (يقص الأطراف)")), &QAction::triggered, this, [v, item]() {
			if (v)
				v->fill(item, true);
		});
		connect(menu.addAction(QStringLiteral("داخل الإطار كامل")), &QAction::triggered, this, [v, item]() {
			if (v)
				v->fill(item, false);
		});
		connect(menu.addAction(QStringLiteral("رجّعه لمكانه الأول")), &QAction::triggered, this, [v, item]() {
			if (v)
				v->placeDefault(item);
		});
		connect(menu.addAction(QStringLiteral("أخفه من العمودي")), &QAction::triggered, this,
			[item]() { obs_sceneitem_set_visible(item, false); });
	}

	obs_source_t *twin = vertical->programTwin();
	std::vector<obs_sceneitem_t *> items = itemsTopFirst(twin ? obs_scene_from_source(twin) : nullptr);
	if (!items.empty()) {
		menu.addSection(QStringLiteral("يظهر في العمودي"));
		for (obs_sceneitem_t *i : items) {
			QAction *shown = menu.addAction(QString::fromUtf8(obs_source_get_name(obs_sceneitem_get_source(i))));
			shown->setCheckable(true);
			shown->setChecked(obs_sceneitem_visible(i));
			connect(shown, &QAction::toggled, this, [i](bool on) { obs_sceneitem_set_visible(i, on); });
		}
		menu.addSeparator();
		connect(menu.addAction(QStringLiteral("رتّب الإطار العمودي من جديد")), &QAction::triggered, this, [v, items]() {
			if (!v)
				return;
			for (obs_sceneitem_t *i : items)
				v->placeDefault(i);
		});
	}

	menu.exec(event->globalPos());

	for (obs_sceneitem_t *i : items)
		obs_sceneitem_release(i);
	obs_source_release(twin);
	obs_sceneitem_release(item);
}

/* Runs on the graphics thread for every frame of the display. */
void TrediaVerticalPreview::draw(void *param, uint32_t cx, uint32_t cy)
{
	auto *self = static_cast<TrediaVerticalPreview *>(param);
	obs_canvas_t *canvas = nullptr;
	obs_sceneitem_t *item = nullptr;
	{
		std::lock_guard<std::mutex> guard(self->lock);
		if (self->drawCanvas)
			canvas = obs_canvas_get_ref(self->drawCanvas);
		if (self->selected) {
			obs_sceneitem_addref(self->selected);
			item = self->selected;
		}
	}

	struct obs_video_info ovi = {};
	if (canvas && cx && cy && obs_canvas_get_video_info(canvas, &ovi) && ovi.base_width && ovi.base_height) {
		const float bw = static_cast<float>(ovi.base_width);
		const float bh = static_cast<float>(ovi.base_height);
		const float scale = std::min(static_cast<float>(cx) / bw, static_cast<float>(cy) / bh);
		const int w = static_cast<int>(bw * scale);
		const int h = static_cast<int>(bh * scale);
		const int x = (static_cast<int>(cx) - w) / 2;
		const int y = (static_cast<int>(cy) - h) / 2;

		gs_viewport_push();
		gs_projection_push();
		gs_matrix_push();
		gs_matrix_identity();
		gs_ortho(0.0f, bw, 0.0f, bh, -100.0f, 100.0f);
		gs_set_viewport(x, y, w, h);
		drawBackdrop(bw, bh);
		obs_canvas_render(canvas);
		gs_matrix_pop();
		gs_projection_pop();
		gs_viewport_pop();

		/* The outline only while its scene is the one showing. */
		obs_source_t *showing = obs_canvas_get_channel(canvas, 0);
		if (item && showing && obs_scene_get_source(obs_sceneitem_get_scene(item)) == showing)
			drawOutline(item, x, y, scale, cx, cy);
		obs_source_release(showing);
	}

	obs_sceneitem_release(item);
	obs_canvas_release(canvas);
}

/* ---- The box beside OBS's preview ------------------------------------------------------------------ */

TrediaVerticalBox::TrediaVerticalBox(TrediaVertical *vertical_, QWidget *obsPreview_, QWidget *parent)
	: QWidget(parent),
	  vertical(vertical_),
	  obsPreview(obsPreview_)
{
	setObjectName("trediaVerticalBox");
	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(2, 0, 2, 0);
	layout->setSpacing(6);
	caption = new QLabel(QStringLiteral("عمودي 9:16 · تيك توك والشورتس"));
	caption->setObjectName("trediaPreviewCaption");
	caption->setToolTip(QStringLiteral("اسحب المصدر عشان تحرّكه، وعجلة الفأرة فوقه تكبّره وتصغّره، والزر اليمين للباقي."));
	layout->addWidget(caption);
	preview = new TrediaVerticalPreview(vertical_, this);
	layout->addWidget(preview, 1);
	if (vertical_)
		connect(vertical_, &TrediaVertical::changed, this, &TrediaVerticalBox::apply);
	apply();
}

void TrediaVerticalBox::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	fitWidth();
}

/* Placed in OBS's preview row: follow the row's width too (a narrower window, a narrower box). */
bool TrediaVerticalBox::event(QEvent *event)
{
	if (event->type() == QEvent::ParentChange && parentWidget())
		parentWidget()->installEventFilter(this);
	return QWidget::event(event);
}

bool TrediaVerticalBox::eventFilter(QObject *watched, QEvent *event)
{
	if (watched == parentWidget() && event->type() == QEvent::Resize)
		fitWidth();
	return QWidget::eventFilter(watched, event);
}

/* Horizontal: OBS's preview only. Both: side by side. Vertical: the vertical preview only. */
void TrediaVerticalBox::apply()
{
	const bool on = vertical && vertical->enabled();
	const QString view = vertical ? vertical->view() : QStringLiteral("both");
	setVisible(on && view != QStringLiteral("h"));
	if (obsPreview)
		obsPreview->setVisible(!on || view != QStringLiteral("v"));
	fitWidth();
}

/* Beside OBS's preview the box is as wide as a 9:16 picture of its height; alone, it takes the room. */
void TrediaVerticalBox::fitWidth()
{
	if (!vertical || vertical->view() == QStringLiteral("v")) {
		setMinimumWidth(0);
		setMaximumWidth(QWIDGETSIZE_MAX);
		return;
	}
	const int content = std::max(0, height() - caption->sizeHint().height() - 6);
	int w = content * 9 / 16 + 4;
	/* Never more than 40% of the row: the horizontal picture stays the big one. */
	if (QWidget *row = parentWidget())
		w = std::min(w, row->width() * 2 / 5);
	w = std::max(w, 90);
	if (minimumWidth() != w || maximumWidth() != w)
		setFixedWidth(w);
}
