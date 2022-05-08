/*
 * MIT License
 *
 * Copyright (C) 2022 by wangwenx190 (Yuhang Zhao)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "framelesswidgetshelper.h"
#include "framelesswidgetshelper_p.h"
#include <QtCore/qmutex.h>
#include <QtCore/qhash.h>
#include <QtCore/qpointer.h>
#include <QtCore/qtimer.h>
#include <QtGui/qwindow.h>
#include <QtWidgets/qwidget.h>
#include <framelessmanager.h>
#include <framelessconfig_p.h>
#include <utils.h>

FRAMELESSHELPER_BEGIN_NAMESPACE

using namespace Global;

struct WidgetsHelperData
{
    bool attached = false;
    SystemParameters params = {};
    QPointer<QWidget> titleBarWidget = nullptr;
    QWidgetList hitTestVisibleWidgets = {};
    QPointer<QWidget> windowIconButton = nullptr;
    QPointer<QWidget> contextHelpButton = nullptr;
    QPointer<QWidget> minimizeButton = nullptr;
    QPointer<QWidget> maximizeButton = nullptr;
    QPointer<QWidget> closeButton = nullptr;
};

struct WidgetsHelper
{
    QMutex mutex;
    QHash<WId, WidgetsHelperData> data = {};
};

Q_GLOBAL_STATIC(WidgetsHelper, g_widgetsHelper)

FramelessWidgetsHelperPrivate::FramelessWidgetsHelperPrivate(FramelessWidgetsHelper *q) : QObject(q)
{
    Q_ASSERT(q);
    if (!q) {
        return;
    }
    q_ptr = q;
}

FramelessWidgetsHelperPrivate::~FramelessWidgetsHelperPrivate() = default;

FramelessWidgetsHelperPrivate *FramelessWidgetsHelperPrivate::get(FramelessWidgetsHelper *pub)
{
    Q_ASSERT(pub);
    if (!pub) {
        return nullptr;
    }
    return pub->d_func();
}

const FramelessWidgetsHelperPrivate *FramelessWidgetsHelperPrivate::get(const FramelessWidgetsHelper *pub)
{
    Q_ASSERT(pub);
    if (!pub) {
        return nullptr;
    }
    return pub->d_func();
}

bool FramelessWidgetsHelperPrivate::isWindowFixedSize() const
{
    const QWidget * const window = getWindow();
    if (!window) {
        return false;
    }
    if (window->windowFlags() & Qt::MSWindowsFixedSizeDialogHint) {
        return true;
    }
    const QSize minSize = window->minimumSize();
    const QSize maxSize = window->maximumSize();
    if (!minSize.isEmpty() && !maxSize.isEmpty() && (minSize == maxSize)) {
        return true;
    }
    if (window->sizePolicy() == QSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed)) {
        return true;
    }
    return false;
}

void FramelessWidgetsHelperPrivate::setWindowFixedSize(const bool value)
{
    QWidget * const window = getWindow();
    if (!window) {
        return;
    }
    if (isWindowFixedSize() == value) {
        return;
    }
    if (value) {
        window->setFixedSize(window->size());
        window->setWindowFlags(window->windowFlags() | Qt::MSWindowsFixedSizeDialogHint);
    } else {
        window->setWindowFlags(window->windowFlags() & ~Qt::MSWindowsFixedSizeDialogHint);
        window->setMinimumSize(kDefaultWindowSize);
        window->setMaximumSize(QSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX));
    }
#ifdef Q_OS_WINDOWS
    Utils::setAeroSnappingEnabled(window->winId(), !value);
#endif
}

void FramelessWidgetsHelperPrivate::setTitleBarWidget(QWidget *widget)
{
    Q_ASSERT(widget);
    if (!widget) {
        return;
    }
    QMutexLocker locker(&g_widgetsHelper()->mutex);
    WidgetsHelperData *data = getWindowDataMutable();
    if (!data) {
        return;
    }
    if (data->titleBarWidget == widget) {
        return;
    }
    data->titleBarWidget = widget;
    Q_Q(FramelessWidgetsHelper);
    Q_EMIT q->titleBarWidgetChanged();
}

QWidget *FramelessWidgetsHelperPrivate::getTitleBarWidget() const
{
    return getWindowData().titleBarWidget;
}

void FramelessWidgetsHelperPrivate::setHitTestVisible(QWidget *widget)
{
    Q_ASSERT(widget);
    if (!widget) {
        return;
    }
    QMutexLocker locker(&g_widgetsHelper()->mutex);
    WidgetsHelperData *data = getWindowDataMutable();
    if (!data) {
        return;
    }
    static constexpr const bool visible = true;
    const bool exists = data->hitTestVisibleWidgets.contains(widget);
    if (visible && !exists) {
        data->hitTestVisibleWidgets.append(widget);
    }
    if constexpr (!visible && exists) {
        data->hitTestVisibleWidgets.removeAll(widget);
    }
}

void FramelessWidgetsHelperPrivate::attachToWindow()
{
    QWidget * const window = getWindow();
    Q_ASSERT(window);
    if (!window) {
        return;
    }

    g_widgetsHelper()->mutex.lock();
    WidgetsHelperData *data = getWindowDataMutable();
    if (!data) {
        g_widgetsHelper()->mutex.unlock();
        return;
    }
    const bool attached = data->attached;
    g_widgetsHelper()->mutex.unlock();

    if (attached) {
        return;
    }

    // Without this flag, Qt will always create an invisible native parent window
    // for any native widgets which will intercept some win32 messages and confuse
    // our own native event filter, so to prevent some weired bugs from happening,
    // just disable this feature.
    window->setAttribute(Qt::WA_DontCreateNativeAncestors);
    // Force the widget become a native window now so that we can deal with its
    // win32 events as soon as possible.
    window->setAttribute(Qt::WA_NativeWindow);

    SystemParameters params = {};
    params.getWindowId = [window]() -> WId { return window->winId(); };
    params.getWindowFlags = [window]() -> Qt::WindowFlags { return window->windowFlags(); };
    params.setWindowFlags = [window](const Qt::WindowFlags flags) -> void { window->setWindowFlags(flags); };
    params.getWindowSize = [window]() -> QSize { return window->size(); };
    params.setWindowSize = [window](const QSize &size) -> void { window->resize(size); };
    params.getWindowPosition = [window]() -> QPoint { return window->pos(); };
    params.setWindowPosition = [window](const QPoint &pos) -> void { window->move(pos); };
    params.getWindowScreen = [window]() -> QScreen * {
#if (QT_VERSION >= QT_VERSION_CHECK(5, 14, 0))
        return window->screen();
#else
        return window->windowHandle()->screen();
#endif
    };
    params.isWindowFixedSize = [this]() -> bool { return isWindowFixedSize(); };
    params.setWindowFixedSize = [this](const bool value) -> void { setWindowFixedSize(value); };
    params.getWindowState = [window]() -> Qt::WindowState { return Utils::windowStatesToWindowState(window->windowState()); };
    params.setWindowState = [window](const Qt::WindowState state) -> void { window->setWindowState(state); };
    params.getWindowHandle = [window]() -> QWindow * { return window->windowHandle(); };
    params.windowToScreen = [window](const QPoint &pos) -> QPoint { return window->mapToGlobal(pos); };
    params.screenToWindow = [window](const QPoint &pos) -> QPoint { return window->mapFromGlobal(pos); };
    params.isInsideSystemButtons = [this](const QPoint &pos, SystemButtonType *button) -> bool { return isInSystemButtons(pos, button); };
    params.isInsideTitleBarDraggableArea = [this](const QPoint &pos) -> bool { return isInTitleBarDraggableArea(pos); };
    params.getWindowDevicePixelRatio = [window]() -> qreal { return window->devicePixelRatioF(); };
    params.setSystemButtonState = [this](const SystemButtonType button, const ButtonState state) -> void { setSystemButtonState(button, state); };
    params.shouldIgnoreMouseEvents = [this](const QPoint &pos) -> bool { return shouldIgnoreMouseEvents(pos); };
    params.showSystemMenu = [this](const QPoint &pos) -> void { showSystemMenu(pos); };

    g_widgetsHelper()->mutex.lock();
    data->params = params;
    data->attached = true;
    g_widgetsHelper()->mutex.unlock();

    FramelessManager::instance()->addWindow(params);

    // We have to wait for a little time before moving the top level window
    // , because the platform window may not finish initializing by the time
    // we reach here, and all the modifications from the Qt side will be lost
    // due to QPA will reset the position and size of the window during it's
    // initialization process.
    QTimer::singleShot(0, this, [this, window](){
        if (FramelessConfig::instance()->isSet(Option::CenterWindowBeforeShow)) {
            moveWindowToDesktopCenter();
        }
        window->setVisible(true);
    });
}

QWidget *FramelessWidgetsHelperPrivate::getWindow() const
{
    Q_Q(const FramelessWidgetsHelper);
    const auto parentWidget = qobject_cast<QWidget *>(q->parent());
    if (!parentWidget) {
        return nullptr;
    }
    QWidget * const nativeParentWidget = parentWidget->nativeParentWidget();
    if (nativeParentWidget) {
        return nativeParentWidget;
    }
    return parentWidget->window();
}

WidgetsHelperData FramelessWidgetsHelperPrivate::getWindowData() const
{
    const QWidget * const window = getWindow();
    //Q_ASSERT(window);
    if (!window) {
        return {};
    }
    const WId windowId = window->winId();
    QMutexLocker locker(&g_widgetsHelper()->mutex);
    if (!g_widgetsHelper()->data.contains(windowId)) {
        g_widgetsHelper()->data.insert(windowId, {});
    }
    return g_widgetsHelper()->data.value(windowId);
}

WidgetsHelperData *FramelessWidgetsHelperPrivate::getWindowDataMutable() const
{
    const QWidget * const window = getWindow();
    //Q_ASSERT(window);
    if (!window) {
        return nullptr;
    }
    const WId windowId = window->winId();
    if (!g_widgetsHelper()->data.contains(windowId)) {
        g_widgetsHelper()->data.insert(windowId, {});
    }
    return &g_widgetsHelper()->data[windowId];
}

QRect FramelessWidgetsHelperPrivate::mapWidgetGeometryToScene(const QWidget * const widget) const
{
    Q_ASSERT(widget);
    if (!widget) {
        return {};
    }
    const QWidget * const window = getWindow();
    if (!window) {
        return {};
    }
    const QPoint originPoint = widget->mapTo(window, QPoint(0, 0));
    const QSize size = widget->size();
    return QRect(originPoint, size);
}

bool FramelessWidgetsHelperPrivate::isInSystemButtons(const QPoint &pos, SystemButtonType *button) const
{
    Q_ASSERT(button);
    if (!button) {
        return false;
    }
    *button = SystemButtonType::Unknown;
    const WidgetsHelperData data = getWindowData();
    if (data.windowIconButton) {
        if (data.windowIconButton->geometry().contains(pos)) {
            *button = SystemButtonType::WindowIcon;
            return true;
        }
    }
    if (data.contextHelpButton) {
        if (data.contextHelpButton->geometry().contains(pos)) {
            *button = SystemButtonType::Help;
            return true;
        }
    }
    if (data.minimizeButton) {
        if (data.minimizeButton->geometry().contains(pos)) {
            *button = SystemButtonType::Minimize;
            return true;
        }
    }
    if (data.maximizeButton) {
        if (data.maximizeButton->geometry().contains(pos)) {
            *button = SystemButtonType::Maximize;
            return true;
        }
    }
    if (data.closeButton) {
        if (data.closeButton->geometry().contains(pos)) {
            *button = SystemButtonType::Close;
            return true;
        }
    }
    return false;
}

bool FramelessWidgetsHelperPrivate::isInTitleBarDraggableArea(const QPoint &pos) const
{
    const WidgetsHelperData data = getWindowData();
    if (!data.titleBarWidget) {
        return false;
    }
    QRegion region = mapWidgetGeometryToScene(data.titleBarWidget);
    const auto systemButtons = {data.windowIconButton, data.contextHelpButton,
                     data.minimizeButton, data.maximizeButton, data.closeButton};
    for (auto &&button : qAsConst(systemButtons)) {
        if (button) {
            region -= mapWidgetGeometryToScene(button);
        }
    }
    if (!data.hitTestVisibleWidgets.isEmpty()) {
        for (auto &&widget : qAsConst(data.hitTestVisibleWidgets)) {
            Q_ASSERT(widget);
            if (widget) {
                region -= mapWidgetGeometryToScene(widget);
            }
        }
    }
    return region.contains(pos);
}

bool FramelessWidgetsHelperPrivate::shouldIgnoreMouseEvents(const QPoint &pos) const
{
    const QWidget * const window = getWindow();
    if (!window) {
        return false;
    }
    const bool withinFrameBorder = [&pos, window]() -> bool {
        if (pos.y() < kDefaultResizeBorderThickness) {
            return true;
        }
#ifdef Q_OS_WINDOWS
        if (Utils::isWindowFrameBorderVisible()) {
            return false;
        }
#endif
        return ((pos.x() < kDefaultResizeBorderThickness)
                || (pos.x() >= (window->width() - kDefaultResizeBorderThickness)));
    }();
    return ((Utils::windowStatesToWindowState(window->windowState()) == Qt::WindowNoState) && withinFrameBorder);
}

void FramelessWidgetsHelperPrivate::setSystemButtonState(const SystemButtonType button, const ButtonState state)
{
    Q_ASSERT(button != SystemButtonType::Unknown);
    if (button == SystemButtonType::Unknown) {
        return;
    }
    const WidgetsHelperData data = getWindowData();
    QWidget *widgetButton = nullptr;
    switch (button) {
    case SystemButtonType::Unknown: {
        Q_ASSERT(false);
    } break;
    case SystemButtonType::WindowIcon: {
        if (data.windowIconButton) {
            widgetButton = data.windowIconButton;
        }
    } break;
    case SystemButtonType::Help: {
        if (data.contextHelpButton) {
            widgetButton = data.contextHelpButton;
        }
    } break;
    case SystemButtonType::Minimize: {
        if (data.minimizeButton) {
            widgetButton = data.minimizeButton;
        }
    } break;
    case SystemButtonType::Maximize:
    case SystemButtonType::Restore: {
        if (data.maximizeButton) {
            widgetButton = data.maximizeButton;
        }
    } break;
    case SystemButtonType::Close: {
        if (data.closeButton) {
            widgetButton = data.closeButton;
        }
    } break;
    }
    if (widgetButton) {
        const auto updateButtonState = [state](QWidget *btn) -> void {
            Q_ASSERT(btn);
            if (!btn) {
                return;
            }
            switch (state) {
            case ButtonState::Unspecified: {
                QMetaObject::invokeMethod(btn, "setPressed", Q_ARG(bool, false));
                QMetaObject::invokeMethod(btn, "setHovered", Q_ARG(bool, false));
            } break;
            case ButtonState::Hovered: {
                QMetaObject::invokeMethod(btn, "setPressed", Q_ARG(bool, false));
                QMetaObject::invokeMethod(btn, "setHovered", Q_ARG(bool, true));
            } break;
            case ButtonState::Pressed: {
                QMetaObject::invokeMethod(btn, "setHovered", Q_ARG(bool, true));
                QMetaObject::invokeMethod(btn, "setPressed", Q_ARG(bool, true));
            } break;
            case ButtonState::Clicked: {
                // Clicked: pressed --> released, so behave like hovered.
                QMetaObject::invokeMethod(btn, "setPressed", Q_ARG(bool, false));
                QMetaObject::invokeMethod(btn, "setHovered", Q_ARG(bool, true));
                // Trigger the clicked signal.
                QMetaObject::invokeMethod(btn, "clicked");
            } break;
            }
        };
        if (const auto mo = widgetButton->metaObject()) {
            const int pressedIndex = mo->indexOfSlot(QMetaObject::normalizedSignature("setPressed(bool)").constData());
            const int hoveredIndex = mo->indexOfSlot(QMetaObject::normalizedSignature("setHovered(bool)").constData());
            const int clickedIndex = mo->indexOfSignal(QMetaObject::normalizedSignature("clicked()").constData());
            if ((pressedIndex >= 0) && (hoveredIndex >= 0) && (clickedIndex >= 0)) {
                updateButtonState(widgetButton);
            }
        }
    }
}

void FramelessWidgetsHelperPrivate::moveWindowToDesktopCenter()
{
    QWidget * const window = getWindow();
    if (!window) {
        return;
    }
    Utils::moveWindowToDesktopCenter([window]() -> QScreen * {
#if (QT_VERSION >= QT_VERSION_CHECK(5, 14, 0))
        return window->screen();
#else
        return window->windowHandle()->screen();
#endif
        },
        [window]() -> QSize { return window->size(); },
        [window](const QPoint &pos) -> void { window->move(pos); }, true);
}

void FramelessWidgetsHelperPrivate::bringWindowToFront()
{
    QWidget * const window = getWindow();
    if (!window) {
        return;
    }
    if (window->isHidden()) {
        window->show();
    }
    if (window->isMinimized()) {
        window->setWindowState(window->windowState() & ~Qt::WindowMinimized);
    }
    window->raise();
    window->activateWindow();
}

void FramelessWidgetsHelperPrivate::showSystemMenu(const QPoint &pos)
{
#ifdef Q_OS_WINDOWS
    const QWidget * const window = getWindow();
    if (!window) {
        return;
    }
    const QPoint globalPos = window->mapToGlobal(pos);
    const QPoint nativePos = QPointF(QPointF(globalPos) * window->devicePixelRatioF()).toPoint();
    Utils::showSystemMenu(window->winId(), nativePos, false, [this]() -> bool { return isWindowFixedSize(); });
#else
    Q_UNUSED(pos);
#endif
}

void FramelessWidgetsHelperPrivate::windowStartSystemMove2(const QPoint &pos)
{
    const QWidget * const window = getWindow();
    if (!window) {
        return;
    }
    Utils::startSystemMove(window->windowHandle(), pos);
}

void FramelessWidgetsHelperPrivate::windowStartSystemResize2(const Qt::Edges edges, const QPoint &pos)
{
    const QWidget * const window = getWindow();
    if (!window) {
        return;
    }
    if (edges == Qt::Edges{}) {
        return;
    }
    Utils::startSystemResize(window->windowHandle(), edges, pos);
}

void FramelessWidgetsHelperPrivate::setSystemButton(QWidget *widget, const SystemButtonType buttonType)
{
    Q_ASSERT(widget);
    Q_ASSERT(buttonType != SystemButtonType::Unknown);
    if (!widget || (buttonType == SystemButtonType::Unknown)) {
        return;
    }
    QMutexLocker locker(&g_widgetsHelper()->mutex);
    WidgetsHelperData *data = getWindowDataMutable();
    if (!data) {
        return;
    }
    switch (buttonType) {
    case SystemButtonType::Unknown:
        Q_ASSERT(false);
        break;
    case SystemButtonType::WindowIcon:
        data->windowIconButton = widget;
        break;
    case SystemButtonType::Help:
        data->contextHelpButton = widget;
        break;
    case SystemButtonType::Minimize:
        data->minimizeButton = widget;
        break;
    case SystemButtonType::Maximize:
    case SystemButtonType::Restore:
        data->maximizeButton = widget;
        break;
    case SystemButtonType::Close:
        data->closeButton = widget;
        break;
    }
}

FramelessWidgetsHelper::FramelessWidgetsHelper(QObject *parent)
    : QObject(parent), d_ptr(new FramelessWidgetsHelperPrivate(this))
{
}

FramelessWidgetsHelper::~FramelessWidgetsHelper() = default;

FramelessWidgetsHelper *FramelessWidgetsHelper::get(QObject *object)
{
    Q_ASSERT(object);
    if (!object) {
        return nullptr;
    }
    FramelessWidgetsHelper *instance = nullptr;
    QObject *parent = nullptr;
    if (object->isWidgetType()) {
        const auto widget = qobject_cast<QWidget *>(object);
        parent = (widget->nativeParentWidget() ? widget->nativeParentWidget() : widget->window());
    } else {
        parent = object;
    }
    instance = parent->findChild<FramelessWidgetsHelper *>();
    if (!instance) {
        instance = new FramelessWidgetsHelper(parent);
        instance->d_func()->attachToWindow();
    }
    return instance;
}

QWidget *FramelessWidgetsHelper::titleBarWidget() const
{
    Q_D(const FramelessWidgetsHelper);
    return d->getTitleBarWidget();
}

bool FramelessWidgetsHelper::isWindowFixedSize() const
{
    Q_D(const FramelessWidgetsHelper);
    return d->isWindowFixedSize();
}

void FramelessWidgetsHelper::extendsContentIntoTitleBar()
{
    // Intentionally not doing anything here.
}

void FramelessWidgetsHelper::setTitleBarWidget(QWidget *widget)
{
    Q_ASSERT(widget);
    if (!widget) {
        return;
    }
    Q_D(FramelessWidgetsHelper);
    d->setTitleBarWidget(widget);
}

void FramelessWidgetsHelper::setSystemButton(QWidget *widget, const SystemButtonType buttonType)
{
    Q_ASSERT(widget);
    Q_ASSERT(buttonType != SystemButtonType::Unknown);
    if (!widget || (buttonType == SystemButtonType::Unknown)) {
        return;
    }
    Q_D(FramelessWidgetsHelper);
    d->setSystemButton(widget, buttonType);
}

void FramelessWidgetsHelper::setHitTestVisible(QWidget *widget)
{
    Q_ASSERT(widget);
    if (!widget) {
        return;
    }
    Q_D(FramelessWidgetsHelper);
    d->setHitTestVisible(widget);
}

void FramelessWidgetsHelper::showSystemMenu(const QPoint &pos)
{
    Q_D(FramelessWidgetsHelper);
    d->showSystemMenu(pos);
}

void FramelessWidgetsHelper::windowStartSystemMove2(const QPoint &pos)
{
    Q_D(FramelessWidgetsHelper);
    d->windowStartSystemMove2(pos);
}

void FramelessWidgetsHelper::windowStartSystemResize2(const Qt::Edges edges, const QPoint &pos)
{
    if (edges == Qt::Edges{}) {
        return;
    }
    Q_D(FramelessWidgetsHelper);
    d->windowStartSystemResize2(edges, pos);
}

void FramelessWidgetsHelper::moveWindowToDesktopCenter()
{
    Q_D(FramelessWidgetsHelper);
    d->moveWindowToDesktopCenter();
}

void FramelessWidgetsHelper::bringWindowToFront()
{
    Q_D(FramelessWidgetsHelper);
    d->bringWindowToFront();
}

void FramelessWidgetsHelper::setWindowFixedSize(const bool value)
{
    Q_D(FramelessWidgetsHelper);
    d->setWindowFixedSize(value);
}

FRAMELESSHELPER_END_NAMESPACE
