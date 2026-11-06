#pragma once

/* Rows of the Vertical dock: the scene list (program row, drag handle, link
 * icon) and the destination rows (platform tile, status line, ▶ / ■, ⋯). */

#include "core/dock-model.hpp"

#include <QLabel>
#include <QListWidget>
#include <QStyledItemDelegate>
#include <QToolButton>
#include <QWidget>

namespace vertical::ui {

class PlatformTile;
class StatusDot;

/* QLabel that elides its plain text on the right instead of growing. */
class ElidedLabel : public QLabel {
	Q_OBJECT

public:
	explicit ElidedLabel(QWidget *parent = nullptr);
	void setFullText(const QString &text);
	QString fullText() const { return full_; }
	QSize minimumSizeHint() const override;

protected:
	void resizeEvent(QResizeEvent *e) override;

private:
	void refresh();
	QString full_;
};

/* ---------------------------------------------------------------- scenes */

class SceneList : public QListWidget {
	Q_OBJECT

public:
	enum Role { NameRole = Qt::UserRole, ProgramRole, LinkedRole };

	explicit SceneList(QWidget *parent = nullptr);
	void setRowHeight(int h);
	int rowHeight() const { return rowHeight_; }
	/* Link icon of a row, in viewport coordinates. */
	QRect linkRect(const QRect &row) const;
	QRect linkRectFor(const QString &scene) const;
	QListWidgetItem *itemNamed(const QString &scene) const;
	void restyle();

signals:
	void linkRequested(const QString &scene, const QPoint &globalPos);

protected:
	void mousePressEvent(QMouseEvent *e) override;
	void changeEvent(QEvent *e) override;

private:
	int rowHeight_;
};

class SceneDelegate : public QStyledItemDelegate {
	Q_OBJECT

public:
	explicit SceneDelegate(SceneList *list);
	void paint(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &index) const override;
	QSize sizeHint(const QStyleOptionViewItem &opt, const QModelIndex &index) const override;

private:
	SceneList *list_;
};

/* ---------------------------------------------------------------- destinations */

class DestinationRow : public QWidget {
	Q_OBJECT

public:
	DestinationRow(const std::string &id, core::Platform platform, const QString &name, bool compact,
		       QWidget *parent = nullptr);
	const std::string &id() const { return id_; }
	void setStatus(const core::StatusView &status, bool enabled);
	void setSeparator(bool on);
	QString statusText() const; /* the status line, as shown in Studio */
	bool showsStop() const { return running_; }
	QToolButton *toggleButton() const { return toggle_; }

signals:
	void toggleRequested(const std::string &id);
	void editRequested(const std::string &id);
	void duplicateRequested(const std::string &id);
	void deleteRequested(const std::string &id);

protected:
	void paintEvent(QPaintEvent *e) override;
	void contextMenuEvent(QContextMenuEvent *e) override;

private:
	void showMenu(const QPoint &globalPos);

	std::string id_;
	bool compact_;
	bool separator_ = false;
	bool running_ = false;
	QString statusText_;
	PlatformTile *tile_;
	ElidedLabel *name_;
	StatusDot *dot_;
	ElidedLabel *status_ = nullptr;
	QToolButton *toggle_;
	QToolButton *more_ = nullptr;
};

/* ▶ / ■ button of 28 px: neutral when stopped, dark red when running. */
void styleStartStop(QToolButton *b, bool running);
/* Plain icon button (⋯, gear, +). */
QToolButton *iconButton(const char *iconName, const QString &tip, int size, bool plain, QWidget *parent);
void restyleIconButton(QToolButton *b, const char *iconName, bool plain);

/* Round icon with a halo (recording dot, replay). */
class RoundIcon : public QWidget {
	Q_OBJECT

public:
	RoundIcon(const char *iconName, QRgb halo, QRgb color, QWidget *parent = nullptr);

protected:
	void paintEvent(QPaintEvent *e) override;

private:
	const char *icon_;
	QRgb halo_, color_;
};

/* Dashed "+ Add destination" row. */
class AddRow : public QToolButton {
	Q_OBJECT

public:
	explicit AddRow(const QString &text, QWidget *parent = nullptr);

protected:
	void paintEvent(QPaintEvent *e) override;
};

} // namespace vertical::ui
