#include "tredia-dock.hpp"

#include <obs-module.h>
#include <graphics/vec2.h>
#include <util/bmem.h>
#include <util/platform.h>

#include <algorithm>

#include <QButtonGroup>
#include <QCheckBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QStackedWidget>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>

#define TREDIA_STUDIO_VERSION "1.0.0"

namespace {

const QString warningText = QStringLiteral(
	"بطيء ويضعف الأداء: جهازك يرسل نسخة كاملة لكل منصة، فتحتاج سرعة رفع مضروبة في عدد المنصات، "
	"ويزيد الحمل على جهازك، وأي ضعف في الشبكة يقطّع كل البثوث مع بعض. ننصحك بتيربو.");

QString moduleFile(const char *name)
{
	char *path = obs_module_config_path(name);
	QString result = QString::fromUtf8(path);
	bfree(path);
	return result;
}

QLabel *wrapLabel(const QString &text, const char *objectName = nullptr)
{
	auto *label = new QLabel(text);
	label->setWordWrap(true);
	label->setTextFormat(Qt::PlainText);
	if (objectName)
		label->setObjectName(objectName);
	return label;
}

QFrame *card(QWidget *parent = nullptr)
{
	auto *frame = new QFrame(parent);
	frame->setObjectName("trediaCard");
	frame->setFrameShape(QFrame::StyledPanel);
	return frame;
}

void openUrl(const QString &url)
{
	if (!url.isEmpty())
		QDesktopServices::openUrl(QUrl(url));
}

QString platformName(const QString &p)
{
	if (p == QStringLiteral("youtube"))
		return QStringLiteral("يوتيوب");
	if (p == QStringLiteral("twitch"))
		return QStringLiteral("تويتش");
	if (p == QStringLiteral("kick"))
		return QStringLiteral("كيك");
	if (p == QStringLiteral("tiktok"))
		return QStringLiteral("تيك توك");
	return QStringLiteral("RTMP");
}

QString targetStatus(const QString &s)
{
	if (s == QStringLiteral("live"))
		return QStringLiteral("مباشر");
	if (s == QStringLiteral("starting"))
		return QStringLiteral("يبدأ…");
	if (s == QStringLiteral("failed"))
		return QStringLiteral("فيه مشكلة");
	return QStringLiteral("متوقف");
}

} // namespace

TrediaDock::TrediaDock(QWidget *parent) : QWidget(parent), api(new TrediaApi(this)), multi(new MultiOutput(this))
{
	setObjectName("trediaDock");
	setMinimumWidth(280);

	auto *outer = new QVBoxLayout(this);
	outer->setContentsMargins(0, 0, 0, 0);
	auto *scroll = new QScrollArea(this);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	outer->addWidget(scroll);

	pages = new QStackedWidget;
	scroll->setWidget(pages);

	auto *signedOut = new QWidget;
	auto *signedIn = new QWidget;
	buildSignedOut(signedOut);
	buildSignedIn(signedIn);
	pages->addWidget(signedOut);
	pages->addWidget(signedIn);

	loadMode();

	connect(api, &TrediaApi::linkCode, this, [this](const QString &code, const QString &url) {
		verifyUrl = url;
		codeLabel->setText(code);
		codeBox->setVisible(true);
		signInButton->setVisible(false);
		signInError->clear();
		openUrl(url);
	});
	connect(api, &TrediaApi::linkFailed, this, [this](const QString &reason) {
		codeBox->setVisible(false);
		signInButton->setVisible(true);
		signInError->setText(reason);
	});
	connect(api, &TrediaApi::signedInChanged, this, [this]() { refreshAll(); });
	connect(multi, &MultiOutput::statusChanged, this, [this](const QString &id, const QString &status) {
		if (auto *label = findChild<QLabel *>(QStringLiteral("local-status-") + id))
			label->setText(status);
	});

	livePoll.setInterval(10000);
	connect(&livePoll, &QTimer::timeout, this, &TrediaDock::refreshLive);

	refreshAll();
	checkForUpdate();
}

/* ---- Signed out: device sign-in --------------------------------------------------------------- */

