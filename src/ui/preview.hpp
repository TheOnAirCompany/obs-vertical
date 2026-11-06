#pragma once

/* Live preview of the vertical canvas in the dock (obs_display). While it is
 * visible it holds the "Preview" reason of the render gate. */

#include <obs.h>

#include <QWidget>

#include <mutex>

namespace vertical::ui {

class Preview : public QWidget {
	Q_OBJECT

public:
	explicit Preview(QWidget *parent = nullptr);
	~Preview() override;

	QPaintEngine *paintEngine() const override { return nullptr; }

public slots:
	void setCanvas(obs_canvas_t *canvas);
	void setActive(bool active); /* user toggle: off = no display, no render */

protected:
	void resizeEvent(QResizeEvent *e) override;
	void showEvent(QShowEvent *e) override;
	void hideEvent(QHideEvent *e) override;
	void paintEvent(QPaintEvent *e) override;

private:
	void createDisplay();
	void destroyDisplay();
	void updateState();
	static void draw(void *data, uint32_t cx, uint32_t cy);

	obs_display_t *display_ = nullptr;
	bool active_ = true;
	bool holdsRender_ = false;
	bool destroying_ = false;

	std::mutex canvasMutex_;
	obs_weak_canvas_t *canvas_ = nullptr;
};

} // namespace vertical::ui
