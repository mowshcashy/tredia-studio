#include "main-window.hpp"
#include "vertical-preview.hpp"
#include "vertical.hpp"

#include <obs-frontend-api.h>

#include <QAction>
#include <QDockWidget>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QToolButton>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

/* A panel's header as in Tredia's own studio: its name on one side, its main action on the other. */
QWidget *panelHeader(const QString &title, QWidget *action = nullptr)
{
	auto *header = new QWidget;
	header->setObjectName("trediaPanelHeader");
	header->setAttribute(Qt::WA_StyledBackground, true);
	header->setFixedHeight(48);
	auto *row = new QHBoxLayout(header);
	row->setContentsMargins(18, 0, 10, 0);
	row->setSpacing(6);
	auto *label = new QLabel(title);
	label->setObjectName("trediaPanelTitle");
	row->addWidget(label);
	row->addStretch();
	if (action)
		row->addWidget(action);
	return header;
}

QAction *mainAction(QMainWindow *main, const char *name)
{
	return main->findChild<QAction *>(QString::fromUtf8(name));
}

void setHeader(QMainWindow *main, const char *dockName, QWidget *header)
{
	auto *dock = main->findChild<QDockWidget *>(QString::fromUtf8(dockName));
	if (!dock) {
		delete header;
		return;
	}
	dock->setTitleBarWidget(header);
}

} // namespace

namespace TrediaWindow {

void style(QMainWindow *main)
{
	if (!main)
		return;

	/* Scenes: "+" makes a new scene (renaming, ordering and removing are on a right-click). */
	auto *addScene = new QToolButton;
	addScene->setObjectName("trediaPanelAdd");
	addScene->setToolTip(QStringLiteral("مشهد جديد"));
	addScene->setAccessibleName(addScene->toolTip());
	addScene->setAutoRaise(true);
	addScene->setIconSize(QSize(16, 16));
	QObject::connect(addScene, &QToolButton::clicked, main, [main]() {
		if (QAction *a = mainAction(main, "actionAddScene"))
			a->trigger();
	});
	setHeader(main, "scenesDock", panelHeader(QStringLiteral("المشاهد"), addScene));

	/* Sources: "+ مصدر" opens OBS's list of sources to add. */
	auto *addSource = new QPushButton(QStringLiteral("مصدر"));
	addSource->setObjectName("trediaPanelPrimary");
	addSource->setToolTip(QStringLiteral("أضف مصدر للمشهد"));
	addSource->setIconSize(QSize(14, 14));
	addSource->setCursor(Qt::PointingHandCursor);
	QObject::connect(addSource, &QPushButton::clicked, main, [main]() {
		if (QAction *a = mainAction(main, "actionAddSource"))
			a->trigger();
	});
	setHeader(main, "sourcesDock", panelHeader(QStringLiteral("المصادر"), addSource));

	setHeader(main, "mixerDock", panelHeader(QStringLiteral("الصوت")));
	setHeader(main, "transitionsDock", panelHeader(QStringLiteral("الانتقالات")));
	setHeader(main, "tredia", panelHeader(QStringLiteral("تريديا")));

	/* No menu bar row: OBS's menus open from the top bar's menu button. The bar keeps no height but
	 * stays shown, so every keyboard shortcut in its menus keeps working. */
	if (QMenuBar *bar = main->menuBar())
		bar->setFixedHeight(0);
}

void addVerticalPreview(QMainWindow *main, TrediaVertical *vertical)
{
	if (!main || !vertical || main->findChild<TrediaVerticalBox *>())
		return;
	/* OBS's preview area is a row (its preview, and the program view in studio mode); the vertical
	 * picture's box goes at its end, which is the left in Arabic, as in Tredia's own studio. */
	auto *row = main->findChild<QHBoxLayout *>(QStringLiteral("previewLayout"));
	auto *obsPreview = main->findChild<QWidget *>(QStringLiteral("previewContainer"));
	if (!row)
		return;
	row->addWidget(new TrediaVerticalBox(vertical, obsPreview));
}

void fillMenu(QMainWindow *main, QMenu *menu)
{
	menu->clear();
	if (!main)
		return;
	if (QMenuBar *bar = main->menuBar())
		for (QAction *a : bar->actions())
			if (a->menu() && a->isVisible())
				menu->addAction(a); /* each of OBS's menus, as a submenu */
}

void tintCaption(QMainWindow *main)
{
#ifdef _WIN32
	using SetAttribute = HRESULT(WINAPI *)(HWND, DWORD, LPCVOID, DWORD);
	static const SetAttribute set = []() -> SetAttribute {
		HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
		return dwm ? reinterpret_cast<SetAttribute>(GetProcAddress(dwm, "DwmSetWindowAttribute")) : nullptr;
	}();
	if (!set || !main)
		return;
	const bool dark = obs_frontend_is_theme_dark();
	/* The top bar's own colours (--tr_bar in the themes), so the title bar and the top bar read as one
	 * band. Windows 11; older Windows keeps its usual title bar. */
	const COLORREF caption = dark ? RGB(0x1A, 0x10, 0x33) : RGB(0xF6, 0xF4, 0xFC);
	const COLORREF text = dark ? RGB(0xF5, 0xF1, 0xFF) : RGB(0x1D, 0x12, 0x36);
	const BOOL immersiveDark = dark ? TRUE : FALSE;
	HWND hwnd = reinterpret_cast<HWND>(main->winId());
	set(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &immersiveDark, sizeof(immersiveDark));
	set(hwnd, 34 /* DWMWA_BORDER_COLOR */, &caption, sizeof(caption));
	set(hwnd, 35 /* DWMWA_CAPTION_COLOR */, &caption, sizeof(caption));
	set(hwnd, 36 /* DWMWA_TEXT_COLOR */, &text, sizeof(text));
#else
	(void)main;
#endif
}

} // namespace TrediaWindow
