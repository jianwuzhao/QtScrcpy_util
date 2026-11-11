#include <QDebug>
#include <QCursor>
#include <QGuiApplication>
#include <QTimer>
#include <QTime>
#include <QRandomGenerator>
#include <QtEndian>

#ifdef Q_OS_MACOS
#include <CoreGraphics/CoreGraphics.h>
#endif

#include "inputconvertgame.h"
#include "adbprocessimpl.h"
#include "controller.h"

#define CURSOR_POS_CHECK 50

InputConvertGame::InputConvertGame(Controller *controller) : InputConvertNormal(controller) {
    m_ctrlSteerWheel.delayData.timer = new QTimer(this);
    m_ctrlSteerWheel.delayData.timer->setSingleShot(true);
    connect(m_ctrlSteerWheel.delayData.timer, &QTimer::timeout, this, &InputConvertGame::onSteerWheelTimer);
    m_steerSprintTimer.setSingleShot(true);
    m_steerSprintTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_steerSprintTimer, &QTimer::timeout, this, [this]() {
        m_ctrlSteerWheel.sprinting = true;
        moveSteerWheel();
    });
    connect(qGuiApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
        if (state != Qt::ApplicationActive) {
            m_cursorKeyHeld = false;
            stopSteerWheel();
            if (m_weaponCapture) {
                m_weaponCapture->kill();
            }
            stopMouseMoveTimer();
            mouseMoveStopTouch();
        }
        updateMouseCursor();
    });
}

InputConvertGame::~InputConvertGame()
{
    if (m_weaponCapture) {
        m_weaponCapture->disconnect(this);
        m_weaponCapture->kill();
    }
    if (m_cursorHidden) {
#ifdef QT_NO_DEBUG
        emit grabCursor(false);
#endif
        hideMouseCursor(false);
    }
}

void InputConvertGame::mouseEvent(const QMouseEvent *from, const QSize &frameSize, const QSize &showSize)
{
    // 处理开关按键
    if (m_keyMap.isSwitchOnKeyboard() == false && m_keyMap.getSwitchKey() == static_cast<int>(from->button())) {
        if (from->type() != QEvent::MouseButtonPress) {
            return;
        }
        switchGameMap();
        return;
    }

    if (m_gameMap && isTemporaryCursorActive()) {
        const bool leftButton = from->button() == Qt::LeftButton;
        const bool outsidePanel = from->pos().x() < showSize.width() * m_cursorPanelLeft;
        if (leftButton && (from->type() == QEvent::MouseButtonPress ||
                           from->type() == QEvent::MouseButtonDblClick)) {
            m_pointerClickActive = true;
            m_pointerPressOutsidePanel = outsidePanel;
        }

        if (from->type() == QEvent::MouseButtonDblClick) {
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
            const QMouseEvent press(QEvent::MouseButtonPress, from->localPos(), from->screenPos(),
                                    from->button(), from->buttons(), from->modifiers());
#else
            const QMouseEvent press(QEvent::MouseButtonPress, from->position(), from->globalPosition(),
                                    from->button(), from->buttons(), from->modifiers());
#endif
            InputConvertNormal::mouseEvent(&press, frameSize, showSize);
        } else {
            InputConvertNormal::mouseEvent(from, frameSize, showSize);
        }

        if (leftButton && from->type() == QEvent::MouseButtonRelease && m_pointerClickActive) {
            m_pointerClickActive = false;
            if (m_pointerPressOutsidePanel && outsidePanel) {
                m_cursorUntilClick = false;
            }
            updateMouseCursor();
        }
        return;
    }

    if (!m_needBackMouseMove && m_gameMap) {
        updateSize(frameSize, showSize);
        // mouse move
        if (m_keyMap.isValidMouseMoveMap()) {
            if (processMouseMove(from)) {
                return;
            }
        }
        // mouse click
        if (processMouseClick(from)) {
            return;
        }
    }
    InputConvertNormal::mouseEvent(from, frameSize, showSize);
}

void InputConvertGame::wheelEvent(const QWheelEvent *from, const QSize &frameSize, const QSize &showSize)
{
    if (m_gameMap && !isTemporaryCursorActive()) {
        updateSize(frameSize, showSize);
    } else {
        InputConvertNormal::wheelEvent(from, frameSize, showSize);
    }
}

