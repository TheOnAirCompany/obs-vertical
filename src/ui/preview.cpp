#include "ui/preview.hpp"

#include "obs/text.hpp"
#include "obs/vertical.hpp"
#include "ui/dock-decisions.hpp"
#include "ui/dock-style.hpp"

#include <graphics/graphics.h>
#include <graphics/matrix4.h>
#if !defined(_WIN32) && !defined(__APPLE__)
#include <obs-nix-platform.h>
#endif

#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPlatformSurfaceEvent>
#include <QResizeEvent>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWindow>

#include <algorithm>

namespace vertical::ui {

namespace {

bool toGsWindow(QWindow *window, gs_window &gswindow)
{
#ifdef _WIN32
	gswindow.hwnd = (HWND)window->winId();
	return true;
#elif __APPLE__
	gswindow.view = (id)window->winId();
	return true;
#else
	switch (obs_get_nix_platform()) {
	case OBS_NIX_PLATFORM_X11_EGL:
		gswindow.id = window->winId();
		gswindow.display = obs_get_nix_platform_display();
		return true;
	case OBS_NIX_PLATFORM_WAYLAND:
		gswindow.display = (void *)window->winId();
		return gswindow.display != nullptr;
	default:
		return false;
	}
#endif
}

} // namespace

Preview::Preview(QWidget *parent) : QWidget(parent)
{
	setAttribute(Qt::WA_PaintOnScreen);
	setAttribute(Qt::WA_StaticContents);
	setAttribute(Qt::WA_NoSystemBackground);
	setAttribute(Qt::WA_OpaquePaintEvent);
	setAttribute(Qt::WA_DontCreateNativeAncestors);
	setAttribute(Qt::WA_NativeWindow);
	setMinimumSize(36, 64);

	connect(windowHandle(), &QWindow::visibleChanged, this, [this](bool) { updateState(); });

	struct SurfaceFilter : QObject {
		Preview *p;
		explicit SurfaceFilter(Preview *preview) : QObject(preview), p(preview) {}
		bool eventFilter(QObject *o, QEvent *e) override
		{
			if (e->type() == QEvent::PlatformSurface &&
			    static_cast<QPlatformSurfaceEvent *>(e)->surfaceEventType() ==
				    QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed)
				p->destroyDisplay();
			return QObject::eventFilter(o, e);
		}
	};
	windowHandle()->installEventFilter(new SurfaceFilter(this));
}

Preview::~Preview()
{
	destroying_ = true;
	destroyDisplay();
	setCanvas(nullptr);
	if (holdsRender_)
		Vertical::instance().releaseRender(core::RenderReason::Preview);
}

void Preview::setCanvas(obs_canvas_t *canvas)
{
	std::lock_guard<std::mutex> lock(canvasMutex_);
	if (canvas_)
		obs_weak_canvas_release(canvas_);
	canvas_ = canvas ? obs_canvas_get_weak_canvas(canvas) : nullptr;
}

void Preview::setActive(bool active)
{
	active_ = active;
	updateState();
}

void Preview::createDisplay()
{
	if (display_ || destroying_ || !windowHandle() || !windowHandle()->isExposed())
		return;
	const QSize sz = size() * devicePixelRatioF();
	gs_init_data info = {};
	info.cx = (uint32_t)std::max(1, sz.width());
	info.cy = (uint32_t)std::max(1, sz.height());
	info.format = GS_BGRA;
	info.zsformat = GS_ZS_NONE;
	if (!toGsWindow(windowHandle(), info.window))
		return;
	display_ = obs_display_create(&info, 0xFF16181C); /* 0xAABBGGRR: dark grey letterbox */
	if (display_)
		obs_display_add_draw_callback(display_, &Preview::draw, this);
}

void Preview::destroyDisplay()
{
	if (!display_)
		return;
	obs_display_remove_draw_callback(display_, &Preview::draw, this);
	obs_display_destroy(display_);
	display_ = nullptr;
	obs_enter_graphics();
	for (gs_texture_t **t : {&pillTex_, &tagTex_, &chevTex_}) {
		if (*t)
			gs_texture_destroy(*t);
		*t = nullptr;
	}
	obs_leave_graphics();
	std::lock_guard<std::mutex> lock(overlayMutex_);
	overlayDirty_ = true;
}

void Preview::updateState()
{
	const bool visible = active_ && isVisible() && windowHandle() && windowHandle()->isVisible();
	if (visible) {
		createDisplay();
		if (display_) {
			const QSize sz = size() * devicePixelRatioF();
			obs_display_resize(display_, (uint32_t)sz.width(), (uint32_t)sz.height());
		}
	} else {
		/* A hidden preview owns no display: nothing is drawn for it. */
		destroyDisplay();
	}
	const bool wantRender = visible && display_;
	if (wantRender != holdsRender_) {
		holdsRender_ = wantRender;
		if (wantRender)
			Vertical::instance().acquireRender(core::RenderReason::Preview);
		else
			Vertical::instance().releaseRender(core::RenderReason::Preview);
	}
}

void Preview::resizeEvent(QResizeEvent *e)
{
	QWidget::resizeEvent(e);
	if (dpr_.load() != devicePixelRatioF())
		rebuildOverlayImages();
	if (display_) {
		const QSize sz = e->size() * devicePixelRatioF();
		obs_display_resize(display_, (uint32_t)sz.width(), (uint32_t)sz.height());
	}
}

void Preview::showEvent(QShowEvent *e)
{
	QWidget::showEvent(e);
	updateState();
}

void Preview::hideEvent(QHideEvent *e)
{
	QWidget::hideEvent(e);
	updateState();
}

void Preview::paintEvent(QPaintEvent *)
{
	if (!display_)
		updateState();
}

void Preview::draw(void *data, uint32_t cx, uint32_t cy)
{
	auto *self = static_cast<Preview *>(data);
	obs_canvas_t *canvas = nullptr;
	{
		std::lock_guard<std::mutex> lock(self->canvasMutex_);
		if (self->canvas_)
			canvas = obs_weak_canvas_get_canvas(self->canvas_);
	}
	if (!canvas)
		return;
	obs_video_info ovi = {};
	if (obs_canvas_get_video_info(canvas, &ovi) && ovi.base_width && ovi.base_height) {
		const double scale = std::min((double)cx / ovi.base_width, (double)cy / ovi.base_height);
		const int w = (int)(ovi.base_width * scale);
		const int h = (int)(ovi.base_height * scale);
		const int x = ((int)cx - w) / 2;
		const int y = ((int)cy - h) / 2;
		gs_viewport_push();
		gs_projection_push();
		gs_ortho(0.0f, (float)ovi.base_width, 0.0f, (float)ovi.base_height, -100.0f, 100.0f);
		gs_set_viewport(x, y, w, h);
		obs_render_canvas_texture(canvas);
		gs_projection_pop();
		gs_viewport_pop();
		self->drawOverlays(cx, cy, x, y, w, h);
	}
	obs_canvas_release(canvas);
}

/* ------------------------------------------------------------------ overlays */

void Preview::setOverlay(bool live, const QString &tag, bool collapseButton)
{
	if (overlayBuilt_ && live == live_ && tag == tag_ && collapseButton == collapseButton_)
		return;
	overlayBuilt_ = true;
	live_ = live;
	tag_ = tag;
	collapseButton_ = collapseButton;
	rebuildOverlayImages();
}

void Preview::rebuildOverlayImages()
{
	const qreal dpr = devicePixelRatioF();
	dpr_ = dpr;
	QFont pillFont = scaledFont(font(), 10.0 / 12.0, true);
	QFont tagFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
	tagFont.setPointSizeF(pillFont.pointSizeF() > 0 ? pillFont.pointSizeF() : 9.0);
	tagFont.setBold(true);
	const QImage pill = live_ ? badgeImage(T("Dock.LivePill"), QColor::fromRgba(tokens::kRed), Qt::white, pillFont,
					       dpr, true, 5)
				  : QImage();
	const QImage tag = tag_.isEmpty() ? QImage()
					  : badgeImage(tag_, QColor::fromRgba(tokens::kPreviewTagFill), Qt::white,
						       tagFont, dpr, false, 4);
	QImage chev;
	if (collapseButton_) {
		const Surfaces s = surfaces(this);
		const int b = tokens::kSmallButton;
		chev = QImage(QSize(b, b) * dpr, QImage::Format_ARGB32_Premultiplied);
		chev.setDevicePixelRatio(dpr);
		chev.fill(Qt::transparent);
		QPainter p(&chev);
		p.setRenderHint(QPainter::Antialiasing);
		p.setPen(QPen(s.line, 1));
		p.setBrush(s.row);
		p.drawRoundedRect(QRectF(0.5, 0.5, b - 1, b - 1), 5, 5);
		const int ic = tokens::kIcon;
		p.drawPixmap(QPoint((b - ic) / 2, (b - ic) / 2), iconPixmap("chev", s.text, ic, dpr));
	}
	std::lock_guard<std::mutex> lock(overlayMutex_);
	pillImage_ = pill;
	tagImage_ = tag;
	chevImage_ = chev;
	overlayDirty_ = true;
}

QRect Preview::collapseButtonRect() const
{
	if (!collapseButton_)
		return QRect();
	const qreal dpr = devicePixelRatioF();
	const int b = tokens::kSmallButton;
	const int margin = 6;
	/* Bottom right of the canvas area (the display letterboxes the canvas). */
	const int cw = canvasW_.load(), ch = canvasH_.load();
	QRect area = rect();
	if (cw > 0 && ch > 0)
		area = QRect(qRound(canvasX_.load() / dpr), qRound(canvasY_.load() / dpr), qRound(cw / dpr),
			     qRound(ch / dpr));
	return QRect(area.right() - margin - b + 1, area.bottom() - margin - b + 1, b, b);
}

void Preview::drawOverlays(uint32_t cx, uint32_t cy, int x, int y, int w, int h)
{
	canvasX_ = x;
	canvasY_ = y;
	canvasW_ = w;
	canvasH_ = h;
	{
		std::lock_guard<std::mutex> lock(overlayMutex_);
		if (overlayDirty_) {
			const auto upload = [](gs_texture_t *&tex, const QImage &img) {
				if (tex)
					gs_texture_destroy(tex);
				tex = nullptr;
				if (img.isNull())
					return;
				const uint8_t *bits = img.constBits();
				/* QImage ARGB32 rows are BGRA in memory on little-endian machines. */
				tex = gs_texture_create((uint32_t)img.width(), (uint32_t)img.height(), GS_BGRA, 1,
							&bits, 0);
			};
			upload(pillTex_, pillImage_);
			upload(tagTex_, tagImage_);
			upload(chevTex_, chevImage_);
			overlayDirty_ = false;
		}
	}
	if (!pillTex_ && !tagTex_ && !chevTex_)
		return;

	const double dpr = dpr_.load();
	const int m = (int)(8 * dpr);
	gs_viewport_push();
	gs_projection_push();
	gs_set_viewport(0, 0, (int)cx, (int)cy);
	gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);
	gs_blend_state_push();
	gs_enable_blending(true);
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	const bool srgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(false);
	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
	const auto blit = [&](gs_texture_t *tex, int px, int py) {
		if (!tex)
			return;
		gs_matrix_push();
		gs_matrix_identity();
		gs_matrix_translate3f((float)px, (float)py, 0.0f);
		gs_effect_set_texture(image, tex);
		while (gs_effect_loop(effect, "Draw"))
			gs_draw_sprite(tex, 0, gs_texture_get_width(tex), gs_texture_get_height(tex));
		gs_matrix_pop();
	};
	blit(pillTex_, x + m, y + m);
	if (tagTex_)
		blit(tagTex_, x + w - m - (int)gs_texture_get_width(tagTex_), y + m);
	if (chevTex_) {
		const int mc = (int)(6 * dpr);
		blit(chevTex_, x + w - mc - (int)gs_texture_get_width(chevTex_),
		     y + h - mc - (int)gs_texture_get_height(chevTex_));
	}
	gs_enable_framebuffer_srgb(srgb);
	gs_blend_state_pop();
	gs_projection_pop();
	gs_viewport_pop();
}

