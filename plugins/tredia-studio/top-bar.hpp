#pragma once

#include <obs-frontend-api.h>

#include <QElapsedTimer>
#include <QPointer>
#include <QTimer>
#include <QToolBar>

class QLabel;
class QMainWindow;
class QPushButton;
class QToolButton;
class TrediaDock;

/* Tredia Studio's top bar: the logo, whether you're live (and for how long), the light/dark switch,
 * studio mode, settings, and the two buttons that matter most: go live and record. */
class TrediaTopBar : public QToolBar {
	Q_OBJECT

public:
	TrediaTopBar(QMainWindow *main, TrediaDock *dock);
	void handleEvent(enum obs_frontend_event event);

private:
	QPointer<TrediaDock> dock;
	QLabel *status;
	QPushButton *liveButton;
	QPushButton *recordButton;
	QToolButton *themeButton;
	QToolButton *studioButton;
	QToolButton *settingsButton;
	QTimer clock;
	QElapsedTimer liveSince;
	QElapsedTimer recordSince;
	bool starting = false;
	bool busy = false;

	void refresh();
	void triggerMainAction(const char *name);
};
