#include "ui/mix-meter.hpp"

#include "obs/audio-mix.hpp"

#include <obs-frontend-api.h>

#include <QApplication>
#include <QEvent>
#include <QMainWindow>
#include <QPainter>
#include <QVariant>

#include <algorithm>

namespace vertical::ui {

namespace {

int totalHeight()
{
	return meter::kMarginTop + 2 * meter::kBarThickness + meter::kChannelGap;
}

/* Colour of a themed OBS VolumeMeter property, or the fallback (Yami). */
QColor themed(const QWidget *volumeMeter, const char *property, QRgb fallback)
{
	if (volumeMeter) {
		const QVariant v = volumeMeter->property(property);
		if (v.isValid() && v.value<QColor>().isValid())
			return v.value<QColor>();
	}
	return QColor::fromRgb(fallback);
}

} // namespace

MixMeter::MixMeter(QWidget *parent) : QWidget(parent)
{
	setObjectName(QStringLiteral("verticalMixMeter"));
	setAttribute(Qt::WA_TransparentForMouseEvents);
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	setFixedHeight(totalHeight());
	timer_.setInterval(meter::kRefreshMs);
	timer_.setTimerType(Qt::PreciseTimer);
	connect(&timer_, &QTimer::timeout, this, &MixMeter::tick);
	loadColors();
}

MixMeter::~MixMeter()
{
	if (listening_)
		AudioMix::instance().releaseMeter();
}

QSize MixMeter::sizeHint() const
{
	return QSize(120, totalHeight());
}

QSize MixMeter::minimumSizeHint() const
{
	return QSize(20, totalHeight());
}

void MixMeter::loadColors()
{
	/* The themes style VolumeMeter with qproperty-*Color; read one of the mixer's. */
	const QWidget *vm = nullptr;
	if (auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window())) {
		for (QWidget *w : main->findChildren<QWidget *>())
			if (QString::fromUtf8(w->metaObject()->className()) == QLatin1String("VolumeMeter")) {
				vm = w;
				break;
			}
	}
	/* Fallbacks: the Yami theme (OBS default). */
	bgNominal_ = themed(vm, "backgroundNominalColor", 0x17641E);
	bgWarning_ = themed(vm, "backgroundWarningColor", 0x6E520D);
	bgError_ = themed(vm, "backgroundErrorColor", 0x7D1224);
	fgNominal_ = themed(vm, "foregroundNominalColor", 0x37D247);
	fgWarning_ = themed(vm, "foregroundWarningColor", 0xE5AF24);
	fgError_ = themed(vm, "foregroundErrorColor", 0xE33B57);
}

void MixMeter::showEvent(QShowEvent *e)
{
	QWidget::showEvent(e);
	if (!listening_) {
		AudioMix::instance().acquireMeter();
		listening_ = true;
	}
	clock_.restart();
	timer_.start();
}

void MixMeter::hideEvent(QHideEvent *e)
{
	QWidget::hideEvent(e);
	timer_.stop();
	if (listening_) {
		AudioMix::instance().releaseMeter();
		listening_ = false;
	}
	left_.reset();
	right_.reset();
}

void MixMeter::changeEvent(QEvent *e)
{
	QWidget::changeEvent(e);
	if (e->type() == QEvent::StyleChange || e->type() == QEvent::PaletteChange)
		loadColors();
}

void MixMeter::tick()
{
	const double dt = clock_.isValid() ? clock_.restart() / 1000.0 : 0.0;
	float l = 0.0f, r = 0.0f;
	if (AudioMix::instance().takePeaks(l, r))
		received_ = true;
	const float oldL = left_.level(), oldR = right_.level();
	left_.update(core::mulToDb(l), dt);
	right_.update(core::mulToDb(r), dt);
	if (left_.level() != oldL || right_.level() != oldR)
		update();
}

void MixMeter::paintBar(QPainter &p, const QRect &r, float db)
{
	/* Three zones like the OBS meter: background colour for the whole zone, the
	 * foreground colour up to the level. */
	const int w = r.width();
	const int xWarn = r.left() + qRound(core::meterFraction(core::kMeterWarningDb) * w);
	const int xErr = r.left() + qRound(core::meterFraction(core::kMeterErrorDb) * w);
	const int xLevel = r.left() + qRound(core::meterFraction(db) * w);
	const struct {
		int from, to;
		QColor bg, fg;
	} zones[3] = {{r.left(), xWarn, bgNominal_, fgNominal_},
		      {xWarn, xErr, bgWarning_, fgWarning_},
		      {xErr, r.right() + 1, bgError_, fgError_}};
	for (const auto &z : zones) {
		if (z.to <= z.from)
			continue;
		const int lit = std::clamp(xLevel, z.from, z.to);
		if (lit > z.from)
			p.fillRect(QRect(z.from, r.top(), lit - z.from, r.height()), z.fg);
		if (z.to > lit)
			p.fillRect(QRect(lit, r.top(), z.to - lit, r.height()), z.bg);
	}
}

void MixMeter::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	const int y0 = meter::kMarginTop;
	paintBar(p, QRect(0, y0, width(), meter::kBarThickness), left_.level());
	paintBar(p, QRect(0, y0 + meter::kBarThickness + meter::kChannelGap, width(), meter::kBarThickness),
		 right_.level());
}

} // namespace vertical::ui