QImage Preview::snapshot(const QImage &canvasFrame) const
{
	const qreal dpr = devicePixelRatioF();
	QImage out(size() * dpr, QImage::Format_ARGB32_Premultiplied);
	out.setDevicePixelRatio(dpr);
	out.fill(QColor(0x1C, 0x18, 0x16)); /* letterbox colour of the display */
	QPainter p(&out);
	p.setRenderHint(QPainter::SmoothPixmapTransform);
	QRectF area(QPointF(0, 0), QSizeF(size()));
	if (!canvasFrame.isNull()) {
		const qreal scale =
			std::min(width() / (qreal)canvasFrame.width(), height() / (qreal)canvasFrame.height());
		const QSizeF sz(canvasFrame.width() * scale, canvasFrame.height() * scale);
		area = QRectF(QPointF((width() - sz.width()) / 2, (height() - sz.height()) / 2), sz);
		p.drawImage(area, canvasFrame);
	}
	std::lock_guard<std::mutex> lock(overlayMutex_);
	const auto logical = [](const QImage &img) {
		return QSizeF(img.size()) / img.devicePixelRatio();
	};
	if (!pillImage_.isNull())
		p.drawImage(area.topLeft() + QPointF(8, 8), pillImage_);
	if (!tagImage_.isNull())
		p.drawImage(QPointF(area.right() - 8 - logical(tagImage_).width(), area.top() + 8), tagImage_);
	if (!chevImage_.isNull())
		p.drawImage(QPointF(area.right() - 6 - logical(chevImage_).width(),
				    area.bottom() - 6 - logical(chevImage_).height()),
			    chevImage_);
	return out;
}

