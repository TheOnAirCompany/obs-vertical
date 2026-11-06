#pragma once

/* Edits one streaming destination, with live validation. */

#include "core/destination.hpp"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

namespace vertical::ui {

class DestinationDialog : public QDialog {
	Q_OBJECT

public:
	DestinationDialog(const core::Destination &d, QWidget *parent = nullptr);
	core::Destination destination() const;

private:
	void onPlatformChanged();
	void onServerChanged();
	void validateNow();

	core::Destination base_;
	QLineEdit *name_;
	QComboBox *platform_;
	QLineEdit *server_;
	QLineEdit *key_;
	QCheckBox *enabled_;
	QSpinBox *videoKbps_;
	QSpinBox *keyint_;
	QComboBox *codec_;
	QComboBox *encoder_;
	QSpinBox *audioKbps_;
	QSpinBox *track_;
	QLabel *issues_;
	QPushButton *ok_ = nullptr;
};

} // namespace vertical::ui
