#include "next-preview-dock.hpp"

#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QHBoxLayout>
#include <QMenu>
#include <QAction>
#include <QActionGroup>
#include <utility>
#include <QListWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QImageReader>
#include <QFileInfo>

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/config-file.h>

static const char *CONFIG_SECTION = "SlideshowPreview";
static const char *CONFIG_NAV_MODE = "NavButtonMode";
static const char *CONFIG_LIST_MODE = "SlideListMode";
static const QSize THUMB_SIZE(96, 54);

NextPreviewDock::NextPreviewDock(QWidget *parent) : QWidget(parent)
{
	combo = new QComboBox;
	image = new QLabel;
	image->installEventFilter(this);
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

	// スライド一覧(選択すると、そのスライドへ切り替わる)
	list = new QListWidget;
	list->setIconSize(THUMB_SIZE);
	list->setUniformItemSizes(true);
	layout->addWidget(list, 1);

	thumbTimer = new QTimer(this);
	thumbTimer->setSingleShot(true);
	connect(thumbTimer, &QTimer::timeout, this, &NextPreviewDock::loadNextThumbs);

	// ユーザー操作のときだけ切り替える(プログラムからの選択変更は QSignalBlocker で止める)
	connect(list, &QListWidget::currentRowChanged, this, [this](int row) {
		if (row < 0 || !weak)
			return;
		if (obs_source_t *src = obs_weak_source_get_source(weak)) {
			calldata_t cd = {0};
			calldata_set_int(&cd, "index", row);
			proc_handler_call(obs_source_get_proc_handler(src), "set_slide", &cd);
			calldata_free(&cd);
			obs_source_release(src);
		}
	});

	// 歯車ボタンと右クリックの両方から、同じ設定メニューを開く
	gearBtn = new QPushButton;
	gearBtn->setProperty("class", "icon-gear"); // OBS 標準の歯車アイコン(テーマ定義)
	gearBtn->setFlat(true);
	gearBtn->setIconSize(QSize(16, 16));
	gearBtn->setFixedSize(combo->sizeHint().height(), combo->sizeHint().height());
	gearBtn->setToolTip(obs_module_text("DockSettings"));
	topRow->addWidget(gearBtn);
	connect(gearBtn, &QPushButton::clicked, this,
		[this] { showSettingsMenu(gearBtn->mapToGlobal(QPoint(0, gearBtn->height()))); });

	setContextMenuPolicy(Qt::CustomContextMenu);
	connect(this, &QWidget::customContextMenuRequested, this,
		[this](const QPoint &pos) { showSettingsMenu(mapToGlobal(pos)); });

	config_t *cfg = obs_frontend_get_user_config();
	config_set_default_int(cfg, CONFIG_SECTION, CONFIG_NAV_MODE, (int)NavMode::Hidden);
	config_set_default_int(cfg, CONFIG_SECTION, CONFIG_LIST_MODE, (int)ListMode::Hidden);

	int navValue = (int)config_get_int(cfg, CONFIG_SECTION, CONFIG_NAV_MODE);
	if (navValue < (int)NavMode::Hidden || navValue > (int)NavMode::Compact)
		navValue = (int)NavMode::Compact;
	setNavMode((NavMode)navValue);

	int listValue = (int)config_get_int(cfg, CONFIG_SECTION, CONFIG_LIST_MODE);
	if (listValue < (int)ListMode::Hidden || listValue > (int)ListMode::Thumbnails)
		listValue = (int)ListMode::Thumbnails;
	setListMode((ListMode)listValue);

	connect(combo, &QComboBox::currentIndexChanged, this, &NextPreviewDock::onComboChanged);

	signal_handler_t *gsh = obs_get_signal_handler();
	signal_handler_connect(gsh, "source_create", onSourceListChanged, this);
	signal_handler_connect(gsh, "source_remove", onSourceListChanged, this);
	signal_handler_connect(gsh, "source_destroy", onSourceListChanged, this);
	signal_handler_connect(gsh, "source_rename", onSourceListChanged, this);

	refreshSources();
}

