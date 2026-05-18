#!/usr/bin/env python3
from pathlib import Path
import sys


INPUT_MANAGER_HPP_NEEDLE = """    void sectionNext();
    void sectionPrev();
    void sectionMenu();
    /* Program */
"""

INPUT_MANAGER_HPP_REPLACEMENT = """    void sectionNext();
    void sectionPrev();
    void sectionMenu();
    void sectionPopupMenu();
    /* Program */
"""

INPUT_MANAGER_CPP_NEEDLE = """void InputManager::sectionMenu()
{
    if( hasInput() )
    {
        var_TriggerCallback( p_input, "menu-title" );
    }
}

/*
 *  Teletext Functions
 */
"""

INPUT_MANAGER_CPP_REPLACEMENT = """void InputManager::sectionMenu()
{
    if( hasInput() )
    {
        var_TriggerCallback( p_input, "menu-title" );
    }
}

void InputManager::sectionPopupMenu()
{
    if( hasInput() && var_Type( p_input, "menu-popup" ) != 0 )
    {
        var_TriggerCallback( p_input, "menu-popup" );
    }
}

/*
 *  Teletext Functions
 */
"""

MENUS_CPP_NEEDLE = """QMenu *VLCMenuBar::NavigMenu( intf_thread_t *p_intf, QMenu *menu )
{
    QAction *action;
    QMenu *submenu;

    addActionWithSubmenu( menu, "title", qtr( "T&itle" ) );
    submenu = addActionWithSubmenu( menu, "chapter", qtr( "&Chapter" ) );
    submenu->setTearOffEnabled( true );
    addActionWithSubmenu( menu, "program", qtr( "&Program" ) );

    submenu = new QMenu( qtr( I_MENU_BOOKMARK ), menu );
"""

MENUS_CPP_REPLACEMENT = """QMenu *VLCMenuBar::NavigMenu( intf_thread_t *p_intf, QMenu *menu )
{
    QAction *action;
    QMenu *submenu;
    input_thread_t *p_input = THEMIM->getInput();
    bool b_has_popup = p_input != NULL && var_Type( p_input, "menu-popup" ) != 0;
    bool b_popup_available = p_input != NULL &&
                             var_Type( p_input, "menu-popup-available" ) != 0 &&
                             var_GetBool( p_input, "menu-popup-available" );

    addActionWithSubmenu( menu, "title", qtr( "T&itle" ) );
    submenu = addActionWithSubmenu( menu, "chapter", qtr( "&Chapter" ) );
    submenu->setTearOffEnabled( true );
    addActionWithSubmenu( menu, "program", qtr( "&Program" ) );

    action = menu->addAction( qtr( "Popup &Menu" ), THEMIM->getIM(),
                              SLOT( sectionPopupMenu() ) );
    action->setObjectName( "popupMenuAction" );
    action->setData( ACTION_STATIC );
    if( !b_has_popup || !b_popup_available )
        action->setEnabled( false );
    msg_Dbg( p_intf,
             "open3d qt popup action phase=create enabled=%d has_input=%d has_menu_popup=%d popup_available=%d",
             action->isEnabled(), p_input != NULL, b_has_popup, b_popup_available );

    submenu = new QMenu( qtr( I_MENU_BOOKMARK ), menu );
"""

REBUILD_NAV_NEEDLE = """    /* */
    EnableStaticEntries( menu, (p_object != NULL ) );
    Populate( p_intf, menu, varnames, objects );

    /* Remove playback actions to recreate them */
"""

REBUILD_NAV_REPLACEMENT = """    /* */
    EnableStaticEntries( menu, (p_object != NULL ) );
    QList<QAction*> actions = menu->actions();
    for( int i = 0; i < actions.count(); ++i )
    {
        if( actions[i]->objectName() == QLatin1String( "popupMenuAction" ) )
        {
            bool b_has_popup = p_object != NULL &&
                               var_Type( p_object, "menu-popup" ) != 0;
            bool b_popup_available = p_object != NULL &&
                                     var_Type( p_object, "menu-popup-available" ) != 0 &&
                                     var_GetBool( p_object, "menu-popup-available" );
            bool b_enabled = b_has_popup && b_popup_available;
            actions[i]->setEnabled( b_enabled );
            msg_Dbg( p_intf,
                     "open3d qt popup action phase=rebuild enabled=%d has_input=%d has_menu_popup=%d popup_available=%d",
                     b_enabled, p_object != NULL, b_has_popup, b_popup_available );
            break;
        }
    }
    Populate( p_intf, menu, varnames, objects );

    /* Remove playback actions to recreate them */
"""

