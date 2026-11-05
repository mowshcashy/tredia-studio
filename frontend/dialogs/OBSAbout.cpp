#include "OBSAbout.hpp"

#include <widgets/OBSBasic.hpp>
#include <utility/RemoteTextThread.hpp>

#include <qt-wrappers.hpp>

#include <json11.hpp>

#include "moc_OBSAbout.cpp"

using namespace json11;

extern bool steam;

OBSAbout::OBSAbout(QWidget *parent) : QDialog(parent), ui(new Ui::OBSAbout)
{
	setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

	ui->setupUi(this);

	QString bitness;

	if (sizeof(void *) == 4) {
		bitness = " (32 bit)";
	} else if (sizeof(void *) == 8) {
		bitness = " (64 bit)";
	}

	QString ver = obs_get_version_string();

	/* Tredia Studio: our version first, then the OBS Studio release it is built on. */
	ui->version->setText(QStringLiteral("1.0.0") + bitness + QStringLiteral(" (OBS Studio ") + ver + QStringLiteral(")"));

	ui->contribute->setText("<a href='https://tredia.cloud'>tredia.cloud</a> &nbsp;&middot;&nbsp; "
				 "<a href='mailto:support@tredia.cloud'>support@tredia.cloud</a>");
	ui->contribute->setTextInteractionFlags(Qt::TextBrowserInteraction);
	ui->contribute->setOpenExternalLinks(true);
	delete ui->donate;
	delete ui->getInvolved;

	ui->about->setText("<a href='#'>" + QTStr("About") + "</a>");
	ui->authors->setText("<a href='#'>" + QTStr("About.Authors") + "</a>");
	ui->license->setText("<a href='#'>" + QTStr("About.License") + "</a>");

	ui->name->setProperty("class", "text-heading");
	ui->version->setProperty("class", "text-large");
	ui->about->setProperty("class", "bg-base");
	ui->authors->setProperty("class", "bg-base");
	ui->license->setProperty("class", "bg-base");
	ui->info->setProperty("class", "");

	ui->textBrowser->setOpenExternalLinks(true);

	connect(ui->about, &ClickableLabel::clicked, this, &OBSAbout::ShowAbout);
	connect(ui->authors, &ClickableLabel::clicked, this, &OBSAbout::ShowAuthors);
	connect(ui->license, &ClickableLabel::clicked, this, &OBSAbout::ShowLicense);

	ShowAbout();
}

void OBSAbout::ShowAbout()
{
	/* Tredia Studio: who makes the app and what it is built on (no OBS Patreon list). */
	QString text;
	text += "<h2>Tredia Studio</h2>";
	text += "<p>" + QTStr("About.Info").toHtmlEscaped() + "</p>";
	text += "<p><a href='https://tredia.cloud'>tredia.cloud</a> &nbsp;&middot;&nbsp; "
		"<a href='https://github.com/mowshcashy/tredia-studio'>github.com/mowshcashy/tredia-studio</a></p>";
	text += "<p>Based on OBS Studio by the OBS Project and its contributors, under the GNU GPL v2. "
		"See the Authors and License tabs.</p>";
	ui->textBrowser->setHtml(text);
}

void OBSAbout::ShowAuthors()
{
	std::string path;
	QString error = QTStr("About.Error").arg("https://github.com/obsproject/obs-studio/blob/master/AUTHORS");

#ifdef __APPLE__
	if (!GetDataFilePath("AUTHORS", path)) {
#else
	if (!GetDataFilePath("authors/AUTHORS", path)) {
#endif
		ui->textBrowser->setPlainText(error);
		return;
	}

	ui->textBrowser->setPlainText(QString::fromStdString(path));

	BPtr<char> text = os_quick_read_utf8_file(path.c_str());

	if (!text || !*text) {
		ui->textBrowser->setPlainText(error);
		return;
	}

	ui->textBrowser->setPlainText(QT_UTF8(text));
}

void OBSAbout::ShowLicense()
{
	std::string path;
	QString error = QTStr("About.Error").arg("https://github.com/obsproject/obs-studio/blob/master/COPYING");

	if (!GetDataFilePath("license/gplv2.txt", path)) {
		ui->textBrowser->setPlainText(error);
		return;
	}

	BPtr<char> text = os_quick_read_utf8_file(path.c_str());

	if (!text || !*text) {
		ui->textBrowser->setPlainText(error);
		return;
	}

	ui->textBrowser->setPlainText(QT_UTF8(text));
}