void InputConvertGame::keyEvent(const QKeyEvent *from, const QSize &frameSize, const QSize &showSize)
{
    // 处理开关按键
    if (m_keyMap.isSwitchOnKeyboard() && m_keyMap.getSwitchKey() == from->key()) {
        if (QEvent::KeyPress != from->type()) {
            return;
        }
        switchGameMap();
        return;
    }

    const KeyMap::KeyMapNode &node = m_keyMap.getKeyMapNodeKey(from->key());
    if (m_gameMap && m_keyMap.isValidMouseMoveMap() && from->key() == m_keyMap.cursorHoldKey()) {
        if (!from->isAutoRepeat()) {
            m_cursorKeyHeld = from->type() == QEvent::KeyPress;
            updateMouseCursor();
        }
        return;
    }

    // 处理特殊按键：可以释放出鼠标的按键
    if (m_needBackMouseMove && KeyMap::KMT_CLICK == node.type && node.data.click.switchMap) {
        updateSize(frameSize, showSize);
        // Qt::Key_Tab Qt::Key_M for PUBG mobile
        processKeyClick(node.data.click.keyNode.pos, false, node.data.click.switchMap, from);
        return;
    }

    if (m_gameMap) {
        updateSize(frameSize, showSize);
        if (!from || from->isAutoRepeat()) {
            return;
        }

        if (from->key() == m_keyMap.weaponSwitchKey()) {
            if (from->type() == QEvent::KeyPress) {
                requestWeaponSwitch(from->key());
            }
            return;
        }

        // small eyes
        if (m_keyMap.isValidMouseMoveMap() && from->key() == m_keyMap.getMouseMoveMap().data.mouseMove.smallEyes.key) {
            m_ctrlMouseMove.smallEyes = (QEvent::KeyPress == from->type());

            if (QEvent::KeyPress == from->type()) {
                m_processMouseMove = false;
                int delay = 30;
                QTimer::singleShot(delay, this, [this]() { mouseMoveStopTouch(); });
                QTimer::singleShot(delay * 2, this, [this]() {
                    mouseMoveStartTouch(nullptr);
                    m_processMouseMove = true;
                });

                stopMouseMoveTimer();
            } else {
                mouseMoveStopTouch();
                mouseMoveStartTouch(nullptr);
            }
            return;
        }

        switch (node.type) {
        // 处理方向盘
        case KeyMap::KMT_STEER_WHEEL:
            processSteerWheel(node, from);
            return;
        // 处理普通按键
        case KeyMap::KMT_CLICK:
            processKeyClick(node.data.click.keyNode.pos, false, node.data.click.switchMap, from);
            if (node.releaseCursorUntilClick && from->type() == QEvent::KeyRelease) {
                m_cursorUntilClick = true;
                m_cursorPanelLeft = node.cursorPanelLeft;
                m_needBackMouseMove = false;
                updateMouseCursor();
            }
            processAndroidKey(node.data.click.keyNode.androidKey, from);
            return;
        case KeyMap::KMT_CLICK_TWICE:
            processKeyClick(node.data.clickTwice.keyNode.pos, true, false, from);
            processAndroidKey(node.data.clickTwice.keyNode.androidKey, from);
            return;
        case KeyMap::KMT_CLICK_MULTI:
            processKeyClickMulti(node.data.clickMulti.keyNode.delayClickNodes, node.data.clickMulti.keyNode.delayClickNodesCount, from);
            return;
        case KeyMap::KMT_DRAG:
            processKeyDrag(node.data.drag.keyNode.pos, node.data.drag.keyNode.extendPos,
                         node.data.drag.startDelay, node.data.drag.dragSpeed, from);
            return;
        case KeyMap::KMT_ANDROID_KEY:
            processAndroidKey(node.data.androidKey.keyNode.androidKey, from);
        default:
            break;
        }
    } else {
        InputConvertNormal::keyEvent(from, frameSize, showSize);
    }
}

bool InputConvertGame::isCurrentCustomKeymap()
{
    return m_gameMap;
}

void InputConvertGame::loadKeyMap(const QString &json)
{
    m_keyMap.loadKeyMap(json);
}

void InputConvertGame::updateSize(const QSize &frameSize, const QSize &showSize)
{
    if (showSize != m_showSize) {
        if (m_cursorHidden) {
#ifdef QT_NO_DEBUG
            // show size change, resize grab cursor
            emit grabCursor(true);
#endif
        }
    }
    m_frameSize = frameSize;
    m_showSize = showSize;
}

void InputConvertGame::sendTouchDownEvent(int id, QPointF pos)
{
    sendTouchEvent(id, pos, AMOTION_EVENT_ACTION_DOWN);
}

void InputConvertGame::sendTouchMoveEvent(int id, QPointF pos)
{
    sendTouchEvent(id, pos, AMOTION_EVENT_ACTION_MOVE);
}

