#include "tredia-dock.hpp"

#include <obs-module.h>
#include <graphics/vec2.h>
#include <util/bmem.h>
#include <util/platform.h>

#include <algorithm>
#include <functional>

#include <QButtonGroup>
#include <QEvent>
#include <QIcon>
#include <QMouseEvent>
#include <QStyle>
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
	return frame;
}

QLabel *heading(const QString &text)
{
	auto *label = new QLabel(text);
	label->setObjectName("trediaHeading");
	return label;
}

/* The theme draws a "selected" card with the accent border. */
void setSelected(QWidget *w, bool on)
{
	if (w->property("selected").toBool() == on)
		return;
	w->setProperty("selected", on);
	w->style()->unpolish(w);
	w->style()->polish(w);
}

QString initials(const QString &name)
{
	QString out;
	for (const QString &part : name.split(' ', Qt::SkipEmptyParts)) {
		out += part.at(0).toUpper();
		if (out.size() == 2)
			break;
	}
	return out.isEmpty() ? QStringLiteral("T") : out;
}

QString platformColor(const QString &p)
{
	if (p == QStringLiteral("youtube"))
		return QStringLiteral("#FF0033");
	if (p == QStringLiteral("twitch"))
		return QStringLiteral("#9146FF");
	if (p == QStringLiteral("kick"))
		return QStringLiteral("#53FC18");
	if (p == QStringLiteral("tiktok"))
		return QStringLiteral("#25F4EE");
	return QStringLiteral("#A597BF");
}

/* Clicking anywhere on a card (not only on its radio button or checkbox) runs `fn`. */
class CardClick : public QObject {
public:
	CardClick(QObject *parent, std::function<void()> fn) : QObject(parent), fn(std::move(fn)) {}

protected:
	bool eventFilter(QObject *watched, QEvent *event) override
	{
		if (event->type() == QEvent::MouseButtonPress &&
		    static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton)
			fn();
		return QObject::eventFilter(watched, event);
	}

private:
	std::function<void()> fn;
};

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
	/* A plain QWidget only paints the theme's background with this set. */
	setAttribute(Qt::WA_StyledBackground, true);
	setMinimumWidth(300);

	auto *outer = new QVBoxLayout(this);
	outer->setContentsMargins(0, 0, 0, 0);
	auto *scroll = new QScrollArea(this);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
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
	layout->setContentsMargins(14, 16, 14, 14);
	layout->setSpacing(12);

	auto *logo = new QLabel;
	logo->setPixmap(QIcon(QStringLiteral(":/res/images/obs.png")).pixmap(QSize(48, 48), devicePixelRatioF()));
	layout->addWidget(logo);

	auto *title = new QLabel(QStringLiteral("اربط حسابك في تريديا"));
	title->setObjectName("trediaTitle");
	layout->addWidget(title);
	layout->addWidget(wrapLabel(QStringLiteral(
		"عشان تبث على يوتيوب وتويتش وكيك وتيك توك بضغطة وحدة (تيربو)، وتشوف تقرير بثك، وترفع تسجيلاتك لمكتبتك."),
		"trediaMuted"));

	signInButton = new QPushButton(QStringLiteral("سجّل دخولك"));
	signInButton->setObjectName("trediaPrimary");
	connect(signInButton, &QPushButton::clicked, this, [this]() {
		signInError->clear();
		api->startLink();
	});
	layout->addWidget(signInButton);

	codeBox = card();
	auto *codeLayout = new QVBoxLayout(codeBox);
	codeLayout->setContentsMargins(14, 14, 14, 14);
	codeLayout->setSpacing(10);
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
	cancel->setFlat(true);
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
	layout->addWidget(noAccount, 0, Qt::AlignLeading);
	layout->addStretch();
}

/* ---- Signed in -------------------------------------------------------------------------------- */

