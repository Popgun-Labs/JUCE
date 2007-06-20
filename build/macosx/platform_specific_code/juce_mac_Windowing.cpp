/*
  ==============================================================================

   This file is part of the JUCE library - "Jules' Utility Class Extensions"
   Copyright 2004-7 by Raw Material Software ltd.

  ------------------------------------------------------------------------------

   JUCE can be redistributed and/or modified under the terms of the
   GNU General Public License, as published by the Free Software Foundation;
   either version 2 of the License, or (at your option) any later version.

   JUCE is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with JUCE; if not, visit www.gnu.org/licenses or write to the
   Free Software Foundation, Inc., 59 Temple Place, Suite 330, 
   Boston, MA 02111-1307 USA

  ------------------------------------------------------------------------------

   If you'd like to release a closed-source product which uses JUCE, commercial
   licenses are also available: visit www.rawmaterialsoftware.com/juce for
   more information.

  ==============================================================================
*/

#include "../../../src/juce_core/basics/juce_StandardHeader.h"
#include <Carbon/Carbon.h>
#include <fnmatch.h>

#if JUCE_OPENGL
#include <agl/agl.h>
#endif

BEGIN_JUCE_NAMESPACE

#include "../../../src/juce_appframework/events/juce_Timer.h"
#include "../../../src/juce_appframework/application/juce_DeletedAtShutdown.h"
#include "../../../src/juce_appframework/events/juce_AsyncUpdater.h"
#include "../../../src/juce_appframework/events/juce_MessageManager.h"
#include "../../../src/juce_core/basics/juce_Singleton.h"
#include "../../../src/juce_core/basics/juce_Random.h"
#include "../../../src/juce_core/threads/juce_Process.h"
#include "../../../src/juce_appframework/application/juce_SystemClipboard.h"
#include "../../../src/juce_appframework/gui/components/keyboard/juce_KeyPress.h"
#include "../../../src/juce_appframework/gui/components/windows/juce_AlertWindow.h"
#include "../../../src/juce_appframework/gui/graphics/geometry/juce_RectangleList.h"
#include "../../../src/juce_appframework/gui/graphics/contexts/juce_LowLevelGraphicsSoftwareRenderer.h"
#include "../../../src/juce_appframework/gui/components/juce_Desktop.h"
#include "../../../src/juce_appframework/gui/components/menus/juce_MenuBarModel.h"
#include "../../../src/juce_core/misc/juce_PlatformUtilities.h"
#include "../../../src/juce_appframework/application/juce_Application.h"
#include "../../../src/juce_appframework/gui/components/special/juce_OpenGLComponent.h"
#include "../../../src/juce_appframework/gui/components/mouse/juce_DragAndDropContainer.h"
#include "../../../src/juce_appframework/gui/components/keyboard/juce_KeyPressMappingSet.h"
#include "../../../src/juce_appframework/gui/graphics/imaging/juce_ImageFileFormat.h"

#undef Point

const WindowRegionCode windowRegionToUse = kWindowContentRgn;

static HIObjectClassRef viewClassRef = 0;
static CFStringRef juceHiViewClassNameCFString = 0;
static ComponentPeer* juce_currentMouseTrackingPeer = 0;


//==============================================================================
static VoidArray keysCurrentlyDown;

bool KeyPress::isKeyCurrentlyDown (int keyCode)
{
    if (keysCurrentlyDown.contains ((void*) keyCode))
        return true;

    if (keyCode >= 'A' && keyCode <= 'Z'
        && keysCurrentlyDown.contains ((void*) (int) CharacterFunctions::toLowerCase ((tchar) keyCode)))
        return true;

    if (keyCode >= 'a' && keyCode <= 'z'
        && keysCurrentlyDown.contains ((void*) (int) CharacterFunctions::toUpperCase ((tchar) keyCode)))
        return true;

    return false;
}

//==============================================================================
static VoidArray minimisedWindows;

static void setWindowMinimised (WindowRef ref, const bool isMinimised)
{
    if (isMinimised != minimisedWindows.contains (ref))
        CollapseWindow (ref, isMinimised);
}

void juce_maximiseAllMinimisedWindows()
{
    const VoidArray minWin (minimisedWindows);

    for (int i = minWin.size(); --i >= 0;)
        setWindowMinimised ((WindowRef) (minWin[i]), false);
}

//==============================================================================
class HIViewComponentPeer;
static HIViewComponentPeer* currentlyFocusedPeer = 0;


//==============================================================================
static int currentModifiers = 0;

static void updateModifiers (EventRef theEvent)
{
    currentModifiers &= ~ (ModifierKeys::shiftModifier | ModifierKeys::ctrlModifier
                           | ModifierKeys::altModifier | ModifierKeys::commandModifier);

    UInt32 m;

    if (theEvent != 0)
        GetEventParameter (theEvent, kEventParamKeyModifiers, typeUInt32, 0, sizeof(m), 0, &m);
    else
        m = GetCurrentEventKeyModifiers();

    if ((m & (shiftKey | rightShiftKey)) != 0)
        currentModifiers |= ModifierKeys::shiftModifier;

    if ((m & (controlKey | rightControlKey)) != 0)
        currentModifiers |= ModifierKeys::ctrlModifier;

    if ((m & (optionKey | rightOptionKey)) != 0)
        currentModifiers |= ModifierKeys::altModifier;

    if ((m & cmdKey) != 0)
        currentModifiers |= ModifierKeys::commandModifier;
}

void ModifierKeys::updateCurrentModifiers()
{
    currentModifierFlags = currentModifiers;
}

static int64 getEventTime (EventRef event)
{
    const int64 millis = (int64) (1000.0 * (event != 0 ? GetEventTime (event)
                                                       : GetCurrentEventTime()));

    static int64 offset = 0;
    if (offset == 0)
        offset = Time::currentTimeMillis() - millis;

    return offset + millis;
}


//==============================================================================
class MacBitmapImage  : public Image
{
public:
    //==============================================================================
    CGColorSpaceRef colourspace;
    CGDataProviderRef provider;

    //==============================================================================
    MacBitmapImage (const PixelFormat format_,
                    const int w, const int h, const bool clearImage)
        : Image (format_, w, h)
    {
        jassert (format_ == RGB || format_ == ARGB);

        pixelStride = (format_ == RGB) ? 3 : 4;

        lineStride = (w * pixelStride + 3) & ~3;
        const int imageSize = lineStride * h;

        if (clearImage)
            imageData = (uint8*) juce_calloc (imageSize);
        else
            imageData = (uint8*) juce_malloc (imageSize);

        //colourspace = CGColorSpaceCreateWithName (kCGColorSpaceUserRGB);

        CMProfileRef prof;
        CMGetSystemProfile (&prof);
        colourspace = CGColorSpaceCreateWithPlatformColorSpace (prof);

        provider = CGDataProviderCreateWithData (0, imageData, h * lineStride, 0);
    }

    MacBitmapImage::~MacBitmapImage()
    {
        CGDataProviderRelease (provider);
        CGColorSpaceRelease (colourspace);

        juce_free (imageData);
        imageData = 0; // to stop the base class freeing this
    }

    void blitToContext (CGContextRef context, const float dx, const float dy)
    {
        CGImageRef tempImage = CGImageCreate (getWidth(), getHeight(),
                                              8, pixelStride << 3, lineStride, colourspace,
#if MACOS_10_3_OR_EARLIER || JUCE_BIG_ENDIAN
                                              hasAlphaChannel() ? kCGImageAlphaPremultipliedFirst
                                                                : kCGImageAlphaNone,
#else
                                              hasAlphaChannel() ? kCGBitmapByteOrder32Little | kCGImageAlphaPremultipliedFirst
                                                                : kCGImageAlphaNone,
#endif
                                              provider, 0, false,
                                              kCGRenderingIntentDefault);

        HIRect r;
        r.origin.x = dx;
        r.origin.y = dy;
        r.size.width = (float) getWidth();
        r.size.height = (float) getHeight();

        HIViewDrawCGImage (context, &r, tempImage);

        CGImageRelease (tempImage);
    }

    juce_UseDebuggingNewOperator
};


//==============================================================================
class MouseCheckTimer  : private Timer,
                         private DeletedAtShutdown
{
    HIViewComponentPeer* lastPeerUnderMouse;
    int lastX, lastY;

public:
    MouseCheckTimer()
        : lastX (0),
          lastY (0)
    {
        lastPeerUnderMouse = 0;
        resetMouseMoveChecker();
    }

    ~MouseCheckTimer()
    {
        clearSingletonInstance();
    }

    juce_DeclareSingleton_SingleThreaded (MouseCheckTimer, false)

    bool hasEverHadAMouseMove;

    void moved (HIViewComponentPeer* const peer)
    {
        if (hasEverHadAMouseMove)
            startTimer (200);

        lastPeerUnderMouse = peer;
    }

    void resetMouseMoveChecker()
    {
        hasEverHadAMouseMove = false;
        startTimer (1000 / 16);
    }

    void timerCallback();
};

juce_ImplementSingleton_SingleThreaded (MouseCheckTimer)

//==============================================================================
#if JUCE_QUICKTIME
extern void OfferMouseClickToQuickTime (WindowRef window, ::Point where, long when, long modifiers,
                                        Component* topLevelComp);
#endif