void NextPreviewDock::showSettingsMenu(const QPoint &globalPos)
{
	QMenu menu(this);

	auto addRadio = [this](QMenu *parent, const char *titleKey,
			       std::initializer_list<std::pair<int, const char *>> items, int current,
			       std::function<void(int)> apply) {
		QMenu *sub = parent->addMenu(obs_module_text(titleKey));
		auto *group = new QActionGroup(sub);
		for (const auto &item : items) {
			const int v = item.first; // C++17 では構造化束縛をキャプチャできないため
			QAction *act = sub->addAction(obs_module_text(item.second));
			act->setCheckable(true);
			act->setActionGroup(group);
			act->setChecked(v == current);
			connect(act, &QAction::triggered, this, [apply, v] { apply(v); });
		}
	};

	addRadio(&menu, "NavButtons",
		 {{(int)NavMode::Hidden, "NavMode.Hidden"},
		  {(int)NavMode::Standard, "NavMode.Standard"},
		  {(int)NavMode::Compact, "NavMode.Compact"}},
		 (int)navMode, [this](int v) { setNavMode((NavMode)v); });
	addRadio(&menu, "SlideList",
		 {{(int)ListMode::Hidden, "ListMode.Hidden"},
		  {(int)ListMode::Names, "ListMode.Names"},
		  {(int)ListMode::Thumbnails, "ListMode.Thumbnails"}},
		 (int)listMode, [this](int v) { setListMode((ListMode)v); });

	menu.exec(globalPos);
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

void NextPreviewDock::setListMode(ListMode mode)
{
	listMode = mode;
	list->setVisible(mode != ListMode::Hidden);
	rebuildList(); // Names: アイコンなし / Thumbnails: 読み込み開始

	config_t *cfg = obs_frontend_get_user_config();
	config_set_int(cfg, CONFIG_SECTION, CONFIG_LIST_MODE, (int)mode);
	config_save(cfg);
}

// スライドショーソースから一覧を作り直す
void NextPreviewDock::rebuildList()
{
	QSignalBlocker block(list); // 選択の変更でスライドが切り替わらないようにする
	thumbTimer->stop();
	thumbNext = 0;
	list->clear();

	if (listMode == ListMode::Hidden || !weak)
		return;

	obs_source_t *src = obs_weak_source_get_source(weak);
	if (!src)
		return;
	proc_handler_t *ph = obs_source_get_proc_handler(src);

	calldata_t cd = {0};
	proc_handler_call(ph, "total_files", &cd);
	const long long total = calldata_int(&cd, "total_files");
	calldata_free(&cd);

	for (long long i = 0; i < total; i++) {
		calldata_t c = {0};
		calldata_set_int(&c, "index", i);
		proc_handler_call(ph, "file_path", &c);
		const char *p = calldata_string(&c, "path");
		const QString path = QString::fromUtf8(p ? p : "");
		calldata_free(&c);

		auto *item = new QListWidgetItem(QFileInfo(path).fileName());
		item->setData(Qt::UserRole, path);
		item->setToolTip(path);
		list->addItem(item);
	}

	calldata_t cur = {0};
	proc_handler_call(ph, "current_index", &cur);
	const int row = (int)calldata_int(&cur, "current_index");
	calldata_free(&cur);
	obs_source_release(src);

	selectRow(row);

	if (listMode == ListMode::Thumbnails)
		thumbTimer->start();
}

void NextPreviewDock::selectRow(int row)
{
	QSignalBlocker block(list);
	list->setCurrentRow(row);
	if (QListWidgetItem *it = list->item(row))
		list->scrollToItem(it);
}

// UI が固まらないよう、サムネイルは数枚ずつ読み込む
void NextPreviewDock::loadNextThumbs()
{
	for (int n = 0; n < 4 && thumbNext < list->count(); n++, thumbNext++) {
		QListWidgetItem *it = list->item(thumbNext);
		QImageReader reader(it->data(Qt::UserRole).toString());
		QSize size = reader.size();
		if (size.isValid()) {
			size.scale(THUMB_SIZE, Qt::KeepAspectRatio);
			reader.setScaledSize(size); // 縮小デコードで高速化
		}
		const QImage img = reader.read();
		if (!img.isNull())
			it->setIcon(QPixmap::fromImage(img));
	}

	if (thumbNext < list->count() && listMode == ListMode::Thumbnails)
		thumbTimer->start();
}

void NextPreviewDock::onFilesChanged(void *param, calldata_t *)
{
	auto *self = static_cast<NextPreviewDock *>(param);
	QMetaObject::invokeMethod(self, [self]() { self->rebuildList(); }, Qt::QueuedConnection);
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
	signal_handler_connect(obs_source_get_signal_handler(src), "files_changed", onFilesChanged, this);
	weak = obs_source_get_weak_source(src);
	setNavEnabled(true);

	// 初期表示:次のスライドをプロシージャで取得
	calldata_t cd = {0};
	proc_handler_call(obs_source_get_proc_handler(src), "next_path", &cd);
	const char *p = calldata_string(&cd, "next_path");
	showImage(QString::fromUtf8(p ? p : ""));
	calldata_free(&cd);

	obs_source_release(src);

	rebuildList();
}

void NextPreviewDock::detach()
{
	if (!weak)
		return;

	if (obs_source_t *src = obs_weak_source_get_source(weak)) {
		signal_handler_disconnect(obs_source_get_signal_handler(src), "slide_changed", onSlideChanged, this);
		signal_handler_disconnect(obs_source_get_signal_handler(src), "files_changed", onFilesChanged, this);
		obs_source_release(src);
	}
	obs_weak_source_release(weak);
	weak = nullptr;

	setNavEnabled(false);
	rebuildList(); // weak が空なので一覧が空になる

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
	const int index = (int)calldata_int(cd, "index");

	QMetaObject::invokeMethod(
		self,
		[self, path, index]() {
			self->showImage(path);
			self->selectRow(index); // 一覧の選択を現在のスライドに合わせる
		},
		Qt::QueuedConnection);
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

// ボタン行や一覧の表示切替などでラベルの領域が変わったときも、プレビューを追従させる
bool NextPreviewDock::eventFilter(QObject *obj, QEvent *e)
{
	if (obj == image && e->type() == QEvent::Resize)
		updateScaled();
	return QWidget::eventFilter(obj, e);
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