void TrediaDock::buildSignedIn(QWidget *page)
{
	auto *layout = new QVBoxLayout(page);
	layout->setContentsMargins(12, 12, 12, 12);
	layout->setSpacing(8);

	/* Who is signed in */
	auto *header = card();
	header->setObjectName("trediaHeader");
	auto *head = new QHBoxLayout(header);
	head->setContentsMargins(12, 12, 12, 12);
	head->setSpacing(10);
	avatarLabel = new QLabel;
	avatarLabel->setObjectName("trediaAvatar");
	avatarLabel->setFixedSize(40, 40);
	avatarLabel->setAlignment(Qt::AlignCenter);
	head->addWidget(avatarLabel, 0, Qt::AlignTop);
	auto *who = new QVBoxLayout;
	who->setSpacing(2);
	helloLabel = new QLabel;
	helloLabel->setObjectName("trediaName");
	who->addWidget(helloLabel);
	planLabel = wrapLabel(QString(), "trediaMuted");
	who->addWidget(planLabel);
	head->addLayout(who, 1);
	auto *signOut = new QPushButton(QStringLiteral("خروج"));
	signOut->setFlat(true);
	connect(signOut, &QPushButton::clicked, this, [this]() {
		if (QMessageBox::question(this, QStringLiteral("تسجيل الخروج"), QStringLiteral("تطلع من حسابك في تريديا على هذا الجهاز؟")) ==
		    QMessageBox::Yes)
			api->signOut();
	});
	head->addWidget(signOut, 0, Qt::AlignTop);
	layout->addWidget(header);

	updateLabel = new QLabel;
	updateLabel->setObjectName("trediaUpdate");
	updateLabel->setWordWrap(true);
	updateLabel->setOpenExternalLinks(true);
	updateLabel->setVisible(false);
	layout->addWidget(updateLabel);

	/* How to multistream: Turbo (Tredia's servers) or from this PC. */
	layout->addSpacing(6);
	layout->addWidget(heading(QStringLiteral("طريقة البث على أكثر من منصة")));

	auto *modes = new QButtonGroup(this);
	turboCard = card();
	turboCard->setObjectName("trediaOption");
	auto *turboLayout = new QVBoxLayout(turboCard);
	turboLayout->setContentsMargins(12, 10, 12, 12);
	turboLayout->setSpacing(4);
	turboRadio = new QRadioButton(QStringLiteral("تيربو من تريديا"));
	turboRadio->setObjectName("trediaOptionTitle");
	turboLayout->addWidget(turboRadio);
	turboLayout->addWidget(wrapLabel(QStringLiteral("ننصح فيه. جهازك يرسل بث واحد لتريديا، وسيرفراتنا توصله لكل منصاتك. "
							"أخف على جهازك وشبكتك، ومعه الشات الموحد والتقرير."),
					 "trediaMuted"));
	turboCard->installEventFilter(new CardClick(turboCard, [this]() { turboRadio->setChecked(true); }));
	layout->addWidget(turboCard);

	localCard = card();
	localCard->setObjectName("trediaOption");
	auto *localLayout = new QVBoxLayout(localCard);
	localLayout->setContentsMargins(12, 10, 12, 12);
	localLayout->setSpacing(6);
	localRadio = new QRadioButton(QStringLiteral("من جهازك"));
	localRadio->setObjectName("trediaOptionTitle");
	localLayout->addWidget(localRadio);
	localLayout->addWidget(wrapLabel(QStringLiteral("تبث لأكثر من جهة من جهازك مباشرة، بدون تريديا."), "trediaMuted"));
	localLayout->addWidget(wrapLabel(warningText, "trediaWarning"));
	localCard->installEventFilter(new CardClick(localCard, [this]() { localRadio->setChecked(true); }));
	layout->addWidget(localCard);

	modes->addButton(turboRadio);
	modes->addButton(localRadio);
	connect(turboRadio, &QRadioButton::toggled, this, [this](bool on) {
		if (on != turbo)
			setTurbo(on, true);
	});

	/* The platforms of this stream: the same checklist as tredia.cloud/live, in both modes. */
	auto *platforms = new QWidget;
	auto *platformsBox = new QVBoxLayout(platforms);
	platformsBox->setContentsMargins(0, 6, 0, 0);
	platformsBox->setSpacing(6);
	platformsBox->addWidget(heading(QStringLiteral("منصاتك في هذا البث")));
	destinationsBox = new QVBoxLayout;
	destinationsBox->setSpacing(6);
	platformsBox->addLayout(destinationsBox);
	turboNote = wrapLabel(QString(), "trediaMuted");
	platformsBox->addWidget(turboNote);
	auto *manage = new QPushButton(QStringLiteral("أضف منصة أو اربط حساب"));
	manage->setFlat(true);
	connect(manage, &QPushButton::clicked, this, [this]() { openUrl(api->baseUrl() + QStringLiteral("/live")); });
	platformsBox->addWidget(manage, 0, Qt::AlignLeading);
	layout->addWidget(platforms);

	/* Turbo: what Tredia is doing with the stream */
	turboPanel = new QWidget;
	auto *turboBox = new QVBoxLayout(turboPanel);
	turboBox->setContentsMargins(0, 0, 0, 0);
	turboBox->setSpacing(6);
	liveStatus = wrapLabel(QString());
	liveStatus->setObjectName("trediaLive");
	liveStatus->setVisible(false);
	turboBox->addWidget(liveStatus);
	auto *links = new QHBoxLayout;
	links->setSpacing(8);
	dashboardButton = new QPushButton(QStringLiteral("لوحة البث"));
	dashboardButton->setToolTip(QStringLiteral("الشات الموحد وحالة كل منصة"));
	connect(dashboardButton, &QPushButton::clicked, this, [this]() {
		const QJsonObject s = liveInfo.value(QStringLiteral("session")).toObject();
		openUrl(s.isEmpty() ? api->baseUrl() + QStringLiteral("/live") : s.value(QStringLiteral("dashboardUrl")).toString());
	});
	reportButton = new QPushButton(QStringLiteral("آخر تقرير"));
	connect(reportButton, &QPushButton::clicked, this, [this]() {
		openUrl(liveInfo.value(QStringLiteral("lastSession")).toObject().value(QStringLiteral("reportUrl")).toString());
	});
	links->addWidget(dashboardButton, 1);
	links->addWidget(reportButton, 1);
	turboBox->addLayout(links);
	layout->addWidget(turboPanel);

	/* From this PC: extra destinations */
	localPanel = new QWidget;
	auto *localBox = new QVBoxLayout(localPanel);
	localBox->setContentsMargins(0, 6, 0, 0);
	localBox->setSpacing(6);
	localBox->addWidget(heading(QStringLiteral("جهات إضافية (اختياري)")));
	localBox->addWidget(wrapLabel(QStringLiteral("لجهة مو مضافة في تريديا. تبدأ مع البث وتوقف معه:"), "trediaMuted"));
	localList = new QVBoxLayout;
	localList->setSpacing(6);
	localBox->addLayout(localList);
	auto *formCard = card();
	auto *form = new QFormLayout(formCard);
	form->setContentsMargins(12, 12, 12, 12);
	form->setSpacing(8);
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
	localError = wrapLabel(QString(), "trediaError");
	form->addRow(localError);
	auto *addLocal = new QPushButton(QStringLiteral("أضف الجهة"));
	addLocal->setObjectName("trediaPrimary");
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
	form->addRow(addLocal);
	localBox->addWidget(formCard);
	layout->addWidget(localPanel);

	/* Overlays */
	layout->addSpacing(6);
	layout->addWidget(heading(QStringLiteral("إضافات البث")));
	layout->addWidget(wrapLabel(QStringLiteral("الشات والتنبيهات وعدّاد المشاهدين، جاهزة على مشهدك."), "trediaMuted"));
	auto *overlays = new QPushButton(QStringLiteral("أضفها لمشهدك"));
	connect(overlays, &QPushButton::clicked, this, &TrediaDock::addOverlays);
	layout->addWidget(overlays);

	/* Recordings */
	layout->addSpacing(6);
	layout->addWidget(heading(QStringLiteral("تسجيلاتك")));
	layout->addWidget(wrapLabel(QStringLiteral("أول ما توقف التسجيل نسألك ترفعه لمكتبتك في تريديا، وتنشره على يوتيوب وتيك توك من هناك."),
				    "trediaMuted"));
	auto *pick = new QPushButton(QStringLiteral("ارفع تسجيل…"));
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
	uploadBar->setTextVisible(false);
	uploadBar->setVisible(false);
	layout->addWidget(uploadBar);

	layout->addSpacing(4);
	auto *site = new QPushButton(QStringLiteral("افتح لوحتي في تريديا"));
	site->setFlat(true);
	connect(site, &QPushButton::clicked, this, [this]() { openUrl(api->baseUrl() + QStringLiteral("/creator")); });
	layout->addWidget(site, 0, Qt::AlignLeading);
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
	avatarLabel->setText(initials(api->userName()));
	api->get(QStringLiteral("/api/app/me"), [this](const QJsonObject &me, int, const QString &error) {
		if (!error.isEmpty()) {
			planLabel->setText(error);
			return;
		}
		const QJsonObject plan = me.value(QStringLiteral("plan")).toObject();
		const int hoursLeft = static_cast<int>(plan.value(QStringLiteral("remainingSeconds")).toDouble() / 3600.0);
		const QJsonValue max = plan.value(QStringLiteral("maxPlatforms"));
		const QString platforms = max.isNull() ? QStringLiteral("كل المنصات") : QStringLiteral("حتى %1 منصات").arg(max.toInt());
		planLabel->setText(QStringLiteral("باقة %1 · %2 · باقي %3 ساعة هالشهر")
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
		const QString platform = d.value(QStringLiteral("platform")).toString();
		auto *row = card();
		row->setObjectName("trediaRow");
		auto *h = new QHBoxLayout(row);
		h->setContentsMargins(12, 8, 12, 8);
		h->setSpacing(10);
		auto *dot = new QLabel;
		dot->setFixedSize(10, 10);
		dot->setStyleSheet(QStringLiteral("background: %1; border-radius: 5px;").arg(platformColor(platform)));
		h->addWidget(dot);
		auto *names = new QVBoxLayout;
		names->setSpacing(0);
		auto *label = new QLabel(d.value(QStringLiteral("label")).toString());
		label->setObjectName("trediaRowTitle");
		names->addWidget(label);
		names->addWidget(wrapLabel(platformName(platform), "trediaMuted"));
		h->addLayout(names, 1);
		auto *box = new QCheckBox;
		box->setToolTip(QStringLiteral("يبث على هذي المنصة"));
		box->setChecked(d.value(QStringLiteral("enabled")).toBool());
		h->addWidget(box);
		row->installEventFilter(new CardClick(row, [box]() {
			if (box->isEnabled())
				box->toggle();
		}));
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
		destinationsBox->addWidget(row);
	}
	updateNote();
}

void TrediaDock::updateNote()
{
	if (!turbo) {
		turboNote->setText(QStringLiteral("من جهازك: يبث مباشرة لكل منصة مفعّلة هنا، بدون ما تكتب سيرفر أو مفتاح. "
						  "يوتيوب ينفتح له بث جديد كل مرة."));
		return;
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
		auto *row = card();
		row->setObjectName("trediaRow");
		auto *h = new QHBoxLayout(row);
		h->setContentsMargins(12, 6, 8, 6);
		h->setSpacing(8);
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
	setSelected(turboCard, on);
	setSelected(localCard, !on);
	updateNote();
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

/* ---- Going live ------------------------------------------------------------------------------- */

void TrediaDock::goLive()
{
	if (obs_frontend_streaming_active())
		return;
	if (turbo || !api->signedIn()) {
		if (turbo)
			applyTurboService();
		obs_frontend_streaming_start();
		return;
	}

	/* From this PC: Tredia hands over the push address of every platform ticked above. The first one
	 * becomes the main stream; the others (and any extra destinations) go out alongside it. */
	emit goLiveBusy(true);
	api->post(QStringLiteral("/api/app/live/direct"), QJsonObject(), [this](const QJsonObject &data, int, const QString &error) {
		emit goLiveBusy(false);
		if (!error.isEmpty()) {
			turboNote->setText(error);
			return;
		}
		QList<LocalTarget> site;
		for (const QJsonValue &v : data.value(QStringLiteral("targets")).toArray()) {
			const QJsonObject o = v.toObject();
			LocalTarget t;
			t.id = QStringLiteral("site-") + o.value(QStringLiteral("id")).toString();
			t.name = platformName(o.value(QStringLiteral("platform")).toString());
			t.server = o.value(QStringLiteral("server")).toString();
			t.key = o.value(QStringLiteral("key")).toString();
			if (!t.server.isEmpty() && !t.key.isEmpty())
				site.append(t);
		}
		QStringList problems;
		for (const QJsonValue &v : data.value(QStringLiteral("failed")).toArray()) {
			const QJsonObject o = v.toObject();
			problems << platformName(o.value(QStringLiteral("platform")).toString()) + QStringLiteral(": ") +
					    o.value(QStringLiteral("error")).toString();
		}
		if (problems.isEmpty())
			updateNote();
		else
			turboNote->setText(problems.join('\n'));

		if (site.isEmpty()) {
			/* Nothing ticked on Tredia: the stream goes to Settings > Stream, plus the extra destinations. */
			multi->setSiteTargets({});
			obs_frontend_streaming_start();
			return;
		}

		if (!directActive) {
			serviceBeforeDirect = QJsonObject();
			if (obs_service_t *current = obs_frontend_get_streaming_service()) {
				obs_data_t *s = obs_service_get_settings(current);
				serviceBeforeDirect.insert(QStringLiteral("type"), QString::fromUtf8(obs_service_get_type(current)));
				serviceBeforeDirect.insert(QStringLiteral("settings"), QString::fromUtf8(obs_data_get_json(s)));
				obs_data_release(s);
			}
		}
		const LocalTarget first = site.takeFirst();
		obs_data_t *settings = obs_data_create();
		obs_data_set_string(settings, "server", first.server.toUtf8().constData());
		obs_data_set_string(settings, "key", first.key.toUtf8().constData());
		obs_service_t *service = obs_service_create("rtmp_custom", "default_service", settings, nullptr);
		obs_data_release(settings);
		if (!service) {
			turboNote->setText(QStringLiteral("تعذّر تجهيز البث. جرّب مرة ثانية."));
			return;
		}
		obs_frontend_set_streaming_service(service);
		obs_service_release(service);
		multi->setSiteTargets(site);
		directActive = true;
		obs_frontend_streaming_start();
	});
}

/* After a "from this PC" stream: Settings > Stream goes back to what it was (the keys Tredia handed
 * over were for that stream only). */
void TrediaDock::restoreAfterDirect()
{
	multi->setSiteTargets({});
	if (!directActive)
		return;
	directActive = false;
	const QByteArray type = serviceBeforeDirect.value(QStringLiteral("type")).toString().toUtf8();
	const QByteArray json = serviceBeforeDirect.value(QStringLiteral("settings")).toString().toUtf8();
	if (type.isEmpty())
		return;
	obs_data_t *settings = obs_data_create_from_json(json.constData());
	obs_service_t *service = obs_service_create(type.constData(), "default_service", settings, nullptr);
	obs_data_release(settings);
	if (service) {
		obs_frontend_set_streaming_service(service);
		obs_service_release(service);
	}
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
		restoreAfterDirect();
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
