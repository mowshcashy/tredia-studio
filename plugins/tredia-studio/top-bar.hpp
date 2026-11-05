#pragma once

#include <obs-frontend-api.h>

#include <QElapsedTimer>
#include <QPointer>
#include <QTimer>
#include <QToolBar>

class QCheckBox;
class QLabel;
class QMainWindow;
class QPushButton;
class QToolButton;
class TrediaDock;
class TrediaVertical;

/* Tredia Studio's top bar, as in Tredia's own studio: the logo and name, the menu (OBS's menus),
 * whether you're live (and for how long), the light/dark switch, studio mode, settings, and the two
 * buttons that matter most: record and go live. */
class TrediaTopBar : public QToolBar {
	Q_OBJECT

public:
	TrediaTopBar(QMainWindow *main, TrediaDock *dock, TrediaVertical *vertical);
	void handleEvent(enum obs_frontend_event event);

private:
	QPointer<TrediaDock> dock;
	QPointer<TrediaVertical> vertical;
	/* The vertical picture: which previews show (horizontal, both, vertical), and on/off. */
	QWidget *viewBox;
	QToolButton *viewButtons[3];
	QCheckBox *verticalSwitch;
	QLabel *status;
	QPushButton *liveButton;
	QPushButton *recordButton;
	QToolButton *menuButton;
	QToolButton *themeButton;
	QToolButton *studioButton;
	QToolButton *settingsButton;
	QTimer clock;
	QElapsedTimer liveSince;
	QElapsedTimer recordSince;
	bool starting = false;
	bool busy = false;

	void refresh();
	void refreshVertical();
	void triggerMainAction(const char *name);
};