//==============================================================================
class HIViewComponentPeer  : public ComponentPeer,
                             private Timer
{
public:
    //==============================================================================
    HIViewComponentPeer (Component* const component,
                         const int windowStyleFlags,
                         HIViewRef viewToAttachTo)
        : ComponentPeer (component, windowStyleFlags),
          fullScreen (false),
          isCompositingWindow (false),
          windowRef (0),
          viewRef (0)
    {
        repainter = new RepaintManager (this);

        eventHandlerRef = 0;

        if (viewToAttachTo != 0)
        {
            isSharedWindow = true;
        }
        else
        {
            isSharedWindow = false;

            WindowRef newWindow = createNewWindow (windowStyleFlags);

            GetRootControl (newWindow, (ControlRef*) &viewToAttachTo);
            jassert (viewToAttachTo != 0);

            HIViewRef growBox = 0;
            HIViewFindByID (HIViewGetRoot (newWindow), kHIViewWindowGrowBoxID, &growBox);

            if (growBox != 0)
                HIGrowBoxViewSetTransparent (growBox, true);
        }

        createNewHIView();

        HIViewAddSubview (viewToAttachTo, viewRef);
        HIViewSetVisible (viewRef, component->isVisible());

        setTitle (component->getName());

        if (component->isVisible() && ! isSharedWindow)
        {
            ShowWindow (windowRef);
            ActivateWindow (windowRef, component->getWantsKeyboardFocus());
        }
    }

    ~HIViewComponentPeer()
    {
        minimisedWindows.removeValue (windowRef);

        if (IsValidWindowPtr (windowRef))
        {
            if (! isSharedWindow)
            {
                CFRelease (viewRef);
                viewRef = 0;

                DisposeWindow (windowRef);
            }
            else
            {
                if (eventHandlerRef != 0)
                    RemoveEventHandler (eventHandlerRef);

                CFRelease (viewRef);
                viewRef = 0;
            }

            windowRef = 0;
        }

        if (currentlyFocusedPeer == this)
            currentlyFocusedPeer = 0;

        delete repainter;
    }

    //==============================================================================
    void* getNativeHandle() const
    {
        return windowRef;
    }

    void setVisible (bool shouldBeVisible)
    {
        HIViewSetVisible (viewRef, shouldBeVisible);

        if ((! isSharedWindow) && IsValidWindowPtr (windowRef))
        {
            if (shouldBeVisible)
                ShowWindow (windowRef);
            else
                HideWindow (windowRef);

            resizeViewToFitWindow();

            // If nothing else is focused, then grab the focus too
            if (shouldBeVisible
                 && Component::getCurrentlyFocusedComponent() == 0
                 && Process::isForegroundProcess())
            {
                component->toFront (true);
            }
        }
    }

    void setTitle (const String& title)
    {
        if ((! isSharedWindow) && IsValidWindowPtr (windowRef))
        {
            CFStringRef t = PlatformUtilities::juceStringToCFString (title);
            SetWindowTitleWithCFString (windowRef, t);
            CFRelease (t);
        }
    }

    void setPosition (int x, int y)
    {
        if (isSharedWindow)
        {
            HIViewPlaceInSuperviewAt (viewRef, x, y);
        }
        else if (IsValidWindowPtr (windowRef))
        {
            Rect r;
            GetWindowBounds (windowRef, windowRegionToUse, &r);
            r.right += x - r.left;
            r.bottom += y - r.top;
            r.left = x;
            r.top = y;
            SetWindowBounds (windowRef, windowRegionToUse, &r);
        }
    }

    void setSize (int w, int h)
    {
        w = jmax (0, w);
        h = jmax (0, h);

        if (w != getComponent()->getWidth()
            || h != getComponent()->getHeight())
        {
            repainter->repaint (0, 0, w, h);
        }

        if (isSharedWindow)
        {
            HIRect r;
            HIViewGetFrame (viewRef, &r);
            r.size.width = (float) w;
            r.size.height = (float) h;
            HIViewSetFrame (viewRef, &r);
        }
        else if (IsValidWindowPtr (windowRef))
        {
            Rect r;
            GetWindowBounds (windowRef, windowRegionToUse, &r);
            r.right = r.left + w;
            r.bottom = r.top + h;
            SetWindowBounds (windowRef, windowRegionToUse, &r);
        }
    }

    void setBounds (int x, int y, int w, int h, const bool isNowFullScreen)
    {
        fullScreen = isNowFullScreen;
        w = jmax (0, w);
        h = jmax (0, h);

        if (w != getComponent()->getWidth()
            || h != getComponent()->getHeight())
        {
            repainter->repaint (0, 0, w, h);
        }

        if (isSharedWindow)
        {
            HIRect r;
            r.origin.x = (float) x;
            r.origin.y = (float) y;
            r.size.width = (float) w;
            r.size.height = (float) h;
            HIViewSetFrame (viewRef, &r);
        }
        else if (IsValidWindowPtr (windowRef))
        {
            Rect r;
            r.left = x;
            r.top = y;
            r.right = x + w;
            r.bottom = y + h;
            SetWindowBounds (windowRef, windowRegionToUse, &r);
        }
    }

    void getBounds (int& x, int& y, int& w, int& h, const bool global) const
    {
        HIRect hiViewPos;
        HIViewGetFrame (viewRef, &hiViewPos);

        if (global)
        {
            HIViewRef content = 0;
            HIViewFindByID (HIViewGetRoot (windowRef), kHIViewWindowContentID, &content);
            HIPoint p = { 0.0f, 0.0f };
            HIViewConvertPoint (&p, viewRef, content);

            x = (int) p.x;
            y = (int) p.y;

            if (IsValidWindowPtr (windowRef))
            {
                Rect windowPos;
                GetWindowBounds (windowRef, kWindowContentRgn, &windowPos);

                x += windowPos.left;
                y += windowPos.top;
            }
        }
        else
        {
            x = (int) hiViewPos.origin.x;
            y = (int) hiViewPos.origin.y;
        }

        w = (int) hiViewPos.size.width;
        h = (int) hiViewPos.size.height;
    }

    void getBounds (int& x, int& y, int& w, int& h) const
    {
        getBounds (x, y, w, h, ! isSharedWindow);
    }

    int getScreenX() const
    {
        int x, y, w, h;
        getBounds (x, y, w, h, true);
        return x;
    }

    int getScreenY() const
    {
        int x, y, w, h;
        getBounds (x, y, w, h, true);
        return y;
    }

    void relativePositionToGlobal (int& x, int& y)
    {
        int wx, wy, ww, wh;
        getBounds (wx, wy, ww, wh, true);

        x += wx;
        y += wy;
    }

    void globalPositionToRelative (int& x, int& y)
    {
        int wx, wy, ww, wh;
        getBounds (wx, wy, ww, wh, true);

        x -= wx;
        y -= wy;
    }

    void setMinimised (bool shouldBeMinimised)
    {
        if (! isSharedWindow)
            setWindowMinimised (windowRef, shouldBeMinimised);
    }

    bool isMinimised() const
    {
        return minimisedWindows.contains (windowRef);
    }

    void setFullScreen (bool shouldBeFullScreen)
    {
        if (! isSharedWindow)
        {
            setMinimised (false);

            if (fullScreen != shouldBeFullScreen)
            {
                Rectangle r (lastNonFullscreenBounds);

                if (shouldBeFullScreen)
                    r = Desktop::getInstance().getMainMonitorArea();

                // (can't call the component's setBounds method because that'll reset our fullscreen flag)
                if (r != getComponent()->getBounds() && ! r.isEmpty())
                    setBounds (r.getX(), r.getY(), r.getWidth(), r.getHeight(), shouldBeFullScreen);
            }
        }
    }

    bool isFullScreen() const
    {
        return fullScreen;
    }

    bool contains (int x, int y, bool trueIfInAChildWindow) const
    {
        if (x < 0 || y < 0
            || x >= component->getWidth() || y >= component->getHeight()
            || ! IsValidWindowPtr (windowRef))
            return false;

        Rect r;
        GetWindowBounds (windowRef, windowRegionToUse, &r);

        ::Point p;
        p.h = r.left + x;
        p.v = r.top + y;

        WindowRef ref2 = 0;
        FindWindow (p, &ref2);

        if (windowRef != ref2)
            return false;

        if (trueIfInAChildWindow)
            return true;

        HIPoint p2;
        p2.x = (float) x;
        p2.y = (float) y;
        HIViewRef hit;

        HIViewGetSubviewHit (viewRef, &p2, true, &hit);
        return hit == 0 || hit == viewRef;
    }

    const BorderSize getFrameSize() const
    {
        return BorderSize();
    }

    bool setAlwaysOnTop (bool alwaysOnTop)
    {
        // can't do this so return false and let the component create a new window
        return false;
    }

    void toFront (bool makeActiveWindow)
    {
        makeActiveWindow = makeActiveWindow
                            && component->isValidComponent()
                            && (component->getWantsKeyboardFocus()
                                || component->isCurrentlyModal());

        if (windowRef != FrontWindow()
             || (makeActiveWindow && ! IsWindowActive (windowRef))
             || ! Process::isForegroundProcess())
        {
            if (! Process::isForegroundProcess())
            {
                ProcessSerialNumber psn;
                GetCurrentProcess (&psn);
                SetFrontProcess (&psn);
            }

            if (IsValidWindowPtr (windowRef))
            {
                if (makeActiveWindow)
                {
                    SelectWindow (windowRef);
                    SetUserFocusWindow (windowRef);
                    HIViewAdvanceFocus (viewRef, 0);
                }
                else
                {
                    BringToFront (windowRef);
                }

                handleBroughtToFront();
            }
        }
    }

    void toBehind (ComponentPeer* other)
    {
        HIViewComponentPeer* const otherWindow = dynamic_cast <HIViewComponentPeer*> (other);

        if (other != 0 && windowRef != 0 && otherWindow->windowRef != 0)
        {
            if (windowRef == otherWindow->windowRef)
            {
                HIViewSetZOrder (viewRef, kHIViewZOrderBelow, otherWindow->viewRef);
            }
            else
            {
                SendBehind (windowRef, otherWindow->windowRef);
            }
        }
    }

    void setIcon (const Image& /*newIcon*/)
    {
        // to do..
    }

    //==============================================================================
    void viewFocusGain()
    {
        const MessageManagerLock messLock;

        if (currentlyFocusedPeer != this)
        {
            if (ComponentPeer::isValidPeer (currentlyFocusedPeer))
                currentlyFocusedPeer->handleFocusLoss();

            currentlyFocusedPeer = this;

            handleFocusGain();
        }
    }

    void viewFocusLoss()
    {
        if (currentlyFocusedPeer == this)
        {
            currentlyFocusedPeer = 0;
            handleFocusLoss();
        }
    }

    bool isFocused() const
    {
        return windowRef == GetUserFocusWindow()
                && HIViewSubtreeContainsFocus (viewRef);
    }

    void grabFocus()
    {
        if ((! isFocused()) && IsValidWindowPtr (windowRef))
        {
            SetUserFocusWindow (windowRef);
            HIViewAdvanceFocus (viewRef, 0);
        }
    }

    //==============================================================================
    void repaint (int x, int y, int w, int h)
    {
        if (Rectangle::intersectRectangles (x, y, w, h,
                                            0, 0,
                                            getComponent()->getWidth(),
                                            getComponent()->getHeight()))
        {
            if ((getStyleFlags() & windowRepaintedExplictly) == 0)
            {
                if (isCompositingWindow)
                {
#if MACOS_10_3_OR_EARLIER
                    RgnHandle rgn = NewRgn();
                    SetRectRgn (rgn, x, y, x + w, y + h);
                    HIViewSetNeedsDisplayInRegion (viewRef, rgn, true);
                    DisposeRgn (rgn);
#else
                    HIRect r;
                    r.origin.x = x;
                    r.origin.y = y;
                    r.size.width = w;
                    r.size.height = h;

                    HIViewSetNeedsDisplayInRect (viewRef, &r, true);
#endif
                }
                else
                {
                    if (! isTimerRunning())
                        startTimer (20);
                }
            }

            repainter->repaint (x, y, w, h);
        }
    }

    void timerCallback()
    {
        performAnyPendingRepaintsNow();
    }

    void performAnyPendingRepaintsNow()
    {
        stopTimer();

        if (component->isVisible())
        {
#if MACOS_10_2_OR_EARLIER
            if (! isCompositingWindow)
            {
                Rect w;
                GetWindowBounds (windowRef, windowRegionToUse, &w);

                RgnHandle rgn = NewRgn();
                SetRectRgn (rgn, 0, 0, w.right - w.left, w.bottom - w.top);
                UpdateControls (windowRef, rgn);
                DisposeRgn (rgn);
            }
            else
            {
                EventRef theEvent;

                EventTypeSpec eventTypes[1];
                eventTypes[0].eventClass = kEventClassControl;
                eventTypes[0].eventKind  = kEventControlDraw;

                int n = 3;
                while (--n >= 0
                        && ReceiveNextEvent (1, eventTypes, kEventDurationNoWait, true, &theEvent) == noErr)
                {
                    if (GetEventClass (theEvent) == kEventClassAppleEvent)
                    {
                        EventRecord eventRec;
                        if (ConvertEventRefToEventRecord (theEvent, &eventRec))
                            AEProcessAppleEvent (&eventRec);
                    }
                    else
                    {
                        EventTargetRef theTarget = GetEventDispatcherTarget();
                        SendEventToEventTarget (theEvent, theTarget);
                    }

                    ReleaseEvent (theEvent);
                }
            }
#else
            HIViewRender (viewRef);
#endif
        }
    }

    //==============================================================================
    juce_UseDebuggingNewOperator

    WindowRef windowRef;
    HIViewRef viewRef;

private:
    EventHandlerRef eventHandlerRef;
    bool fullScreen, isSharedWindow, isCompositingWindow;

    //==============================================================================
    class RepaintManager : public Timer
    {
public:
        RepaintManager (HIViewComponentPeer* const peer_)
            : peer (peer_),
              image (0)
        {
        }

        ~RepaintManager()
        {
            delete image;
        }

        void timerCallback()
        {
            stopTimer();
            deleteAndZero (image);
        }

        void repaint (int x, int y, int w, int h)
        {
            regionsNeedingRepaint.add (x, y, w, h);
        }

        void repaintAnyRemainingRegions()
        {
            // if any regions have been invaldated during the paint callback,
            // we need to repaint them explicitly because the mac throws this
            // stuff away
            for (RectangleList::Iterator i (regionsNeedingRepaint); i.next();)
            {
                const Rectangle& r = i.getRectangle();
                peer->repaint (r.getX(), r.getY(), r.getWidth(), r.getHeight());
            }
        }

        void paint (CGContextRef cgContext, int x, int y, int w, int h)
        {
            if (w > 0 && h > 0)
            {
                bool refresh = false;
                int imW = image != 0 ? image->getWidth() : 0;
                int imH = image != 0 ? image->getHeight() : 0;

                if (imW < w || imH < h)
                {
                    imW = jmin (peer->getComponent()->getWidth(), (w + 31) & ~31);
                    imH = jmin (peer->getComponent()->getHeight(), (h + 31) & ~31);

                    delete image;
                    image = new MacBitmapImage (peer->getComponent()->isOpaque() ? Image::RGB
                                                                                 : Image::ARGB,
                                                imW, imH, false);

                    refresh = true;
                }
                else if (imageX > x || imageY > y
                          || imageX + imW < x + w
                          || imageY + imH < y + h)
                {
                    refresh = true;
                }

                if (refresh)
                {
                    regionsNeedingRepaint.clear();
                    regionsNeedingRepaint.addWithoutMerging (Rectangle (x, y, imW, imH));
                    imageX = x;
                    imageY = y;
                }

                LowLevelGraphicsSoftwareRenderer context (*image);
                context.setOrigin (-imageX, -imageY);

                if (context.reduceClipRegion (regionsNeedingRepaint))
                {
                    regionsNeedingRepaint.clear();

                    if (! peer->getComponent()->isOpaque())
                    {
                        for (RectangleList::Iterator i (*context.getRawClipRegion()); i.next();)
                        {
                            const Rectangle& r = i.getRectangle();
                            image->clear (r.getX(), r.getY(), r.getWidth(), r.getHeight());
                        }
                    }

                    regionsNeedingRepaint.clear();
                    peer->clearMaskedRegion();
                    peer->handlePaint (context);
                }
                else
                {
                    regionsNeedingRepaint.clear();
                }

                if (! peer->maskedRegion.isEmpty())
                {
                    RectangleList total (Rectangle (x, y, w, h));
                    total.subtract (peer->maskedRegion);

                    CGRect* rects = (CGRect*) juce_malloc (sizeof (CGRect) * total.getNumRectangles());
                    int n = 0;

                    for (RectangleList::Iterator i (total); i.next();)
                    {
                        const Rectangle& r = i.getRectangle();
                        rects[n].origin.x = (int) r.getX();
                        rects[n].origin.y = (int) r.getY();
                        rects[n].size.width = roundFloatToInt (r.getWidth());
                        rects[n++].size.height = roundFloatToInt (r.getHeight());
                    }

                    CGContextClipToRects (cgContext, rects, n);
                    juce_free (rects);
                }

                if (peer->isSharedWindow)
                {
                    CGRect clip;
                    clip.origin.x = x;
                    clip.origin.y = y;
                    clip.size.width = jmin (w, peer->getComponent()->getWidth() - x);
                    clip.size.height = jmin (h, peer->getComponent()->getHeight() - y);

                    CGContextClipToRect (cgContext, clip);
                }

                image->blitToContext (cgContext, imageX, imageY);
            }

            startTimer (3000);
        }

    private:
        HIViewComponentPeer* const peer;
        MacBitmapImage* image;
        int imageX, imageY;
        RectangleList regionsNeedingRepaint;

        RepaintManager (const RepaintManager&);
        const RepaintManager& operator= (const RepaintManager&);
    };

    RepaintManager* repainter;

    friend class RepaintManager;

    //==============================================================================
    static OSStatus handleFrameRepaintEvent (EventHandlerCallRef myHandler,
                                             EventRef theEvent,
                                             void* userData)
    {
        // don't draw the frame..
        return noErr;
    }

    //==============================================================================
    OSStatus handleWindowClassEvent (EventRef theEvent)
    {
        switch (GetEventKind (theEvent))
        {
        case kEventWindowBoundsChanged:
            resizeViewToFitWindow();
            break; // allow other handlers in the event chain to also get a look at the events

        case kEventWindowBoundsChanging:
            if ((styleFlags & (windowIsResizable | windowHasTitleBar)) == (windowIsResizable | windowHasTitleBar))
            {
                UInt32 atts = 0;
                GetEventParameter (theEvent, kEventParamAttributes, typeUInt32,
                                   0, sizeof (UInt32), 0, &atts);

                if ((atts & (kWindowBoundsChangeUserDrag | kWindowBoundsChangeUserResize)) != 0)
                {
                    if (component->isCurrentlyBlockedByAnotherModalComponent())
                    {
                        Component* const modal = Component::getCurrentlyModalComponent();
                        if (modal != 0)
                            modal->inputAttemptWhenModal();
                    }

                    if ((atts & kWindowBoundsChangeUserResize) != 0
                         && constrainer != 0 && ! isSharedWindow)
                    {
                        Rect current;
                        GetEventParameter (theEvent, kEventParamCurrentBounds, typeQDRectangle,
                                           0, sizeof (Rect), 0, &current);

                        int x = current.left;
                        int y = current.top;
                        int w = current.right - current.left;
                        int h = current.bottom - current.top;

                        const Rectangle currentRect (getComponent()->getBounds());

                        constrainer->checkBounds (x, y, w, h, currentRect,
                                                  Desktop::getInstance().getAllMonitorDisplayAreas().getBounds(),
                                                  y != currentRect.getY() && y + h == currentRect.getBottom(),
                                                  x != currentRect.getX() && x + w == currentRect.getRight(),
                                                  y == currentRect.getY() && y + h != currentRect.getBottom(),
                                                  x == currentRect.getX() && x + w != currentRect.getRight());

                        current.left = x;
                        current.top = y;
                        current.right = x + w;
                        current.bottom = y + h;

                        SetEventParameter (theEvent, kEventParamCurrentBounds, typeQDRectangle,
                                           sizeof (Rect), &current);

                        return noErr;
                    }
                }
            }
            break;

        case kEventWindowFocusAcquired:
            keysCurrentlyDown.clear();

            if ((! isSharedWindow) || HIViewSubtreeContainsFocus (viewRef))
                viewFocusGain();

            break; // allow other handlers in the event chain to also get a look at the events

        case kEventWindowFocusRelinquish:
            keysCurrentlyDown.clear();
            viewFocusLoss();

            break; // allow other handlers in the event chain to also get a look at the events

        case kEventWindowCollapsed:
            minimisedWindows.addIfNotAlreadyThere (windowRef);
            handleMovedOrResized();
            break; // allow other handlers in the event chain to also get a look at the events

        case kEventWindowExpanded:
            minimisedWindows.removeValue (windowRef);
            handleMovedOrResized();
            break; // allow other handlers in the event chain to also get a look at the events

        case kEventWindowShown:
            break; // allow other handlers in the event chain to also get a look at the events

        case kEventWindowClose:
            if (! isSharedWindow)
                handleUserClosingWindow();

            return noErr;  // (returning a notHandledErr would cause the OS to delete the window itself)

        default:
            break;
        }

        return eventNotHandledErr;
    }

    OSStatus handleKeyEvent (EventRef theEvent, juce_wchar textCharacter)
    {
        updateModifiers (theEvent);

        UniChar unicodeChars [4];
        zeromem (unicodeChars, sizeof (unicodeChars));
        GetEventParameter (theEvent, kEventParamKeyUnicodes, typeUnicodeText, 0, sizeof (unicodeChars), 0, unicodeChars);

        int keyCode = (int) (unsigned int) unicodeChars[0];

        UInt32 rawKey = 0;
        GetEventParameter (theEvent, kEventParamKeyCode, typeUInt32, 0, sizeof (UInt32), 0, &rawKey);

        if ((currentModifiers & ModifierKeys::ctrlModifier) != 0)
        {
            if (keyCode >= 1 && keyCode <= 26)
                keyCode += ('A' - 1);
        }

        static const int keyTranslations[] =
        {
            122, KeyPress::F1Key, 120, KeyPress::F2Key,
            99, KeyPress::F3Key, 118, KeyPress::F4Key,
            96, KeyPress::F5Key, 97, KeyPress::F6Key,
            98, KeyPress::F7Key, 100, KeyPress::F8Key,
            101, KeyPress::F9Key, 109, KeyPress::F10Key,
            103, KeyPress::F11Key, 111, KeyPress::F12Key,
            105, KeyPress::F13Key, 107, KeyPress::F14Key,
            113, KeyPress::F15Key, 106, KeyPress::F16Key,
            36, KeyPress::returnKey, 51, KeyPress::backspaceKey,
            123, KeyPress::leftKey, 124, KeyPress::rightKey,
            126, KeyPress::upKey, 125, KeyPress::downKey,
            115, KeyPress::homeKey, 119, KeyPress::endKey,
            116, KeyPress::pageUpKey, 121, KeyPress::pageDownKey,
            76, KeyPress::returnKey,
            82, KeyPress::numberPad0, 83, KeyPress::numberPad1,
            84, KeyPress::numberPad2, 85, KeyPress::numberPad3,
            86, KeyPress::numberPad4, 87, KeyPress::numberPad5,
            88, KeyPress::numberPad6, 89, KeyPress::numberPad7,
            91, KeyPress::numberPad8, 92, KeyPress::numberPad9,
            65, KeyPress::numberPadDecimalPoint, 78, KeyPress::numberPadSubtract,
            67, KeyPress::numberPadMultiply, 75, KeyPress::numberPadDivide,
            69, KeyPress::numberPadAdd,
            0
        };

        const int* kt = keyTranslations;

        while (*kt != 0)
        {
            if ((int) rawKey == *kt)
            {
                keyCode = *++kt;
                break;
            }

            kt += 2;
        }

        static juce_wchar lastTextCharacter = 0;

        switch (GetEventKind (theEvent))
        {
        case kEventRawKeyDown:
            keysCurrentlyDown.addIfNotAlreadyThere ((void*) keyCode);
            handleKeyUpOrDown();
            lastTextCharacter = textCharacter;
            handleKeyPress (keyCode, textCharacter);
            break;

        case kEventRawKeyUp:
            keysCurrentlyDown.removeValue ((void*) keyCode);
            handleKeyUpOrDown();
            lastTextCharacter = 0;
            break;

        case kEventRawKeyRepeat:
            handleKeyPress (keyCode, lastTextCharacter);
            break;

        case kEventRawKeyModifiersChanged:
            handleModifierKeysChange();
            break;

        default:
            jassertfalse
            break;
        }

        return noErr;
    }

    OSStatus handleTextInputEvent (EventRef theEvent)
    {
        UniChar uc;
        GetEventParameter (theEvent, kEventParamTextInputSendText, typeUnicodeText, 0, sizeof (uc), 0, &uc);

        EventRef originalEvent;
        GetEventParameter (theEvent, kEventParamTextInputSendKeyboardEvent, typeEventRef, 0, sizeof (originalEvent), 0, &originalEvent);

        handleKeyEvent (originalEvent, (juce_wchar) uc);

        return noErr;
    }

    OSStatus handleMouseEvent (EventHandlerCallRef callRef, EventRef theEvent)
    {
        MouseCheckTimer::getInstance()->moved (this);

        ::Point where;
        GetEventParameter (theEvent, kEventParamMouseLocation, typeQDPoint, 0, sizeof (::Point), 0, &where);
        int x = where.h;
        int y = where.v;
        globalPositionToRelative (x, y);

        int64 time = getEventTime (theEvent);

        switch (GetEventKind (theEvent))
        {
            case kEventMouseMoved:
                MouseCheckTimer::getInstance()->hasEverHadAMouseMove = true;
                updateModifiers (theEvent);
                handleMouseMove (x, y, time);
                break;

            case kEventMouseDragged:
                updateModifiers (theEvent);
                handleMouseDrag (x, y, time);
                break;

            case kEventMouseDown:
            {
                if (! Process::isForegroundProcess())
                {
                    ProcessSerialNumber psn;
                    GetCurrentProcess (&psn);
                    SetFrontProcess (&psn);

                    toFront (true);
                }

#if JUCE_QUICKTIME
                {
                    long mods;
                    GetEventParameter (theEvent, kEventParamKeyModifiers, typeUInt32, 0, sizeof (mods), 0, &mods);

                    ::Point where;
                    GetEventParameter (theEvent, kEventParamMouseLocation, typeQDPoint, 0, sizeof (::Point), 0, &where);

                    OfferMouseClickToQuickTime (windowRef, where, EventTimeToTicks (GetEventTime (theEvent)), mods, component);
                }
#endif

                if (component->isBroughtToFrontOnMouseClick()
                    && ! component->isCurrentlyBlockedByAnotherModalComponent())
                {
                    //ActivateWindow (windowRef, true);
                    SelectWindow (windowRef);
                }

                EventMouseButton button;
                GetEventParameter (theEvent, kEventParamMouseButton, typeMouseButton, 0, sizeof (EventMouseButton), 0, &button);

                // need to clear all these flags because sometimes the mac can swallow (right) mouse-up events and
                // this makes a button get stuck down. Since there's no other way to tell what buttons are down,
                // this is all I can think of doing about it..
                currentModifiers &= ~(ModifierKeys::leftButtonModifier | ModifierKeys::rightButtonModifier | ModifierKeys::middleButtonModifier);

                if (button == kEventMouseButtonPrimary)
                    currentModifiers |= ModifierKeys::leftButtonModifier;
                else if (button == kEventMouseButtonSecondary)
                    currentModifiers |= ModifierKeys::rightButtonModifier;
                else if (button == kEventMouseButtonTertiary)
                    currentModifiers |= ModifierKeys::middleButtonModifier;

                updateModifiers (theEvent);

                juce_currentMouseTrackingPeer = this; // puts the message dispatcher into mouse-tracking mode..
                handleMouseDown (x, y, time);
                break;
            }

            case kEventMouseUp:
            {
                const int oldModifiers = currentModifiers;

                EventMouseButton button;
                GetEventParameter (theEvent, kEventParamMouseButton, typeMouseButton, 0, sizeof (EventMouseButton), 0, &button);

                if (button == kEventMouseButtonPrimary)
                    currentModifiers &= ~ModifierKeys::leftButtonModifier;
                else if (button == kEventMouseButtonSecondary)
                    currentModifiers &= ~ModifierKeys::rightButtonModifier;

                updateModifiers (theEvent);

                handleMouseUp (oldModifiers, x, y, time);
                juce_currentMouseTrackingPeer = 0;
                break;
            }

            case kEventMouseWheelMoved:
            {
                EventMouseWheelAxis axis;
                GetEventParameter (theEvent, kEventParamMouseWheelAxis, typeMouseWheelAxis, 0, sizeof (axis), 0, &axis);

                SInt32 delta;
                GetEventParameter (theEvent, kEventParamMouseWheelDelta,
                                   typeLongInteger, 0, sizeof (delta), 0, &delta);

                updateModifiers (theEvent);

                handleMouseWheel (axis == kEventMouseWheelAxisX ? delta * 10 : 0,
                                  axis == kEventMouseWheelAxisX ? 0 : delta * 10,
                                  time);

                break;
            }
        }

        return noErr;
    }

    OSStatus handleDragAndDrop (EventRef theEvent)
    {
        DragRef dragRef;
        if (GetEventParameter (theEvent, kEventParamDragRef, typeDragRef, 0, sizeof (dragRef), 0, &dragRef) == noErr)
        {
            int mx, my;
            component->getMouseXYRelative (mx, my);

            UInt16 numItems = 0;
            if (CountDragItems (dragRef, &numItems) == noErr)
            {
                StringArray filenames;

                for (int i = 0; i < (int) numItems; ++i)
                {
                    DragItemRef ref;

                    if (GetDragItemReferenceNumber (dragRef, i + 1, &ref) == noErr)
                    {
                        const FlavorType flavorType = kDragFlavorTypeHFS;

                        Size size = 0;
                        if (GetFlavorDataSize (dragRef, ref, flavorType, &size) == noErr)
                        {
                            void* data = juce_calloc (size);

                            if (GetFlavorData (dragRef, ref, flavorType, data, &size, 0) == noErr)
                            {
                                HFSFlavor* f = (HFSFlavor*) data;
                                FSRef fsref;

                                if (FSpMakeFSRef (&f->fileSpec, &fsref) == noErr)
                                {
                                    const String path (PlatformUtilities::makePathFromFSRef (&fsref));

                                    if (path.isNotEmpty())
                                        filenames.add (path);
                                }
                            }

                            juce_free (data);
                        }
                    }
                }

                filenames.trim();
                filenames.removeEmptyStrings();

                if (filenames.size() > 0)
                    handleFilesDropped (mx, my, filenames);
            }
        }

        return noErr;
    }

    void resizeViewToFitWindow()
    {
        HIRect r;

        if (isSharedWindow)
        {
            HIViewGetFrame (viewRef, &r);
            r.size.width = (float) component->getWidth();
            r.size.height = (float) component->getHeight();
        }
        else
        {
            r.origin.x = 0;
            r.origin.y = 0;

            Rect w;
            GetWindowBounds (windowRef, windowRegionToUse, &w);

            r.size.width = (float) (w.right - w.left);
            r.size.height = (float) (w.bottom - w.top);
        }

        HIViewSetFrame (viewRef, &r);

#if MACOS_10_3_OR_EARLIER
        component->repaint();
#endif
    }

    OSStatus hiViewDraw (EventRef theEvent)
    {
        CGContextRef context = 0;
        GetEventParameter (theEvent, kEventParamCGContextRef, typeCGContextRef, 0, sizeof (CGContextRef), 0, &context);

        CGrafPtr oldPort;
        CGrafPtr port = 0;

        if (context == 0)
        {
            GetEventParameter (theEvent, kEventParamGrafPort, typeGrafPtr, 0, sizeof (CGrafPtr), 0, &port);

            GetPort (&oldPort);
            SetPort (port);

            if (port != 0)
                QDBeginCGContext (port, &context);

            if (! isCompositingWindow)
            {
                Rect bounds;
                GetWindowBounds (windowRef, windowRegionToUse, &bounds);
                CGContextTranslateCTM (context, 0, bounds.bottom - bounds.top);
                CGContextScaleCTM (context, 1.0, -1.0);
            }

            if (isSharedWindow)
            {
                // NB - Had terrible problems trying to correctly get the position
                // of this view relative to the window, and this seems wrong, but
                // works better than any other method I've tried..
                HIRect hiViewPos;
                HIViewGetFrame (viewRef, &hiViewPos);
                CGContextTranslateCTM (context, hiViewPos.origin.x, hiViewPos.origin.y);
            }
        }

#if MACOS_10_2_OR_EARLIER
        RgnHandle rgn = 0;
        GetEventParameter (theEvent, kEventParamRgnHandle, typeQDRgnHandle, 0, sizeof (RgnHandle), 0, &rgn);

        CGRect clip;

        if (rgn != 0)
        {
            Rect bounds;
            GetRegionBounds (rgn, &bounds);
            clip.origin.x = bounds.left;
            clip.origin.y = bounds.top;
            clip.size.width = bounds.right - bounds.left;
            clip.size.height = bounds.bottom - bounds.top;
        }
        else
        {
            HIViewGetBounds (viewRef, &clip);
        }
#else
        CGRect clip (CGContextGetClipBoundingBox (context));
#endif

        clip = CGRectIntegral (clip);

        if (clip.origin.x < 0)
        {
            clip.size.width += clip.origin.x;
            clip.origin.x = 0;
        }

        if (clip.origin.y < 0)
        {
            clip.size.height += clip.origin.y;
            clip.origin.y = 0;
        }

        if (! component->isOpaque())
            CGContextClearRect (context, clip);

        repainter->paint (context,
                          (int) clip.origin.x, (int) clip.origin.y,
                          (int) clip.size.width, (int) clip.size.height);

        if (port != 0)
        {
            CGContextFlush (context);
            QDEndCGContext (port, &context);

            SetPort (oldPort);
        }

        repainter->repaintAnyRemainingRegions();

        return noErr;
    }

    static pascal OSStatus handleWindowEvent (EventHandlerCallRef callRef, EventRef theEvent, void* userData)
    {
        MessageManager::delayWaitCursor();

        HIViewComponentPeer* const peer = (HIViewComponentPeer*) userData;

        const MessageManagerLock messLock;

        if (ComponentPeer::isValidPeer (peer))
            return peer->handleWindowEventForPeer (callRef, theEvent);

        return eventNotHandledErr;
    }

    OSStatus handleWindowEventForPeer (EventHandlerCallRef callRef, EventRef theEvent)
    {
        switch (GetEventClass (theEvent))
        {
            case kEventClassMouse:
            {
                static HIViewComponentPeer* lastMouseDownPeer = 0;

                const UInt32 eventKind = GetEventKind (theEvent);
                HIViewRef view = 0;

                if (eventKind == kEventMouseDragged)
                {
                    view = viewRef;
                }
                else
                {
                    HIViewGetViewForMouseEvent (HIViewGetRoot (windowRef), theEvent, &view);

                    if (view != viewRef)
                    {
                        if ((eventKind == kEventMouseUp
                              || eventKind == kEventMouseExited)
                            && ComponentPeer::isValidPeer (lastMouseDownPeer))
                        {
                            return lastMouseDownPeer->handleMouseEvent (callRef, theEvent);
                        }

                        return eventNotHandledErr;
                    }
                }

                if (eventKind == kEventMouseDown
                    || eventKind == kEventMouseDragged
                    || eventKind == kEventMouseEntered)
                {
                    lastMouseDownPeer = this;
                }

                return handleMouseEvent (callRef, theEvent);
            }
            break;

            case kEventClassWindow:
                return handleWindowClassEvent (theEvent);

            case kEventClassKeyboard:
                if (isFocused())
                    return handleKeyEvent (theEvent, 0);

                break;

            case kEventClassTextInput:
                if (isFocused())
                    return handleTextInputEvent (theEvent);

                break;

            default:
                break;
        }

        return eventNotHandledErr;
    }

    static pascal OSStatus hiViewEventHandler (EventHandlerCallRef myHandler, EventRef theEvent, void* userData)
    {
        MessageManager::delayWaitCursor();

        const UInt32 eventKind = GetEventKind (theEvent);
        const UInt32 eventClass = GetEventClass (theEvent);

        if (eventClass == kEventClassHIObject)
        {
            switch (eventKind)
            {
                case kEventHIObjectConstruct:
                {
                    void* data = juce_calloc (sizeof (void*));
                    SetEventParameter (theEvent, kEventParamHIObjectInstance,
                                       typeVoidPtr, sizeof (void*), &data);

                    return noErr;
                }

                case kEventHIObjectInitialize:
                    GetEventParameter (theEvent, 'peer', typeVoidPtr, 0, sizeof (void*), 0, (void**) userData);
                    return noErr;

                case kEventHIObjectDestruct:
                    juce_free (userData);
                    return noErr;

                default:
                    break;
            }
        }
        else if (eventClass == kEventClassControl)
        {
            HIViewComponentPeer* const peer = *(HIViewComponentPeer**) userData;
            const MessageManagerLock messLock;

            if (! ComponentPeer::isValidPeer (peer))
                return eventNotHandledErr;

            switch (eventKind)
            {
                case kEventControlDraw:
                    return peer->hiViewDraw (theEvent);

                case kEventControlBoundsChanged:
                {
                    HIRect bounds;
                    HIViewGetBounds (peer->viewRef, &bounds);
                    peer->repaint (0, 0, roundFloatToInt (bounds.size.width), roundFloatToInt (bounds.size.height));

                    peer->handleMovedOrResized();
                    return noErr;
                }

                case kEventControlHitTest:
                {
                    HIPoint where;
                    GetEventParameter (theEvent, kEventParamMouseLocation, typeHIPoint, 0, sizeof (HIPoint), 0, &where);

                    HIRect bounds;
                    HIViewGetBounds (peer->viewRef, &bounds);

                    ControlPartCode part = kControlNoPart;

                    if (CGRectContainsPoint (bounds, where))
                        part = 1;

                    SetEventParameter (theEvent, kEventParamControlPart, typeControlPartCode, sizeof (ControlPartCode), &part);
                    return noErr;
                }
                break;

                case kEventControlSetFocusPart:
                {
                    ControlPartCode desiredFocus;
                    if (GetEventParameter (theEvent, kEventParamControlPart, typeControlPartCode, 0, sizeof (ControlPartCode), 0, &desiredFocus) != noErr)
                        break;

                    if (desiredFocus == kControlNoPart)
                        peer->viewFocusLoss();
                    else
                        peer->viewFocusGain();

                    return noErr;
                }
                break;

                case kEventControlDragEnter:
                {
#if MACOS_10_2_OR_EARLIER
                    enum { kEventParamControlWouldAcceptDrop = 'cldg' };
#endif
                    Boolean accept = true;
                    SetEventParameter (theEvent, kEventParamControlWouldAcceptDrop, typeBoolean, sizeof (accept), &accept);
                    return noErr;
                }

                case kEventControlDragWithin:
                    return noErr;

                case kEventControlDragReceive:
                    return peer->handleDragAndDrop (theEvent);

                case kEventControlOwningWindowChanged:
                    return peer->ownerWindowChanged (theEvent);

#if ! MACOS_10_2_OR_EARLIER
                case kEventControlGetFrameMetrics:
                {
                    CallNextEventHandler (myHandler, theEvent);
                    HIViewFrameMetrics metrics;
                    GetEventParameter (theEvent, kEventParamControlFrameMetrics, typeControlFrameMetrics, 0, sizeof (metrics), 0, &metrics);
                    metrics.top = metrics.bottom = 0;
                    SetEventParameter (theEvent, kEventParamControlFrameMetrics, typeControlFrameMetrics, sizeof (metrics), &metrics);
                    return noErr;
                }
#endif

                case kEventControlInitialize:
                {
                    UInt32 features = kControlSupportsDragAndDrop
                                        | kControlSupportsFocus
                                        | kControlHandlesTracking
                                        | kControlSupportsEmbedding
                                        | (1 << 8) /*kHIViewFeatureGetsFocusOnClick*/;

                    SetEventParameter (theEvent, kEventParamControlFeatures, typeUInt32, sizeof (UInt32), &features);
                    return noErr;
                }

                default:
                    break;
            }
        }

        return eventNotHandledErr;
    }

    WindowRef createNewWindow (const int windowStyleFlags)
    {
        jassert (windowRef == 0);

        static ToolboxObjectClassRef customWindowClass = 0;

        if (customWindowClass == 0)
        {
            // Register our window class
            const EventTypeSpec customTypes[] = { { kEventClassWindow, kEventWindowDrawFrame } };

            UnsignedWide t;
            Microseconds (&t);
            const String randomString ((int) (t.lo & 0x7ffffff));
            const String juceWindowClassName (T("JUCEWindowClass_") + randomString);
            CFStringRef juceWindowClassNameCFString = PlatformUtilities::juceStringToCFString (juceWindowClassName);

            RegisterToolboxObjectClass (juceWindowClassNameCFString,
                                        0, 1, customTypes,
                                        NewEventHandlerUPP (handleFrameRepaintEvent),
                                        0, &customWindowClass);

            CFRelease (juceWindowClassNameCFString);
        }

        Rect pos;
        pos.left = getComponent()->getX();
        pos.top = getComponent()->getY();
        pos.right = getComponent()->getRight();
        pos.bottom = getComponent()->getBottom();

        int attributes = kWindowStandardHandlerAttribute | kWindowCompositingAttribute;
        if ((windowStyleFlags & windowHasDropShadow) == 0)
            attributes |= kWindowNoShadowAttribute;

        if ((windowStyleFlags & windowIgnoresMouseClicks) != 0)
            attributes |= kWindowIgnoreClicksAttribute;

#if ! MACOS_10_3_OR_EARLIER
        if ((windowStyleFlags & windowIsTemporary) != 0)
            attributes |= kWindowDoesNotCycleAttribute;
#endif

        WindowRef newWindow = 0;

        if ((windowStyleFlags & windowHasTitleBar) == 0)
        {
            attributes |= kWindowCollapseBoxAttribute;

            WindowDefSpec customWindowSpec;
            customWindowSpec.defType = kWindowDefObjectClass;
            customWindowSpec.u.classRef = customWindowClass;

            CreateCustomWindow (&customWindowSpec,
                                ((windowStyleFlags & windowIsTemporary) != 0) ? kUtilityWindowClass :
                                (getComponent()->isAlwaysOnTop() ? kUtilityWindowClass
                                                                 : kDocumentWindowClass),
                                attributes,
                                &pos,
                                &newWindow);
        }
        else
        {
            if ((windowStyleFlags & windowHasCloseButton) != 0)
                attributes |= kWindowCloseBoxAttribute;

            if ((windowStyleFlags & windowHasMinimiseButton) != 0)
                attributes |= kWindowCollapseBoxAttribute;

            if ((windowStyleFlags & windowHasMaximiseButton) != 0)
                attributes |= kWindowFullZoomAttribute;

            if ((windowStyleFlags & windowIsResizable) != 0)
                attributes |= kWindowResizableAttribute | kWindowLiveResizeAttribute;

            CreateNewWindow (kDocumentWindowClass, attributes, &pos, &newWindow);
        }

        jassert (newWindow != 0);
        if (newWindow != 0)
        {
            HideWindow (newWindow);

            SetAutomaticControlDragTrackingEnabledForWindow (newWindow, true);

            if (! getComponent()->isOpaque())
                SetWindowAlpha (newWindow, 0.9999999f); // to fool it into giving the window an alpha-channel
        }

        return newWindow;
    }

    OSStatus ownerWindowChanged (EventRef theEvent)
    {
        WindowRef newWindow = 0;
        GetEventParameter (theEvent, kEventParamControlCurrentOwningWindow, typeWindowRef, 0, sizeof (newWindow), 0, &newWindow);

        if (windowRef != newWindow)
        {
            if (eventHandlerRef != 0)
            {
                RemoveEventHandler (eventHandlerRef);
                eventHandlerRef = 0;
            }

            windowRef = newWindow;

            if (windowRef != 0)
            {
                const EventTypeSpec eventTypes[] =
                {
                    { kEventClassWindow, kEventWindowBoundsChanged },
                    { kEventClassWindow, kEventWindowBoundsChanging },
                    { kEventClassWindow, kEventWindowFocusAcquired },
                    { kEventClassWindow, kEventWindowFocusRelinquish },
                    { kEventClassWindow, kEventWindowCollapsed },
                    { kEventClassWindow, kEventWindowExpanded },
                    { kEventClassWindow, kEventWindowShown },
                    { kEventClassWindow, kEventWindowClose },
                    { kEventClassMouse, kEventMouseDown },
                    { kEventClassMouse, kEventMouseUp },
                    { kEventClassMouse, kEventMouseMoved },
                    { kEventClassMouse, kEventMouseDragged },
                    { kEventClassMouse, kEventMouseEntered },
                    { kEventClassMouse, kEventMouseExited },
                    { kEventClassMouse, kEventMouseWheelMoved },
                    //{ kEventClassKeyboard, kEventRawKeyDown },
                    { kEventClassKeyboard, kEventRawKeyUp },
                    { kEventClassKeyboard, kEventRawKeyRepeat },
                    { kEventClassKeyboard, kEventRawKeyModifiersChanged },
                    { kEventClassTextInput, kEventTextInputUnicodeForKeyEvent }
                };

                static EventHandlerUPP handleWindowEventUPP = 0;

                if (handleWindowEventUPP == 0)
                    handleWindowEventUPP = NewEventHandlerUPP (handleWindowEvent);

                InstallWindowEventHandler (windowRef, handleWindowEventUPP,
                                           GetEventTypeCount (eventTypes), eventTypes,
                                           (void*) this, (EventHandlerRef*) &eventHandlerRef);

                WindowAttributes attributes;
                GetWindowAttributes (windowRef, &attributes);

#if MACOS_10_3_OR_EARLIER
                isCompositingWindow = ((attributes & kWindowCompositingAttribute) != 0);
#else
                isCompositingWindow = HIViewIsCompositingEnabled (viewRef);
#endif

                MouseCheckTimer::getInstance()->resetMouseMoveChecker();
            }
        }

        resizeViewToFitWindow();
        return noErr;
    }

    void createNewHIView()
    {
        jassert (viewRef == 0);

        if (viewClassRef == 0)
        {
            // Register our HIView class
            EventTypeSpec viewEvents[] =
            {
                { kEventClassHIObject, kEventHIObjectConstruct },
                { kEventClassHIObject, kEventHIObjectInitialize },
                { kEventClassHIObject, kEventHIObjectDestruct },
                { kEventClassControl, kEventControlInitialize },
                { kEventClassControl, kEventControlDraw },
                { kEventClassControl, kEventControlBoundsChanged },
                { kEventClassControl, kEventControlSetFocusPart },
                { kEventClassControl, kEventControlHitTest },
                { kEventClassControl, kEventControlDragEnter },
                { kEventClassControl, kEventControlDragWithin },
                { kEventClassControl, kEventControlDragReceive },
                { kEventClassControl, kEventControlOwningWindowChanged }
            };

            UnsignedWide t;
            Microseconds (&t);
            const String randomString ((int) (t.lo & 0x7ffffff));
            const String juceHiViewClassName (T("JUCEHIViewClass_") + randomString);
            juceHiViewClassNameCFString = PlatformUtilities::juceStringToCFString (juceHiViewClassName);

            HIObjectRegisterSubclass (juceHiViewClassNameCFString,
                                      kHIViewClassID, 0,
                                      NewEventHandlerUPP (hiViewEventHandler),
                                      GetEventTypeCount (viewEvents),
                                      viewEvents, 0,
                                      &viewClassRef);
        }

        EventRef event;
        CreateEvent (0, kEventClassHIObject, kEventHIObjectInitialize, GetCurrentEventTime(), kEventAttributeNone, &event);

        void* thisPointer = this;
        SetEventParameter (event, 'peer', typeVoidPtr, sizeof (void*), &thisPointer);

        HIObjectCreate (juceHiViewClassNameCFString, event, (HIObjectRef*) &viewRef);

        SetControlDragTrackingEnabled (viewRef, true);

        if (isSharedWindow)
        {
            setBounds (component->getX(), component->getY(),
                       component->getWidth(), component->getHeight(), false);
        }
    }
};

