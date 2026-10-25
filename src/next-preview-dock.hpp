#pragma once
#include <QWidget>
#include <QPixmap>
#include <obs.h>

class QComboBox;
class QLabel;

class NextPreviewDock : public QWidget {
	Q_OBJECT
public:
	explicit NextPreviewDock(QWidget *parent = nullptr);
	~NextPreviewDock() override;

protected:
	void resizeEvent(QResizeEvent *e) override;

private slots:
	void refreshSources();
	void onComboChanged(int index);

private:
	void attach(const QString &name);
	void detach();
	void showImage(const QString &path);
	void updateScaled();

	static void onSlideChanged(void *param, calldata_t *cd);
	static void onSourceListChanged(void *param, calldata_t *cd);

	QComboBox *combo;
	QLabel *image;
	QLabel *info;
	QPixmap original;
	obs_weak_source_t *weak = nullptr;
};