MENUS_CPP_PREVIOUS_PATCH = """QMenu *VLCMenuBar::NavigMenu( intf_thread_t *p_intf, QMenu *menu )
{
    QAction *action;
    QMenu *submenu;
    input_thread_t *p_input = THEMIM->getInput();

    addActionWithSubmenu( menu, "title", qtr( "T&itle" ) );
    submenu = addActionWithSubmenu( menu, "chapter", qtr( "&Chapter" ) );
    submenu->setTearOffEnabled( true );
    addActionWithSubmenu( menu, "program", qtr( "&Program" ) );

    action = menu->addAction( qtr( "Popup &Menu" ), THEMIM->getIM(),
                              SLOT( sectionPopupMenu() ) );
    action->setObjectName( "popupMenuAction" );
    action->setData( ACTION_STATIC );
    if( p_input == NULL || var_Type( p_input, "menu-popup" ) == 0 ||
        var_Type( p_input, "menu-popup-available" ) == 0 ||
        !var_GetBool( p_input, "menu-popup-available" ) )
        action->setEnabled( false );

    submenu = new QMenu( qtr( I_MENU_BOOKMARK ), menu );
"""

REBUILD_NAV_PREVIOUS_PATCH = """    /* */
    EnableStaticEntries( menu, (p_object != NULL ) );
    QList<QAction*> actions = menu->actions();
    for( int i = 0; i < actions.count(); ++i )
    {
        if( actions[i]->objectName() == QLatin1String( "popupMenuAction" ) )
        {
            actions[i]->setEnabled(
                p_object != NULL &&
                var_Type( p_object, "menu-popup" ) != 0 &&
                var_Type( p_object, "menu-popup-available" ) != 0 &&
                var_GetBool( p_object, "menu-popup-available" )
            );
            break;
        }
    }
    Populate( p_intf, menu, varnames, objects );

    /* Remove playback actions to recreate them */
"""

MAIN_INTERFACE_DISPLAY_NORMAL_NEEDLE = """void MainInterface::displayNormalView()
{
    menuBar()->setVisible( false );
    controls->setVisible( false );
#ifndef QT_NO_STATUSBAR
    statusBar()->setVisible( false );
#endif
    inputC->setVisible( false );
}
"""

MAIN_INTERFACE_DISPLAY_NORMAL_REPLACEMENT = """void MainInterface::displayNormalView()
{
    msg_Dbg( p_intf,
             "open3d qt view displayNormalView before menu=%d controls=%d input=%d",
             menuBar()->isVisible(), controls->isVisible(), inputC->isVisible() );
    menuBar()->setVisible( false );
    controls->setVisible( false );
#ifndef QT_NO_STATUSBAR
    statusBar()->setVisible( false );
#endif
    inputC->setVisible( false );
    msg_Dbg( p_intf,
             "open3d qt view displayNormalView after menu=%d controls=%d input=%d",
             menuBar()->isVisible(), controls->isVisible(), inputC->isVisible() );
}
"""

MAIN_INTERFACE_SET_MINIMAL_LOCALS_NEEDLE = """void MainInterface::setMinimalView( bool b_minimal )
{
    bool b_menuBarVisible = menuBar()->isVisible();
    bool b_controlsVisible = controls->isVisible();
#ifndef QT_NO_STATUSBAR
    bool b_statusBarVisible = statusBar()->isVisible();
#endif
    bool b_inputCVisible = inputC->isVisible();
"""

MAIN_INTERFACE_SET_MINIMAL_LOCALS_REPLACEMENT = """void MainInterface::setMinimalView( bool b_minimal )
{
    bool b_menuBarVisible = menuBar()->isVisible();
    bool b_controlsVisible = controls->isVisible();
#ifndef QT_NO_STATUSBAR
    bool b_statusBarVisible = statusBar()->isVisible();
#endif
    bool b_inputCVisible = inputC->isVisible();
    msg_Dbg( p_intf,
             "open3d qt view setMinimalView begin requested=%d menu_before=%d controls_before=%d input_before=%d fullscreen=%d video_fullscreen=%d",
             b_minimal, b_menuBarVisible, b_controlsVisible, b_inputCVisible,
             isFullScreen(), b_videoFullScreen );
"""

