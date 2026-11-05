#include "SourceTree.hpp"
#include "SourceTreeDelegate.hpp"
#include "moc_SourceTreeDelegate.cpp"

#include <algorithm>

SourceTreeDelegate::SourceTreeDelegate(QObject *parent) : QStyledItemDelegate(parent) {}

QSize SourceTreeDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
	SourceTree *tree = qobject_cast<SourceTree *>(parent());
	QWidget *item = tree->indexWidget(index);

	if (!item) {
		return QStyledItemDelegate::sizeHint(option, index);
	}

	/* Tredia Studio: polish the row first so its size uses the theme's font, and keep room under the
	 * baseline: without this, larger Arabic names were clipped at the bottom (dots and descenders). */
	item->ensurePolished();
	QSize hint = item->sizeHint();
	hint.setHeight(std::max(hint.height(), item->fontMetrics().height() + 10));
	return hint;
}