void InputConvertGame::sendTouchUpEvent(int id, QPointF pos)
{
    sendTouchEvent(id, pos, AMOTION_EVENT_ACTION_UP);
}

void InputConvertGame::sendTouchEvent(int id, QPointF pos, AndroidMotioneventAction action)
{
    if (0 > id || MULTI_TOUCH_MAX_NUM - 1 < id) {
        Q_ASSERT(0);
        return;
    }
    //qDebug() << "id:" << id << " pos:" << pos << " action" << action;
    ControlMsg *controlMsg = new ControlMsg(ControlMsg::CMT_INJECT_TOUCH);
    if (!controlMsg) {
        return;
    }

    QPoint absolutePos = calcFrameAbsolutePos(pos).toPoint();
    static QPoint lastAbsolutePos = absolutePos;
    if (AMOTION_EVENT_ACTION_MOVE == action && lastAbsolutePos == absolutePos) {
        delete controlMsg;
        return;
    }
    lastAbsolutePos = absolutePos;

    controlMsg->setInjectTouchMsgData(
        static_cast<quint64>(id),
        action,
        static_cast<AndroidMotioneventButtons>(0),
        static_cast<AndroidMotioneventButtons>(0),
        QRect(absolutePos, m_frameSize),
        AMOTION_EVENT_ACTION_DOWN == action ? 1.0f : 0.0f);
    sendControlMsg(controlMsg);
}

void InputConvertGame::sendKeyEvent(AndroidKeyeventAction action, AndroidKeycode keyCode) {
    ControlMsg *controlMsg = new ControlMsg(ControlMsg::CMT_INJECT_KEYCODE);
    if (!controlMsg) {
        return;
    }

    controlMsg->setInjectKeycodeMsgData(action, keyCode, 0, AMETA_NONE);
    sendControlMsg(controlMsg);
}

QPointF InputConvertGame::calcFrameAbsolutePos(QPointF relativePos)
{
    QPointF absolutePos;
    absolutePos.setX(m_frameSize.width() * relativePos.x());
    absolutePos.setY(m_frameSize.height() * relativePos.y());
    return absolutePos;
}

QPointF InputConvertGame::calcScreenAbsolutePos(QPointF relativePos)
{
    QPointF absolutePos;
    absolutePos.setX(m_showSize.width() * relativePos.x());
    absolutePos.setY(m_showSize.height() * relativePos.y());
    return absolutePos;
}

int InputConvertGame::attachTouchID(int key)
{
    for (int i = 0; i < MULTI_TOUCH_MAX_NUM; i++) {
        if (0 == m_multiTouchID[i]) {
            m_multiTouchID[i] = key;
            return i;
        }
    }
    return -1;
}

void InputConvertGame::detachTouchID(int key)
{
    for (int i = 0; i < MULTI_TOUCH_MAX_NUM; i++) {
        if (key == m_multiTouchID[i]) {
            m_multiTouchID[i] = 0;
            return;
        }
    }
}

int InputConvertGame::getTouchID(int key)
{
    for (int i = 0; i < MULTI_TOUCH_MAX_NUM; i++) {
        if (key == m_multiTouchID[i]) {
            return i;
        }
    }
    return -1;
}

// -------- steer wheel event --------

void InputConvertGame::getDelayQueue(const QPointF& start, const QPointF& end,
                                     const double& distanceStep, const double& posStepconst,
                                     quint32 lowestTimer, quint32 highestTimer,
                                     QQueue<QPointF>& queuePos, QQueue<quint32>& queueTimer) {
    double x1 = start.x();
    double y1 = start.y();
    double x2 = end.x();
    double y2 = end.y();

    double dx=x2-x1;
    double dy=y2-y1;
    double e=(fabs(dx)>fabs(dy))?fabs(dx):fabs(dy);
    e /= distanceStep;
    if (e == 0.0) {
        queuePos.clear();
        queueTimer.clear();
        return;
    }
    dx/=e;
    dy/=e;

    QQueue<QPointF> queue;
    QQueue<quint32> queue2;
    for(int i=1;i<=e;i++) {
        QPointF pos(x1+(QRandomGenerator::global()->bounded(posStepconst*2)-posStepconst), y1+(QRandomGenerator::global()->bounded(posStepconst*2)-posStepconst));
        queue.enqueue(pos);
        queue2.enqueue(QRandomGenerator::global()->bounded(lowestTimer, highestTimer));
        x1+=dx;
        y1+=dy;
    }

    queuePos = queue;
    queueTimer = queue2;
}