bool juce_isHIViewCreatedByJuce (HIViewRef view)
{
    return juceHiViewClassNameCFString != 0
             && HIObjectIsOfClass ((HIObjectRef) view, juceHiViewClassNameCFString);
}

static void trackNextMouseEvent()
{
    UInt32 mods;
    MouseTrackingResult result;
    ::Point where;

    if (TrackMouseLocationWithOptions ((GrafPtr) -1, 0, 0.01, //kEventDurationForever,
                                       &where, &mods, &result) != noErr
        || ! ComponentPeer::isValidPeer (juce_currentMouseTrackingPeer))
    {
        juce_currentMouseTrackingPeer = 0;
        return;
    }

    if (result == kMouseTrackingTimedOut)
        return;

#if MACOS_10_3_OR_EARLIER
    const int x = where.h - juce_currentMouseTrackingPeer->getScreenX();
    const int y = where.v - juce_currentMouseTrackingPeer->getScreenY();
#else
    HIPoint p;
    p.x = where.h;
    p.y = where.v;
    HIPointConvert (&p, kHICoordSpaceScreenPixel, 0,
                    kHICoordSpaceView, ((HIViewComponentPeer*) juce_currentMouseTrackingPeer)->viewRef);
    const int x = p.x;
    const int y = p.y;
#endif

    if (result == kMouseTrackingMouseDragged)
    {
        updateModifiers (0);
        juce_currentMouseTrackingPeer->handleMouseDrag (x, y, getEventTime (0));

        if (! ComponentPeer::isValidPeer (juce_currentMouseTrackingPeer))
        {
            juce_currentMouseTrackingPeer = 0;
            return;
        }
    }
    else if (result == kMouseTrackingMouseUp
              || result == kMouseTrackingUserCancelled
              || result == kMouseTrackingMouseMoved)
    {
        if (ComponentPeer::isValidPeer (juce_currentMouseTrackingPeer))
        {
            const int oldModifiers = currentModifiers;
            currentModifiers &= ~(ModifierKeys::leftButtonModifier | ModifierKeys::rightButtonModifier | ModifierKeys::middleButtonModifier);
            updateModifiers (0);

            juce_currentMouseTrackingPeer->handleMouseUp (oldModifiers, x, y, getEventTime (0));
        }

        juce_currentMouseTrackingPeer = 0;
    }
}

