#pragma once

#include "multi-output.hpp"
#include "tredia-api.hpp"

#include <obs-frontend-api.h>

#include <QJsonObject>
#include <QPointer>
#include <QTimer>
#include <QWidget>

class QButtonGroup;
class QCheckBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QRadioButton;
class QStackedWidget;
class QVBoxLayout;

/* The "تريديا" panel: sign in, choose Turbo or "from this PC", manage destinations,
 * add the overlays, and everything that happens after a stream or a recording. */
class TrediaDock : public QWidget {
	Q_OBJECT

public:
	explicit TrediaDock(QWidget *parent = nullptr);
	void handleEvent(enum obs_frontend_event event);

private:
	TrediaApi *api;
	MultiOutput *multi;

	QStackedWidget *pages;
	/* signed out */
	QPushButton *signInButton;
	QWidget *codeBox;
	QLabel *codeLabel;
	QPushButton *openLinkButton;
	QLabel *signInError;
	QString verifyUrl;
	/* signed in */
	QLabel *helloLabel;
	QLabel *planLabel;
	QLabel *updateLabel;
	QRadioButton *turboRadio;
	QRadioButton *localRadio;
	QWidget *turboPanel;
	QWidget *localPanel;
	QVBoxLayout *destinationsBox;
	QLabel *turboNote;
	QLabel *liveStatus;
	QPushButton *dashboardButton;
	QPushButton *reportButton;
	QVBoxLayout *localList;
	QLineEdit *localName;
	QLineEdit *localServer;
	QLineEdit *localKey;
	QLabel *localError;
	QLabel *uploadLabel;
	QProgressBar *uploadBar;

	QJsonObject liveInfo;
	QTimer livePoll;
	bool turbo = true;

	void buildSignedOut(QWidget *page);
	void buildSignedIn(QWidget *page);
	void refreshAll();
	void refreshLive();
	void renderDestinations();
	void renderLocalTargets();
	void setTurbo(bool on, bool applySettings);
	void applyTurboService();
	void restoreOwnService();
	void addOverlays();
	void afterStream();
	void afterRecording();
	void startUpload(const QString &path, const QString &title);
	void checkForUpdate();
	void saveMode() const;
	void loadMode();
};