void InputConvertGame::onSteerWheelTimer()
{
    auto &wheel = m_ctrlSteerWheel;
    auto &path = wheel.delayData;
    if (path.queuePos.isEmpty()) {
        return;
    }
    path.currentPos = path.queuePos.dequeue();
    sendTouchMoveEvent(getTouchID(wheel.touchKey), path.currentPos);
    if (!path.queuePos.isEmpty()) {
        path.timer->start(path.queueTimer.dequeue());
    }
}

void InputConvertGame::moveSteerWheel()
{
    auto &wheel = m_ctrlSteerWheel;
    auto &path = wheel.delayData;
    if (!wheel.node) {
        return;
    }
    const double multiplier = wheel.sprinting ? wheel.node->sprintMultiplier : 1.0;
    const QPointF target = wheel.node->data.steerWheel.centerPos + wheel.offset * multiplier;
    const QPointF bounded(qBound(0.001, target.x(), 0.999), qBound(0.001, target.y(), 0.999));
    path.timer->stop();
    getDelayQueue(path.currentPos, bounded, 0.01, 0.002, 2, 8, path.queuePos, path.queueTimer);
    path.queuePos.enqueue(bounded);
    path.queueTimer.enqueue(4);
    path.timer->start(path.queueTimer.dequeue());
}

void InputConvertGame::stopSteerWheel()
{
    auto &wheel = m_ctrlSteerWheel;
    m_steerSprintTimer.stop();
    wheel.delayData.timer->stop();
    wheel.delayData.queuePos.clear();
    wheel.delayData.queueTimer.clear();
    if (wheel.node) {
        sendTouchUpEvent(getTouchID(wheel.touchKey), wheel.delayData.currentPos);
        detachTouchID(wheel.touchKey);
    }
    wheel.node = nullptr;
    wheel.touchKey = Qt::Key_unknown;
    wheel.offset = QPointF();
    wheel.sprinting = false;
    wheel.pressedUp = wheel.pressedDown = wheel.pressedLeft = wheel.pressedRight = false;
}

void InputConvertGame::processSteerWheel(const KeyMap::KeyMapNode &node, const QKeyEvent *from)
{
    auto &wheel = m_ctrlSteerWheel;
    const auto &mapping = node.data.steerWheel;
    const int key = from->key();
    const bool pressed = from->type() == QEvent::KeyPress;
    if (key == mapping.up.key) {
        wheel.pressedUp = pressed;
    } else if (key == mapping.right.key) {
        wheel.pressedRight = pressed;
    } else if (key == mapping.down.key) {
        wheel.pressedDown = pressed;
    } else {
        wheel.pressedLeft = pressed;
    }

    if (!wheel.pressedUp && !wheel.pressedRight && !wheel.pressedDown && !wheel.pressedLeft) {
        stopSteerWheel();
        return;
    }

    if (!wheel.node) {
        const int id = attachTouchID(key);
        if (id < 0) {
            return;
        }
        wheel.node = &node;
        wheel.touchKey = key;
        wheel.delayData.currentPos = mapping.centerPos;
        sendTouchDownEvent(id, mapping.centerPos);
    }

    wheel.offset = QPointF((wheel.pressedRight ? mapping.right.extendOffset : 0.0) -
                           (wheel.pressedLeft ? mapping.left.extendOffset : 0.0),
                           (wheel.pressedDown ? mapping.down.extendOffset : 0.0) -
                           (wheel.pressedUp ? mapping.up.extendOffset : 0.0));
    if (wheel.offset.isNull()) {
        m_steerSprintTimer.stop();
        wheel.sprinting = false;
    } else if (node.sprintDelayMs > 0 && node.sprintMultiplier > 1.0 &&
               !wheel.sprinting && !m_steerSprintTimer.isActive()) {
        m_steerSprintTimer.start(node.sprintDelayMs);
    }
    moveSteerWheel();
}

// -------- key event --------

void InputConvertGame::processKeyClick(const QPointF &clickPos, bool clickTwice, bool switchMap, const QKeyEvent *from)
{
    if (switchMap && QEvent::KeyRelease == from->type()) {
        m_needBackMouseMove = !m_needBackMouseMove;
        updateMouseCursor();
    }

    if (QEvent::KeyPress == from->type()) {
        int id = attachTouchID(from->key());
        sendTouchDownEvent(id, clickPos);
        if (clickTwice) {
            sendTouchUpEvent(getTouchID(from->key()), clickPos);
            detachTouchID(from->key());
        }
    } else if (QEvent::KeyRelease == from->type()) {
        if (clickTwice) {
            int id = attachTouchID(from->key());
            sendTouchDownEvent(id, clickPos);
        }
        sendTouchUpEvent(getTouchID(from->key()), clickPos);
        detachTouchID(from->key());
    }
}

