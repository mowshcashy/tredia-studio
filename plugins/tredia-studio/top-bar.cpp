#include "top-bar.hpp"
#include "main-window.hpp"
#include "tredia-dock.hpp"
#include "vertical.hpp"

#include <QAction>
#include <QButtonGroup>
#include <QCheckBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMainWindow>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStyle>
#include <QToolButton>

namespace {

QString clockText(qint64 ms)
{
	const qint64 s = ms / 1000;
	return QStringLiteral("%1:%2:%3")
		.arg(s / 3600, 2, 10, QLatin1Char('0'))
		.arg((s / 60) % 60, 2, 10, QLatin1Char('0'))
		.arg(s % 60, 2, 10, QLatin1Char('0'));
}

/* Dynamic properties the theme styles on (state, live, on). */
void restyle(QWidget *w, const char *property, const QVariant &value)
{
	if (w->property(property) == value)
		return;
	w->setProperty(property, value);
	w->style()->unpolish(w);
	w->style()->polish(w);
}

QToolButton *iconButton(const char *objectName, const QString &tip)
{
	auto *b = new QToolButton;
	b->setObjectName(objectName);
	b->setToolTip(tip);
	b->setAccessibleName(tip);
	b->setAutoRaise(true);
	b->setIconSize(QSize(20, 20));
	return b;
}

QWidget *spacer()
{
	auto *w = new QWidget;
	w->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
	return w;
}

} // namespace

TrediaTopBar::TrediaTopBar(QMainWindow *main, TrediaDock *dock_, TrediaVertical *vertical_)
	: QToolBar(main),
	  dock(dock_),
	  vertical(vertical_)
{
	setObjectName("trediaTopBar");
	setWindowTitle(QStringLiteral("شريط تريديا"));
	setMovable(false);
	setFloatable(false);
	setContextMenuPolicy(Qt::PreventContextMenu);

	auto *logo = new QLabel;
	logo->setObjectName("trediaTopLogo");
	logo->setPixmap(QIcon(QStringLiteral(":/res/images/obs.png")).pixmap(QSize(28, 28), devicePixelRatioF()));
	addWidget(logo);
	auto *name = new QLabel(QStringLiteral("تريديا ستوديو"));
	name->setObjectName("trediaTopName");
	addWidget(name);

	/* OBS's menus (File, Edit, View, panels, profiles, tools, help) in one button: the window has no
	 * menu bar row, like Tredia's own studio. */
	menuButton = iconButton("trediaMenuButton", QStringLiteral("القائمة"));
	menuButton->setPopupMode(QToolButton::InstantPopup);
	auto *menu = new QMenu(menuButton);
	menu->setObjectName("trediaMainMenu");
	QPointer<QMainWindow> mainWindow(main);
	connect(menu, &QMenu::aboutToShow, this, [mainWindow, menu]() { TrediaWindow::fillMenu(mainWindow, menu); });
	menuButton->setMenu(menu);
	addWidget(menuButton);

	/* The vertical picture, as in Tredia's own studio: which previews show, and the picture on/off. */
	viewBox = new QWidget;
	viewBox->setObjectName("trediaSegment");
	viewBox->setAttribute(Qt::WA_StyledBackground, true);
	auto *views = new QHBoxLayout(viewBox);
	views->setContentsMargins(3, 3, 3, 3);
	views->setSpacing(2);
	auto *group = new QButtonGroup(viewBox);
	group->setExclusive(true);
	const char *const modes[3] = {"h", "both", "v"};
	const QString labels[3] = {QStringLiteral("أفقي"), QStringLiteral("الاثنين"), QStringLiteral("عمودي")};
	for (int i = 0; i < 3; i++) {
		auto *button = new QToolButton;
		button->setObjectName("trediaSegmentButton");
		button->setText(labels[i]);
		button->setCheckable(true);
		button->setToolButtonStyle(Qt::ToolButtonTextOnly);
		button->setCursor(Qt::PointingHandCursor);
		const QString mode = QString::fromUtf8(modes[i]);
		connect(button, &QToolButton::clicked, this, [this, mode]() {
			if (vertical)
				vertical->setView(mode);
		});
		group->addButton(button);
		views->addWidget(button);
		viewButtons[i] = button;
	}
	addWidget(viewBox);

	verticalSwitch = new QCheckBox(QStringLiteral("العمودي"));
	verticalSwitch->setObjectName("trediaSwitch");
	verticalSwitch->setToolTip(QStringLiteral("الإطار العمودي: صورة طولية لتيك توك والشورتس جنب الأفقية"));
	verticalSwitch->setCursor(Qt::PointingHandCursor);
	connect(verticalSwitch, &QCheckBox::toggled, this, [this](bool on) {
		if (!vertical)
			return;
		if (obs_frontend_streaming_active() || obs_frontend_recording_active()) {
			/* The picture can't change shape mid-stream, as in Tredia's own studio. */
			QSignalBlocker block(verticalSwitch);
			verticalSwitch->setChecked(vertical->enabled());
			QMessageBox::information(this, QStringLiteral("تريديا"),
						 QStringLiteral("وقّف البث والتسجيل أول، بعدها شغّل أو أطفئ العمودي."));
			return;
		}
		vertical->setEnabled(on);
	});
	addWidget(verticalSwitch);
	if (vertical)
		connect(vertical, &TrediaVertical::changed, this, &TrediaTopBar::refreshVertical);
	refreshVertical();

	addWidget(spacer());
	status = new QLabel;
	status->setObjectName("trediaStatus");
	status->setAlignment(Qt::AlignCenter);
	addWidget(status);
	addWidget(spacer());

	themeButton = iconButton("trediaThemeButton", QStringLiteral("الوضع الفاتح أو الداكن"));
	connect(themeButton, &QToolButton::clicked, this, [this]() { triggerMainAction("trediaThemeToggle"); });
	addWidget(themeButton);

	studioButton = iconButton("trediaStudioButton", QStringLiteral("طور الاستوديو: جهّز المشهد الجاي قبل ما يطلع"));
	studioButton->setCheckable(true);
	connect(studioButton, &QToolButton::clicked, this, [](bool on) { obs_frontend_set_preview_program_mode(on); });
	addWidget(studioButton);

	settingsButton = iconButton("trediaSettingsButton", QStringLiteral("الإعدادات"));
	connect(settingsButton, &QToolButton::clicked, this, [this]() { triggerMainAction("action_Settings"); });
	addWidget(settingsButton);

	recordButton = new QPushButton;
	recordButton->setObjectName("trediaRecord");
	connect(recordButton, &QPushButton::clicked, this, []() {
		if (obs_frontend_recording_active())
			obs_frontend_recording_stop();
		else
			obs_frontend_recording_start();
	});
	addWidget(recordButton);

	liveButton = new QPushButton;
	liveButton->setObjectName("trediaGoLive");
	connect(liveButton, &QPushButton::clicked, this, [this]() {
		if (obs_frontend_streaming_active()) {
			obs_frontend_streaming_stop();
			return;
		}
		if (dock)
			dock->goLive();
		else
			obs_frontend_streaming_start();
	});
	addWidget(liveButton);

	if (dock)
		connect(dock, &TrediaDock::goLiveBusy, this, [this](bool on) {
			busy = on;
			refresh();
		});

	clock.setInterval(1000);
	connect(&clock, &QTimer::timeout, this, &TrediaTopBar::refresh);
	refresh();
}