MAIN_INTERFACE_SET_MINIMAL_APPLY_NEEDLE = """    menuBar()->setVisible( !b_minimal );
    controls->setVisible( !b_minimal );
#ifndef QT_NO_STATUSBAR
    statusBar()->setVisible( !b_minimal && b_statusbarVisible );
#endif
    inputC->setVisible( !b_minimal );
"""

MAIN_INTERFACE_SET_MINIMAL_APPLY_REPLACEMENT = """    menuBar()->setVisible( !b_minimal );
    controls->setVisible( !b_minimal );
#ifndef QT_NO_STATUSBAR
    statusBar()->setVisible( !b_minimal && b_statusbarVisible );
#endif
    inputC->setVisible( !b_minimal );
    msg_Dbg( p_intf,
             "open3d qt view setMinimalView applied requested=%d menu_after=%d controls_after=%d input_after=%d",
             b_minimal, menuBar()->isVisible(), controls->isVisible(),
             inputC->isVisible() );
"""

MAIN_INTERFACE_TOGGLE_MINIMAL_START_NEEDLE = """void MainInterface::toggleMinimalView( bool b_minimal )
{
    if( !b_minimalView && b_autoresize ) /* Normal mode */
"""

MAIN_INTERFACE_TOGGLE_MINIMAL_START_REPLACEMENT = """void MainInterface::toggleMinimalView( bool b_minimal )
{
    msg_Dbg( p_intf,
             "open3d qt view toggleMinimalView begin requested=%d current=%d menu=%d controls=%d input=%d video_fullscreen=%d",
             b_minimal, b_minimalView, menuBar()->isVisible(), controls->isVisible(),
             inputC->isVisible(), b_videoFullScreen );
    if( !b_minimalView && b_autoresize ) /* Normal mode */
"""

MAIN_INTERFACE_TOGGLE_MINIMAL_END_NEEDLE = """    emit minimalViewToggled( b_minimalView );
}
"""

MAIN_INTERFACE_TOGGLE_MINIMAL_END_REPLACEMENT = """    msg_Dbg( p_intf,
             "open3d qt view toggleMinimalView end requested=%d current=%d menu=%d controls=%d input=%d video_fullscreen=%d",
             b_minimal, b_minimalView, menuBar()->isVisible(), controls->isVisible(),
             inputC->isVisible(), b_videoFullScreen );
    emit minimalViewToggled( b_minimalView );
}
"""

MAIN_INTERFACE_STARTUP_LOG_NEEDLE = """    setVisible( !b_hideAfterCreation );

    /* Switch to minimal view if needed, must be called after the show() */
    if( b_minimalView )
        toggleMinimalView( true );

    computeMinimumSize();
}
"""

MAIN_INTERFACE_STARTUP_LOG_REPLACEMENT = """    setVisible( !b_hideAfterCreation );

    /* Switch to minimal view if needed, must be called after the show() */
    if( b_minimalView )
        toggleMinimalView( true );

    QList<QAction*> menu_actions = menuBar()->actions();
    QRect menu_geom = menuBar()->geometry();
    QRect media_rect = menu_actions.count() > 0 ? menuBar()->actionGeometry( menu_actions[0] ) : QRect();
    QRect playback_rect = menu_actions.count() > 1 ? menuBar()->actionGeometry( menu_actions[1] ) : QRect();
    QRect audio_rect = menu_actions.count() > 2 ? menuBar()->actionGeometry( menu_actions[2] ) : QRect();
    msg_Dbg( p_intf,
             "open3d qt menu bar startup visible=%d native=%d geom=%d,%d %dx%d actions=%d hidden_after_creation=%d minimal=%d window=%dx%d",
             menuBar()->isVisible(), menuBar()->isNativeMenuBar(),
             menu_geom.x(), menu_geom.y(), menu_geom.width(), menu_geom.height(),
             menu_actions.count(), b_hideAfterCreation, b_minimalView,
             width(), height() );
    msg_Dbg( p_intf,
             "open3d qt menu bar actions media=%d,%d %dx%d playback=%d,%d %dx%d audio=%d,%d %dx%d",
             media_rect.x(), media_rect.y(), media_rect.width(), media_rect.height(),
             playback_rect.x(), playback_rect.y(), playback_rect.width(), playback_rect.height(),
             audio_rect.x(), audio_rect.y(), audio_rect.width(), audio_rect.height() );

    computeMinimumSize();
}
"""