void Preview::mousePressEvent(QMouseEvent *e)
{
	if (e->button() == Qt::LeftButton && collapseButtonRect().contains(e->pos())) {
		emit collapseRequested();
		return;
	}
	QWidget::mousePressEvent(e);
}

void Preview::mouseDoubleClickEvent(QMouseEvent *e)
{
	if (e->button() == Qt::LeftButton && !collapseButtonRect().contains(e->pos())) {
		emit detachRequested();
		return;
	}
	QWidget::mouseDoubleClickEvent(e);
}

/* ------------------------------------------------------------------ PreviewFrame */

PreviewFrame::PreviewFrame(QWidget *parent) : QWidget(parent), maxHeight_(tokens::kPreviewMaxStudio)
{
	setObjectName(QStringLiteral("previewFrame"));
	preview_ = new Preview(this);
	preview_->setToolTip(T("Dock.PreviewTip"));
	preview_->setOverlay(false, QStringLiteral("9:16"), true);

	/* Shown instead of the preview once it is collapsed. */
	expandBar_ = new QWidget(this);
	expandBar_->setObjectName(QStringLiteral("previewExpandBar"));
	auto *h = new QHBoxLayout(expandBar_);
	h->setContentsMargins(0, 0, 0, 0);
	auto *show = new QToolButton(expandBar_);
	show->setObjectName(QStringLiteral("previewExpand"));
	show->setText(T("Dock.ShowPreview"));
	show->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
	show->setIcon(icon("chevdown", surfaces(this).text));
	show->setAutoRaise(true);
	show->setFixedHeight(tokens::kCollapsedBar);
	h->addWidget(show);
	h->addStretch();
	expandBar_->hide();

	setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
	connect(preview_, &Preview::collapseRequested, this, [this] { setCollapsed(true); });
	connect(preview_, &Preview::detachRequested, this, &PreviewFrame::detachRequested);
	connect(show, &QToolButton::clicked, this, [this] { setCollapsed(false); });
}