void InputConvertGame::processKeyClickMulti(const KeyMap::DelayClickNode *nodes, const int count, const QKeyEvent *from)
{
    if (QEvent::KeyPress != from->type()) {
        return;
    }

    int key = from->key();
    int delay = 0;
    QPointF clickPos;

    for (int i = 0; i < count; i++) {
        delay += nodes[i].delay;
        clickPos = nodes[i].pos;
        QTimer::singleShot(delay, this, [this, key, clickPos]() {
            int id = attachTouchID(key);
            sendTouchDownEvent(id, clickPos);
        });

        // Don't up it too fast
        delay += 20;
        QTimer::singleShot(delay, this, [this, key, clickPos]() {
            int id = getTouchID(key);
            sendTouchUpEvent(id, clickPos);
            detachTouchID(key);
        });
    }
}

void InputConvertGame::onDragTimer() {
    if(m_dragDelayData.queuePos.empty()) {
        return;
    }
    int id = getTouchID(m_dragDelayData.pressKey);
    m_dragDelayData.currentPos = m_dragDelayData.queuePos.dequeue();
    sendTouchMoveEvent(id, m_dragDelayData.currentPos);

    if(m_dragDelayData.queuePos.empty()) {
        delete m_dragDelayData.timer;
        m_dragDelayData.timer = nullptr;

        sendTouchUpEvent(id, m_dragDelayData.currentPos);
        detachTouchID(m_dragDelayData.pressKey);

        m_dragDelayData.currentPos = QPointF();
        m_dragDelayData.pressKey = 0;
        return;
    }

    if(!m_dragDelayData.queuePos.empty()) {
        m_dragDelayData.timer->start(m_dragDelayData.queueTimer.dequeue());
    }
}

void InputConvertGame::processKeyDrag(const QPointF &startPos, QPointF endPos, quint32 startDelay, float dragSpeed, const QKeyEvent *from)
{
    if (QEvent::KeyPress == from->type()) {
        // stop last
        if (m_dragDelayData.timer && m_dragDelayData.timer->isActive()) {
            m_dragDelayData.timer->stop();
            delete m_dragDelayData.timer;
            m_dragDelayData.timer = nullptr;
            m_dragDelayData.queuePos.clear();
            m_dragDelayData.queueTimer.clear();

            sendTouchUpEvent(getTouchID(m_dragDelayData.pressKey), m_dragDelayData.currentPos);
            detachTouchID(m_dragDelayData.pressKey);

            m_dragDelayData.currentPos = QPointF();
            m_dragDelayData.pressKey = 0;
        }

        // start this
        int id = attachTouchID(from->key());
        sendTouchDownEvent(id, startPos);

        m_dragDelayData.timer = new QTimer(this);
        m_dragDelayData.timer->setSingleShot(true);
        connect(m_dragDelayData.timer, &QTimer::timeout, this, &InputConvertGame::onDragTimer);
        m_dragDelayData.pressKey = from->key();
        m_dragDelayData.currentPos = startPos;
        m_dragDelayData.queuePos.clear();
        m_dragDelayData.queueTimer.clear();

        // Clamp dragSpeed to 0-1 range
        const float speed = qBound(0.0f, static_cast<float>(dragSpeed), 1.0f);
        
        // Calculate delays based on dragSpeed
        // dragSpeed = 1 -> minDelay = 1, maxDelay = 2 (fastest)
        // dragSpeed = 0 -> minDelay = 30, maxDelay = 40 (slowest)
        const quint32 minDelay = static_cast<quint32>(1 + (1.0f - speed) * 29);  // 1 to 30
        const quint32 maxDelay = minDelay + static_cast<quint32>((1.0f - speed) * 9) + 1;  // // min + (0 to 9) + 1

        getDelayQueue(startPos, endPos,
                      0.01f, 0.0005f,
                      minDelay,
                      maxDelay,
                      m_dragDelayData.queuePos,
                      m_dragDelayData.queueTimer);

        m_dragDelayData.timer->start(startDelay);
    }
}