void TrediaTopBar::refreshVertical()
{
	const bool on = vertical && vertical->enabled();
	{
		QSignalBlocker block(verticalSwitch);
		verticalSwitch->setChecked(on);
	}
	viewBox->setVisible(on);
	const QString mode = vertical ? vertical->view() : QStringLiteral("both");
	const char *const modes[3] = {"h", "both", "v"};
	for (int i = 0; i < 3; i++)
		viewButtons[i]->setChecked(mode == QString::fromUtf8(modes[i]));
}

void TrediaTopBar::triggerMainAction(const char *name)
{
	auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	if (QAction *action = main ? main->findChild<QAction *>(QString::fromUtf8(name)) : nullptr)
		action->trigger();
}

void TrediaTopBar::refresh()
{
	const bool live = obs_frontend_streaming_active();
	const bool rec = obs_frontend_recording_active();
	const bool connecting = busy || (starting && !live);

	QString text;
	QString state;
	if (connecting) {
		text = QStringLiteral("يتصل…");
		state = QStringLiteral("starting");
	} else if (live) {
		const QString time = clockText(liveSince.isValid() ? liveSince.elapsed() : 0);
		text = rec ? QStringLiteral("مباشر ويسجّل · %1").arg(time) : QStringLiteral("مباشر · %1").arg(time);
		state = QStringLiteral("live");
	} else if (rec) {
		text = QStringLiteral("يسجّل · %1").arg(clockText(recordSince.isValid() ? recordSince.elapsed() : 0));
		state = QStringLiteral("rec");
	} else {
		text = QStringLiteral("جاهز للبث");
		state = QStringLiteral("idle");
	}
	status->setText(text);
	restyle(status, "state", state);

	liveButton->setText(live ? QStringLiteral("إيقاف البث") : connecting ? QStringLiteral("يتصل…") : QStringLiteral("ابدأ البث"));
	liveButton->setEnabled(!connecting);
	restyle(liveButton, "live", live);

	recordButton->setText(rec ? QStringLiteral("إيقاف التسجيل") : QStringLiteral("تسجيل"));
	restyle(recordButton, "on", rec);

	{
		QSignalBlocker block(studioButton);
		studioButton->setChecked(obs_frontend_preview_program_mode_active());
	}

	if (live || rec) {
		if (!clock.isActive())
			clock.start();
	} else {
		clock.stop();
	}
}

void TrediaTopBar::handleEvent(enum obs_frontend_event event)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_STREAMING_STARTING:
		starting = true;
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STARTED:
		starting = false;
		liveSince.start();
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STOPPED:
		starting = false;
		liveSince.invalidate();
		break;
	case OBS_FRONTEND_EVENT_RECORDING_STARTED:
		recordSince.start();
		break;
	case OBS_FRONTEND_EVENT_RECORDING_STOPPED:
		recordSince.invalidate();
		break;
	default:
		break;
	}
	refresh();
}
