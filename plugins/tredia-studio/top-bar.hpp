#pragma once

#include <obs-frontend-api.h>

#include <QElapsedTimer>
#include <QPointer>
#include <QTimer>
#include <QToolBar>

class QAction;
class QCheckBox;
class QLabel;
class QMainWindow;
class QPushButton;
class QToolButton;
class TrediaDock;
class TrediaMontage;
class TrediaVertical;

/* Tredia Studio's top bar, as in Tredia's own studio: the logo and name, the menu (OBS's menus),
 * whether you're live (and for how long), the montage, the light/dark switch, studio mode, settings,
 * and the two buttons that matter most: record and go live. */
class TrediaTopBar : public QToolBar {
	Q_OBJECT

public:
	TrediaTopBar(QMainWindow *main, TrediaDock *dock, TrediaVertical *vertical, TrediaMontage *montage);
	void handleEvent(enum obs_frontend_event event);

private:
	QPointer<TrediaDock> dock;
	QPointer<TrediaVertical> vertical;
	QPointer<TrediaMontage> montage;
	/* After a recording stops: "edit it" opens it in the montage (until the next recording). */
	QPushButton *editRecording;
	QAction *editRecordingAction;
	QString lastRecording;
	/* The vertical picture: which previews show (horizontal, both, vertical), and on/off. */
	QWidget *viewBox;
	/* A toolbar shows or hides its widgets through their actions, not the widgets themselves. */
	QAction *viewAction;
	QToolButton *viewButtons[3];
	QCheckBox *verticalSwitch;
	QLabel *status;
	QPushButton *liveButton;
	QPushButton *recordButton;
	QToolButton *menuButton;
	QToolButton *themeButton;
	QToolButton *studioButton;
	QToolButton *settingsButton;
	QToolButton *montageButton;
	QTimer clock;
	QElapsedTimer liveSince;
	QElapsedTimer recordSince;
	bool starting = false;
	bool busy = false;

	void refresh();
	void refreshVertical();
	void triggerMainAction(const char *name);
};