bool juce_dispatchNextMessageOnSystemQueue (bool returnIfNoPendingMessages)
{
    if (juce_currentMouseTrackingPeer != 0)
        trackNextMouseEvent();

    EventRef theEvent;

    if (ReceiveNextEvent (0, 0, (returnIfNoPendingMessages) ? kEventDurationNoWait
                                                            : kEventDurationForever,
                          true, &theEvent) == noErr)
    {
        if (GetEventClass (theEvent) == kEventClassAppleEvent)
        {
            EventRecord eventRec;
            if (ConvertEventRefToEventRecord (theEvent, &eventRec))
                AEProcessAppleEvent (&eventRec);
        }
        else
        {
            EventTargetRef theTarget = GetEventDispatcherTarget();
            SendEventToEventTarget (theEvent, theTarget);
        }

        ReleaseEvent (theEvent);
        return true;
    }

    return false;
}

//==============================================================================
ComponentPeer* Component::createNewPeer (int styleFlags, void* windowToAttachTo)
{
    return new HIViewComponentPeer (this, styleFlags, (HIViewRef) windowToAttachTo);
}

//==============================================================================
void MouseCheckTimer::timerCallback()
{
    if (ModifierKeys::getCurrentModifiersRealtime().isAnyMouseButtonDown())
        return;

    if (Process::isForegroundProcess())
    {
        bool stillOver = false;
        int x = 0, y = 0, w = 0, h = 0;
        int mx = 0, my = 0;
        const bool validWindow = ComponentPeer::isValidPeer (lastPeerUnderMouse);

        if (validWindow)
        {
            lastPeerUnderMouse->getBounds (x, y, w, h, true);
            Desktop::getMousePosition (mx, my);

            stillOver = (mx >= x && my >= y && mx < x + w && my < y + h);

            if (stillOver)
            {
                // check if it's over an embedded HIView
                int rx = mx, ry = my;
                lastPeerUnderMouse->globalPositionToRelative (rx, ry);
                HIPoint hipoint;
                hipoint.x = rx;
                hipoint.y = ry;

                HIViewRef root;
                GetRootControl ((WindowRef) lastPeerUnderMouse->getNativeHandle(), &root);

                HIViewRef hitview;
                if (HIViewGetSubviewHit (root, &hipoint, true, &hitview) == noErr && hitview != 0)
                {
                    stillOver = HIObjectIsOfClass ((HIObjectRef) hitview, juceHiViewClassNameCFString);
                }
            }
        }

        if (! stillOver)
        {
            // mouse is outside our windows so set a normal cursor (only
            // if we're running as an app, not a plugin)
            if (JUCEApplication::getInstance() != 0)
                SetThemeCursor (kThemeArrowCursor);

            if (validWindow)
                lastPeerUnderMouse->handleMouseExit (mx - x, my - y, Time::currentTimeMillis());

            if (hasEverHadAMouseMove)
                stopTimer();
        }

        if ((! hasEverHadAMouseMove) && validWindow
             && (mx != lastX || my != lastY))
        {
            lastX = mx;
            lastY = my;

            if (stillOver)
                lastPeerUnderMouse->handleMouseMove (mx - x, my - y, Time::currentTimeMillis());
        }
    }
}

