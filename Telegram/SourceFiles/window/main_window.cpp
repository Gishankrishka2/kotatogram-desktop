/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "window/main_window.h"

#include "storage/localstorage.h"
#include "platform/platform_specific.h"
#include "ui/platform/ui_platform_window.h"
#include "platform/platform_window_title.h"
#include "base/platform/base_platform_info.h"
#include "history/history.h"
#include "window/window_session_controller.h"
#include "window/window_lock_widgets.h"
#include "window/window_outdated_bar.h"
#include "window/window_controller.h"
#include "main/main_account.h" // Account::sessionValue.
#include "core/application.h"
#include "core/sandbox.h"
#include "lang/lang_keys.h"
#include "data/data_session.h"
#include "main/main_session.h"
#include "main/main_session_settings.h"
#include "base/crc32hash.h"
#include "base/call_delayed.h"
#include "ui/toast/toast.h"
#include "ui/widgets/shadow.h"
#include "ui/ui_utility.h"
#include "apiwrap.h"
#include "mainwindow.h"
#include "mainwidget.h" // session->content()->windowShown().
#include "facades.h"
#include "app.h"
#include "styles/style_widgets.h"
#include "styles/style_window.h"

#include <QtWidgets/QDesktopWidget>
#include <QtCore/QMimeData>
#include <QtCore/QDir>
#include <QtGui/QGuiApplication>
#include <QtGui/QWindow>
#include <QtGui/QScreen>
#include <QtGui/QDrag>

