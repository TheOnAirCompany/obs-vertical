#pragma once

/* Stereo output meter of the vertical mix: a thin bar under the dock preview, with the
 * scale, zones and colours of OBS's own mixer meters (the colours are read from the
 * themed VolumeMeter widgets of the main window when there are some). It listens to
 * the vertical track only while it is visible. */

#include "core/meter.hpp"

#include <QColor>
#include <QElapsedTimer>
#include <QTimer>
#include <QWidget>

namespace vertical::ui {

namespace meter {
constexpr int kBarThickness = 3; /* one channel, like OBS's meterThickness */
constexpr int kChannelGap = 1;
constexpr int kMarginTop = 2;  /* space under the preview */
constexpr int kRefreshMs = 33; /* ~30 Hz */
} // namespace meter

class MixMeter : public QWidget {
	Q_OBJECT

public:
	explicit MixMeter(QWidget *parent = nullptr);
	~MixMeter() override;

	QSize sizeHint() const override;
	QSize minimumSizeHint() const override;

	/* Displayed levels (dBFS), for the self-test. */
	float levelDb(int channel) const { return channel ? right_.level() : left_.level(); }
	/* True once audio of the vertical track reached the meter. */
	bool receivedAudio() const { return received_; }

protected:
	void paintEvent(QPaintEvent *e) override;
	void showEvent(QShowEvent *e) override;
	void hideEvent(QHideEvent *e) override;
	void changeEvent(QEvent *e) override;

private:
	void tick();
	void loadColors();
	void paintBar(QPainter &p, const QRect &r, float db);

	core::MeterBallistics left_;
	core::MeterBallistics right_;
	QTimer timer_;
	QElapsedTimer clock_;
	bool listening_ = false;
	bool received_ = false;

	QColor bgNominal_, bgWarning_, bgError_;
	QColor fgNominal_, fgWarning_, fgError_;
};

} // namespace vertical::ui