//==============================================================================
// called from juce_Messaging.cpp
void juce_HandleProcessFocusChange()
{
    keysCurrentlyDown.clear();

    if (HIViewComponentPeer::isValidPeer (currentlyFocusedPeer))
    {
        if (Process::isForegroundProcess())
            currentlyFocusedPeer->handleFocusGain();
        else
            currentlyFocusedPeer->handleFocusLoss();
    }
}

static bool performDrag (DragRef drag)
{
    EventRecord event;
    event.what = mouseDown;
    event.message = 0;
    event.when = TickCount();

    int x, y;
    Desktop::getMousePosition (x, y);
    event.where.h = x;
    event.where.v = y;

    event.modifiers = GetCurrentKeyModifiers();

    RgnHandle rgn = NewRgn();
    RgnHandle rgn2 = NewRgn();
    SetRectRgn (rgn,
                event.where.h - 8, event.where.v - 8,
                event.where.h + 8, event.where.v + 8);
    CopyRgn (rgn, rgn2);
    InsetRgn (rgn2, 1, 1);
    DiffRgn (rgn, rgn2, rgn);
    DisposeRgn (rgn2);

    bool result = TrackDrag (drag, &event, rgn) == noErr;

    DisposeRgn (rgn);
    return result;
}

bool DragAndDropContainer::performExternalDragDropOfFiles (const StringArray& files, const bool canMoveFiles)
{
    for (int i = ComponentPeer::getNumPeers(); --i >= 0;)
        ComponentPeer::getPeer (i)->performAnyPendingRepaintsNow();

    DragRef drag;
    bool result = false;

    if (NewDrag (&drag) == noErr)
    {
        for (int i = 0; i < files.size(); ++i)
        {
            HFSFlavor hfsData;

            if (PlatformUtilities::makeFSSpecFromPath (&hfsData.fileSpec, files[i]))
            {
                FInfo info;
                if (FSpGetFInfo (&hfsData.fileSpec, &info) == noErr)
                {
                    hfsData.fileType = info.fdType;
                    hfsData.fileCreator = info.fdCreator;
                    hfsData.fdFlags = info.fdFlags;

                    AddDragItemFlavor (drag, i + 1, kDragFlavorTypeHFS, &hfsData, sizeof (hfsData), 0);
                    result = true;
                }
            }
        }

        SetDragAllowableActions (drag, canMoveFiles ? kDragActionAll
                                                    : kDragActionCopy, false);

        if (result)
            result = performDrag (drag);

        DisposeDrag (drag);
    }

    return result;
}