namespace Window {
namespace {

constexpr auto kSaveWindowPositionTimeout = crl::time(1000);

} // namespace

QString LogoVariant(int variant) {
	switch (variant) {
		case 1: return QString("_blue");
		case 2: return QString("_green");
		case 3: return QString("_orange");
		case 4: return QString("_red");
		case 5: return QString("_old");
	}

	return QString();
}

QImage LoadLogo(int variant) {
	return QImage(qsl(":/gui/art/logo_256%1.png").arg(LogoVariant(variant)));
}

QImage LoadLogoNoMargin(int variant) {
	return QImage(qsl(":/gui/art/logo_256_no_margin%1.png").arg(LogoVariant(variant)));
}

void ConvertIconToBlack(QImage &image) {
	if (image.format() != QImage::Format_ARGB32_Premultiplied) {
		image = std::move(image).convertToFormat(
			QImage::Format_ARGB32_Premultiplied);
	}
	//const auto gray = red * 0.299 + green * 0.587 + blue * 0.114;
	//const auto result = (gray - 100 < 0) ? 0 : (gray - 100) * 255 / 155;
	constexpr auto scale = 255 / 155.;
	constexpr auto red = 0.299;
	constexpr auto green = 0.587;
	constexpr auto blue = 0.114;
	static constexpr auto shift = (1 << 24);
	auto shifter = [](double value) {
		return uint32(value * shift);
	};
	constexpr auto iscale = shifter(scale);
	constexpr auto ired = shifter(red);
	constexpr auto igreen = shifter(green);
	constexpr auto iblue = shifter(blue);
	constexpr auto threshold = 100;

	const auto width = image.width();
	const auto height = image.height();
	const auto data = reinterpret_cast<uint32*>(image.bits());
	const auto intsPerLine = image.bytesPerLine() / 4;
	const auto intsPerLineAdded = intsPerLine - width;

	auto pixel = data;
	for (auto j = 0; j != height; ++j) {
		for (auto i = 0; i != width; ++i) {
			const auto value = *pixel;
			const auto gray = (((value >> 16) & 0xFF) * ired
				+ ((value >> 8) & 0xFF) * igreen
				+ (value & 0xFF) * iblue) >> 24;
			const auto small = gray - threshold;
			const auto test = ~small;
			const auto result = (test >> 31) * small * iscale;
			const auto component = (result >> 24) & 0xFF;
			*pixel++ = (value & 0xFF000000U)
				| (component << 16)
				| (component << 8)
				| component;
		}
		pixel += intsPerLineAdded;
	}
}

QIcon CreateOfficialIcon(Main::Session *session) {
	const auto customIcon = QImage(cWorkingDir() + "tdata/icon.png");

	auto image = customIcon.isNull()
		? Core::IsAppLaunched()
			? Core::App().logo(cCustomAppIcon())
			: LoadLogo(cCustomAppIcon())
		: customIcon;

	if (session && session->supportMode()) {
		ConvertIconToBlack(image);
	}
	return QIcon(Ui::PixmapFromImage(std::move(image)));
}

QIcon CreateIcon(Main::Session *session) {
	auto result = CreateOfficialIcon(session);

#if defined Q_OS_UNIX && !defined Q_OS_MAC
	if ((session && session->supportMode())
		|| (cCustomAppIcon() != 0)
		|| QFileInfo::exists(cWorkingDir() + "tdata/icon.png")) {
		return result;
	}

	const auto iconFromTheme = QIcon::fromTheme(
		Platform::GetIconName(),
		result);

	result = QIcon();

	static const auto iconSizes = {
		16,
		22,
		32,
		48,
		64,
		128,
		256,
	};

	// Qt's standard QIconLoaderEngine sets availableSizes
	// to XDG directories sizes, since svg icons are scalable,
	// they could be only in one XDG folder (like 48x48)
	// and Qt will set only a 48px icon to the window
	// even though the icon could be scaled to other sizes.
	// Thus, scale it manually to the most widespread sizes.
	for (const auto iconSize : iconSizes) {
		// We can't use QIcon::actualSize here
		// since it works incorrectly with svg icon themes
		const auto iconPixmap = iconFromTheme.pixmap(iconSize);

		const auto iconPixmapSize = iconPixmap.size()
			/ iconPixmap.devicePixelRatio();

		// Not a svg icon, don't scale it
		if (iconPixmapSize.width() != iconSize) {
			return iconFromTheme;
		}

		result.addPixmap(iconPixmap);
	}
#endif

	return result;
}

MainWindow::MainWindow(not_null<Controller*> controller)
: _controller(controller)
, _positionUpdatedTimer([=] { savePosition(); })
, _outdated(CreateOutdatedBar(body()))
, _body(body()) {
	style::PaletteChanged(
	) | rpl::start_with_next([=] {
		updatePalette();
	}, lifetime());

	Core::App().unreadBadgeChanges(
	) | rpl::start_with_next([=] {
		updateUnreadCounter();
	}, lifetime());

	Core::App().settings().workModeChanges(
	) | rpl::start_with_next([=](Core::Settings::WorkMode mode) {
		workmodeUpdated(mode);
	}, lifetime());

	Ui::Toast::SetDefaultParent(_body.data());

	if (_outdated) {
		_outdated->heightValue(
		) | rpl::filter([=] {
			return window()->windowHandle() != nullptr;
		}) | rpl::start_with_next([=](int height) {
			if (!height) {
				crl::on_main(this, [=] { _outdated.destroy(); });
			}
			updateControlsGeometry();
		}, _outdated->lifetime());
	}
}

Main::Account &MainWindow::account() const {
	return _controller->account();
}

Window::SessionController *MainWindow::sessionController() const {
	return _controller->sessionController();
}

bool MainWindow::hideNoQuit() {
	if (App::quitting()) {
		return false;
	}
	const auto workMode = Core::App().settings().workMode();
	if (workMode == Core::Settings::WorkMode::TrayOnly
		|| workMode == Core::Settings::WorkMode::WindowAndTray) {
		if (minimizeToTray()) {
			if (const auto controller = sessionController()) {
				Ui::showChatsList(&controller->session());
			}
			return true;
		}
	} else if (Platform::IsMac()) {
		closeWithoutDestroy();
		controller().updateIsActiveBlur();
		updateGlobalMenu();
		if (const auto controller = sessionController()) {
			Ui::showChatsList(&controller->session());
		}
		return true;
	}
	return false;
}

void MainWindow::clearWidgets() {
	clearWidgetsHook();
	updateGlobalMenu();
}

void MainWindow::updateIsActive() {
	const auto isActive = computeIsActive();
	if (_isActive != isActive) {
		_isActive = isActive;
		activeChangedHook();
	}
}

bool MainWindow::computeIsActive() const {
	return isActiveWindow() && isVisible() && !(windowState() & Qt::WindowMinimized);
}

void MainWindow::updateWindowIcon() {
	const auto session = sessionController()
		? &sessionController()->session()
		: nullptr;
	const auto supportIcon = session && session->supportMode();
	if (supportIcon != _usingSupportIcon || _icon.isNull() || _customIconId != cCustomAppIcon()) {
		_icon = CreateIcon(session);
		_usingSupportIcon = supportIcon;
		_customIconId = cCustomAppIcon();
	}
	setWindowIcon(_icon);
}

QRect MainWindow::desktopRect() const {
	const auto now = crl::now();
	if (!_monitorLastGot || now >= _monitorLastGot + crl::time(1000)) {
		_monitorLastGot = now;
		_monitorRect = computeDesktopRect();
	}
	return _monitorRect;
}

void MainWindow::init() {
	createWinId();

	initHook();
	updateWindowIcon();

	// Non-queued activeChanged handlers must use QtSignalProducer.
	connect(
		windowHandle(),
		&QWindow::activeChanged,
		this,
		[=] { handleActiveChanged(); },
		Qt::QueuedConnection);
	connect(
		windowHandle(),
		&QWindow::windowStateChanged,
		this,
		[=](Qt::WindowState state) { handleStateChanged(state); });
	connect(
		windowHandle(),
		&QWindow::visibleChanged,
		this,
		[=](bool visible) { handleVisibleChanged(visible); });

	updatePalette();

	if (Ui::Platform::NativeWindowFrameSupported()) {
		Core::App().settings().nativeWindowFrameChanges(
		) | rpl::start_with_next([=](bool native) {
			refreshTitleWidget();
			recountGeometryConstraints();
		}, lifetime());
	}
	refreshTitleWidget();

	initGeometry();
	updateUnreadCounter();
}

void MainWindow::handleStateChanged(Qt::WindowState state) {
	stateChangedHook(state);
	updateControlsGeometry();
	if (state == Qt::WindowMinimized) {
		controller().updateIsActiveBlur();
	} else {
		controller().updateIsActiveFocus();
	}
	Core::App().updateNonIdle();
	using WorkMode = Core::Settings::WorkMode;
	if (state == Qt::WindowMinimized
		&& (Core::App().settings().workMode() == WorkMode::TrayOnly)) {
		minimizeToTray();
	}
	savePosition(state);
}

void MainWindow::handleActiveChanged() {
	if (isActiveWindow()) {
		Core::App().checkMediaViewActivation();
	}
	base::call_delayed(1, this, [this] {
		handleActiveChangedHook();
	});
}

void MainWindow::handleVisibleChanged(bool visible) {
	if (visible) {
		if (_maximizedBeforeHide) {
			DEBUG_LOG(("Window Pos: Window was maximized before hidding, setting maximized."));
			setWindowState(Qt::WindowMaximized);
		}
	} else {
		_maximizedBeforeHide = Core::App().settings().windowPosition().maximized;
	}

	handleVisibleChangedHook(visible);
}

void MainWindow::showFromTray() {
	base::call_delayed(1, this, [this] {
		updateGlobalMenu();
	});
	activate();
	updateUnreadCounter();
}

void MainWindow::quitFromTray() {
	App::quit();
}

void MainWindow::activate() {
	bool wasHidden = !isVisible();
	setWindowState(windowState() & ~Qt::WindowMinimized);
	setVisible(true);
	psActivateProcess();
	raise();
	activateWindow();
	controller().updateIsActiveFocus();
	if (wasHidden) {
		if (const auto session = sessionController()) {
			session->content()->windowShown();
		}
	}
}

void MainWindow::updatePalette() {
	Ui::ForceFullRepaint(this);

	auto p = palette();
	p.setColor(QPalette::Window, st::windowBg->c);
	setPalette(p);
}

int MainWindow::computeMinWidth() const {
	auto result = st::windowMinWidth;
	if (const auto session = _controller->sessionController()) {
		if (const auto add = session->filtersWidth()) {
			result += add;
		}
	}
	if (_rightColumn) {
		result += _rightColumn->width();
	}
	return result;
}

int MainWindow::computeMinHeight() const {
	const auto outdated = [&] {
		if (!_outdated) {
			return 0;
		}
		_outdated->resizeToWidth(st::windowMinWidth);
		return _outdated->height();
	}();
	return outdated + st::windowMinHeight;
}

void MainWindow::refreshTitleWidget() {
	if (Ui::Platform::NativeWindowFrameSupported()
		&& Core::App().settings().nativeWindowFrame()) {
		setNativeFrame(true);
		if (Platform::NativeTitleRequiresShadow()) {
			_titleShadow.create(this);
			_titleShadow->show();
		}
	} else {
		setNativeFrame(false);
		_titleShadow.destroy();
	}
}

void MainWindow::updateMinimumSize() {
	setMinimumWidth(computeMinWidth());
	setMinimumHeight(computeMinHeight());
}

void MainWindow::recountGeometryConstraints() {
	updateMinimumSize();
	updateControlsGeometry();
	fixOrder();
}

Core::WindowPosition MainWindow::positionFromSettings() const {
	auto position = Core::App().settings().windowPosition();
	DEBUG_LOG(("Window Pos: Initializing first %1, %2, %3, %4 "
		"(scale %5%, maximized %6)")
		.arg(position.x)
		.arg(position.y)
		.arg(position.w)
		.arg(position.h)
		.arg(position.scale)
		.arg(Logs::b(position.maximized)));

	if (!position.scale) {
		return position;
	}
	const auto scaleFactor = cScale() / float64(position.scale);
	position.x *= scaleFactor;
	position.y *= scaleFactor;
	position.w *= scaleFactor;
	position.h *= scaleFactor;
	return position;
}

QRect MainWindow::countInitialGeometry(Core::WindowPosition position) {
	const auto primaryScreen = QGuiApplication::primaryScreen();
	const auto primaryAvailable = primaryScreen
		? primaryScreen->availableGeometry()
		: QRect(0, 0, st::windowDefaultWidth, st::windowDefaultHeight);
	const auto initialWidth = Core::Settings::ThirdColumnByDefault()
		? st::windowBigDefaultWidth
		: st::windowDefaultWidth;
	const auto initialHeight = Core::Settings::ThirdColumnByDefault()
		? st::windowBigDefaultHeight
		: st::windowDefaultHeight;
	const auto initial = QRect(
		primaryAvailable.x() + std::max(
			(primaryAvailable.width() - initialWidth) / 2,
			0),
		primaryAvailable.y() + std::max(
			(primaryAvailable.height() - initialHeight) / 2,
			0),
		initialWidth,
		initialHeight);
	if (!position.w || !position.h) {
		return initial;
	}
	const auto screen = [&]() -> QScreen* {
		for (const auto screen : QGuiApplication::screens()) {
			if (position.moncrc == screenNameChecksum(screen->name())) {
				return screen;
			}
		}
		return nullptr;
	}();
	if (!screen) {
		return initial;
	}
	const auto frame = [&] {
		if (!Core::App().settings().nativeWindowFrame()) {
			return QMargins();
		}
		const auto inner = geometry();
		const auto outer = frameGeometry();
		return QMargins(
			inner.x() - outer.x(),
			inner.y() - outer.y(),
			outer.x() + outer.width() - inner.x() - inner.width(),
			outer.y() + outer.height() - inner.y() - inner.height());
	}();

	const auto screenGeometry = screen->geometry();
	const auto availableGeometry = screen->availableGeometry();
	const auto spaceForInner = availableGeometry.marginsRemoved(
		frame);
	DEBUG_LOG(("Window Pos: "
		"Screen found, screen geometry: %1, %2, %3, %4"
		).arg(screenGeometry.x()
		).arg(screenGeometry.y()
		).arg(screenGeometry.width()
		).arg(screenGeometry.height()));

	const auto x = spaceForInner.x() - screenGeometry.x();
	const auto y = spaceForInner.y() - screenGeometry.y();
	const auto w = spaceForInner.width();
	const auto h = spaceForInner.height();
	if (w < st::windowMinWidth || h < st::windowMinHeight) {
		return initial;
	}
	if (position.x < x) position.x = x;
	if (position.y < y) position.y = y;
	if (position.w > w) position.w = w;
	if (position.h > h) position.h = h;
	const auto rightPoint = position.x + position.w;
	const auto screenRightPoint = x + w;
	if (rightPoint > screenRightPoint) {
		const auto distance = rightPoint - screenRightPoint;
		const auto newXPos = position.x - distance;
		if (newXPos >= x) {
			position.x = newXPos;
		} else {
			position.x = x;
			const auto newRightPoint = position.x + position.w;
			const auto newDistance = newRightPoint - screenRightPoint;
			position.w -= newDistance;
		}
	}
	const auto bottomPoint = position.y + position.h;
	const auto screenBottomPoint = y + h;
	if (bottomPoint > screenBottomPoint) {
		const auto distance = bottomPoint - screenBottomPoint;
		const auto newYPos = position.y - distance;
		if (newYPos >= y) {
			position.y = newYPos;
		} else {
			position.y = y;
			const auto newBottomPoint = position.y + position.h;
			const auto newDistance = newBottomPoint - screenBottomPoint;
			position.h -= newDistance;
		}
	}
	position.x += screenGeometry.x();
	position.y += screenGeometry.y();
	if ((position.x + st::windowMinWidth
		> screenGeometry.x() + screenGeometry.width())
		|| (position.y + st::windowMinHeight
			> screenGeometry.y() + screenGeometry.height())) {
		return initial;
	}
	DEBUG_LOG(("Window Pos: Resulting geometry is %1, %2, %3, %4"
		).arg(position.x
		).arg(position.y
		).arg(position.w
		).arg(position.h));
	return QRect(position.x, position.y, position.w, position.h);
}

void MainWindow::initGeometry() {
	updateMinimumSize();
	if (initGeometryFromSystem()) {
		return;
	}
	const auto geometry = countInitialGeometry(positionFromSettings());
	DEBUG_LOG(("Window Pos: Setting first %1, %2, %3, %4"
		).arg(geometry.x()
		).arg(geometry.y()
		).arg(geometry.width()
		).arg(geometry.height()));
	setGeometry(geometry);
}

void MainWindow::positionUpdated() {
	_positionUpdatedTimer.callOnce(kSaveWindowPositionTimeout);
}

int32 MainWindow::screenNameChecksum(const QString &name) const {
	const auto bytes = name.toUtf8();
	return base::crc32(bytes.constData(), bytes.size());
}

void MainWindow::setPositionInited() {
	_positionInited = true;
}

void MainWindow::attachToTrayIcon(not_null<QSystemTrayIcon*> icon) {
	const auto workdir = QDir::toNativeSeparators(QDir::cleanPath(cWorkingDir()));
	icon->setToolTip(AppName.utf16()+"\n"+workdir);
	connect(icon, &QSystemTrayIcon::activated, this, [=](
			QSystemTrayIcon::ActivationReason reason) {
		Core::Sandbox::Instance().customEnterFromEventLoop([&] {
			handleTrayIconActication(reason);
		});
	});
}

void MainWindow::resizeEvent(QResizeEvent *e) {
	updateControlsGeometry();
}

rpl::producer<> MainWindow::leaveEvents() const {
	return _leaveEvents.events();
}

void MainWindow::leaveEventHook(QEvent *e) {
	_leaveEvents.fire({});
}

void MainWindow::updateControlsGeometry() {
	const auto inner = body()->rect();
	auto bodyLeft = inner.x();
	auto bodyTop = inner.y();
	auto bodyWidth = inner.width();
	if (_titleShadow) {
		_titleShadow->setGeometry(inner.x(), bodyTop, inner.width(), st::lineWidth);
	}
	if (_outdated) {
		Ui::SendPendingMoveResizeEvents(_outdated.data());
		_outdated->resizeToWidth(inner.width());
		_outdated->moveToLeft(inner.x(), bodyTop);
		bodyTop += _outdated->height();
	}
	if (_rightColumn) {
		bodyWidth -= _rightColumn->width();
		_rightColumn->setGeometry(bodyWidth, bodyTop, inner.width() - bodyWidth, inner.height() - (bodyTop - inner.y()));
	}
	_body->setGeometry(bodyLeft, bodyTop, bodyWidth, inner.height() - (bodyTop - inner.y()));
}

void MainWindow::updateUnreadCounter() {
	if (App::quitting()) {
		return;
	}

	const auto counter = Core::App().unreadBadge();
	setTitle((counter > 0) ? qsl("Kotatogram (%1)").arg(counter) : qsl("Kotatogram"));

	unreadCounterChangedHook();
}

QRect MainWindow::computeDesktopRect() const {
	return QApplication::desktop()->availableGeometry(this);
}

void MainWindow::savePosition(Qt::WindowState state) {
	if (state == Qt::WindowActive) {
		state = windowHandle()->windowState();
	}

	if (state == Qt::WindowMinimized
		|| !isVisible()
		|| !positionInited()) {
		return;
	}

	const auto &savedPosition = Core::App().settings().windowPosition();
	auto realPosition = savedPosition;

	if (state == Qt::WindowMaximized) {
		realPosition.maximized = 1;
		DEBUG_LOG(("Window Pos: Saving maximized position."));
	} else {
		auto r = body()->mapToGlobal(body()->rect());
		realPosition.x = r.x();
		realPosition.y = r.y();
		realPosition.w = r.width() - (_rightColumn ? _rightColumn->width() : 0);
		realPosition.h = r.height();
		realPosition.scale = cScale();
		realPosition.maximized = 0;
		realPosition.moncrc = 0;

		DEBUG_LOG(("Window Pos: Saving non-maximized position: %1, %2, %3, %4").arg(realPosition.x).arg(realPosition.y).arg(realPosition.w).arg(realPosition.h));

		auto centerX = realPosition.x + realPosition.w / 2;
		auto centerY = realPosition.y + realPosition.h / 2;
		int minDelta = 0;
		QScreen *chosen = nullptr;
		const auto screens = QGuiApplication::screens();
		for (auto screen : screens) {
			auto delta = (screen->geometry().center() - QPoint(centerX, centerY)).manhattanLength();
			if (!chosen || delta < minDelta) {
				minDelta = delta;
				chosen = screen;
			}
		}
		if (chosen) {
			auto screenGeometry = chosen->geometry();
			DEBUG_LOG(("Window Pos: Screen found, geometry: %1, %2, %3, %4"
				).arg(screenGeometry.x()
				).arg(screenGeometry.y()
				).arg(screenGeometry.width()
				).arg(screenGeometry.height()));
			realPosition.x -= screenGeometry.x();
			realPosition.y -= screenGeometry.y();
			realPosition.moncrc = screenNameChecksum(chosen->name());
		}
	}
	if (realPosition.w >= st::windowMinWidth && realPosition.h >= st::windowMinHeight) {
		if (realPosition.x != savedPosition.x
			|| realPosition.y != savedPosition.y
			|| realPosition.w != savedPosition.w
			|| realPosition.h != savedPosition.h
			|| realPosition.scale != savedPosition.scale
			|| realPosition.moncrc != savedPosition.moncrc
			|| realPosition.maximized != savedPosition.maximized) {
			DEBUG_LOG(("Window Pos: Writing: %1, %2, %3, %4 (scale %5%, maximized %6)")
				.arg(realPosition.x)
				.arg(realPosition.y)
				.arg(realPosition.w)
				.arg(realPosition.h)
				.arg(realPosition.scale)
				.arg(Logs::b(realPosition.maximized)));
			Core::App().settings().setWindowPosition(realPosition);
			Core::App().saveSettingsDelayed();
		}
	}
}

bool MainWindow::minimizeToTray() {
	if (App::quitting() || !hasTrayIcon()) return false;

	closeWithoutDestroy();
	controller().updateIsActiveBlur();
	updateGlobalMenu();
	showTrayTooltip();
	return true;
}

void MainWindow::reActivateWindow() {
#if defined Q_OS_UNIX && !defined Q_OS_MAC
	const auto weak = Ui::MakeWeak(this);
	const auto reActivate = [=] {
		if (const auto w = weak.data()) {
			if (auto f = QApplication::focusWidget()) {
				f->clearFocus();
			}
			w->activate();
			if (auto f = QApplication::focusWidget()) {
				f->clearFocus();
			}
			w->setInnerFocus();
		}
	};
	crl::on_main(this, reActivate);
	base::call_delayed(200, this, reActivate);
#endif // Q_OS_UNIX && !Q_OS_MAC
}

void MainWindow::showRightColumn(object_ptr<TWidget> widget) {
	const auto wasWidth = width();
	const auto wasRightWidth = _rightColumn ? _rightColumn->width() : 0;
	_rightColumn = std::move(widget);
	if (_rightColumn) {
		_rightColumn->setParent(body());
		_rightColumn->show();
		_rightColumn->setFocus();
	} else {
		setInnerFocus();
	}
	const auto nowRightWidth = _rightColumn ? _rightColumn->width() : 0;
	const auto wasMinimumWidth = minimumWidth();
	const auto nowMinimumWidth = computeMinWidth();
	const auto firstResize = (nowMinimumWidth < wasMinimumWidth);
	if (firstResize) {
		setMinimumWidth(nowMinimumWidth);
	}
	if (!isMaximized()) {
		tryToExtendWidthBy(wasWidth + nowRightWidth - wasRightWidth - width());
	} else {
		updateControlsGeometry();
	}
	if (!firstResize) {
		setMinimumWidth(nowMinimumWidth);
	}
}

int MainWindow::maximalExtendBy() const {
	auto desktop = QDesktopWidget().availableGeometry(this);
	return std::max(desktop.width() - body()->width(), 0);
}

bool MainWindow::canExtendNoMove(int extendBy) const {
	auto desktop = QDesktopWidget().availableGeometry(this);
	auto inner = body()->mapToGlobal(body()->rect());
	auto innerRight = (inner.x() + inner.width() + extendBy);
	auto desktopRight = (desktop.x() + desktop.width());
	return innerRight <= desktopRight;
}

int MainWindow::tryToExtendWidthBy(int addToWidth) {
	auto desktop = QDesktopWidget().availableGeometry(this);
	auto inner = body()->mapToGlobal(body()->rect());
	accumulate_min(
		addToWidth,
		std::max(desktop.width() - inner.width(), 0));
	auto newWidth = inner.width() + addToWidth;
	auto newLeft = std::min(
		inner.x(),
		desktop.x() + desktop.width() - newWidth);
	if (inner.x() != newLeft || inner.width() != newWidth) {
		setGeometry(QRect(newLeft, inner.y(), newWidth, inner.height()));
	} else {
		updateControlsGeometry();
	}
	return addToWidth;
}

void MainWindow::launchDrag(
		std::unique_ptr<QMimeData> data,
		Fn<void()> &&callback) {
	auto drag = std::make_unique<QDrag>(this);
	drag->setMimeData(data.release());
	drag->exec(Qt::CopyAction);

	// We don't receive mouseReleaseEvent when drag is finished.
	ClickHandler::unpressed();
	callback();
}

MainWindow::~MainWindow() {
	// Otherwise:
	// ~QWidget
	// QWidgetPrivate::close_helper
	// QWidgetPrivate::setVisible
	// QWidgetPrivate::hide_helper
	// QWidgetPrivate::hide_sys
	// QWindowPrivate::setVisible
	// QMetaObject::activate
	// Window::MainWindow::handleVisibleChanged on a destroyed MainWindow.
	hide();
}

} // namespace Window