void TrediaDock::buildSignedOut(QWidget *page)
{
	auto *layout = new QVBoxLayout(page);
	layout->setContentsMargins(12, 12, 12, 12);
	layout->setSpacing(10);

	auto *title = new QLabel(QStringLiteral("اربط حسابك في تريديا"));
	title->setObjectName("trediaTitle");
	layout->addWidget(title);
	layout->addWidget(wrapLabel(QStringLiteral(
		"عشان تبث على يوتيوب وتويتش وكيك وتيك توك بضغطة وحدة (تيربو)، وتشوف تقرير بثك، وترفع تسجيلاتك لمكتبتك.")));

	signInButton = new QPushButton(QStringLiteral("سجّل دخولك"));
	signInButton->setObjectName("trediaPrimary");
	connect(signInButton, &QPushButton::clicked, this, [this]() {
		signInError->clear();
		api->startLink();
	});
	layout->addWidget(signInButton);

	codeBox = card();
	auto *codeLayout = new QVBoxLayout(codeBox);
	codeLayout->addWidget(wrapLabel(QStringLiteral("فتحنا لك صفحة في المتصفح. تأكد إن الرمز نفسه واضغط «اسمح»:")));
	codeLabel = new QLabel;
	codeLabel->setObjectName("trediaCode");
	codeLabel->setAlignment(Qt::AlignCenter);
	codeLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
	codeLayout->addWidget(codeLabel);
	openLinkButton = new QPushButton(QStringLiteral("افتح الصفحة مرة ثانية"));
	connect(openLinkButton, &QPushButton::clicked, this, [this]() { openUrl(verifyUrl); });
	codeLayout->addWidget(openLinkButton);
	auto *cancel = new QPushButton(QStringLiteral("إلغاء"));
	connect(cancel, &QPushButton::clicked, this, [this]() {
		api->cancelLink();
		codeBox->setVisible(false);
		signInButton->setVisible(true);
	});
	codeLayout->addWidget(cancel);
	codeBox->setVisible(false);
	layout->addWidget(codeBox);

	signInError = wrapLabel(QString(), "trediaError");
	layout->addWidget(signInError);

	auto *noAccount = new QPushButton(QStringLiteral("ما عندك حساب؟ سجّل مجانًا"));
	noAccount->setFlat(true);
	connect(noAccount, &QPushButton::clicked, this, [this]() { openUrl(api->baseUrl() + QStringLiteral("/sign-up")); });
	layout->addWidget(noAccount);
	layout->addStretch();
}

/* ---- Signed in -------------------------------------------------------------------------------- */