bool DragAndDropContainer::performExternalDragDropOfText (const String& text)
{
    jassertfalse    // not implemented!
    return false;
}


//==============================================================================
bool Process::isForegroundProcess() throw()
{
    ProcessSerialNumber psn, front;
    GetCurrentProcess (&psn);
    GetFrontProcess (&front);

    Boolean b;
    return (SameProcess (&psn, &front, &b) == noErr) && b;
}

//==============================================================================
bool Desktop::canUseSemiTransparentWindows()
{
    return true;
}


//==============================================================================
void Desktop::getMousePosition (int& x, int& y)
{
    CGrafPtr currentPort;
    GetPort (&currentPort);

    if (! IsValidPort (currentPort))
    {
        WindowRef front = FrontWindow();

        if (front != 0)
        {
            SetPortWindowPort (front);
        }
        else
        {
            x = y = 0;
            return;
        }
    }

    ::Point p;
    GetMouse (&p);
    LocalToGlobal (&p);
    x = p.h;
    y = p.v;

    SetPort (currentPort);
}

void Desktop::setMousePosition (int x, int y)
{
    CGDirectDisplayID mainDisplayID = CGMainDisplayID();
    CGPoint pos = { x, y };
    CGDisplayMoveCursorToPoint (mainDisplayID, pos);
}

const ModifierKeys ModifierKeys::getCurrentModifiersRealtime()
{
    return ModifierKeys (currentModifiers);
}

//==============================================================================
void juce_updateMultiMonitorInfo (Array <Rectangle>& monitorCoords, const bool clipToWorkArea)
{
    int mainMon = 0;
    int distFrom00 = 0x7fffff;

    GDHandle h = DMGetFirstScreenDevice (true);

    while (h != 0)
    {
        Rect rect;

        GetAvailableWindowPositioningBounds (h, &rect);

        const int dist = abs (rect.left) + abs (rect.top);
        if (distFrom00 > dist)
        {
            distFrom00 = dist;
            mainMon = monitorCoords.size();
        }

        monitorCoords.add (Rectangle (rect.left,
                                      rect.top,
                                      rect.right - rect.left,
                                      rect.bottom - rect.top));

        h = DMGetNextScreenDevice (h, true);
    }

    // make sure the first in the list is the main monitor
    if (mainMon > 0)
        monitorCoords.swap (mainMon, 0);

    if (monitorCoords.size() == 0)
    {
        BitMap screenBits;
        Rect r = GetQDGlobalsScreenBits (&screenBits)->bounds;

        monitorCoords.add (Rectangle (r.left,
                                      r.top + GetMBarHeight(),
                                      r.right - r.left,
                                      r.bottom - r.top));
    }

    //xxx need to register for display change callbacks
}

//==============================================================================
struct CursorWrapper
{
    Cursor* cursor;
    ThemeCursor themeCursor;
};

void* juce_createMouseCursorFromImage (const Image& image, int hotspotX, int hotspotY) throw()
{
    const int maxW = 16;
    const int maxH = 16;

    const Image* im = &image;
    Image* newIm = 0;

    if (image.getWidth() > maxW || image.getHeight() > maxH)
    {
        im = newIm = image.createCopy (maxW, maxH);

        hotspotX = (hotspotX * maxW) / image.getWidth();
        hotspotY = (hotspotY * maxH) / image.getHeight();
    }

    Cursor* c = new Cursor();
    c->hotSpot.h = hotspotX;
    c->hotSpot.v = hotspotY;

    for (int y = 0; y < maxH; ++y)
    {
        c->data[y] = 0;
        c->mask[y] = 0;

        for (int x = 0; x < maxW; ++x)
        {
            const Colour pixelColour (im->getPixelAt (15 - x, y));

            if (pixelColour.getAlpha() > 0.5f)
            {
                c->mask[y] |= (1 << x);

                if (pixelColour.getBrightness() < 0.5f)
                    c->data[y] |= (1 << x);
            }
        }

        c->data[y] = CFSwapInt16BigToHost (c->data[y]);
        c->mask[y] = CFSwapInt16BigToHost (c->mask[y]);
    }

    if (newIm != 0)
        delete newIm;

    CursorWrapper* cw = new CursorWrapper();
    cw->cursor = c;
    cw->themeCursor = kThemeArrowCursor;
    return (void*)cw;
}

static void* cursorFromData (const unsigned char* data, const int size, int hx, int hy)
{
    Image* const im = ImageFileFormat::loadFrom ((const char*) data, size);
    jassert (im != 0);
    void* curs = juce_createMouseCursorFromImage (*im, hx, hy);
    delete im;
    return curs;
}

const unsigned int kSpecialNoCursor = 'nocr';

