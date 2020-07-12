#include "input.h"

#include <Limelight.h>
#include <SDL.h>
#include "streaming/streamutils.h"

void SdlInputHandler::handleMouseButtonEvent(SDL_MouseButtonEvent* event)
{
    int button;

    if (event->which == SDL_TOUCH_MOUSEID) {
        // Ignore synthetic mouse events
        return;
    }
    else if (!isCaptureActive()) {
        if (event->button == SDL_BUTTON_LEFT && event->state == SDL_RELEASED) {
            // Capture the mouse again if clicked when unbound.
            // We start capture on left button released instead of
            // pressed to avoid sending an errant mouse button released
            // event to the host when clicking into our window (since
            // the pressed event was consumed by this code).
            setCaptureActive(true);
        }

        // Not capturing
        return;
    }

    switch (event->button)
    {
        case SDL_BUTTON_LEFT:
            button = BUTTON_LEFT;
            break;
        case SDL_BUTTON_MIDDLE:
            button = BUTTON_MIDDLE;
            break;
        case SDL_BUTTON_RIGHT:
            button = BUTTON_RIGHT;
            break;
        case SDL_BUTTON_X1:
            button = BUTTON_X1;
            break;
        case SDL_BUTTON_X2:
            button = BUTTON_X2;
            break;
        default:
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Unhandled button event: %d",
                        event->button);
            return;
    }

    LiSendMouseButtonEvent(event->state == SDL_PRESSED ?
                               BUTTON_ACTION_PRESS :
                               BUTTON_ACTION_RELEASE,
                           button);
}

void SdlInputHandler::updateMousePositionReport(int mouseX, int mouseY)
{
    int windowWidth, windowHeight;

    // Call SDL_GetWindowSize() before entering the spinlock
    SDL_GetWindowSize(m_Window, &windowWidth, &windowHeight);

    SDL_AtomicLock(&m_MousePositionLock);
    m_MousePositionReport.x = mouseX;
    m_MousePositionReport.y = mouseY;
    m_MousePositionReport.windowWidth = windowWidth;
    m_MousePositionReport.windowHeight = windowHeight;
    SDL_AtomicUnlock(&m_MousePositionLock);
    SDL_AtomicSet(&m_MousePositionUpdated, 1);
}

void SdlInputHandler::handleMouseMotionEvent(SDL_MouseMotionEvent* event)
{
    if (!isCaptureActive()) {
        // Not capturing
        return;
    }
    else if (event->which == SDL_TOUCH_MOUSEID) {
        // Ignore synthetic mouse events
        return;
    }

    // Batch until the next mouse polling window or we'll get awful
    // input lag everything except GFE 3.14 and 3.15.
    if (m_AbsoluteMouseMode) {
        updateMousePositionReport(event->x, event->y);
    }
    else {
        SDL_AtomicAdd(&m_MouseDeltaX, event->xrel);
        SDL_AtomicAdd(&m_MouseDeltaY, event->yrel);
    }
}

void SdlInputHandler::handleMouseWheelEvent(SDL_MouseWheelEvent* event)
{
    if (!isCaptureActive()) {
        // Not capturing
        return;
    }
    else if (event->which == SDL_TOUCH_MOUSEID) {
        // Ignore synthetic mouse events
        return;
    }

    if (event->y != 0) {
        LiSendScrollEvent((signed char)event->y);
    }
}

bool SdlInputHandler::isMouseInVideoRegion(int mouseX, int mouseY, int windowWidth, int windowHeight)
{
    SDL_Rect src, dst;

    if (windowWidth < 0 || windowHeight < 0) {
        SDL_GetWindowSize(m_Window, &windowWidth, &windowHeight);
    }

    src.x = src.y = 0;
    src.w = m_StreamWidth;
    src.h = m_StreamHeight;

    dst.x = dst.y = 0;
    dst.w = windowWidth;
    dst.h = windowHeight;

    // Use the stream and window sizes to determine the video region
    StreamUtils::scaleSourceToDestinationSurface(&src, &dst);

    return (mouseX >= dst.x && mouseX <= dst.x + dst.w) &&
           (mouseY >= dst.y && mouseY <= dst.y + dst.h);
}

Uint32 SdlInputHandler::mouseMoveTimerCallback(Uint32 interval, void *param)
{
    auto me = reinterpret_cast<SdlInputHandler*>(param);

    short deltaX = (short)SDL_AtomicSet(&me->m_MouseDeltaX, 0);
    short deltaY = (short)SDL_AtomicSet(&me->m_MouseDeltaY, 0);

    if (deltaX != 0 || deltaY != 0) {
        LiSendMouseMoveEvent(deltaX, deltaY);
    }

    bool hasNewPosition = SDL_AtomicSet(&me->m_MousePositionUpdated, 0) != 0;
    if (hasNewPosition) {
        // If the lock is held now, the main thread is trying to update
        // the mouse position. We'll pick up the new position next time.
        if (SDL_AtomicTryLock(&me->m_MousePositionLock)) {
            SDL_Rect src, dst;

            src.x = src.y = 0;
            src.w = me->m_StreamWidth;
            src.h = me->m_StreamHeight;

            dst.x = dst.y = 0;
            dst.w = me->m_MousePositionReport.windowWidth;
            dst.h = me->m_MousePositionReport.windowHeight;

            // Use the stream and window sizes to determine the video region
            StreamUtils::scaleSourceToDestinationSurface(&src, &dst);

            // Clamp motion to the video region
            short x = qMin(qMax(me->m_MousePositionReport.x - dst.x, 0), dst.w);
            short y = qMin(qMax(me->m_MousePositionReport.y - dst.y, 0), dst.h);

            // Release the spinlock to unblock the main thread
            SDL_AtomicUnlock(&me->m_MousePositionLock);

            // Send the mouse position update
            LiSendMousePositionEvent(x, y, dst.w, dst.h);
        }
    }

#ifdef Q_OS_WIN32
    // See comment in SdlInputHandler::notifyMouseLeave()
    if (me->m_AbsoluteMouseMode && me->m_PendingMouseLeaveButtonUp != 0 && me->isCaptureActive()) {
        int mouseX, mouseY;
        int windowX, windowY;
        Uint32 mouseState = SDL_GetGlobalMouseState(&mouseX, &mouseY);
        SDL_GetWindowPosition(me->m_Window, &windowX, &windowY);

        // If the button is now up, send the synthetic mouse up event
        if ((mouseState & SDL_BUTTON(me->m_PendingMouseLeaveButtonUp)) == 0) {
            SDL_Event event;

            event.button.type = SDL_MOUSEBUTTONUP;
            event.button.timestamp = SDL_GetTicks();
            event.button.windowID = SDL_GetWindowID(me->m_Window);
            event.button.which = 0;
            event.button.button = me->m_PendingMouseLeaveButtonUp;
            event.button.state = SDL_RELEASED;
            event.button.clicks = 1;
            event.button.x = mouseX - windowX;
            event.button.y = mouseY - windowY;
            SDL_PushEvent(&event);

            me->m_PendingMouseLeaveButtonUp = 0;
        }
    }
#endif

    return interval;
}