void TrediaDock::buildSignedIn(QWidget *page)
{
	auto *layout = new QVBoxLayout(page);
	layout->setContentsMargins(12, 12, 12, 12);
	layout->setSpacing(10);

	updateLabel = new QLabel;
	updateLabel->setObjectName("trediaUpdate");
	updateLabel->setWordWrap(true);
	updateLabel->setOpenExternalLinks(true);
	updateLabel->setVisible(false);
	layout->addWidget(updateLabel);

	auto *head = new QHBoxLayout;
	helloLabel = new QLabel;
	helloLabel->setObjectName("trediaTitle");
	head->addWidget(helloLabel, 1);
	auto *signOut = new QPushButton(QStringLiteral("خروج"));
	signOut->setFlat(true);
	connect(signOut, &QPushButton::clicked, this, [this]() {
		if (QMessageBox::question(this, QStringLiteral("تسجيل الخروج"), QStringLiteral("تطلع من حسابك في تريديا على هذا الجهاز؟")) ==
		    QMessageBox::Yes)
			api->signOut();
	});
	head->addWidget(signOut);
	layout->addLayout(head);
	planLabel = wrapLabel(QString(), "trediaMuted");
	layout->addWidget(planLabel);

	/* How to multistream: Turbo (Tredia's servers) or from this PC. */
	auto *modeTitle = new QLabel(QStringLiteral("كيف تبث على أكثر من منصة؟"));
	modeTitle->setObjectName("trediaHeading");
	layout->addWidget(modeTitle);

	auto *modes = new QButtonGroup(this);
	auto *turboCard = card();
	auto *turboLayout = new QVBoxLayout(turboCard);
	turboRadio = new QRadioButton(QStringLiteral("تيربو من تريديا (ننصح فيه)"));
	turboRadio->setObjectName("trediaModeTurbo");
	turboLayout->addWidget(turboRadio);
	turboLayout->addWidget(wrapLabel(QStringLiteral(
					       "جهازك يرسل بث واحد لتريديا، وسيرفراتنا توصله لكل منصاتك. أخف على جهازك وشبكتك، ومعه الشات الموحد والتقرير."),
				       "trediaMuted"));
	layout->addWidget(turboCard);

	auto *localCard = card();
	auto *localLayout = new QVBoxLayout(localCard);
	localRadio = new QRadioButton(QStringLiteral("من جهازك"));
	localLayout->addWidget(localRadio);
	localLayout->addWidget(wrapLabel(QStringLiteral("تبث لأكثر من جهة من جهازك مباشرة، بدون تريديا."), "trediaMuted"));
	localLayout->addWidget(wrapLabel(warningText, "trediaWarning"));
	layout->addWidget(localCard);

	modes->addButton(turboRadio);
	modes->addButton(localRadio);
	connect(turboRadio, &QRadioButton::toggled, this, [this](bool on) {
		if (on != turbo)
			setTurbo(on, true);
	});

	/* Turbo panel */
	turboPanel = new QWidget;
	auto *turboBox = new QVBoxLayout(turboPanel);
	turboBox->setContentsMargins(0, 0, 0, 0);
	auto *destTitle = new QLabel(QStringLiteral("منصاتك في هذا البث"));
	destTitle->setObjectName("trediaHeading");
	turboBox->addWidget(destTitle);
	destinationsBox = new QVBoxLayout;
	turboBox->addLayout(destinationsBox);
	auto *manage = new QPushButton(QStringLiteral("أضف منصة أو اربط حساب"));
	connect(manage, &QPushButton::clicked, this, [this]() { openUrl(api->baseUrl() + QStringLiteral("/live")); });
	turboBox->addWidget(manage);
	turboNote = wrapLabel(QString(), "trediaMuted");
	turboBox->addWidget(turboNote);
	liveStatus = wrapLabel(QString());
	liveStatus->setObjectName("trediaLive");
	liveStatus->setVisible(false);
	turboBox->addWidget(liveStatus);
	auto *links = new QHBoxLayout;
	dashboardButton = new QPushButton(QStringLiteral("لوحة البث والشات"));
	connect(dashboardButton, &QPushButton::clicked, this, [this]() {
		const QJsonObject s = liveInfo.value(QStringLiteral("session")).toObject();
		openUrl(s.isEmpty() ? api->baseUrl() + QStringLiteral("/live") : s.value(QStringLiteral("dashboardUrl")).toString());
	});
	reportButton = new QPushButton(QStringLiteral("آخر تقرير"));
	connect(reportButton, &QPushButton::clicked, this, [this]() {
		openUrl(liveInfo.value(QStringLiteral("lastSession")).toObject().value(QStringLiteral("reportUrl")).toString());
	});
	links->addWidget(dashboardButton);
	links->addWidget(reportButton);
	turboBox->addLayout(links);
	layout->addWidget(turboPanel);

	/* From this PC panel */
	localPanel = new QWidget;
	auto *localBox = new QVBoxLayout(localPanel);
	localBox->setContentsMargins(0, 0, 0, 0);
	auto *localTitle = new QLabel(QStringLiteral("جهات إضافية"));
	localTitle->setObjectName("trediaHeading");
	localBox->addWidget(localTitle);
	localBox->addWidget(wrapLabel(QStringLiteral("البث الرئيسي يروح للي في الإعدادات ← البث. وهذي تبدأ معه وتوقف معه:"),
				      "trediaMuted"));
	localList = new QVBoxLayout;
	localBox->addLayout(localList);
	auto *form = new QFormLayout;
	localName = new QLineEdit;
	localName->setPlaceholderText(QStringLiteral("مثال: كيك"));
	localServer = new QLineEdit;
	localServer->setPlaceholderText(QStringLiteral("rtmp://…"));
	localServer->setLayoutDirection(Qt::LeftToRight);
	localKey = new QLineEdit;
	localKey->setEchoMode(QLineEdit::Password);
	localKey->setLayoutDirection(Qt::LeftToRight);
	form->addRow(QStringLiteral("الاسم"), localName);
	form->addRow(QStringLiteral("السيرفر"), localServer);
	form->addRow(QStringLiteral("مفتاح البث"), localKey);
	localBox->addLayout(form);
	localError = wrapLabel(QString(), "trediaError");
	localBox->addWidget(localError);
	auto *addLocal = new QPushButton(QStringLiteral("أضف الجهة"));
	connect(addLocal, &QPushButton::clicked, this, [this]() {
		const QString server = localServer->text().trimmed();
		if (!server.startsWith(QStringLiteral("rtmp://")) && !server.startsWith(QStringLiteral("rtmps://"))) {
			localError->setText(QStringLiteral("السيرفر لازم يبدأ بـ rtmp:// أو rtmps://"));
			return;
		}
		if (localKey->text().trimmed().isEmpty()) {
			localError->setText(QStringLiteral("اكتب مفتاح البث."));
			return;
		}
		QList<LocalTarget> list = multi->targets();
		LocalTarget t;
		t.id = QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
		t.name = localName->text().trimmed().isEmpty() ? QStringLiteral("جهة %1").arg(list.size() + 1) : localName->text().trimmed();
		t.server = server;
		t.key = localKey->text().trimmed();
		list.append(t);
		multi->setTargets(list);
		localName->clear();
		localServer->clear();
		localKey->clear();
		localError->clear();
		renderLocalTargets();
	});
	localBox->addWidget(addLocal);
	layout->addWidget(localPanel);

	/* Overlays */
	auto *overlayTitle = new QLabel(QStringLiteral("إضافات البث"));
	overlayTitle->setObjectName("trediaHeading");
	layout->addWidget(overlayTitle);
	auto *overlays = new QPushButton(QStringLiteral("أضف الشات والتنبيهات وعدّاد المشاهدين لمشهدك"));
	connect(overlays, &QPushButton::clicked, this, &TrediaDock::addOverlays);
	layout->addWidget(overlays);

	/* Recordings */
	auto *recTitle = new QLabel(QStringLiteral("تسجيلاتك"));
	recTitle->setObjectName("trediaHeading");
	layout->addWidget(recTitle);
	layout->addWidget(wrapLabel(QStringLiteral("أول ما توقف التسجيل نسألك ترفعه لمكتبتك في تريديا، وتنشره على يوتيوب وتيك توك من هناك."),
				    "trediaMuted"));
	auto *pick = new QPushButton(QStringLiteral("ارفع تسجيل من جهازك…"));
	connect(pick, &QPushButton::clicked, this, [this]() {
		const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("اختر التسجيل"), QString(),
								  QStringLiteral("فيديو (*.mp4 *.mov *.m4v)"));
		if (!path.isEmpty())
			startUpload(path, QFileInfo(path).completeBaseName());
	});
	layout->addWidget(pick);
	uploadLabel = wrapLabel(QString(), "trediaMuted");
	uploadLabel->setVisible(false);
	layout->addWidget(uploadLabel);
	uploadBar = new QProgressBar;
	uploadBar->setRange(0, 1000);
	uploadBar->setVisible(false);
	layout->addWidget(uploadBar);

	auto *site = new QPushButton(QStringLiteral("افتح لوحتي في تريديا"));
	site->setFlat(true);
	connect(site, &QPushButton::clicked, this, [this]() { openUrl(api->baseUrl() + QStringLiteral("/creator")); });
	layout->addWidget(site);
	layout->addStretch();
}