void PreviewFrame::setAspect(int width, int height)
{
	if (width <= 0 || height <= 0)
		return;
	aspectW_ = width;
	aspectH_ = height;
	updateGeometry();
	place();
}

void PreviewFrame::setLive(bool live)
{
	if (live == live_)
		return;
	live_ = live;
	update();
}

void PreviewFrame::setMaxPreviewHeight(int h)
{
	maxHeight_ = h;
	updateGeometry();
	place();
}

void PreviewFrame::setCollapsed(bool collapsed)
{
	if (collapsed == collapsed_)
		return;
	collapsed_ = collapsed;
	preview_->setVisible(!collapsed);
	preview_->setActive(!collapsed);
	expandBar_->setVisible(collapsed);
	updateGeometry();
	place();
	update();
	emit collapsedChanged(collapsed);
}

int PreviewFrame::heightForWidth(int w) const
{
	if (collapsed_)
		return tokens::kCollapsedBar;
	const int f = tokens::kPreviewFrame;
	int h = (int)((qint64)(w - 2 * f) * aspectH_ / aspectW_) + 2 * f;
	if (maxHeight_ > 0)
		h = std::min(h, maxHeight_);
	return std::max(h, tokens::kPreviewMinHeight);
}

QSize PreviewFrame::sizeHint() const
{
	if (collapsed_)
		return QSize(tokens::kStudioLeftColumn, tokens::kCollapsedBar);
	return QSize(tokens::kStudioLeftColumn, heightForWidth(tokens::kStudioLeftColumn));
}

