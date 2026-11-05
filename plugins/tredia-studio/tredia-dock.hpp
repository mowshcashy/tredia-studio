#pragma once

#include "multi-output.hpp"
#include "tredia-api.hpp"

#include <obs-frontend-api.h>

#include <QHash>
#include <QJsonObject>
#include <QPointer>
#include <QTimer>
#include <QWidget>

class QButtonGroup;
class QCheckBox;
class QCompleter;
class QFrame;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QRadioButton;
class QStackedWidget;
class QStringListModel;
class QVBoxLayout;
class TrediaVertical;

/* The "تريديا" panel: sign in, choose Turbo or "from this PC", manage destinations,
 * add the overlays, and everything that happens after a stream or a recording. */
class TrediaDock : public QWidget {
	Q_OBJECT

public:
	explicit TrediaDock(QWidget *parent = nullptr, TrediaVertical *vertical = nullptr);
	void handleEvent(enum obs_frontend_event event);

	/* The top bar's "go live": through Tredia (Turbo), or from this PC straight to the platforms
	 * ticked on Tredia, fetched fresh so nothing has to be typed in here. */
	void goLive();

signals:
	void goLiveBusy(bool busy);

private:
	TrediaApi *api;
	MultiOutput *multi;
	/* The vertical picture: platforms set to vertical get it, from this PC or through Tredia. */
	QPointer<TrediaVertical> vertical;

	QStackedWidget *pages;
	/* signed out */
	QPushButton *signInButton;
	QWidget *codeBox;
	QLabel *codeLabel;
	QPushButton *openLinkButton;
	QLabel *signInError;
	QString verifyUrl;
	/* signed in */
	QLabel *avatarLabel;
	QLabel *helloLabel;
	QLabel *planLabel;
	QLabel *updateLabel;
	QRadioButton *turboRadio;
	QRadioButton *localRadio;
	QFrame *turboCard;
	QFrame *localCard;
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
	/* The stream's title, game and description (as in Tredia's own studio) */
	QLineEdit *infoTitle;
	QLineEdit *infoGame;
	QPlainTextEdit *infoDescription;
	QLabel *infoMessage;
	QPushButton *infoSave;
	QCompleter *gameCompleter;
	QStringListModel *gameModel;
	QTimer gameSearch;
	QHash<QString, QJsonObject> gameResults;
	QJsonObject savedInfo;
	bool infoLoaded = false;
	bool infoSaving = false;

	QJsonObject liveInfo;
	QTimer livePoll;
	bool turbo = true;
	/* While a "from this PC" stream uses Tredia's destinations: the service to put back afterwards. */
	bool directActive = false;
	QJsonObject serviceBeforeDirect;

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
	void updateNote();
	void restoreAfterDirect();
	void startVertical();
	bool wantsVertical() const;
	void buildStreamInfo(QVBoxLayout *layout);
	void loadStreamInfo();
	void updateInfoButton();
	void searchGames();
	void saveStreamInfo();
};