/* ---- Data ------------------------------------------------------------------------------------- */

void TrediaDock::refreshAll()
{
	if (!api->signedIn()) {
		pages->setCurrentIndex(0);
		codeBox->setVisible(false);
		signInButton->setVisible(true);
		livePoll.stop();
		return;
	}
	pages->setCurrentIndex(1);
	helloLabel->setText(QStringLiteral("أهلًا %1").arg(api->userName()));
	api->get(QStringLiteral("/api/app/me"), [this](const QJsonObject &me, int, const QString &error) {
		if (!error.isEmpty()) {
			planLabel->setText(error);
			return;
		}
		const QJsonObject plan = me.value(QStringLiteral("plan")).toObject();
		const int hoursLeft = static_cast<int>(plan.value(QStringLiteral("remainingSeconds")).toDouble() / 3600.0);
		const QJsonValue max = plan.value(QStringLiteral("maxPlatforms"));
		const QString platforms = max.isNull() ? QStringLiteral("كل المنصات") : QStringLiteral("حتى %1 منصات").arg(max.toInt());
		planLabel->setText(QStringLiteral("باقة %1: %2، وباقي لك %3 ساعة بث هالشهر.")
					   .arg(plan.value(QStringLiteral("name")).toString(), platforms)
					   .arg(hoursLeft));
	});
	renderLocalTargets();
	refreshLive();
}

void TrediaDock::refreshLive()
{
	if (!api->signedIn())
		return;
	api->get(QStringLiteral("/api/app/live"), [this](const QJsonObject &data, int, const QString &error) {
		if (!error.isEmpty()) {
			turboNote->setText(error);
			return;
		}
		const bool first = liveInfo.isEmpty();
		liveInfo = data;
		renderDestinations();
		reportButton->setEnabled(!data.value(QStringLiteral("lastSession")).toObject().isEmpty());

		const QJsonObject session = data.value(QStringLiteral("session")).toObject();
		if (session.isEmpty()) {
			liveStatus->setVisible(false);
		} else {
			QStringList lines;
			lines << (session.value(QStringLiteral("status")).toString() == QStringLiteral("live") ? QStringLiteral("أنت مباشر عبر تريديا")
														: QStringLiteral("تريديا تستقبل بثك…"));
			for (const QJsonValue &v : session.value(QStringLiteral("targets")).toArray()) {
				const QJsonObject t = v.toObject();
				QString line = platformName(t.value(QStringLiteral("platform")).toString()) + QStringLiteral(": ") +
					       targetStatus(t.value(QStringLiteral("status")).toString());
				const QString err = t.value(QStringLiteral("error")).toString();
				if (!err.isEmpty())
					line += QStringLiteral(" (") + err + QStringLiteral(")");
				lines << line;
			}
			liveStatus->setText(lines.join('\n'));
			liveStatus->setVisible(true);
		}
		/* First time we know the ingest: in Turbo, point OBS at Tredia. */
		if (first && turbo)
			applyTurboService();
	});
}

