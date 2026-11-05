/* Tredia Studio: the Tredia panel (sign-in, Turbo, "from this PC", overlays, report, uploads). */

#include "tredia-dock.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/bmem.h>
#include <util/platform.h>

#include <QDockWidget>
#include <QFile>
#include <QMainWindow>
#include <QPointer>

OBS_DECLARE_MODULE()
OBS_MODULE_AUTHOR("Tredia")

MODULE_EXPORT const char *obs_module_name(void)
{
	return "Tredia";
}

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Tredia account, Turbo multistreaming through Tredia, streaming to several places from this PC, and uploads.";
}

static QPointer<TrediaDock> dock;

/* The first time Tredia Studio opens, the Tredia panel is shown docked on the side; after that the
 * person's own layout wins (OBS saves and restores docks). */
static void showOnFirstRun()
{
	char *flag = obs_module_config_path("first-run-done");
	const bool done = os_file_exists(flag);
	if (!done) {
		char *dir = obs_module_config_path("");
		os_mkdirs(dir);
		bfree(dir);
		QFile file(QString::fromUtf8(flag));
		if (file.open(QIODevice::WriteOnly))
			file.write("1");
	}
	bfree(flag);
	if (done || !dock)
		return;
	if (auto *panel = qobject_cast<QDockWidget *>(dock->parentWidget())) {
		panel->setFloating(false);
		panel->setVisible(true);
		panel->raise();
	}
}

static void onFrontendEvent(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING)
		showOnFirstRun();
	if (dock)
		dock->handleEvent(event);
}

bool obs_module_load(void)
{
	auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	dock = new TrediaDock(main);
	if (!obs_frontend_add_dock_by_id("tredia", "تريديا", dock)) {
		delete dock;
		return false;
	}
	obs_frontend_add_event_callback(onFrontendEvent, nullptr);
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(onFrontendEvent, nullptr);
}