MAIN_INTERFACE_GETVIDEOSLOT_START_NEEDLE = """void MainInterface::getVideoSlot( struct vout_window_t *p_wnd,
                                  unsigned i_width, unsigned i_height,
                                  bool fullscreen, bool *res )
{
    /* Hidden or minimized, activate */
"""

MAIN_INTERFACE_GETVIDEOSLOT_START_REPLACEMENT = """void MainInterface::getVideoSlot( struct vout_window_t *p_wnd,
                                  unsigned i_width, unsigned i_height,
                                  bool fullscreen, bool *res )
{
    msg_Dbg( p_intf,
             "open3d qt getVideoSlot request width=%u height=%u fullscreen=%d autoresize=%d existing_widget=%d window_type=%u",
             i_width, i_height, fullscreen, b_autoresize, videoWidget != NULL,
             p_wnd != NULL ? p_wnd->type : 0 );
    /* Hidden or minimized, activate */
"""

MAIN_INTERFACE_GETVIDEOSLOT_SETSIZE_NEEDLE = """            videoWidget->setSize( i_width, i_height );
"""

MAIN_INTERFACE_GETVIDEOSLOT_SETSIZE_REPLACEMENT = """            msg_Dbg( p_intf,
                     "open3d qt getVideoSlot setSize width=%u height=%u widget=%dx%d",
                     i_width, i_height,
                     videoWidget->size().width(), videoWidget->size().height() );
            videoWidget->setSize( i_width, i_height );
"""

MAIN_INTERFACE_SETVIDEOSIZE_START_NEEDLE = """void MainInterface::setVideoSize( unsigned int w, unsigned int h )
{
    if (!isFullScreen() && !isMaximized() )
    {
"""

MAIN_INTERFACE_SETVIDEOSIZE_START_REPLACEMENT = """void MainInterface::setVideoSize( unsigned int w, unsigned int h )
{
    msg_Dbg( p_intf,
             "open3d qt setVideoSize request physical=%ux%u fullscreen=%d maximized=%d autoresize=%d minimal=%d widget=%dx%d",
             w, h, isFullScreen(), isMaximized(), b_autoresize, b_minimalView,
             videoWidget != NULL ? videoWidget->size().width() : -1,
             videoWidget != NULL ? videoWidget->size().height() : -1 );
    if (!isFullScreen() && !isMaximized() )
    {
"""

MAIN_INTERFACE_SETVIDEOSIZE_SETSIZE_NEEDLE = """            videoWidget->setSize( w, h );
"""

MAIN_INTERFACE_SETVIDEOSIZE_SETSIZE_REPLACEMENT = """            msg_Dbg( p_intf,
                     "open3d qt setVideoSize setSize logical=%ux%u widget=%dx%d",
                     w, h,
                     videoWidget->size().width(), videoWidget->size().height() );
            videoWidget->setSize( w, h );
"""

QT_WINDOW_RESIZED_NEEDLE = """void WindowResized(vout_window_t *wnd, const QSize& size)
{
#ifdef QT5_HAS_X11
    vout_window_qt_t *sys = (vout_window_qt_t *)wnd->sys;

    if (QX11Info::isPlatformX11())
    {
        XResizeWindow(sys->dpy, wnd->handle.xid, size.width(), size.height());
        XSync(sys->dpy, True);
    }
#endif
    vout_window_ReportSize(wnd, size.width(), size.height());
}
"""

QT_WINDOW_RESIZED_REPLACEMENT = """void WindowResized(vout_window_t *wnd, const QSize& size)
{
#ifdef QT5_HAS_X11
    vout_window_qt_t *sys = (vout_window_qt_t *)wnd->sys;

    if (QX11Info::isPlatformX11())
    {
        msg_Dbg( wnd,
                 "open3d qt window resized xid=0x%08x size=%dx%d report_pending=1",
                 (unsigned)wnd->handle.xid, size.width(), size.height() );
        XResizeWindow(sys->dpy, wnd->handle.xid, size.width(), size.height());
        XSync(sys->dpy, True);
    }
#endif
    msg_Dbg( wnd,
             "open3d qt window report size=%dx%d type=%u",
             size.width(), size.height(), wnd->type );
    vout_window_ReportSize(wnd, size.width(), size.height());
}
"""