void TrediaDock::renderDestinations()
{
	while (QLayoutItem *item = destinationsBox->takeAt(0)) {
		delete item->widget();
		delete item;
	}
	const QJsonArray dests = liveInfo.value(QStringLiteral("destinations")).toArray();
	if (dests.isEmpty()) {
		destinationsBox->addWidget(wrapLabel(QStringLiteral("ما فيه منصات بعد. اربط يوتيوب أو تويتش أو كيك من الموقع."), "trediaMuted"));
	}
	for (const QJsonValue &v : dests) {
		const QJsonObject d = v.toObject();
		auto *box = new QCheckBox(d.value(QStringLiteral("label")).toString() + QStringLiteral(" · ") +
					  platformName(d.value(QStringLiteral("platform")).toString()));
		box->setChecked(d.value(QStringLiteral("enabled")).toBool());
		const QString id = d.value(QStringLiteral("id")).toString();
		QPointer<QCheckBox> guard(box);
		connect(box, &QCheckBox::toggled, this, [this, id, guard](bool on) {
			QCheckBox *box = guard;
			if (!box)
				return;
			QJsonObject body;
			body.insert(QStringLiteral("id"), id);
			body.insert(QStringLiteral("enabled"), on);
			box->setEnabled(false);
			api->post(QStringLiteral("/api/app/live/destinations"), body,
				  [this, guard, on](const QJsonObject &, int, const QString &error) {
					  QCheckBox *box = guard;
					  if (!box)
						  return;
					  box->setEnabled(true);
					  if (!error.isEmpty()) {
						  QSignalBlocker block(box);
						  box->setChecked(!on);
						  turboNote->setText(error);
					  }
				  });
		});
		destinationsBox->addWidget(box);
	}
	const QJsonValue max = liveInfo.value(QStringLiteral("maxPlatforms"));
	turboNote->setText(max.isNull() ? QStringLiteral("باقتك تبث على كل المنصات مع بعض.")
					: QStringLiteral("باقتك تبث على %1 منصات مع بعض. تبي أكثر؟ رقّ باقتك من الموقع.").arg(max.toInt()));
}

void TrediaDock::renderLocalTargets()
{
	while (QLayoutItem *item = localList->takeAt(0)) {
		delete item->widget();
		delete item;
	}
	const QList<LocalTarget> list = multi->targets();
	if (list.isEmpty())
		localList->addWidget(wrapLabel(QStringLiteral("ما أضفت جهات بعد."), "trediaMuted"));
	for (const LocalTarget &t : list) {
		auto *row = new QWidget;
		auto *h = new QHBoxLayout(row);
		h->setContentsMargins(0, 0, 0, 0);
		auto *box = new QCheckBox(t.name);
		box->setChecked(t.enabled);
		const QString id = t.id;
		connect(box, &QCheckBox::toggled, this, [this, id](bool on) {
			QList<LocalTarget> l = multi->targets();
			for (LocalTarget &x : l)
				if (x.id == id)
					x.enabled = on;
			multi->setTargets(l);
		});
		h->addWidget(box, 1);
		auto *status = new QLabel;
		status->setObjectName(QStringLiteral("local-status-") + t.id);
		h->addWidget(status);
		auto *remove = new QPushButton(QStringLiteral("حذف"));
		remove->setFlat(true);
		connect(remove, &QPushButton::clicked, this, [this, id]() {
			QList<LocalTarget> l = multi->targets();
			l.erase(std::remove_if(l.begin(), l.end(), [&](const LocalTarget &x) { return x.id == id; }), l.end());
			multi->setTargets(l);
			QTimer::singleShot(0, this, &TrediaDock::renderLocalTargets);
		});
		h->addWidget(remove);
		localList->addWidget(row);
	}
}

/* ---- Turbo vs from this PC -------------------------------------------------------------------- */