void InputConvertGame::requestWeaponSwitch(int key)
{
    if (m_weaponCapture || isTemporaryCursorActive() || m_needBackMouseMove ||
        !m_controller || m_controller->deviceSerial().isEmpty() || AdbProcessImpl::getAdbPath().isEmpty()) {
        return;
    }

    auto capture = new QProcess(this);
    m_weaponCapture = capture;
    connect(capture, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this, capture, key](int code, QProcess::ExitStatus status) {
        m_weaponCapture = nullptr;
        if (code == 0 && status == QProcess::NormalExit && m_gameMap && !isTemporaryCursorActive() &&
            !m_needBackMouseMove && qGuiApp->applicationState() == Qt::ApplicationActive) {
            switchSelectedWeapon(capture->readAllStandardOutput(), key);
        }
        capture->deleteLater();
    });
    connect(capture, &QProcess::errorOccurred, this, [this, capture](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            m_weaponCapture = nullptr;
            capture->deleteLater();
        }
    });
    QTimer::singleShot(3000, capture, [capture]() {
        if (capture->state() != QProcess::NotRunning) {
            capture->kill();
        }
    });
    capture->start(AdbProcessImpl::getAdbPath(),
                   { QStringLiteral("-s"), m_controller->deviceSerial(), QStringLiteral("exec-out"),
                     QStringLiteral("screencap") });
}

static double weaponBorderScore(const uchar *pixels, const QSize &size, const QPointF &center)
{
    const int x = qRound(center.x() * size.width());
    const int y = qRound(center.y() * size.height());
    const int halfWidth = qMax(1, qRound(size.width() * 72.0 / 2656.0));
    const int firstRow = y - qRound(size.height() * 42.0 / 1220.0);
    const int lastRow = y - qRound(size.height() * 30.0 / 1220.0);
    if (x < halfWidth || x + halfWidth > size.width() || firstRow < 0 || lastRow >= size.height()) {
        return 0.0;
    }

    int best = 0;
    for (int row = firstRow; row <= lastRow; ++row) {
        const uchar *pixel = pixels + (qint64(row) * size.width() + x - halfWidth) * 4;
        int gold = 0;
        for (int column = 0; column < halfWidth * 2; ++column, pixel += 4) {
            const int r = pixel[0], g = pixel[1], b = pixel[2];
            if (r >= 198 && g * 100 >= 64 * r && g * 100 <= 89 * r && b * 100 <= 63 * r) {
                ++gold;
            }
        }
        best = qMax(best, gold);
    }
    return double(best) / (2 * halfWidth);
}

void InputConvertGame::switchSelectedWeapon(const QByteArray &frame, int key)
{
    // Android's raw screencap begins with width, height, pixel format and (on newer devices) color space.
    if (frame.size() < 12) {
        return;
    }
    const auto *data = reinterpret_cast<const uchar *>(frame.constData());
    const auto width = qFromLittleEndian<quint32>(data);
    const auto height = qFromLittleEndian<quint32>(data + 4);
    if (width <= height || height < 360 || width > 8192 || qFromLittleEndian<quint32>(data + 8) != 1) {
        return;
    }
    const auto headerSize = frame.size() - qint64(width) * height * 4;
    if (headerSize != 12 && headerSize != 16) {
        return;
    }

    const auto &upper = m_keyMap.getKeyMapNodeKey(Qt::Key_1);
    const auto &lower = m_keyMap.getKeyMapNodeKey(Qt::Key_2);
    if (upper.type != KeyMap::KMT_CLICK || lower.type != KeyMap::KMT_CLICK) {
        return;
    }
    const QSize size(static_cast<int>(width), static_cast<int>(height));
    const QPointF a = upper.data.click.keyNode.pos, b = lower.data.click.keyNode.pos;
    const double upperScore = weaponBorderScore(data + headerSize, size, a);
    const double lowerScore = weaponBorderScore(data + headerSize, size, b);
    if (qMax(upperScore, lowerScore) < 0.72 || qAbs(upperScore - lowerScore) < 0.34) {
        return;
    }

    const QPointF start = upperScore > lowerScore ? a : b;
    const QPointF end(qMin(0.99, start.x() + 188.0 / 2656.0), start.y());
    if (end.x() - start.x() < 0.025) {
        return;
    }
    const QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
    processKeyDrag(start, end, 0, 0.45f, &press);
}

void InputConvertGame::processAndroidKey(AndroidKeycode androidKey, const QKeyEvent *from)
{
    if (AKEYCODE_UNKNOWN == androidKey) {
        return;
    }

    AndroidKeyeventAction action;
    switch (from->type()) {
    case QEvent::KeyPress:
        action = AKEY_EVENT_ACTION_DOWN;
        break;
    case QEvent::KeyRelease:
        action = AKEY_EVENT_ACTION_UP;
        break;
    default:
        return;
    }

    sendKeyEvent(action, androidKey);
}