QT_WINDOWOPEN_NEEDLE = """    p_wnd->sys = (vout_window_sys_t *)sys;
    msg_Dbg( p_wnd, "requesting video window..." );
"""

QT_WINDOWOPEN_REPLACEMENT = """    p_wnd->sys = (vout_window_sys_t *)sys;
    msg_Dbg( p_wnd,
             "open3d qt WindowOpen cfg width=%u height=%u fullscreen=%d type=%u requested_window_type=%u",
             cfg->width, cfg->height, cfg->is_fullscreen, cfg->type,
             p_intf->p_sys->voutWindowType );
    msg_Dbg( p_wnd, "requesting video window..." );
"""

INTERFACE_WIDGETS_REPORTSIZE_NEEDLE = """void VideoWidget::reportSize()
{
    if( !p_window )
        return;

    QSize size = physicalSize();
    WindowResized(p_window, size);
}
"""

INTERFACE_WIDGETS_REPORTSIZE_REPLACEMENT = """void VideoWidget::reportSize()
{
    if( !p_window )
        return;

    QSize physical = physicalSize();
    msg_Dbg( p_intf,
             "open3d qt video reportSize widget=%dx%d stable=%dx%d physical=%dx%d window_type=%u",
             this->size().width(), this->size().height(),
             stable->size().width(), stable->size().height(),
             physical.width(), physical.height(), p_window->type );
    WindowResized(p_window, physical);
}
"""

INTERFACE_WIDGETS_REPORTSIZE_BROKEN_PATCH = """void VideoWidget::reportSize()
{
    if( !p_window )
        return;

    QSize size = physicalSize();
    msg_Dbg( p_intf,
             "open3d qt video reportSize widget=%dx%d stable=%dx%d physical=%dx%d window_type=%u",
             size().width(), size().height(),
             stable->size().width(), stable->size().height(),
             size.width(), size.height(), p_window->type );
    WindowResized(p_window, size);
}
"""

INTERFACE_WIDGETS_NATIVE_EVENT_NEEDLE = """#if defined(QT5_HAS_X11)
# if defined(QT5_HAS_XCB)
    if ( eventType == "xcb_generic_event_t" )
    {
        const xcb_generic_event_t* xev = static_cast<const xcb_generic_event_t*>( message );

        if ( xev->response_type == XCB_CONFIGURE_NOTIFY )
            reportSize();
    }
# endif
#endif
"""

INTERFACE_WIDGETS_NATIVE_EVENT_REPLACEMENT = """#if defined(QT5_HAS_X11)
# if defined(QT5_HAS_XCB)
    if ( eventType == "xcb_generic_event_t" )
    {
        const xcb_generic_event_t* xev = static_cast<const xcb_generic_event_t*>( message );

        if ( xev->response_type == XCB_CONFIGURE_NOTIFY )
        {
            msg_Dbg( p_intf,
                     "open3d qt native configure notify widget=%dx%d stable=%dx%d",
                     size().width(), size().height(),
                     stable->size().width(), stable->size().height() );
            reportSize();
        }
    }
# endif
#endif
"""

INTERFACE_WIDGETS_RESIZEEVENT_NEEDLE = """void VideoWidget::resizeEvent( QResizeEvent *event )
{
    QWidget::resizeEvent( event );

    if ( p_intf->p_sys->voutWindowType == VOUT_WINDOW_TYPE_XID ||
        p_intf->p_sys->voutWindowType == VOUT_WINDOW_TYPE_HWND )
        return;
    reportSize();
}
"""