void TrediaDock::setTurbo(bool on, bool applySettings)
{
	if (obs_frontend_streaming_active() && applySettings) {
		QMessageBox::information(this, QStringLiteral("تريديا"), QStringLiteral("وقّف البث أول، بعدها غيّر الطريقة."));
		QSignalBlocker a(turboRadio), b(localRadio);
		(turbo ? turboRadio : localRadio)->setChecked(true);
		return;
	}
	turbo = on;
	{
		QSignalBlocker a(turboRadio), b(localRadio);
		turboRadio->setChecked(on);
		localRadio->setChecked(!on);
	}
	turboPanel->setVisible(on);
	localPanel->setVisible(!on);
	saveMode();
	if (applySettings) {
		if (on)
			applyTurboService();
		else
			restoreOwnService();
	}
}

/* Turbo: Settings → Stream points at Tredia. What was there before is kept and comes back in "from this PC". */
void TrediaDock::applyTurboService()
{
	const QJsonObject ingest = liveInfo.value(QStringLiteral("ingest")).toObject();
	const QString server = ingest.value(QStringLiteral("server")).toString();
	const QString key = ingest.value(QStringLiteral("key")).toString();
	if (server.isEmpty() || key.isEmpty() || obs_frontend_streaming_active())
		return;

	obs_service_t *current = obs_frontend_get_streaming_service();
	if (current) {
		obs_data_t *s = obs_service_get_settings(current);
		const char *server_now = obs_data_get_string(s, "server");
		const bool alreadyTredia = server_now && QString::fromUtf8(server_now) == server;
		if (!alreadyTredia) {
			QJsonObject backup;
			backup.insert(QStringLiteral("type"), QString::fromUtf8(obs_service_get_type(current)));
			backup.insert(QStringLiteral("settings"), QString::fromUtf8(obs_data_get_json(s)));
			QSaveFile file(moduleFile("own-service.json"));
			char *dir = obs_module_config_path("");
			os_mkdirs(dir);
			bfree(dir);
			if (file.open(QIODevice::WriteOnly)) {
				file.write(QJsonDocument(backup).toJson());
				file.commit();
			}
		}
		obs_data_release(s);
		if (alreadyTredia)
			return;
	}

	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "server", server.toUtf8().constData());
	obs_data_set_string(settings, "key", key.toUtf8().constData());
	obs_service_t *service = obs_service_create("rtmp_custom", "default_service", settings, nullptr);
	obs_data_release(settings);
	if (service) {
		obs_frontend_set_streaming_service(service);
		obs_frontend_save_streaming_service();
		obs_service_release(service);
	}
}

void TrediaDock::restoreOwnService()
{
	QFile file(moduleFile("own-service.json"));
	if (!file.open(QIODevice::ReadOnly) || obs_frontend_streaming_active())
		return;
	const QJsonObject backup = QJsonDocument::fromJson(file.readAll()).object();
	const QByteArray type = backup.value(QStringLiteral("type")).toString().toUtf8();
	const QByteArray json = backup.value(QStringLiteral("settings")).toString().toUtf8();
	if (type.isEmpty())
		return;
	obs_data_t *settings = obs_data_create_from_json(json.constData());
	obs_service_t *service = obs_service_create(type.constData(), "default_service", settings, nullptr);
	obs_data_release(settings);
	if (service) {
		obs_frontend_set_streaming_service(service);
		obs_frontend_save_streaming_service();
		obs_service_release(service);
	}
}

void TrediaDock::saveMode() const
{
	char *dir = obs_module_config_path("");
	os_mkdirs(dir);
	bfree(dir);
	QSaveFile file(moduleFile("mode.json"));
	if (file.open(QIODevice::WriteOnly)) {
		QJsonObject o;
		o.insert(QStringLiteral("turbo"), turbo);
		file.write(QJsonDocument(o).toJson());
		file.commit();
	}
}

void TrediaDock::loadMode()
{
	QFile file(moduleFile("mode.json"));
	bool on = true;
	if (file.open(QIODevice::ReadOnly))
		on = QJsonDocument::fromJson(file.readAll()).object().value(QStringLiteral("turbo")).toBool(true);
	setTurbo(on, false);
}

/* ---- Overlays --------------------------------------------------------------------------------- */