void* juce_createStandardMouseCursor (MouseCursor::StandardCursorType type) throw()
{
    ThemeCursor id = kThemeArrowCursor;

    switch (type)
    {
    case MouseCursor::NormalCursor:
        id = kThemeArrowCursor;
        break;

    case MouseCursor::NoCursor:
        id = kSpecialNoCursor;
        break;

    case MouseCursor::DraggingHandCursor:
        {
            static const unsigned char cursData[] = {71,73,70,56,57,97,16,0,16,0,145,2,0,0,0,0,255,255,255,0,
              0,0,0,0,0,33,249,4,1,0,0,2,0,44,0,0,0,0,16,0,
              16,0,0,2,52,148,47,0,200,185,16,130,90,12,74,139,107,84,123,39,
              132,117,151,116,132,146,248,60,209,138,98,22,203,114,34,236,37,52,77,217,
              247,154,191,119,110,240,193,128,193,95,163,56,60,234,98,135,2,0,59 };
            const int cursDataSize = 99;

            return cursorFromData (cursData, cursDataSize, 8, 8);
        }
        break;

    case MouseCursor::CopyingCursor:
        id = kThemeCopyArrowCursor;
        break;

    case MouseCursor::WaitCursor:
        id = kThemeWatchCursor;
        break;

    case MouseCursor::IBeamCursor:
        id = kThemeIBeamCursor;
        break;

    case MouseCursor::PointingHandCursor:
        id = kThemePointingHandCursor;
        break;

    case MouseCursor::LeftRightResizeCursor:
    case MouseCursor::LeftEdgeResizeCursor:
    case MouseCursor::RightEdgeResizeCursor:
        {
            static const unsigned char cursData[] = {71,73,70,56,57,97,16,0,16,0,145,0,0,255,255,255,0,0,0,255,
              255,255,0,0,0,33,249,4,1,0,0,2,0,44,0,0,0,0,16,0,
              16,0,0,2,38,148,143,169,203,237,15,19,0,106,202,64,111,22,32,224,
              9,78,30,213,121,230,121,146,99,8,142,71,183,189,152,20,27,86,132,231,
              58,83,0,0,59 };
            const int cursDataSize = 85;

            return cursorFromData (cursData, cursDataSize, 8, 8);
        }

    case MouseCursor::UpDownResizeCursor:
    case MouseCursor::TopEdgeResizeCursor:
    case MouseCursor::BottomEdgeResizeCursor:
        {
            static const unsigned char cursData[] = {71,73,70,56,57,97,16,0,16,0,145,0,0,255,255,255,0,0,0,255,
              255,255,0,0,0,33,249,4,1,0,0,2,0,44,0,0,0,0,16,0,
              16,0,0,2,38,148,111,128,187,16,202,90,152,48,10,55,169,189,192,245,
              106,121,27,34,142,201,99,158,224,86,154,109,216,61,29,155,105,180,61,190,
              121,84,0,0,59 };
            const int cursDataSize = 85;

            return cursorFromData (cursData, cursDataSize, 8, 8);
        }

    case MouseCursor::TopLeftCornerResizeCursor:
    case MouseCursor::BottomRightCornerResizeCursor:
        {
            static const unsigned char cursData[] = {71,73,70,56,57,97,16,0,16,0,145,0,0,255,255,255,0,0,0,255,
                255,255,0,0,0,33,249,4,1,0,0,2,0,44,0,0,0,0,16,0,
                16,0,0,2,43,132,15,162,187,16,255,18,99,14,202,217,44,158,213,221,
                237,9,225,38,94,35,73,5,31,42,170,108,106,174,112,43,195,209,91,185,
                104,174,131,208,77,66,28,10,0,59 };
            const int cursDataSize = 90;

            return cursorFromData (cursData, cursDataSize, 8, 8);
        }

    case MouseCursor::TopRightCornerResizeCursor:
    case MouseCursor::BottomLeftCornerResizeCursor:
        {
            static const unsigned char cursData[] = {71,73,70,56,57,97,16,0,16,0,145,0,0,255,255,255,0,0,0,255,
                255,255,0,0,0,33,249,4,1,0,0,2,0,44,0,0,0,0,16,0,
                16,0,0,2,45,148,127,160,11,232,16,98,108,14,65,73,107,194,122,223,
                92,65,141,216,145,134,162,153,221,25,128,73,166,62,173,16,203,237,188,94,
                120,46,237,105,239,123,48,80,157,2,0,59 };
            const int cursDataSize = 92;

            return cursorFromData (cursData, cursDataSize, 8, 8);
        }

    case MouseCursor::UpDownLeftRightResizeCursor:
        {
            static const unsigned char cursData[] = {71,73,70,56,57,97,15,0,15,0,145,0,0,0,0,0,255,255,255,0,
                128,128,255,255,255,33,249,4,1,0,0,3,0,44,0,0,0,0,15,0,
                15,0,0,2,46,156,63,129,139,1,202,26,152,48,186,73,109,114,65,85,
                195,37,143,88,93,29,215,101,23,198,178,30,149,158,25,56,134,97,179,61,
                158,213,126,203,234,99,220,34,56,70,1,0,59,0,0 };
            const int cursDataSize = 93;

            return cursorFromData (cursData, cursDataSize, 7, 7);
        }

    case MouseCursor::CrosshairCursor:
        id = kThemeCrossCursor;
        break;
    }

    CursorWrapper* cw = new CursorWrapper();
    cw->cursor = 0;
    cw->themeCursor = id;

    return (void*) cw;
}

void juce_deleteMouseCursor (void* const cursorHandle, const bool isStandard) throw()
{
    CursorWrapper* const cw = (CursorWrapper*) cursorHandle;

    if (cw != 0)
    {
        delete cw->cursor;
        delete cw;
    }
}

void MouseCursor::showInAllWindows() const
{
    showInWindow (0);
}

void MouseCursor::showInWindow (ComponentPeer*) const
{
    const CursorWrapper* const cw = (CursorWrapper*) getHandle();

    if (cw != 0)
    {
        static bool isCursorHidden = false;
        static bool showingWaitCursor = false;
        const bool shouldShowWaitCursor = (cw->themeCursor == kThemeWatchCursor);
        const bool shouldHideCursor = (cw->themeCursor == kSpecialNoCursor);

        if (shouldShowWaitCursor != showingWaitCursor
             && Process::isForegroundProcess())
        {
            showingWaitCursor = shouldShowWaitCursor;
            QDDisplayWaitCursor (shouldShowWaitCursor);
        }

        if (shouldHideCursor != isCursorHidden)
        {
            isCursorHidden = shouldHideCursor;

            if (shouldHideCursor)
                HideCursor();
            else
                ShowCursor();
        }

        if (cw->cursor != 0)
            SetCursor (cw->cursor);
        else if (! (shouldShowWaitCursor || shouldHideCursor))
            SetThemeCursor (cw->themeCursor);
    }
}

//==============================================================================
Image* juce_createIconForFile (const File& file)
{
    return 0;
}


//==============================================================================
class MainMenuHandler   : private MenuBarModelListener,
                          private DeletedAtShutdown
{
public:
    MainMenuHandler() throw()
        : currentModel (0)
    {
    }

    ~MainMenuHandler() throw()
    {
        setMenu (0);
    }

    void setMenu (MenuBarModel* const newMenuBarModel) throw()
    {
        if (currentModel != newMenuBarModel)
        {
            if (currentModel != 0)
                currentModel->removeListener (this);

            currentModel = newMenuBarModel;

            if (currentModel != 0)
                currentModel->addListener (this);

            menuBarItemsChanged (0);
        }
    }

    void menuBarItemsChanged (MenuBarModel*)
    {
        ClearMenuBar();

        if (currentModel != 0)
        {
            int id = 1000;
            const StringArray menuNames (currentModel->getMenuBarNames());

            for (int i = 0; i < menuNames.size(); ++i)
            {
                const PopupMenu menu (currentModel->getMenuForIndex (i, menuNames [i]));

                MenuRef m = createMenu (menu, menuNames [i], id);

                InsertMenu (m, 0);
                CFRelease (m);
            }
        }
    }

    void menuCommandInvoked (MenuBarModel*, const ApplicationCommandTarget::InvocationInfo& info)
    {
        MenuRef menu = 0;
        MenuItemIndex index = 0;
        GetIndMenuItemWithCommandID (0, info.commandID, 1, &menu, &index);

        FlashMenuBar (GetMenuID (menu));
        FlashMenuBar (GetMenuID (menu));
    }

    void invoke (const int id, ApplicationCommandManager* commandManager, const int topLevelIndex) const
    {
        if (currentModel != 0)
        {
            if (commandManager != 0)
            {
                ApplicationCommandTarget::InvocationInfo info (id);
                info.invocationMethod = ApplicationCommandTarget::InvocationInfo::fromMenu;

                commandManager->invoke (info, true);
            }

            currentModel->menuItemSelected (id, topLevelIndex);
        }
    }

    MenuBarModel* currentModel;

private:
    static MenuRef createMenu (const PopupMenu menu,
                               const String& menuName,
                               int& id)
    {
        MenuRef m = 0;

        if (CreateNewMenu (id++, kMenuAttrAutoDisable, &m) == noErr)
        {
            CFStringRef name = PlatformUtilities::juceStringToCFString (menuName);
            SetMenuTitleWithCFString (m, name);
            CFRelease (name);

            PopupMenu::MenuItemIterator iter (menu);
            int topLevelIndex = 0;

            while (iter.next())
            {
                MenuItemIndex index = 0;

                int flags = kMenuAttrAutoDisable  | kMenuItemAttrIgnoreMeta | kMenuItemAttrNotPreviousAlternate;
                if (! iter.isEnabled)
                    flags |= kMenuItemAttrDisabled;

                CFStringRef text = PlatformUtilities::juceStringToCFString (iter.itemName.upToFirstOccurrenceOf (T("<end>"), false, true));

                if (iter.isSeparator)
                {
                    AppendMenuItemTextWithCFString (m, text, kMenuItemAttrSeparator, 0, &index);
                }
                else if (iter.isSectionHeader)
                {
                    AppendMenuItemTextWithCFString (m, text, kMenuItemAttrSectionHeader, 0, &index);
                }
                else if (iter.subMenu != 0)
                {
                    AppendMenuItemTextWithCFString (m, text, flags, id++, &index);

                    MenuRef sub = createMenu (*iter.subMenu, iter.itemName, id);
                    SetMenuItemHierarchicalMenu (m, index, sub);
                    CFRelease (sub);
                }
                else
                {
                    AppendMenuItemTextWithCFString (m, text, flags, iter.itemId, &index);

                    if (iter.isTicked)
                        CheckMenuItem (m, index, true);

                    SetMenuItemProperty (m, index, 'juce', 'apcm', sizeof (void*), &iter.commandManager);
                    SetMenuItemProperty (m, index, 'juce', 'topi', sizeof (int), &topLevelIndex);

                    if (iter.commandManager != 0)
                    {
                        const Array <KeyPress> keyPresses (iter.commandManager->getKeyMappings()
                                                            ->getKeyPressesAssignedToCommand (iter.itemId));

                        if (keyPresses.size() > 0)
                        {
                            const KeyPress& kp = keyPresses.getUnchecked(0);
                            int mods = 0;

                            if (kp.getModifiers().isShiftDown())
                                mods |= kMenuShiftModifier;
                            if (kp.getModifiers().isCtrlDown())
                                mods |= kMenuControlModifier;
                            if (kp.getModifiers().isAltDown())
                                mods |= kMenuOptionModifier;
                            if (! kp.getModifiers().isCommandDown())
                                mods |= kMenuNoCommandModifier;

                            tchar keyCode = (tchar) kp.getKeyCode();

                            if (kp.getKeyCode() >= KeyPress::numberPad0
                                && kp.getKeyCode() <= KeyPress::numberPad9)
                            {
                                keyCode = (tchar) ((T('0') - KeyPress::numberPad0) + kp.getKeyCode());
                            }

                            if (CharacterFunctions::isLetterOrDigit (keyCode)
                                 || CharacterFunctions::indexOfChar (T(",.;/\\'[]=-+_<>?{}\":"), keyCode, false) >= 0)
                            {
                                SetMenuItemModifiers (m, index, mods);
                                SetMenuItemCommandKey (m, index, false, CharacterFunctions::toUpperCase (keyCode));
                            }
                            else
                            {
                                const SInt16 glyph = getGlyphForKeyCode (kp.getKeyCode());

                                if (glyph != 0)
                                {
                                    SetMenuItemModifiers (m, index, mods);
                                    SetMenuItemKeyGlyph (m, index, glyph);
                                }
                            }

                            // if we set the key glyph to be a text char, and enable virtual
                            // key triggering, it stops the menu automatically triggering the callback
                            ChangeMenuItemAttributes (m, index, kMenuItemAttrUseVirtualKey, 0);
                        }
                    }
                }

                CFRelease (text);
                ++topLevelIndex;
            }
        }

        return m;
    }

    static SInt16 getGlyphForKeyCode (const int keyCode) throw()
    {
        if (keyCode == KeyPress::spaceKey)
            return kMenuSpaceGlyph;
        else if (keyCode == KeyPress::returnKey)
            return kMenuReturnGlyph;
        else if (keyCode == KeyPress::escapeKey)
            return kMenuEscapeGlyph;
        else if (keyCode == KeyPress::backspaceKey)
            return kMenuDeleteLeftGlyph;
        else if (keyCode == KeyPress::leftKey)
            return kMenuLeftArrowGlyph;
        else if (keyCode == KeyPress::rightKey)
            return kMenuRightArrowGlyph;
        else if (keyCode == KeyPress::upKey)
            return kMenuUpArrowGlyph;
        else if (keyCode == KeyPress::downKey)
            return kMenuDownArrowGlyph;
        else if (keyCode == KeyPress::pageUpKey)
            return kMenuPageUpGlyph;
        else if (keyCode == KeyPress::pageDownKey)
            return kMenuPageDownGlyph;
        else if (keyCode == KeyPress::endKey)
            return kMenuSoutheastArrowGlyph;
        else if (keyCode == KeyPress::homeKey)
            return kMenuNorthwestArrowGlyph;
        else if (keyCode == KeyPress::deleteKey)
            return kMenuDeleteRightGlyph;
        else if (keyCode == KeyPress::tabKey)
            return kMenuTabRightGlyph;
        else if (keyCode == KeyPress::F1Key)
            return kMenuF1Glyph;
        else if (keyCode == KeyPress::F2Key)
            return kMenuF2Glyph;
        else if (keyCode == KeyPress::F3Key)
            return kMenuF3Glyph;
        else if (keyCode == KeyPress::F4Key)
            return kMenuF4Glyph;
        else if (keyCode == KeyPress::F5Key)
            return kMenuF5Glyph;
        else if (keyCode == KeyPress::F6Key)
            return kMenuF6Glyph;
        else if (keyCode == KeyPress::F7Key)
            return kMenuF7Glyph;
        else if (keyCode == KeyPress::F8Key)
            return kMenuF8Glyph;
        else if (keyCode == KeyPress::F9Key)
            return kMenuF9Glyph;
        else if (keyCode == KeyPress::F10Key)
            return kMenuF10Glyph;
        else if (keyCode == KeyPress::F11Key)
            return kMenuF11Glyph;
        else if (keyCode == KeyPress::F12Key)
            return kMenuF12Glyph;
        else if (keyCode == KeyPress::F13Key)
            return kMenuF13Glyph;
        else if (keyCode == KeyPress::F14Key)
            return kMenuF14Glyph;
        else if (keyCode == KeyPress::F15Key)
            return kMenuF15Glyph;

        return 0;
    }
};