INTERFACE_WIDGETS_RESIZEEVENT_LOGONLY_REPLACEMENT = """void VideoWidget::resizeEvent( QResizeEvent *event )
{
    QWidget::resizeEvent( event );

    msg_Dbg( p_intf,
             "open3d qt video resizeEvent widget=%dx%d stable=%dx%d old=%dx%d new=%dx%d window_type=%u",
             size().width(), size().height(),
             stable ? stable->size().width() : -1,
             stable ? stable->size().height() : -1,
             event->oldSize().width(), event->oldSize().height(),
             event->size().width(), event->size().height(),
             p_intf->p_sys->voutWindowType );

    if ( p_intf->p_sys->voutWindowType == VOUT_WINDOW_TYPE_XID ||
        p_intf->p_sys->voutWindowType == VOUT_WINDOW_TYPE_HWND )
        return;
    reportSize();
}
"""

INTERFACE_WIDGETS_RESIZEEVENT_XID_RETURN_NEEDLE = """    if ( p_intf->p_sys->voutWindowType == VOUT_WINDOW_TYPE_XID ||
        p_intf->p_sys->voutWindowType == VOUT_WINDOW_TYPE_HWND )
        return;
    reportSize();
}
"""

INTERFACE_WIDGETS_RESIZEEVENT_XID_RETURN_REPLACEMENT = """    if ( p_intf->p_sys->voutWindowType == VOUT_WINDOW_TYPE_XID ||
        p_intf->p_sys->voutWindowType == VOUT_WINDOW_TYPE_HWND )
    {
        msg_Dbg( p_intf,
                 "open3d qt video resizeEvent reportSize widget=%dx%d stable=%dx%d window_type=%u",
                 size().width(), size().height(),
                 stable ? stable->size().width() : -1,
                 stable ? stable->size().height() : -1,
                 p_intf->p_sys->voutWindowType );
        reportSize();
        return;
    }
    reportSize();
}
"""

INTERFACE_WIDGETS_RESIZEEVENT_REPLACEMENT = """void VideoWidget::resizeEvent( QResizeEvent *event )
{
    QWidget::resizeEvent( event );

    msg_Dbg( p_intf,
             "open3d qt video resizeEvent widget=%dx%d stable=%dx%d old=%dx%d new=%dx%d window_type=%u",
             size().width(), size().height(),
             stable ? stable->size().width() : -1,
             stable ? stable->size().height() : -1,
             event->oldSize().width(), event->oldSize().height(),
             event->size().width(), event->size().height(),
             p_intf->p_sys->voutWindowType );

    if ( p_intf->p_sys->voutWindowType == VOUT_WINDOW_TYPE_XID ||
        p_intf->p_sys->voutWindowType == VOUT_WINDOW_TYPE_HWND )
    {
        msg_Dbg( p_intf,
                 "open3d qt video resizeEvent reportSize widget=%dx%d stable=%dx%d window_type=%u",
                 size().width(), size().height(),
                 stable ? stable->size().width() : -1,
                 stable ? stable->size().height() : -1,
                 p_intf->p_sys->voutWindowType );
        reportSize();
        return;
    }
    reportSize();
}
"""


def replace_once(text: str, old: str, new: str, label: str) -> str:
    if old not in text:
        raise SystemExit(f"{label}: expected anchor not found")
    return text.replace(old, new, 1)