void TrediaDock::addOverlays()
{
	const QJsonObject overlays = liveInfo.value(QStringLiteral("overlays")).toObject();
	if (overlays.isEmpty()) {
		QMessageBox::information(this, QStringLiteral("تريديا"), QStringLiteral("انتظر ثواني لين نجيب روابطك من تريديا، وجرّب مرة ثانية."));
		return;
	}
	obs_source_t *sceneSource = obs_frontend_get_current_scene();
	obs_scene_t *scene = obs_scene_from_source(sceneSource);
	if (!scene) {
		obs_source_release(sceneSource);
		return;
	}
	obs_video_info ovi{};
	obs_get_video_info(&ovi);
	const int w = ovi.base_width ? static_cast<int>(ovi.base_width) : 1920;
	const int h = ovi.base_height ? static_cast<int>(ovi.base_height) : 1080;

	struct Item {
		const char *name;
		QString url;
		int width, height;
		float x, y;
	};
	const Item items[] = {
		{"تريديا: التنبيهات", overlays.value(QStringLiteral("alerts")).toString(), 800, 320, (w - 800) / 2.0f, 40.0f},
		{"تريديا: الشات", overlays.value(QStringLiteral("chat")).toString(), 420, 600, 24.0f, static_cast<float>(h - 624)},
		{"تريديا: المشاهدين", overlays.value(QStringLiteral("viewers")).toString(), 280, 80, static_cast<float>(w - 304), 24.0f},
	};
	for (const Item &it : items) {
		obs_source_t *source = obs_get_source_by_name(it.name);
		if (!source) {
			obs_data_t *s = obs_data_create();
			obs_data_set_string(s, "url", it.url.toUtf8().constData());
			obs_data_set_int(s, "width", it.width);
			obs_data_set_int(s, "height", it.height);
			source = obs_source_create("browser_source", it.name, s, nullptr);
			obs_data_release(s);
		}
		if (source && !obs_scene_find_source(scene, it.name)) {
			obs_sceneitem_t *item = obs_scene_add(scene, source);
			vec2 pos;
			vec2_set(&pos, it.x, it.y);
			obs_sceneitem_set_pos(item, &pos);
		}
		obs_source_release(source);
	}
	obs_source_release(sceneSource);
	QMessageBox::information(this, QStringLiteral("تريديا"),
				 QStringLiteral("أضفنا الشات والتنبيهات وعدّاد المشاهدين لمشهدك. حرّكها وكبّرها مثل ما تبي."));
}

/* ---- After a stream / a recording --------------------------------------------------------------- */

void TrediaDock::afterStream()
{
	if (!api->signedIn() || !turbo)
		return;
	/* Give Tredia a few seconds to close the session and start the report. */
	QTimer::singleShot(6000, this, [this]() {
		api->get(QStringLiteral("/api/app/live"), [this](const QJsonObject &data, int, const QString &error) {
			if (!error.isEmpty())
				return;
			liveInfo = data;
			const QJsonObject last = data.value(QStringLiteral("lastSession")).toObject();
			if (last.isEmpty())
				return;
			QMessageBox box(this);
			box.setWindowTitle(QStringLiteral("خلص بثك"));
			box.setText(QStringLiteral("خلص بثك 🎉"));
			box.setInformativeText(QStringLiteral(
				"تقريرك في تريديا: أعلى مشاهدة، وأفضل لحظة، ومقارنة المنصات، والشات. ومنه تقص مقاطع لشورتس وتيك توك."));
			QPushButton *open = box.addButton(QStringLiteral("افتح التقرير"), QMessageBox::AcceptRole);
			box.addButton(QStringLiteral("بعدين"), QMessageBox::RejectRole);
			box.exec();
			if (box.clickedButton() == open)
				openUrl(last.value(QStringLiteral("reportUrl")).toString());
		});
	});
}

void TrediaDock::afterRecording()
{
	char *last = obs_frontend_get_last_recording();
	const QString path = last ? QString::fromUtf8(last) : QString();
	bfree(last);
	if (path.isEmpty() || !QFileInfo::exists(path))
		return;

	QDialog dialog(this);
	dialog.setWindowTitle(QStringLiteral("خلص التسجيل"));
	auto *layout = new QVBoxLayout(&dialog);
	layout->addWidget(wrapLabel(QStringLiteral("ترفعه لمكتبتك في تريديا؟ من هناك تنشره على يوتيوب وتيك توك أو تجدوله.")));
	auto *title = new QLineEdit(QFileInfo(path).completeBaseName());
	layout->addWidget(new QLabel(QStringLiteral("العنوان")));
	layout->addWidget(title);
	const QString ext = QFileInfo(path).suffix().toLower();
	const bool supported = ext == QStringLiteral("mp4") || ext == QStringLiteral("mov") || ext == QStringLiteral("m4v");
	if (!supported)
		layout->addWidget(wrapLabel(QStringLiteral("التسجيل بصيغة ") + ext.toUpper() +
						    QStringLiteral(". حوّله لـ MP4 من «ملف ← إعادة التغليف»، أو غيّر صيغة التسجيل لـ MP4 من الإعدادات ← الإخراج."),
					    "trediaWarning"));
	if (!api->signedIn())
		layout->addWidget(wrapLabel(QStringLiteral("سجّل دخولك في تريديا من لوحة «تريديا» عشان ترفعه."), "trediaWarning"));
	auto *buttons = new QDialogButtonBox;
	QPushButton *upload = buttons->addButton(QStringLiteral("ارفعه لتريديا"), QDialogButtonBox::AcceptRole);
	buttons->addButton(QStringLiteral("لا، شكرًا"), QDialogButtonBox::RejectRole);
	upload->setEnabled(supported && api->signedIn());
	connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
	layout->addWidget(buttons);
	if (dialog.exec() == QDialog::Accepted)
		startUpload(path, title->text().trimmed().isEmpty() ? QFileInfo(path).completeBaseName() : title->text().trimmed());
}

