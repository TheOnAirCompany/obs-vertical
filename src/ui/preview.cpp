#include "ui/preview.hpp"

#include "obs/vertical.hpp"

#include <graphics/graphics.h>

#include <QPlatformSurfaceEvent>
#include <QResizeEvent>
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
	setMinimumSize(90, 160);

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
	}
	obs_canvas_release(canvas);
}

} // namespace vertical::ui
