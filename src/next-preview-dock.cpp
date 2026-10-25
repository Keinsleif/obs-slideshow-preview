#include "next-preview-dock.hpp"

#include <QComboBox>
#include <QLabel>
#include <QVBoxLayout>
#include <QImageReader>
#include <QFileInfo>

#include <obs-module.h>
#include <obs-frontend-api.h>

NextPreviewDock::NextPreviewDock(QWidget *parent) : QWidget(parent)
{
	combo = new QComboBox;
	image = new QLabel;
	info = new QLabel;

	image->setAlignment(Qt::AlignCenter);
	image->setMinimumSize(160, 90);
	// Ignored にしないと、Pixmap の大きさでラベルが広がり続ける
	image->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);

	auto *layout = new QVBoxLayout(this);
	layout->addWidget(combo);
	layout->addWidget(image, 1);
	layout->addWidget(info);

	connect(combo, &QComboBox::currentIndexChanged, this, &NextPreviewDock::onComboChanged);

	signal_handler_t *gsh = obs_get_signal_handler();
	signal_handler_connect(gsh, "source_create", onSourceListChanged, this);
	signal_handler_connect(gsh, "source_destroy", onSourceListChanged, this);
	signal_handler_connect(gsh, "source_rename", onSourceListChanged, this);

	refreshSources();
}

void NextPreviewDock::onSourceListChanged(void *param, calldata_t *)
{
	auto *self = static_cast<NextPreviewDock *>(param);
	QMetaObject::invokeMethod(self, "refreshSources", Qt::QueuedConnection);
}

void NextPreviewDock::refreshSources()
{
	QString current = combo->currentText();

	{
		QSignalBlocker block(combo);
		combo->clear();
		obs_enum_sources(
			[](void *p, obs_source_t *s) -> bool {
				if (strcmp(obs_source_get_id(s), "sp_slideshow") == 0)
					static_cast<QComboBox *>(p)->addItem(QString::fromUtf8(obs_source_get_name(s)));
				return true;
			},
			combo);

		int idx = combo->findText(current);
		if (idx < 0 && combo->count() > 0)
			idx = 0; // 見つからなければ先頭を選ぶ
		combo->setCurrentIndex(idx);
	}

	if (combo->currentText() != current)
		onComboChanged(combo->currentIndex());
}

void NextPreviewDock::onComboChanged(int index)
{
	if (index < 0)
		detach();
	else
		attach(combo->itemText(index));
}

void NextPreviewDock::attach(const QString &name)
{
	detach();

	obs_source_t *src = obs_get_source_by_name(name.toUtf8().constData());
	if (!src)
		return;

	signal_handler_connect(obs_source_get_signal_handler(src), "slide_changed", onSlideChanged, this);
	weak = obs_source_get_weak_source(src);

	// 初期表示:次のスライドをプロシージャで取得
	calldata_t cd = {0};
	proc_handler_call(obs_source_get_proc_handler(src), "next_path", &cd);
	const char *p = calldata_string(&cd, "next_path");
	showImage(QString::fromUtf8(p ? p : ""));
	calldata_free(&cd);

	obs_source_release(src);
}

void NextPreviewDock::detach()
{
	if (!weak)
		return;

	if (obs_source_t *src = obs_weak_source_get_source(weak)) {
		signal_handler_disconnect(obs_source_get_signal_handler(src), "slide_changed", onSlideChanged, this);
		obs_source_release(src);
	}
	obs_weak_source_release(weak);
	weak = nullptr;

	original = QPixmap();
	image->clear();
	info->clear();
}

void NextPreviewDock::onSlideChanged(void *param, calldata_t *cd)
{
	auto *self = static_cast<NextPreviewDock *>(param);

	const char *next = nullptr;
	calldata_get_string(cd, "next_path", &next);
	QString path = QString::fromUtf8(next ? next : ""); // 即座にコピー

	QMetaObject::invokeMethod(self, [self, path]() { self->showImage(path); }, Qt::QueuedConnection);
}

void NextPreviewDock::showImage(const QString &path)
{
	if (path.isEmpty()) {
		original = QPixmap();
		image->setText(obs_module_text("NoNext"));
		info->clear();
		return;
	}

	QImageReader reader(path);
	reader.setAutoTransform(true); // EXIF の回転を反映
	QImage img = reader.read();

	if (img.isNull()) {
		original = QPixmap();
		image->setText(obs_module_text("LoadFailed"));
		info->setText(QFileInfo(path).fileName());
		return;
	}

	original = QPixmap::fromImage(img);
	info->setText(QFileInfo(path).fileName());
	updateScaled();
}

void NextPreviewDock::updateScaled()
{
	if (original.isNull())
		return;
	image->setPixmap(original.scaled(image->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

void NextPreviewDock::resizeEvent(QResizeEvent *e)
{
	QWidget::resizeEvent(e);
	updateScaled();
}

NextPreviewDock::~NextPreviewDock()
{
	signal_handler_t *gsh = obs_get_signal_handler();
	signal_handler_disconnect(gsh, "source_create", onSourceListChanged, this);
	signal_handler_disconnect(gsh, "source_destroy", onSourceListChanged, this);
	signal_handler_disconnect(gsh, "source_rename", onSourceListChanged, this);
	detach();
}