QSize PreviewFrame::minimumSizeHint() const
{
	if (collapsed_)
		return QSize(60, tokens::kCollapsedBar);
	return QSize(60, tokens::kPreviewMinHeight);
}

QRect PreviewFrame::fitRect() const
{
	const int f = tokens::kPreviewFrame;
	QRect avail = rect().adjusted(f, f, -f, -f);
	if (maxHeight_ > 0 && avail.height() > maxHeight_ - 2 * f)
		avail.setHeight(maxHeight_ - 2 * f);
	if (avail.width() <= 0 || avail.height() <= 0)
		return QRect();
	int w = avail.width();
	int h = (int)((qint64)w * aspectH_ / aspectW_);
	if (h > avail.height()) {
		h = avail.height();
		w = (int)((qint64)h * aspectW_ / aspectH_);
	}
	return QRect(avail.x() + (avail.width() - w) / 2, avail.y(), w, h);
}

void PreviewFrame::place()
{
	if (collapsed_) {
		expandBar_->setGeometry(0, 0, width(), tokens::kCollapsedBar);
		return;
	}
	const QRect r = fitRect();
	if (r.isValid())
		preview_->setGeometry(r);
}

void PreviewFrame::resizeEvent(QResizeEvent *e)
{
	QWidget::resizeEvent(e);
	place();
}

void PreviewFrame::paintEvent(QPaintEvent *)
{
	if (collapsed_ || !live_ || decisions::kPreviewFrame == decisions::PreviewFrame::None)
		return;
	const QRect r = fitRect();
	if (!r.isValid())
		return;
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);
	const QColor c = QColor::fromRgba(decisions::kPreviewFrame == decisions::PreviewFrame::Violet ? tokens::kViolet
												      : tokens::kRed);
	const qreal f = tokens::kPreviewFrame;
	p.setPen(QPen(c, f));
	p.setBrush(Qt::NoBrush);
	p.drawRoundedRect(QRectF(r).adjusted(-f / 2, -f / 2, f / 2, f / 2), 3, 3);
}

/* ------------------------------------------------------------------ PreviewWindow */

PreviewWindow::PreviewWindow(QWidget *parent) : QWidget(parent, Qt::Window)
{
	setObjectName(QStringLiteral("verticalPreviewWindow"));
	setWindowTitle(T("Dock.PreviewWindow"));
	setAttribute(Qt::WA_DeleteOnClose);
	auto *l = new QVBoxLayout(this);
	l->setContentsMargins(0, 0, 0, 0);
	preview_ = new Preview(this);
	l->addWidget(preview_);
	resize(tokens::kDetachedWidth, tokens::kDetachedHeight);
}

} // namespace vertical::ui