void TrediaDock::startUpload(const QString &path, const QString &title)
{
	if (!api->signedIn())
		return;
	uploadLabel->setText(QStringLiteral("يرفع «%1»…").arg(title));
	uploadLabel->setVisible(true);
	uploadBar->setValue(0);
	uploadBar->setVisible(true);
	api->uploadRecording(
		path, title,
		[this](qint64 sent, qint64 total) {
			uploadBar->setValue(total > 0 ? static_cast<int>(sent * 1000 / total) : 0);
		},
		[this, title](const QString &url, const QString &error) {
			uploadBar->setVisible(false);
			if (!error.isEmpty()) {
				uploadLabel->setText(error);
				return;
			}
			uploadLabel->setText(QStringLiteral("وصل «%1» لمكتبتك.").arg(title));
			QMessageBox box(this);
			box.setWindowTitle(QStringLiteral("تريديا"));
			box.setText(QStringLiteral("وصل التسجيل لمكتبتك في تريديا."));
			box.setInformativeText(QStringLiteral("افتحه واختر وين تنشره: يوتيوب أو تيك توك، الحين أو بموعد."));
			QPushButton *open = box.addButton(QStringLiteral("افتحه في تريديا"), QMessageBox::AcceptRole);
			box.addButton(QStringLiteral("بعدين"), QMessageBox::RejectRole);
			box.exec();
			if (box.clickedButton() == open)
				openUrl(url);
		});
}

/* ---- Updates ---------------------------------------------------------------------------------- */

void TrediaDock::checkForUpdate()
{
	api->get(QStringLiteral("/api/app/version"), [this](const QJsonObject &data, int, const QString &error) {
		const QString latest = data.value(QStringLiteral("version")).toString();
		if (!error.isEmpty() || latest.isEmpty() || latest == QStringLiteral(TREDIA_STUDIO_VERSION))
			return;
		const QStringList now = QStringLiteral(TREDIA_STUDIO_VERSION).split('.');
		const QStringList next = latest.split('.');
		bool newer = false;
		for (int i = 0; i < 3; ++i) {
			const int a = i < now.size() ? now[i].toInt() : 0;
			const int b = i < next.size() ? next[i].toInt() : 0;
			if (a != b) {
				newer = b > a;
				break;
			}
		}
		if (!newer)
			return;
		updateLabel->setText(QStringLiteral("نسخة جديدة من تريديا ستوديو (%1). <a href=\"%2\">نزّلها</a>")
					     .arg(latest.toHtmlEscaped(), data.value(QStringLiteral("url")).toString().toHtmlEscaped()));
		updateLabel->setTextFormat(Qt::RichText);
		updateLabel->setVisible(true);
	});
}

/* ---- OBS events ------------------------------------------------------------------------------- */

void TrediaDock::handleEvent(enum obs_frontend_event event)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_STREAMING_STARTED:
		if (turbo) {
			if (api->signedIn())
				livePoll.start();
			QTimer::singleShot(3000, this, &TrediaDock::refreshLive);
		} else {
			QString error;
			multi->startAll(error);
			if (!error.isEmpty())
				localError->setText(error);
		}
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STOPPING:
		multi->stopAll();
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STOPPED:
		livePoll.stop();
		liveStatus->setVisible(false);
		afterStream();
		break;
	case OBS_FRONTEND_EVENT_RECORDING_STOPPED:
		QTimer::singleShot(500, this, &TrediaDock::afterRecording);
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		livePoll.stop();
		multi->stopAll();
		break;
	default:
		break;
	}
}
