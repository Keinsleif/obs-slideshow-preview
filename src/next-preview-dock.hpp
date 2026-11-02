#pragma once
#include <QWidget>
#include <QPixmap>
#include <QList>
#include <obs.h>

class QComboBox;
class QLabel;
class QPushButton;

enum class NavMode { Hidden = 0, Standard = 1, Compact = 2 };

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
	void callSlide(bool next);
	void setNavMode(NavMode mode);
	void setNavEnabled(bool enabled);

	static void onSlideChanged(void *param, calldata_t *cd);
	static void onSourceListChanged(void *param, calldata_t *cd);

	QComboBox *combo;
	QLabel *image;
	QLabel *info;
	QWidget *compactRow;             // コンボボックスの右に置く行
	QWidget *fullRow;                // 情報ラベルの下に置く行
	QList<QPushButton *> navButtons; // 有効/無効の切替用
	NavMode navMode = NavMode::Compact;
	QPixmap original;
	obs_weak_source_t *weak = nullptr;
};
