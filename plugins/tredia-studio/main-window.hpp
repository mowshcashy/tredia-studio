#pragma once

class QMainWindow;
class QMenu;
class TrediaVertical;

/* Tredia Studio's window made to look like Tredia's own studio (Tredia Studio for PC): each panel
 * has a header with its name and its main "+", OBS's menu bar folds into the top bar's menu button,
 * and Windows' own title bar takes the top bar's colours. */
namespace TrediaWindow {

/* Once, after OBS has finished loading (its panels exist by then). */
void style(QMainWindow *main);

/* Once, after OBS has finished loading: the vertical picture's preview beside OBS's own. */
void addVerticalPreview(QMainWindow *main, TrediaVertical *vertical);

/* After every theme change. */
void tintCaption(QMainWindow *main);

/* The top bar's menu: OBS's own menus (File, Edit, View, Docks, Profile, Scene Collection, Tools,
 * Help), refilled each time it opens so plugins' additions show too. */
void fillMenu(QMainWindow *main, QMenu *menu);

} // namespace TrediaWindow