def patch_tree(tree_root: Path) -> None:
    hpp_path = tree_root / "modules/gui/qt/input_manager.hpp"
    cpp_path = tree_root / "modules/gui/qt/input_manager.cpp"
    menus_path = tree_root / "modules/gui/qt/menus.cpp"
    main_interface_path = tree_root / "modules/gui/qt/main_interface.cpp"
    qt_cpp_path = tree_root / "modules/gui/qt/qt.cpp"
    interface_widgets_path = tree_root / "modules/gui/qt/components/interface_widgets.cpp"

    hpp_text = hpp_path.read_text()
    if "void sectionPopupMenu();" not in hpp_text:
        hpp_text = replace_once(
            hpp_text,
            INPUT_MANAGER_HPP_NEEDLE,
            INPUT_MANAGER_HPP_REPLACEMENT,
            "input_manager.hpp",
        )
        hpp_path.write_text(hpp_text)

    cpp_text = cpp_path.read_text()
    if "void InputManager::sectionPopupMenu()" not in cpp_text:
        cpp_text = replace_once(
            cpp_text,
            INPUT_MANAGER_CPP_NEEDLE,
            INPUT_MANAGER_CPP_REPLACEMENT,
            "input_manager.cpp",
        )
        cpp_path.write_text(cpp_text)

    menus_text = menus_path.read_text()
    if MENUS_CPP_PREVIOUS_PATCH in menus_text:
        menus_text = menus_text.replace(
            MENUS_CPP_PREVIOUS_PATCH,
            MENUS_CPP_REPLACEMENT,
            1,
        )
    if 'action->setObjectName( "popupMenuAction" );' not in menus_text:
        menus_text = replace_once(
            menus_text,
            MENUS_CPP_NEEDLE,
            MENUS_CPP_REPLACEMENT,
            "menus.cpp navigation block",
        )
    if REBUILD_NAV_PREVIOUS_PATCH in menus_text:
        menus_text = menus_text.replace(
            REBUILD_NAV_PREVIOUS_PATCH,
            REBUILD_NAV_REPLACEMENT,
            1,
        )
    if 'actions[i]->objectName() == QLatin1String( "popupMenuAction" )' not in menus_text:
        menus_text = replace_once(
            menus_text,
            REBUILD_NAV_NEEDLE,
            REBUILD_NAV_REPLACEMENT,
            "menus.cpp rebuild block",
        )
    menus_path.write_text(menus_text)

    main_interface_text = main_interface_path.read_text()
    if "open3d qt view displayNormalView before" not in main_interface_text:
        main_interface_text = replace_once(
            main_interface_text,
            MAIN_INTERFACE_DISPLAY_NORMAL_NEEDLE,
            MAIN_INTERFACE_DISPLAY_NORMAL_REPLACEMENT,
            "main_interface.cpp displayNormalView",
        )
    if "open3d qt view setMinimalView begin" not in main_interface_text:
        main_interface_text = replace_once(
            main_interface_text,
            MAIN_INTERFACE_SET_MINIMAL_LOCALS_NEEDLE,
            MAIN_INTERFACE_SET_MINIMAL_LOCALS_REPLACEMENT,
            "main_interface.cpp setMinimalView begin",
        )
    if "open3d qt view setMinimalView applied" not in main_interface_text:
        main_interface_text = replace_once(
            main_interface_text,
            MAIN_INTERFACE_SET_MINIMAL_APPLY_NEEDLE,
            MAIN_INTERFACE_SET_MINIMAL_APPLY_REPLACEMENT,
            "main_interface.cpp setMinimalView applied",
        )
    if "open3d qt view toggleMinimalView begin" not in main_interface_text:
        main_interface_text = replace_once(
            main_interface_text,
            MAIN_INTERFACE_TOGGLE_MINIMAL_START_NEEDLE,
            MAIN_INTERFACE_TOGGLE_MINIMAL_START_REPLACEMENT,
            "main_interface.cpp toggleMinimalView begin",
        )
    if "open3d qt view toggleMinimalView end" not in main_interface_text:
        main_interface_text = replace_once(
            main_interface_text,
            MAIN_INTERFACE_TOGGLE_MINIMAL_END_NEEDLE,
            MAIN_INTERFACE_TOGGLE_MINIMAL_END_REPLACEMENT,
            "main_interface.cpp toggleMinimalView end",
        )
    if "open3d qt menu bar startup visible=" not in main_interface_text:
        main_interface_text = replace_once(
            main_interface_text,
            MAIN_INTERFACE_STARTUP_LOG_NEEDLE,
            MAIN_INTERFACE_STARTUP_LOG_REPLACEMENT,
            "main_interface.cpp startup menu bar log",
        )
    if "open3d qt getVideoSlot request width=" not in main_interface_text:
        main_interface_text = replace_once(
            main_interface_text,
            MAIN_INTERFACE_GETVIDEOSLOT_START_NEEDLE,
            MAIN_INTERFACE_GETVIDEOSLOT_START_REPLACEMENT,
            "main_interface.cpp getVideoSlot start",
        )
    if "open3d qt getVideoSlot setSize width=" not in main_interface_text:
        main_interface_text = replace_once(
            main_interface_text,
            MAIN_INTERFACE_GETVIDEOSLOT_SETSIZE_NEEDLE,
            MAIN_INTERFACE_GETVIDEOSLOT_SETSIZE_REPLACEMENT,
            "main_interface.cpp getVideoSlot setSize",
        )
    if "open3d qt setVideoSize request physical=" not in main_interface_text:
        main_interface_text = replace_once(
            main_interface_text,
            MAIN_INTERFACE_SETVIDEOSIZE_START_NEEDLE,
            MAIN_INTERFACE_SETVIDEOSIZE_START_REPLACEMENT,
            "main_interface.cpp setVideoSize start",
        )
    if "open3d qt setVideoSize setSize logical=" not in main_interface_text:
        main_interface_text = replace_once(
            main_interface_text,
            MAIN_INTERFACE_SETVIDEOSIZE_SETSIZE_NEEDLE,
            MAIN_INTERFACE_SETVIDEOSIZE_SETSIZE_REPLACEMENT,
            "main_interface.cpp setVideoSize setSize",
        )
    main_interface_path.write_text(main_interface_text)

    qt_cpp_text = qt_cpp_path.read_text()
    qt_cpp_orig = qt_cpp_text
    if "open3d qt WindowOpen cfg width=" not in qt_cpp_text:
        qt_cpp_text = replace_once(
            qt_cpp_text,
            QT_WINDOWOPEN_NEEDLE,
            QT_WINDOWOPEN_REPLACEMENT,
            "qt.cpp WindowOpen",
        )
    if "open3d qt window resized xid=" not in qt_cpp_text:
        qt_cpp_text = replace_once(
            qt_cpp_text,
            QT_WINDOW_RESIZED_NEEDLE,
            QT_WINDOW_RESIZED_REPLACEMENT,
            "qt.cpp WindowResized",
        )
    if qt_cpp_text != qt_cpp_orig:
        qt_cpp_path.write_text(qt_cpp_text)

    interface_widgets_text = interface_widgets_path.read_text()
    if INTERFACE_WIDGETS_REPORTSIZE_BROKEN_PATCH in interface_widgets_text:
        interface_widgets_text = interface_widgets_text.replace(
            INTERFACE_WIDGETS_REPORTSIZE_BROKEN_PATCH,
            INTERFACE_WIDGETS_REPORTSIZE_REPLACEMENT,
            1,
        )
    if "open3d qt video reportSize widget=" not in interface_widgets_text:
        interface_widgets_text = replace_once(
            interface_widgets_text,
            INTERFACE_WIDGETS_REPORTSIZE_NEEDLE,
            INTERFACE_WIDGETS_REPORTSIZE_REPLACEMENT,
            "interface_widgets.cpp reportSize",
        )
    if "open3d qt native configure notify widget=" not in interface_widgets_text:
        interface_widgets_text = replace_once(
            interface_widgets_text,
            INTERFACE_WIDGETS_NATIVE_EVENT_NEEDLE,
            INTERFACE_WIDGETS_NATIVE_EVENT_REPLACEMENT,
            "interface_widgets.cpp nativeEventFilter",
        )
    if "open3d qt video resizeEvent widget=" not in interface_widgets_text:
        interface_widgets_text = replace_once(
            interface_widgets_text,
            INTERFACE_WIDGETS_RESIZEEVENT_NEEDLE,
            INTERFACE_WIDGETS_RESIZEEVENT_REPLACEMENT,
            "interface_widgets.cpp resizeEvent",
        )
    elif "open3d qt video resizeEvent reportSize widget=" not in interface_widgets_text:
        if INTERFACE_WIDGETS_RESIZEEVENT_LOGONLY_REPLACEMENT in interface_widgets_text:
            interface_widgets_text = replace_once(
                interface_widgets_text,
                INTERFACE_WIDGETS_RESIZEEVENT_LOGONLY_REPLACEMENT,
                INTERFACE_WIDGETS_RESIZEEVENT_REPLACEMENT,
                "interface_widgets.cpp resizeEvent XID reportSize",
            )
        else:
            interface_widgets_text = replace_once(
                interface_widgets_text,
                INTERFACE_WIDGETS_RESIZEEVENT_XID_RETURN_NEEDLE,
                INTERFACE_WIDGETS_RESIZEEVENT_XID_RETURN_REPLACEMENT,
                "interface_widgets.cpp resizeEvent XID reportSize fallback",
            )
    interface_widgets_path.write_text(interface_widgets_text)


def main() -> int:
    if len(sys.argv) != 2:
        print("Usage: patch_vlc3_qt_popup_menu.py /path/to/vlc-source", file=sys.stderr)
        return 2

    tree_root = Path(sys.argv[1]).resolve()
    patch_tree(tree_root)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