// -------- mouse event --------

bool InputConvertGame::processMouseClick(const QMouseEvent *from)
{
    const KeyMap::KeyMapNode &node = m_keyMap.getKeyMapNodeMouse(from->button());
    if (KeyMap::KMT_INVALID == node.type) {
        return false;
    }

    if (QEvent::MouseButtonPress == from->type() || QEvent::MouseButtonDblClick == from->type()) {
        int id = attachTouchID(from->button());
        sendTouchDownEvent(id, node.data.click.keyNode.pos);
        return true;
    }
    if (QEvent::MouseButtonRelease == from->type()) {
        int id = getTouchID(from->button());
        sendTouchUpEvent(id, node.data.click.keyNode.pos);
        detachTouchID(from->button());
        return true;
    }
    return false;
}

bool InputConvertGame::processMouseMove(const QMouseEvent *from)
{
    if (QEvent::MouseMove != from->type()) {
        return false;
    }

    if (checkCursorPos(from)) {
        m_ctrlMouseMove.lastPos = QPointF(0.0, 0.0);
        return true;
    }

    auto lastPos = m_ctrlMouseMove.lastPos;
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    m_ctrlMouseMove.lastPos = from->localPos();
#else
    m_ctrlMouseMove.lastPos = from->position();
#endif

    if (m_ctrlMouseMove.ignoreCount > 0) {
        --m_ctrlMouseMove.ignoreCount;
        return true;
    }

    if (!lastPos.isNull() && m_processMouseMove) {
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
        QPointF distance_raw{from->localPos() - lastPos};
#else
        QPointF distance_raw{from->position() - lastPos};
#endif
        QPointF speedRatio  {m_keyMap.getMouseMoveMap().data.mouseMove.speedRatio};
        QPointF distance    {distance_raw.x() / speedRatio.x(), distance_raw.y() / speedRatio.y()};

        mouseMoveStartTouch(from);
        startMouseMoveTimer();

        m_ctrlMouseMove.lastConverPos.setX(m_ctrlMouseMove.lastConverPos.x() + distance.x() / m_showSize.width());
        m_ctrlMouseMove.lastConverPos.setY(m_ctrlMouseMove.lastConverPos.y() + distance.y() / m_showSize.height());

        if (m_ctrlMouseMove.lastConverPos.x() < 0.05 || m_ctrlMouseMove.lastConverPos.x() > 0.95 || m_ctrlMouseMove.lastConverPos.y() < 0.05
            || m_ctrlMouseMove.lastConverPos.y() > 0.95) {
            if (m_ctrlMouseMove.smallEyes) {
                m_processMouseMove = false;
                int delay = 30;
                QTimer::singleShot(delay, this, [this]() { mouseMoveStopTouch(); });
                QTimer::singleShot(delay * 2, this, [this]() {
                    mouseMoveStartTouch(nullptr);
                    m_processMouseMove = true;
                });
            } else {
                mouseMoveStopTouch();
                m_ctrlMouseMove.ignoreCount = 5;
                return true;
            }
        }

        sendTouchMoveEvent(getTouchID(Qt::ExtraButton24), m_ctrlMouseMove.lastConverPos);
    }

    return true;
}

bool InputConvertGame::checkCursorPos(const QMouseEvent *from)
{
    bool moveCursor = false;
    QPoint pos = from->pos();
    if (pos.x() < CURSOR_POS_CHECK) {
        pos.setX(m_showSize.width() - CURSOR_POS_CHECK);
        moveCursor = true;
    } else if (pos.x() > m_showSize.width() - CURSOR_POS_CHECK) {
        pos.setX(CURSOR_POS_CHECK);
        moveCursor = true;
    } else if (pos.y() < CURSOR_POS_CHECK) {
        pos.setY(m_showSize.height() - CURSOR_POS_CHECK);
        moveCursor = true;
    } else if (pos.y() > m_showSize.height() - CURSOR_POS_CHECK) {
        pos.setY(CURSOR_POS_CHECK);
        moveCursor = true;
    }

    if (moveCursor) {
        moveCursorTo(from, pos);
    }

    return moveCursor;
}