static MainMenuHandler* mainMenu = 0;

void MenuBarModel::setMacMainMenu (MenuBarModel* newMenuBarModel) throw()
{
    if (getMacMainMenu() != newMenuBarModel)
    {
        if (newMenuBarModel == 0)
        {
            deleteAndZero (mainMenu);
        }
        else
        {
            if (mainMenu == 0)
                mainMenu = new MainMenuHandler();

            mainMenu->setMenu (newMenuBarModel);
        }
    }
}

MenuBarModel* MenuBarModel::getMacMainMenu() throw()
{
    return mainMenu != 0 ? mainMenu->currentModel : 0;
}

// these functions are called externally from the message handling code
void juce_MainMenuAboutToBeUsed()
{
    // force an update of the items just before the menu appears..
    if (mainMenu != 0)
        mainMenu->menuBarItemsChanged (0);
}

void juce_InvokeMainMenuCommand (const HICommand& command)
{
    if (mainMenu != 0)
    {
        ApplicationCommandManager* commandManager = 0;
        int topLevelIndex = 0;

        if (GetMenuItemProperty (command.menu.menuRef, command.menu.menuItemIndex,
                                 'juce', 'apcm', sizeof (commandManager), 0, &commandManager) == noErr
             && GetMenuItemProperty (command.menu.menuRef, command.menu.menuItemIndex,
                                     'juce', 'topi', sizeof (topLevelIndex), 0, &topLevelIndex) == noErr)
        {
            mainMenu->invoke (command.commandID, commandManager, topLevelIndex);
        }
    }
}

//==============================================================================
void PlatformUtilities::beep()
{
    SysBeep (30);
}

//==============================================================================
void SystemClipboard::copyTextToClipboard (const String& text)
{
    ClearCurrentScrap();
    ScrapRef ref;
    GetCurrentScrap (&ref);

    const int len = text.length();
    const int numBytes = sizeof (UniChar) * len;
    UniChar* const temp = (UniChar*) juce_calloc (numBytes);

    for (int i = 0; i < len; ++i)
        temp[i] = (UniChar) text[i];

    PutScrapFlavor (ref,
                    kScrapFlavorTypeUnicode,
                    kScrapFlavorMaskNone,
                    numBytes,
                    temp);

    juce_free (temp);
}

const String SystemClipboard::getTextFromClipboard()
{
    String result;

    ScrapRef ref;
    GetCurrentScrap (&ref);
    Size size = 0;

    if (GetScrapFlavorSize (ref, kScrapFlavorTypeUnicode, &size) == noErr
         && size > 0)
    {
        void* const data = juce_calloc (size + 8);

        if (GetScrapFlavorData (ref, kScrapFlavorTypeUnicode, &size, data) == noErr)
        {
            result = PlatformUtilities::convertUTF16ToString ((UniChar*) data);
        }

        juce_free (data);
    }

    return result;
}


//==============================================================================
bool AlertWindow::showNativeDialogBox (const String& title,
                                       const String& bodyText,
                                       bool isOkCancel)
{
    Str255 tit, txt;
    PlatformUtilities::copyToStr255 (tit, title);
    PlatformUtilities::copyToStr255 (txt, bodyText);

    AlertStdAlertParamRec ar;
    ar.movable = true;
    ar.helpButton = false;
    ar.filterProc = 0;
    ar.defaultText = (const unsigned char*)-1;
    ar.cancelText = (const unsigned char*)((isOkCancel) ? -1 : 0);
    ar.otherText = 0;
    ar.defaultButton = kAlertStdAlertOKButton;
    ar.cancelButton = 0;
    ar.position = kWindowDefaultPosition;

    SInt16 result;
    StandardAlert (kAlertNoteAlert, tit, txt, &ar, &result);
    return result == kAlertStdAlertOKButton;
}

//==============================================================================
const int KeyPress::spaceKey        = ' ';
const int KeyPress::returnKey       = kReturnCharCode;
const int KeyPress::escapeKey       = kEscapeCharCode;
const int KeyPress::backspaceKey    = kBackspaceCharCode;
const int KeyPress::leftKey         = kLeftArrowCharCode;
const int KeyPress::rightKey        = kRightArrowCharCode;
const int KeyPress::upKey           = kUpArrowCharCode;
const int KeyPress::downKey         = kDownArrowCharCode;
const int KeyPress::pageUpKey       = kPageUpCharCode;
const int KeyPress::pageDownKey     = kPageDownCharCode;
const int KeyPress::endKey          = kEndCharCode;
const int KeyPress::homeKey         = kHomeCharCode;
const int KeyPress::deleteKey       = kDeleteCharCode;
const int KeyPress::insertKey       = -1;
const int KeyPress::tabKey          = kTabCharCode;
const int KeyPress::F1Key           = 0x10110;
const int KeyPress::F2Key           = 0x10111;
const int KeyPress::F3Key           = 0x10112;
const int KeyPress::F4Key           = 0x10113;
const int KeyPress::F5Key           = 0x10114;
const int KeyPress::F6Key           = 0x10115;
const int KeyPress::F7Key           = 0x10116;
const int KeyPress::F8Key           = 0x10117;
const int KeyPress::F9Key           = 0x10118;
const int KeyPress::F10Key          = 0x10119;
const int KeyPress::F11Key          = 0x1011a;
const int KeyPress::F12Key          = 0x1011b;
const int KeyPress::F13Key          = 0x1011c;
const int KeyPress::F14Key          = 0x1011d;
const int KeyPress::F15Key          = 0x1011e;
const int KeyPress::F16Key          = 0x1011f;
const int KeyPress::numberPad0      = 0x30020;
const int KeyPress::numberPad1      = 0x30021;
const int KeyPress::numberPad2      = 0x30022;
const int KeyPress::numberPad3      = 0x30023;
const int KeyPress::numberPad4      = 0x30024;
const int KeyPress::numberPad5      = 0x30025;
const int KeyPress::numberPad6      = 0x30026;
const int KeyPress::numberPad7      = 0x30027;
const int KeyPress::numberPad8      = 0x30028;
const int KeyPress::numberPad9      = 0x30029;
const int KeyPress::numberPadAdd            = 0x3002a;
const int KeyPress::numberPadSubtract       = 0x3002b;
const int KeyPress::numberPadMultiply       = 0x3002c;
const int KeyPress::numberPadDivide         = 0x3002d;
const int KeyPress::numberPadSeparator      = 0x3002e;
const int KeyPress::numberPadDecimalPoint   = 0x3002f;
const int KeyPress::playKey         = 0x30000;
const int KeyPress::stopKey         = 0x30001;
const int KeyPress::fastForwardKey  = 0x30002;
const int KeyPress::rewindKey       = 0x30003;

//==============================================================================
#if JUCE_OPENGL

struct OpenGLContextInfo
{
    AGLContext renderContext;
};

void* juce_createOpenGLContext (OpenGLComponent* component, void* sharedContext)
{
    jassert (component != 0);

    HIViewComponentPeer* const peer = dynamic_cast <HIViewComponentPeer*> (component->getTopLevelComponent()->getPeer());

    if (peer == 0)
        return 0;

    OpenGLContextInfo* const oc = new OpenGLContextInfo();

    GLint attrib[] = {  AGL_RGBA, AGL_DOUBLEBUFFER,
                        AGL_RED_SIZE, 8,
                        AGL_ALPHA_SIZE, 8,
                        AGL_DEPTH_SIZE, 24,
                        AGL_CLOSEST_POLICY, AGL_NO_RECOVERY,
                        AGL_SAMPLE_BUFFERS_ARB, 1,
                        AGL_SAMPLES_ARB, 4,
                        AGL_NONE };

    oc->renderContext = aglCreateContext (aglChoosePixelFormat (0, 0, attrib),
                                          (sharedContext != 0) ? ((OpenGLContextInfo*) sharedContext)->renderContext
                                                               : 0);

    aglSetDrawable (oc->renderContext,
                    GetWindowPort (peer->windowRef));

    return oc;
}

void juce_updateOpenGLWindowPos (void* context, Component* owner, Component* topComp)
{
    jassert (context != 0);
    OpenGLContextInfo* const oc = (OpenGLContextInfo*) context;

    GLint bufferRect[4];

    bufferRect[0] = owner->getScreenX() - topComp->getScreenX();
    bufferRect[1] = topComp->getHeight() - (owner->getHeight() + owner->getScreenY() - topComp->getScreenY());
    bufferRect[2] = owner->getWidth();
    bufferRect[3] = owner->getHeight();

    aglSetInteger (oc->renderContext, AGL_BUFFER_RECT, bufferRect);
    aglEnable (oc->renderContext, AGL_BUFFER_RECT);
}

void juce_deleteOpenGLContext (void* context)
{
    OpenGLContextInfo* const oc = (OpenGLContextInfo*) context;

    aglDestroyContext (oc->renderContext);

    delete oc;
}

bool juce_makeOpenGLContextCurrent (void* context)
{
    OpenGLContextInfo* const oc = (OpenGLContextInfo*) context;

    return aglSetCurrentContext ((oc != 0) ? oc->renderContext : 0);
}

void juce_swapOpenGLBuffers (void* context)
{
    OpenGLContextInfo* const oc = (OpenGLContextInfo*) context;

    if (oc != 0)
        aglSwapBuffers (oc->renderContext);
}

void juce_repaintOpenGLWindow (void* context)
{
}

#endif

END_JUCE_NAMESPACE
