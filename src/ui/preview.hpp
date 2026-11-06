#pragma once

/* Live preview of the vertical canvas (obs_display). While it is visible it
 * holds the "Preview" reason of the render gate. The LIVE pill, the aspect tag
 * and the collapse button are drawn inside the display (a native surface cannot
 * host child widgets on top of it). */

#include <obs.h>

#include <QImage>
#include <QPointer>
#include <QWidget>

#include <atomic>
#include <mutex>

namespace vertical::ui {

class Preview : public QWidget {
	Q_OBJECT

public:
	explicit Preview(QWidget *parent = nullptr);
	~Preview() override;

	QPaintEngine *paintEngine() const override { return nullptr; }

	/* Overlays: LIVE pill, aspect tag ("9:16"), collapse button (dock preview only). */
	void setOverlay(bool live, const QString &tag, bool collapseButton);
	bool overlayLive() const { return live_; }
	/* Where the collapse button is, in widget coordinates (empty when none). */
	QRect collapseButtonRect() const;
	bool hasDisplay() const { return display_ != nullptr; }
	/* The preview as the user sees it, from a frame of the canvas (self-test
	 * screenshots: a native display cannot be grabbed by Qt). */
	QImage snapshot(const QImage &canvasFrame) const;

public slots:
	void setCanvas(obs_canvas_t *canvas);
	void setActive(bool active); /* user toggle: off = no display, no render */

signals:
	void collapseRequested();
	void detachRequested();

protected:
	void resizeEvent(QResizeEvent *e) override;
	void showEvent(QShowEvent *e) override;
	void hideEvent(QHideEvent *e) override;
	void paintEvent(QPaintEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseDoubleClickEvent(QMouseEvent *e) override;

private:
	void createDisplay();
	void destroyDisplay();
	void updateState();
	void rebuildOverlayImages();
	void drawOverlays(uint32_t cx, uint32_t cy, int x, int y, int w, int h);
	static void draw(void *data, uint32_t cx, uint32_t cy);

	obs_display_t *display_ = nullptr;
	bool active_ = true;
	bool holdsRender_ = false;
	bool destroying_ = false;

	std::mutex canvasMutex_;
	obs_weak_canvas_t *canvas_ = nullptr;

	/* Overlays (images built on the UI thread, textures on the graphics thread). */
	bool live_ = false;
	QString tag_;
	bool collapseButton_ = false;
	bool overlayBuilt_ = false;
	mutable std::mutex overlayMutex_;
	QImage pillImage_, tagImage_, chevImage_;
	bool overlayDirty_ = true;
	gs_texture_t *pillTex_ = nullptr;
	gs_texture_t *tagTex_ = nullptr;
	gs_texture_t *chevTex_ = nullptr;
	std::atomic<double> dpr_{1.0};
	std::atomic<int> canvasX_{0}, canvasY_{0}, canvasW_{0}, canvasH_{0}; /* device px, last frame */
};

/* The dock preview: the display sized to the canvas aspect, a frame that turns
 * red while a destination is live, and a bar to show it again once collapsed. */
class PreviewFrame : public QWidget {
	Q_OBJECT

public:
	explicit PreviewFrame(QWidget *parent = nullptr);

	Preview *preview() const { return preview_; }
	void setAspect(int width, int height);
	void setLive(bool live);
	bool live() const { return live_; }
	void setMaxPreviewHeight(int h);
	void setCollapsed(bool collapsed);
	bool collapsed() const { return collapsed_; }
	QWidget *expandBar() const { return expandBar_; }
	/* Where the picture is drawn (empty while collapsed). */
	QRect pictureRect() const { return collapsed_ ? QRect() : fitRect(); }

	bool hasHeightForWidth() const override { return !collapsed_; }
	int heightForWidth(int w) const override;
	QSize sizeHint() const override;
	QSize minimumSizeHint() const override;

signals:
	void collapsedChanged(bool collapsed);
	void detachRequested();
	void pictureRectChanged();

protected:
	void resizeEvent(QResizeEvent *e) override;
	void paintEvent(QPaintEvent *e) override;

private:
	QRect fitRect() const;
	void place();

	Preview *preview_;
	QWidget *expandBar_;
	int aspectW_ = 9, aspectH_ = 16;
	int maxHeight_;
	bool live_ = false;
	bool collapsed_ = false;
};

/* Detached preview window (double-click on the dock preview). */
class PreviewWindow : public QWidget {
	Q_OBJECT

public:
	explicit PreviewWindow(QWidget *parent = nullptr);
	Preview *preview() const { return preview_; }

private:
	Preview *preview_;
};

} // namespace vertical::ui