void InputConvertGame::moveCursorTo(const QMouseEvent *from, const QPoint &localPosPixel)
{
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    QPoint posOffset = from->pos() - localPosPixel;
    QPoint globalPos = from->globalPos();
#else
    QPoint posOffset = from->position().toPoint() - localPosPixel;
    QPoint globalPos = from->globalPosition().toPoint();
#endif
    globalPos -= posOffset;
    //qDebug()<<"move cursor to "<<globalPos<<" offset "<<posOffset;
#ifdef Q_OS_MACOS
    // On macOS, QCursor::setPos() posts a synthetic mouse-moved event (CGEventPost)
    // and does NOT re-associate the hardware mouse with the on-screen cursor, so the
    // warp does not "stick": the next hardware delta snaps the cursor back. That makes
    // mouseMoveMap re-centering a no-op, and the camera can no longer pan past the edge.
    // CGWarpMouseCursorPosition performs a real warp, and
    // CGAssociateMouseAndMouseCursorPosition(true) immediately restores the
    // mouse<->cursor association so subsequent hardware deltas keep flowing. This
    // mirrors the approach already used in util/mousetap/cocoamousetap.mm.
    CGWarpMouseCursorPosition(CGPointMake(globalPos.x(), globalPos.y()));
    CGAssociateMouseAndMouseCursorPosition(true);
#else
    QCursor::setPos(globalPos);
#endif
}

void InputConvertGame::mouseMoveStartTouch(const QMouseEvent *from)
{
    Q_UNUSED(from)
    if (!m_ctrlMouseMove.touching) {
        QPointF mouseMoveStartPos
            = m_ctrlMouseMove.smallEyes ? m_keyMap.getMouseMoveMap().data.mouseMove.smallEyes.pos : m_keyMap.getMouseMoveMap().data.mouseMove.startPos;
        int id = attachTouchID(Qt::ExtraButton24);
        sendTouchDownEvent(id, mouseMoveStartPos);
        m_ctrlMouseMove.lastConverPos = mouseMoveStartPos;
        m_ctrlMouseMove.touching = true;
    }
}

void InputConvertGame::mouseMoveStopTouch()
{
    if (m_ctrlMouseMove.touching) {
        sendTouchUpEvent(getTouchID(Qt::ExtraButton24), m_ctrlMouseMove.lastConverPos);
        detachTouchID(Qt::ExtraButton24);
        m_ctrlMouseMove.touching = false;
    }
}

void InputConvertGame::startMouseMoveTimer()
{
    stopMouseMoveTimer();
    m_ctrlMouseMove.timer = startTimer(500);
}

void InputConvertGame::stopMouseMoveTimer()
{
    if (0 != m_ctrlMouseMove.timer) {
        killTimer(m_ctrlMouseMove.timer);
        m_ctrlMouseMove.timer = 0;
    }
}

bool InputConvertGame::switchGameMap()
{
    m_gameMap = !m_gameMap;
    qInfo() << QString("current keymap mode: %1").arg(m_gameMap ? "custom" : "normal");

    if (!m_gameMap) {
        stopSteerWheel();
        if (m_weaponCapture) {
            m_weaponCapture->kill();
        }
        m_needBackMouseMove = false;
        m_cursorUntilClick = false;
        m_cursorKeyHeld = false;
        m_pointerClickActive = false;
    }
    updateMouseCursor();
    return m_gameMap;
}

bool InputConvertGame::isTemporaryCursorActive() const
{
    return m_cursorUntilClick || m_cursorKeyHeld || m_pointerClickActive;
}

void InputConvertGame::updateMouseCursor()
{
    const bool hide = m_gameMap && m_keyMap.isValidMouseMoveMap() && !m_needBackMouseMove &&
                      !isTemporaryCursorActive() && qGuiApp->applicationState() == Qt::ApplicationActive;
    if (hide == m_cursorHidden) {
        return;
    }

    m_ctrlMouseMove.lastPos = QPointF();
    if (!hide) {
        stopMouseMoveTimer();
        mouseMoveStopTouch();
    }
#ifdef QT_NO_DEBUG
    emit grabCursor(hide);
#endif
    hideMouseCursor(hide);
}

void InputConvertGame::hideMouseCursor(bool hide)
{
    m_cursorHidden = hide;
    if (hide) {
#ifdef QT_NO_DEBUG
        QGuiApplication::setOverrideCursor(QCursor(Qt::BlankCursor));
#else
        QGuiApplication::setOverrideCursor(QCursor(Qt::CrossCursor));
#endif
    } else {
        QGuiApplication::restoreOverrideCursor();
    }
}

void InputConvertGame::timerEvent(QTimerEvent *event)
{
    if (m_ctrlMouseMove.timer == event->timerId()) {
        stopMouseMoveTimer();
        // Don't auto-reset view when smallEyes mode is active
        if (!m_ctrlMouseMove.smallEyes) {
            mouseMoveStopTouch();
        }
    }
}
