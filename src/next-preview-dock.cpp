#include "next-preview-dock.hpp"

#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QHBoxLayout>
#include <QMenu>
#include <QAction>
#include <QActionGroup>
#include <QVBoxLayout>
#include <QImageReader>
#include <QFileInfo>

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/config-file.h>

static const char *CONFIG_SECTION = "SlideshowPreview";
static const char *CONFIG_NAV_MODE = "NavButtonMode";

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
	auto *topRow = new QHBoxLayout;
	topRow->addWidget(combo, 1);
	layout->addLayout(topRow);
	layout->addWidget(image, 1);
	layout->addWidget(info);

	// 標準版(テキスト)とコンパクト版(アイコン)の2組を作り、表示だけ切り替える
	auto makeRow = [this](bool compact) {
		auto *w = new QWidget;
		auto *h = new QHBoxLayout(w);
		h->setContentsMargins(0, 0, 0, 0);
		h->setSpacing(2);

		auto *prev = new QPushButton;
		auto *next = new QPushButton;
		if (compact) {
			// OBS 標準のメディアコントロールと同じテーマ定義のアイコンを使う
			prev->setProperty("class", "icon-media-prev");
			next->setProperty("class", "icon-media-next");
			const int size = combo->sizeHint().height();
			for (QPushButton *btn : {prev, next}) {
				btn->setFlat(true);
				btn->setIconSize(QSize(20, 20));
				btn->setFixedSize(size, size);
			}
			prev->setToolTip(obs_module_text("PrevSlideButton"));
			next->setToolTip(obs_module_text("NextSlideButton"));
		} else {
			prev->setText(obs_module_text("PrevSlideButton"));
			next->setText(obs_module_text("NextSlideButton"));
		}
		prev->setEnabled(false);
		next->setEnabled(false);

		connect(prev, &QPushButton::clicked, this, [this] { callSlide(false); });
		connect(next, &QPushButton::clicked, this, [this] { callSlide(true); });

		h->addWidget(prev);
		h->addWidget(next);
		navButtons << prev << next;
		return w;
	};

	compactRow = makeRow(true);
	fullRow = makeRow(false);
	topRow->addWidget(compactRow);
	layout->addWidget(fullRow);

	// 右クリックで操作ボタンの表示方法を切り替える
	setContextMenuPolicy(Qt::CustomContextMenu);
	connect(this, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
		QMenu menu(this);
		auto *group = new QActionGroup(&menu);
		const std::pair<NavMode, const char *> items[] = {
			{NavMode::Hidden, "NavMode.Hidden"},
			{NavMode::Standard, "NavMode.Standard"},
			{NavMode::Compact, "NavMode.Compact"},
		};
		for (const auto &item : items) {
			const NavMode m = item.first; // C++17 では構造化束縛をキャプチャできないため
			QAction *act = menu.addAction(obs_module_text(item.second));
			act->setCheckable(true);
			act->setActionGroup(group);
			act->setChecked(m == navMode);
			connect(act, &QAction::triggered, this, [this, m] { setNavMode(m); });
		}
		menu.exec(mapToGlobal(pos));
	});

	config_t *cfg = obs_frontend_get_user_config();
	config_set_default_int(cfg, CONFIG_SECTION, CONFIG_NAV_MODE, (int)NavMode::Compact);
	int mode = (int)config_get_int(cfg, CONFIG_SECTION, CONFIG_NAV_MODE);
	if (mode < (int)NavMode::Hidden || mode > (int)NavMode::Compact)
		mode = (int)NavMode::Compact;
	setNavMode((NavMode)mode);

	connect(combo, &QComboBox::currentIndexChanged, this, &NextPreviewDock::onComboChanged);

	signal_handler_t *gsh = obs_get_signal_handler();
	signal_handler_connect(gsh, "source_create", onSourceListChanged, this);
	signal_handler_connect(gsh, "source_remove", onSourceListChanged, this);
	signal_handler_connect(gsh, "source_destroy", onSourceListChanged, this);
	signal_handler_connect(gsh, "source_rename", onSourceListChanged, this);

	refreshSources();
}

void NextPreviewDock::setNavMode(NavMode mode)
{
	navMode = mode;
	fullRow->setVisible(mode == NavMode::Standard);
	compactRow->setVisible(mode == NavMode::Compact);

	config_t *cfg = obs_frontend_get_user_config();
	config_set_int(cfg, CONFIG_SECTION, CONFIG_NAV_MODE, (int)mode);
	config_save(cfg);
}

void NextPreviewDock::setNavEnabled(bool enabled)
{
	for (QPushButton *btn : navButtons)
		btn->setEnabled(enabled);
}

void NextPreviewDock::callSlide(bool next)
{
	if (!weak)
		return;
	if (obs_source_t *src = obs_weak_source_get_source(weak)) {
		if (next)
			obs_source_media_next(src);
		else
			obs_source_media_previous(src);
		obs_source_release(src);
	}
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
				if (obs_source_removed(s))
					return true; // 削除済み(破棄待ち)は一覧に出さない

				/* ここは今の sp_slideshow 判定のまま */
				if (strcmp(obs_source_get_unversioned_id(s), "sp_slideshow") == 0)
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
	setNavEnabled(true);

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

	setNavEnabled(false);

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
	signal_handler_disconnect(gsh, "source_remove", onSourceListChanged, this);
	signal_handler_disconnect(gsh, "source_destroy", onSourceListChanged, this);
	signal_handler_disconnect(gsh, "source_rename", onSourceListChanged, this);
	detach();
}
