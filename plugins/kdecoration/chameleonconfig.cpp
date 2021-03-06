/*
 * Copyright (C) 2017 ~ 2019 Deepin Technology Co., Ltd.
 *
 * Author:     zccrs <zccrs@live.com>
 *
 * Maintainer: zccrs <zhangjide@deepin.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "chameleonconfig.h"
#include "chameleontheme.h"
#include "chameleonshadow.h"
#include "chameleon.h"
#include "chameleonwindowtheme.h"

#ifndef DISBLE_DDE_KWIN_XCB
#include "kwinutils.h"
#endif

#include <kwineffects.h>

#include <KConfig>
#include <KConfigGroup>
#include <KDecoration2/Decoration>
#include <KDecoration2/DecoratedClient>

#include <QPainter>
#include <QDebug>
#include <QX11Info>
#include <QGuiApplication>
#include <QTimer>

#include <xcb/xcb.h>
#include <X11/Xlib.h>

#define DDE_FORCE_DECORATE "__dde__force_decorate"
#define DDE_NEED_UPDATE_NOBORDER "__dde__need_update_noborder"

Q_DECLARE_METATYPE(QPainterPath)

ChameleonConfig::ChameleonConfig(QObject *parent)
    : QObject(parent)
{
#ifndef DISBLE_DDE_KWIN_XCB
    m_atom_deepin_chameleon = KWinUtils::internAtom(_DEEPIN_CHAMELEON, false);
    m_atom_deepin_no_titlebar = KWinUtils::internAtom(_DEEPIN_NO_TITLEBAR, false);
    m_atom_deepin_force_decorate = KWinUtils::internAtom(_DEEPIN_FORCE_DECORATE, false);
    m_atom_deepin_scissor_window = KWinUtils::internAtom(_DEEPIN_SCISSOR_WINDOW, false);
    m_atom_kde_net_wm_shadow = KWinUtils::internAtom(_KDE_NET_WM_SHADOW, false);
    m_atom_net_wm_window_type = KWinUtils::internAtom(_NET_WM_WINDOW_TYPE, false);

    if (KWinUtils::instance()->isInitialized()) {
        init();
    } else {
        connect(KWinUtils::instance(), &KWinUtils::initialized, this, &ChameleonConfig::init);
    }
#endif
}

ChameleonConfig *ChameleonConfig::instance()
{
    static ChameleonConfig *self = new ChameleonConfig();

    return self;
}

quint32 ChameleonConfig::atomDeepinChameleon() const
{
    return m_atom_deepin_chameleon;
}

quint32 ChameleonConfig::atomDeepinNoTitlebar() const
{
    return m_atom_deepin_no_titlebar;
}

quint32 ChameleonConfig::atomDeepinScissorWindow() const
{
    return m_atom_deepin_scissor_window;
}

bool ChameleonConfig::isActivated() const
{
    return m_activated;
}

QString ChameleonConfig::theme() const
{
    return m_theme;
}

bool ChameleonConfig::setTheme(QString theme)
{
    if (m_theme == theme)
        return false;

    if (ChameleonTheme::instance()->setTheme(theme)) {
        m_theme = theme;
        emit themeChanged(m_theme);

        if (isActivated()) {
            // ????????????????????????????????????
            clearKWinX11ShadowForWindows();
            clearX11ShadowCache();
            buildKWinX11ShadowForNoBorderWindows();
        }

        return true;
    }

    return false;
}

void ChameleonConfig::onConfigChanged()
{
    KConfig config("kwinrc", KConfig::CascadeConfig);
    KConfigGroup group_decoration(&config, "org.kde.kdecoration2");

    bool active = group_decoration.readEntry("library") == "com.deepin.chameleon";

    setActivated(active);

    KConfigGroup group(&config, TARGET_NAME);
    const QString &theme_info = group.readEntry("theme");

    if (setTheme(theme_info) && active) {
        // ???????????????????????????????????????????????????????????????
        buildKWinX11ShadowForNoBorderWindows();
    }
}

void ChameleonConfig::onClientAdded(KWin::Client *client)
{
    QObject *c = reinterpret_cast<QObject*>(client);

    connect(c, SIGNAL(activeChanged()), this, SLOT(updateClientX11Shadow()));
    connect(c, SIGNAL(hasAlphaChanged()), this, SLOT(updateClientX11Shadow()));
    connect(c, SIGNAL(shapedChanged()), this, SLOT(updateClientX11Shadow()));
    connect(c, SIGNAL(geometryChanged()), this, SLOT(updateWindowSize()));

    enforceWindowProperties(c);
    buildKWinX11Shadow(c);
}

void ChameleonConfig::onUnmanagedAdded(KWin::Unmanaged *client)
{
    QObject *c = reinterpret_cast<QObject*>(client);

    connect(c, SIGNAL(shapedChanged()), this, SLOT(updateClientX11Shadow()));
    connect(c, SIGNAL(geometryChanged()), this, SLOT(updateWindowSize()));

    enforceWindowProperties(c);
    buildKWinX11Shadow(c);
}

static bool canForceSetBorder(const QObject *window)
{
    if (!window->property("managed").toBool())
        return false;

    switch (window->property("windowType").toInt()) {
    case NET::Desktop:
    case NET::Dock:
    case NET::TopMenu:
    case NET::Splash:
    case NET::Notification:
    case NET::OnScreenDisplay:
        return false;
    default:
        break;
    }

    return true;
}

void ChameleonConfig::onCompositingToggled(bool active)
{
#ifndef DISBLE_DDE_KWIN_XCB
    if (active && isActivated()) {
        connect(KWin::effects, &KWin::EffectsHandler::windowDataChanged, this, &ChameleonConfig::onWindowDataChanged, Qt::UniqueConnection);
        KWinUtils::instance()->addSupportedProperty(m_atom_deepin_scissor_window);

        // ?????????????????????clip path????????????
        for (QObject *client : KWinUtils::clientList()) {
            updateClientClipPath(client);

            if (!canForceSetBorder(client)) {
                updateClientWindowRadius(client);
            }
        }

        for (QObject *window : KWinUtils::unmanagedList()) {
            updateClientClipPath(window);
            updateClientWindowRadius(window);
        }
    } else {
        KWinUtils::instance()->removeSupportedProperty(m_atom_deepin_scissor_window);
    }
#endif
}

static QObject *findWindow(xcb_window_t xid)
{
    // ???????????????????????????
    QObject *obj = KWinUtils::instance()->findClient(KWinUtils::Predicate::WindowMatch, xid);

    if (!obj) // ??????unmanaged?????????????????????
        obj = KWinUtils::instance()->findClient(KWinUtils::Predicate::UnmanagedMatch, xid);

    return obj;
}

void ChameleonConfig::onWindowPropertyChanged(quint32 windowId, quint32 atom)
{
#ifndef DISBLE_DDE_KWIN_XCB
    if (atom == m_atom_deepin_no_titlebar) {
        emit windowNoTitlebarPropertyChanged(windowId);
    } else if (atom == m_atom_deepin_force_decorate) {
        if (QObject *obj = findWindow(windowId))
            updateClientNoBorder(obj);

        emit windowForceDecoratePropertyChanged(windowId);
    } else if (atom == m_atom_deepin_scissor_window) {
        if (QObject *obj = findWindow(windowId))
            updateClientClipPath(obj);

        emit windowScissorWindowPropertyChanged(windowId);
    } else if (atom == m_atom_net_wm_window_type) {
        QObject *client = KWinUtils::instance()->findClient(KWinUtils::Predicate::WindowMatch, windowId);

        if (!client)
            return;

        //NOTE: if a pending window type change, we ignore the next request
        // (meaning multiple consective window type events)
        if (m_pendingWindows.find(client) != m_pendingWindows.end()) {
            return;
        }

        m_pendingWindows.insert(client, windowId);
        emit windowTypeChanged(client);

        bool force_decorate = client->property(DDE_FORCE_DECORATE).toBool();

        if (!force_decorate)
            return;

        setWindowOverrideType(client, false);
    }
#endif
}

void ChameleonConfig::onWindowDataChanged(KWin::EffectWindow *window, int role)
{
    switch (role) {
    case KWin::WindowBlurBehindRole:
    case WindowRadiusRole:
    case WindowClipPathRole:
        updateWindowBlurArea(window, role);
        break;
    default:
        break;
    }
}

void ChameleonConfig::onWindowShapeChanged(quint32 windowId)
{
    QObject *window = findWindow(windowId);

    if (!window)
        return;

    // ????????????????????????????????????????????????
    buildKWinX11ShadowDelay(window);
}

void ChameleonConfig::updateWindowNoBorderProperty(QObject *window)
{
    // NOTE:
    // since this slot gets executed in the event loop, there is a chance that 
    // window has already been destroyed as of now. so we need to do double 
    // check here.
    auto kv = m_pendingWindows.find(window);
    if (kv != m_pendingWindows.end()) {
        QObject *client = KWinUtils::instance()->findClient(KWinUtils::Predicate::WindowMatch, kv.value());

        m_pendingWindows.remove(window);
        if (!client) {
            return;
        }
    }

    if (window->property(DDE_NEED_UPDATE_NOBORDER).toBool()) {
        // ??????????????????????????????????????????
        window->setProperty(DDE_NEED_UPDATE_NOBORDER, QVariant());

        // ?????????????????????noBorder??????
        if (window->property(DDE_FORCE_DECORATE).toBool()) {
            window->setProperty("noBorder", false);
        } else {
            // ??????noBorder??????
            KWinUtils::instance()->clientCheckNoBorder(window);
        }
    }
}

// role ??????????????????????????????????????????????????????
void ChameleonConfig::updateWindowBlurArea(KWin::EffectWindow *window, int role)
{
    // ????????????__dde__ignore_blur_behind_changed???????????????????????????WindowBlurBehindRole???????????????
    // ???????????????????????????updateWindowBlurArea??????????????????????????????????????????
    if (role == KWin::WindowBlurBehindRole && window->property("__dde__ignore_blur_behind_changed").isValid()) {
        // ?????????????????????
        window->setProperty("__dde__ignore_blur_behind_changed", QVariant());
        return;
    }

    QVariant blur_area = window->data(KWin::WindowBlurBehindRole);

    // ????????????????????????????????????????????????????????????????????????????????????????????????????????????
    if (role != KWin::WindowBlurBehindRole) {
        const QVariant &cache_blur_area = window->property("__dde__blur_behind_role");

        if (cache_blur_area.isValid()) {
            blur_area = cache_blur_area;
        }
    }

    // ????????????????????????????????????
    if (!blur_area.isValid()) {
        // ?????????????????????
        window->setProperty("__dde__blur_behind_role", QVariant());
        return;
    }

    const QVariant &window_clip = window->data(WindowClipPathRole);
    const QVariant &window_radius = window->data(WindowRadiusRole);

    QPainterPath path;
    QPointF radius;

    if (window_clip.isValid()) {
        path = qvariant_cast<QPainterPath>(window_clip);
    }

    if (window_radius.isValid()) {
        radius = window_radius.toPointF();
    }

    // ????????????????????????????????????clip path????????????????????????????????????
    if (path.isEmpty() && (qIsNull(radius.x()) || qIsNull(radius.y()))) {
        const QVariant &blur_area = window->property("__dde__blur_behind_role");

        if (blur_area.isValid()) {
            // ????????????????????????
            window->setProperty("__dde__blur_behind_role", QVariant());
            window->setData(KWin::WindowBlurBehindRole, blur_area);
        }

        return;
    }

    // ?????????????????????????????????, ?????????????????????
    if (role == KWin::WindowBlurBehindRole
            || !window->property("__dde__blur_behind_role").isValid()) {
        window->setProperty("__dde__blur_behind_role", blur_area);
    }

    // ?????????????????????????????????????????????????????????????????????????????????
    if (path.isEmpty()) {
        // ??????????????????????????????????????????????????????????????????????????????????????????border???????????????????????????
        path.addRoundedRect(QRectF(window->rect()).adjusted(0.5, 0.5, -0.5, -0.5), radius.x() + 0.5, radius.y() + 0.5);
    }

    QPainterPath blur_path;
    QRegion blur_region = qvariant_cast<QRegion>(blur_area);

    if (!blur_region.isEmpty()) {
        blur_path.addRegion(blur_region);

        if ((blur_path - path).isEmpty()) {
            // ????????????????????????????????????????????????????????????
            return;
        }

        // ????????????????????????????????????????????????
        blur_path &= path;
    } else {
        blur_path = path;
    }

    blur_region = QRegion(blur_path.toFillPolygon().toPolygon());
    // ????????????????????????????????????????????????????????????????????????
    window->setProperty("__dde__ignore_blur_behind_changed", true);
    window->setData(KWin::WindowBlurBehindRole, blur_region);
    window->setData(WindowMaskTextureRole, QVariant());
}

// ??????????????????radius???????????????clip path????????????resize??????????????????????????????
void ChameleonConfig::updateWindowSize()
{
    QObject *window = QObject::sender();

    if (!window)
        return;

    const QSize &old_size = window->property("__dde__old_size").toSize();
    const QSize &size = window->property("size").toSize();

    if (old_size == size)
        return;

    window->setProperty("__dde_old_size", size);

    KWin::EffectWindow *effect = window->findChild<KWin::EffectWindow*>(QString(), Qt::FindDirectChildrenOnly);

    if (!effect) {
        return;
    }

    if (!effect->data(KWin::WindowBlurBehindRole).isValid())
        return;

    if (effect->data(WindowClipPathRole).isValid())
        return;

    if (!effect->data(WindowRadiusRole).isValid())
        return;

    updateWindowBlurArea(effect, 0);
}

void ChameleonConfig::updateClientX11Shadow()
{
    buildKWinX11Shadow(QObject::sender());
}

void ChameleonConfig::updateClientNoBorder(QObject *client, bool allowReset)
{
#ifndef DISBLE_DDE_KWIN_XCB
    const QByteArray &force_decorate = KWinUtils::instance()->readWindowProperty(client, m_atom_deepin_force_decorate, XCB_ATOM_CARDINAL);
    bool set_border = canForceSetBorder(client);

    if (!force_decorate.isEmpty() && force_decorate.at(0)) {
        // ??????????????????noBorder???????????????????????????noBorder??????
        if (set_border) {
            if (client->property("noBorder").toBool()) {
                // ????????????override???????????????????????????noBorder???????????????????????????????????????????????????????????????
                if (setWindowOverrideType(client, false)) {
                    // ?????????????????????????????????????????????????????????noBorder??????
                    client->setProperty(DDE_NEED_UPDATE_NOBORDER, true);
                } else {
                    client->setProperty("noBorder", false);
                }
                client->setProperty(DDE_FORCE_DECORATE, true);
            }
        } else {
            client->setProperty(DDE_FORCE_DECORATE, true);
        }
    } else if (client->property(DDE_FORCE_DECORATE).toBool()) {
        client->setProperty(DDE_FORCE_DECORATE, QVariant());

        if (allowReset) {
            // ?????????????????????override??????
            // ?????????????????????????????????????????????noBorder???????????????????????????????????????????????????????????????
            if (setWindowOverrideType(client, true)) {
                // ?????????????????????????????????????????????????????????noBorder??????
                client->setProperty(DDE_NEED_UPDATE_NOBORDER, true);
            } else {
                KWinUtils::instance()->clientCheckNoBorder(client);
            }
        }
    }
#endif
}

static ChameleonWindowTheme *buildWindowTheme(QObject *window)
{
    for (QObject *child : window->children()) {
        // ??????ChameleonWindowTheme????????????????????????????????????buildNativeSettings?????????????????????QMetaObject???????????????
        // ????????????QMetaObject::cast?????????????????????QObject::findChild???????????????????????????????????????qobject_cast????????????????????????
        // ???????????????????????????????????????ChameleonWindowTheme??????
        if (strcmp(child->metaObject()->className(), ChameleonWindowTheme::staticMetaObject.className()) == 0) {
            return static_cast<ChameleonWindowTheme*>(child);
        }
    }

    // ??????????????????????????????
    return new ChameleonWindowTheme(window, window);
}

// ???????????????????????????????????????????????????????????????????????????????????????????????????
// ??????????????????????????????????????????????????????radius?????????????????????Chameleon????????????
void ChameleonConfig::updateClientWindowRadius(QObject *client)
{
    // ??????????????????????????????border?????????????????????
    if (canForceSetBorder(client)) {
        return;
    }

    if (!client->property(DDE_FORCE_DECORATE).toBool()) {
        return;
    }

    KWin::EffectWindow *effect = client->findChild<KWin::EffectWindow*>(QString(), Qt::FindDirectChildrenOnly);

    if (!effect)
        return;

    QPointF window_radius = ChameleonTheme::instance()->themeConfig()->unmanaged.decoration.windowRadius;
    ChameleonWindowTheme *window_theme = buildWindowTheme(client);

    if (!window_theme->property("__connected_for_window_radius").toBool()) {
        auto update = [client, this] {
            updateClientWindowRadius(client);
        };

        connect(window_theme, &ChameleonWindowTheme::themeChanged, this, update);
        connect(window_theme, &ChameleonWindowTheme::windowRadiusChanged, this, update);
        connect(window_theme, &ChameleonWindowTheme::windowPixelRatioChanged, this, update);
        // ?????????????????????????????????
        window_theme->setProperty("__connected_for_window_radius", true);
    }

    window_radius *= window_theme->windowPixelRatio();

    if (window_theme->propertyIsValid(ChameleonWindowTheme::WindowRadiusProperty)) {
        // ??????????????????????????????????????????
        window_radius = window_theme->windowRadius();
    } else if (window_theme->propertyIsValid(ChameleonWindowTheme::ThemeProperty)) {
        // ??????????????????????????????????????????
        if (auto config_group = ChameleonTheme::instance()->loadTheme(window_theme->theme())) {
            window_radius = config_group->unmanaged.decoration.windowRadius * window_theme->windowPixelRatio();
        }
    }

    const QVariant &effect_window_radius = effect->data(ChameleonConfig::WindowRadiusRole);
    bool need_update = true;

    if (effect_window_radius.isValid()) {
        auto old_window_radius = effect_window_radius.toPointF();

        if (old_window_radius == window_radius) {
            need_update = false;
        }
    }

    if (need_update) {
        // ??????????????????????????????mask??????
        effect->setData(ChameleonConfig::WindowMaskTextureRole, QVariant());
        // ????????????????????????
        if (window_radius.isNull()) {
            effect->setData(ChameleonConfig::WindowRadiusRole, QVariant());
        } else {
            effect->setData(ChameleonConfig::WindowRadiusRole, QVariant::fromValue(window_radius));
        }
    }
}

void ChameleonConfig::updateClientClipPath(QObject *client)
{
    KWin::EffectWindow *effect = client->findChild<KWin::EffectWindow*>(QString(), Qt::FindDirectChildrenOnly);

    if (!effect)
        return;

    QPainterPath path;
    const QByteArray &clip_data = effect->readProperty(m_atom_deepin_scissor_window, m_atom_deepin_scissor_window, 8);

    if (!clip_data.isEmpty()) {
        QDataStream ds(clip_data);
        ds >> path;
    }

    if (path.isEmpty()) {
        effect->setData(WindowClipPathRole, QVariant());
    } else {
        effect->setData(WindowClipPathRole, QVariant::fromValue(path));
    }
}

void ChameleonConfig::init()
{
#ifndef DISBLE_DDE_KWIN_XCB
    connect(KWinUtils::workspace(), SIGNAL(configChanged()), this, SLOT(onConfigChanged()));
    connect(KWinUtils::workspace(), SIGNAL(clientAdded(KWin::Client*)), this, SLOT(onClientAdded(KWin::Client*)));
    connect(KWinUtils::workspace(), SIGNAL(unmanagedAdded(KWin::Unmanaged*)), this, SLOT(onUnmanagedAdded(KWin::Unmanaged*)));
    connect(KWinUtils::compositor(), SIGNAL(compositingToggled(bool)), this, SLOT(onCompositingToggled(bool)));
    connect(KWinUtils::instance(), &KWinUtils::windowPropertyChanged, this, &ChameleonConfig::onWindowPropertyChanged);
    connect(KWinUtils::instance(), &KWinUtils::windowShapeChanged, this, &ChameleonConfig::onWindowShapeChanged);
#endif

    // ?????????????????????????????????
    for (QObject *c : KWinUtils::instance()->clientList()) {
        connect(c, SIGNAL(activeChanged()), this, SLOT(updateClientX11Shadow()));
        connect(c, SIGNAL(hasAlphaChanged()), this, SLOT(updateClientX11Shadow()));
        connect(c, SIGNAL(shapedChanged()), this, SLOT(updateClientX11Shadow()));
    }

    for (QObject *c : KWinUtils::instance()->unmanagedList()) {
        connect(c, SIGNAL(shapedChanged()), this, SLOT(updateClientX11Shadow()));
    }

    // ????????????????????????????????????????????????kwin??????????????????????????????????????????????????????????????????????????????noBorder??????
    connect(this, &ChameleonConfig::windowTypeChanged, this, &ChameleonConfig::updateWindowNoBorderProperty, Qt::QueuedConnection);

    onConfigChanged();
}

void ChameleonConfig::setActivated(const bool active)
{
    if (m_activated == active)
        return;

    m_activated = active;

#ifndef DISBLE_DDE_KWIN_XCB
    if (active) {
        if (KWinUtils::compositorIsActive()) {
            connect(KWin::effects, &KWin::EffectsHandler::windowDataChanged, this, &ChameleonConfig::onWindowDataChanged, Qt::UniqueConnection);

            KWinUtils::instance()->addSupportedProperty(m_atom_deepin_scissor_window, false);
        }

        KWinUtils::instance()->addSupportedProperty(m_atom_deepin_chameleon, false);
        KWinUtils::instance()->addSupportedProperty(m_atom_deepin_no_titlebar, false);
        KWinUtils::instance()->addSupportedProperty(m_atom_deepin_force_decorate);

        // ??????????????????
        KWinUtils::instance()->addWindowPropertyMonitor(m_atom_deepin_no_titlebar);
        KWinUtils::instance()->addWindowPropertyMonitor(m_atom_deepin_force_decorate);
        KWinUtils::instance()->addWindowPropertyMonitor(m_atom_deepin_scissor_window);
        KWinUtils::instance()->addWindowPropertyMonitor(m_atom_net_wm_window_type);
    } else {
        if (KWin::effects) {
            disconnect(KWin::effects, &KWin::EffectsHandler::windowDataChanged, this, &ChameleonConfig::onWindowDataChanged);
        }

        KWinUtils::instance()->removeSupportedProperty(m_atom_deepin_scissor_window, false);
        KWinUtils::instance()->removeSupportedProperty(m_atom_deepin_chameleon, false);
        KWinUtils::instance()->removeSupportedProperty(m_atom_deepin_no_titlebar, false);
        KWinUtils::instance()->removeSupportedProperty(m_atom_deepin_force_decorate);

        // ????????????????????????
        KWinUtils::instance()->removeWindowPropertyMonitor(m_atom_deepin_no_titlebar);
        KWinUtils::instance()->removeWindowPropertyMonitor(m_atom_deepin_force_decorate);
        KWinUtils::instance()->removeWindowPropertyMonitor(m_atom_deepin_scissor_window);
        KWinUtils::instance()->removeWindowPropertyMonitor(m_atom_net_wm_window_type);
    }
#endif

    if (!active) {
        ChameleonShadow::instance()->clearCache();
        clearX11ShadowCache();
    }

    enforcePropertiesForWindows(active);

    emit activatedChanged(active);
}

enum ShadowElements {
    ShadowElementTop,
    ShadowElementTopRight,
    ShadowElementRight,
    ShadowElementBottomRight,
    ShadowElementBottom,
    ShadowElementBottomLeft,
    ShadowElementLeft,
    ShadowElementTopLeft,
    ShadowElementsCount,
    ShadowTopOffse = ShadowElementsCount,
    ShadowRightOffse = ShadowTopOffse + 1,
    ShadowBottomOffse = ShadowTopOffse + 2,
    ShadowLeftOffse = ShadowTopOffse + 3
};

class ShadowImage
{
public:
    ShadowImage(const QImage &image)
    {
        pixmap = XCreatePixmap(QX11Info::display(), QX11Info::appRootWindow(), image.width(), image.height(), image.depth());
        auto xcb_conn = QX11Info::connection();
        xcb_gcontext_t gc = xcb_generate_id(xcb_conn);
        xcb_create_gc(xcb_conn, gc, pixmap, 0, 0x0);
        xcb_put_image(xcb_conn, XCB_IMAGE_FORMAT_Z_PIXMAP, pixmap, gc,
                      image.width(), image.height(), 0, 0,
                      0, image.depth(),
                      image.byteCount(), image.constBits());
        xcb_free_gc(xcb_conn, gc);
    }

    ~ShadowImage() {
        XFreePixmap(QX11Info::display(), pixmap);
    }

    Pixmap pixmap;
};

class X11Shadow {
public:
    X11Shadow() {

    }

    ~X11Shadow() {
        if (valid)
            clear();
    }

    bool isValid() const {
        return valid;
    }

    void init(const QSharedPointer<KDecoration2::DecorationShadow> &shadow) {
        if (valid)
            return;

        QList<QRect> shadow_rect_list {
            shadow->topGeometry(),
            shadow->topRightGeometry(),
            shadow->rightGeometry(),
            shadow->bottomRightGeometry(),
            shadow->bottomGeometry(),
            shadow->bottomLeftGeometry(),
            shadow->leftGeometry(),
            shadow->topLeftGeometry()
        };

        const QImage &shadow_image = shadow->shadow();

        for (int i = 0; i < ShadowElements::ShadowElementsCount; ++i) {
            const QRect &rect = shadow_rect_list[i];
            ShadowImage *window = new ShadowImage(shadow_image.copy(rect));

            shadowWindowList[i] = window;
        }

        shadowOffset << shadow->paddingTop()
                     << shadow->paddingRight()
                     << shadow->paddingBottom()
                     << shadow->paddingLeft();

        valid = true;
    }

    void clear() {
        valid = false;

        for (int i = 0; i < ShadowElements::ShadowElementsCount; ++i) {
            delete shadowWindowList[i];
        }
    }

    QVector<quint32> toX11ShadowProperty() const {
        QVector<quint32> xwinid_list;

        for (int i = 0; i < ShadowElements::ShadowElementsCount; ++i) {
            xwinid_list << shadowWindowList[i]->pixmap;
        }

        xwinid_list << shadowOffset;

        return xwinid_list;
    }

private:
    bool valid = false;
    QVector<quint32> shadowOffset;
    ShadowImage *shadowWindowList[ShadowElements::ShadowElementsCount];
};

void ChameleonConfig::buildKWinX11Shadow(QObject *window)
{
    bool force_decorate = window->property(DDE_FORCE_DECORATE).toBool();
    bool can_build_shadow = canForceSetBorder(window);

    // ????????????????????????????????????????????????
    if (!force_decorate && window->property("popupMenu").toBool()) {
        // ????????? GTK_FRAME_EXTENTS ???????????????????????????
        if (!window->property("clientSideDecorated").toBool())
            can_build_shadow = true;
    }

    if (can_build_shadow) {
        // ?????????????????????????????????????????????, "?????????????????????????????????/???????????????/??????????????????"????????????????????????
        if (force_decorate
                || (window->property("noBorder").isValid() && !window->property("noBorder").toBool())
                || window->property("alpha").toBool()) {
            return;
        }
    } else if (!force_decorate) {
        return;
    }

    ChameleonWindowTheme *window_theme = buildWindowTheme(window);

    if (!window_theme->property("__connected_for_shadow").toBool()) {
        auto update = [window, this] {
            buildKWinX11Shadow(window);
        };

        connect(window_theme, &ChameleonWindowTheme::themeChanged, this, update);
        connect(window_theme, &ChameleonWindowTheme::shadowRadiusChanged, this, update);
        connect(window_theme, &ChameleonWindowTheme::shadowOffectChanged, this, update);
        connect(window_theme, &ChameleonWindowTheme::shadowColorChanged, this, update);
        connect(window_theme, &ChameleonWindowTheme::windowRadiusChanged, this, update);
        connect(window_theme, &ChameleonWindowTheme::borderColorChanged, this, update);
        connect(window_theme, &ChameleonWindowTheme::borderWidthChanged, this, update);
        connect(window_theme, &ChameleonWindowTheme::windowPixelRatioChanged, this, update);
        // ?????????????????????????????????
        window_theme->setProperty("__connected_for_shadow", true);
    }

    ShadowCacheType shadow_type;

    if (canForceSetBorder(window)) {
        shadow_type = window->property("active").toBool() ? ActiveType : InactiveType;
    } else {
        shadow_type = UnmanagedType;
    }

    auto theme_group = ChameleonTheme::instance()->themeConfig();
    ChameleonTheme::DecorationConfig theme_config;

    // ????????????????????????????????????????????????
    if (window_theme->propertyIsValid(ChameleonWindowTheme::ThemeProperty)) {
        if (auto new_theme = ChameleonTheme::instance()->loadTheme(window_theme->theme())) {
            theme_group = new_theme;
        }
    }

    switch (shadow_type) {
    case ActiveType:
        theme_config = theme_group->normal.decoration;
        break;
    case InactiveType:
        theme_config = theme_group->inactive.decoration;
        break;
    case UnmanagedType:
        theme_config = theme_group->unmanaged.decoration;
    default:
        break;
    }

    qreal scale = window_theme->windowPixelRatio();

    if (window_theme->propertyIsValid(ChameleonWindowTheme::WindowRadiusProperty)) {
        theme_config.windowRadius = window_theme->windowRadius();
        scale = 1.0; // ???????????????????????????????????????
    }

    if (window_theme->propertyIsValid(ChameleonWindowTheme::BorderWidthProperty)) {
        theme_config.borderWidth = window_theme->borderWidth();
    }

    if (window_theme->propertyIsValid(ChameleonWindowTheme::BorderColorProperty)) {
        theme_config.borderColor = window_theme->borderColor();
    }

    if (window_theme->propertyIsValid(ChameleonWindowTheme::ShadowColorProperty)) {
        theme_config.shadowColor = window_theme->shadowColor();
    }

    if (window_theme->propertyIsValid(ChameleonWindowTheme::ShadowOffsetProperty)) {
        theme_config.shadowOffset = window_theme->shadowOffset();
    }

    if (window_theme->propertyIsValid(ChameleonWindowTheme::ShadowRadiusProperty)) {
        theme_config.shadowRadius = window_theme->shadowRadius();
    }

    if (!force_decorate) {
        // ?????????
        theme_config.borderWidth = 0;
    }

    const QString &shadow_key = ChameleonShadow::buildShadowCacheKey(&theme_config, scale);
    X11Shadow *shadow = m_x11ShadowCache.value(shadow_key);

    if (!shadow) {
        auto s = ChameleonShadow::instance()->getShadow(&theme_config, scale);

        if (s) {
            shadow = new X11Shadow();
            shadow->init(s);
            m_x11ShadowCache[shadow_key] = shadow;
        }
    }

    if (!shadow) {
        return;
    }

    auto property_data = shadow->toX11ShadowProperty();

    if (window->property("shaped").toBool()) {
        KWin::EffectWindow *effect = nullptr;

        effect = window->findChild<KWin::EffectWindow*>(QString(), Qt::FindDirectChildrenOnly);

        if (effect) {
            QRect shape_rect = effect->shape().boundingRect();
            const QRect window_rect(QPoint(0, 0), window->property("size").toSize());

            // ???????????????shape??????
            if (shape_rect.isValid() && window_rect.isValid()) {
                property_data[ShadowTopOffse] -= shape_rect.top();
                property_data[ShadowRightOffse] -= window_rect.right() - shape_rect.right();
                property_data[ShadowBottomOffse] -= window_rect.bottom() - shape_rect.bottom();
                property_data[ShadowLeftOffse] -= shape_rect.left();
            }
        }
    }

    KWinUtils::setWindowProperty(window, m_atom_kde_net_wm_shadow,
                                 XCB_ATOM_CARDINAL, 32,
                                 QByteArray((char*)property_data.constData(), property_data.size() * 4));
}

void ChameleonConfig::buildKWinX11ShadowDelay(QObject *client, int delay)
{
    // ?????????????????????????????????????????????????????????????????????????????????
    if (client->property("__dde__delay_build_shadow").toBool())
        return;

    QPointer<ChameleonConfig> self(this);
    auto buildClientShadow = [client, self] {
        if (!self) {
            return;
        }

        // ?????????????????????
        client->setProperty("__dde__delay_build_shadow", QVariant());
        self->buildKWinX11Shadow(client);
    };

    // ?????????????????????????????????
    client->setProperty("__dde__delay_build_shadow", true);
    QTimer::singleShot(delay, client, buildClientShadow);
}

void ChameleonConfig::buildKWinX11ShadowForNoBorderWindows()
{
    for (QObject *client : KWinUtils::clientList()) {
        buildKWinX11Shadow(client);
    }

    for (QObject *client : KWinUtils::unmanagedList()) {
        buildKWinX11Shadow(client);
    }
}

void ChameleonConfig::clearKWinX11ShadowForWindows()
{
    for (const QObject *client : KWinUtils::clientList()) {
        KWinUtils::setWindowProperty(client, m_atom_kde_net_wm_shadow, 0, 0, QByteArray());
    }
}

void ChameleonConfig::clearX11ShadowCache()
{
    qDeleteAll(m_x11ShadowCache);
    m_x11ShadowCache.clear();
}

void ChameleonConfig::enforceWindowProperties(QObject *client)
{
    updateClientNoBorder(client, false);
    updateClientClipPath(client);
    updateClientWindowRadius(client);
}

void ChameleonConfig::enforcePropertiesForWindows(bool enable)
{
#ifndef DISBLE_DDE_KWIN_XCB
    for (QObject *client : KWinUtils::clientList()) {
        if (enable) {
            enforceWindowProperties(client);
        } else {
            // ???????????????noborder??????
            KWinUtils::instance()->clientCheckNoBorder(client);

            // ?????????????????????
            if (KWin::EffectWindow *effect = findChild<KWin::EffectWindow*>(QString(), Qt::FindDirectChildrenOnly)) {
                effect->setData(WindowClipPathRole, QVariant());
            }
        }
    }

    for (QObject *unmanaged : KWinUtils::unmanagedList()) {
        if (enable) {
            enforceWindowProperties(unmanaged);
        } else {
            // ?????????????????????
            if (KWin::EffectWindow *effect = findChild<KWin::EffectWindow*>(QString(), Qt::FindDirectChildrenOnly)) {
                effect->setData(WindowClipPathRole, QVariant());
            }
        }
    }
#endif
}

bool ChameleonConfig::setWindowOverrideType(QObject *client, bool enable)
{
#ifndef DISBLE_DDE_KWIN_XCB
    // ????????????override?????????????????????????????????override??????
    if (enable && !client->property("__dde__override_type").toBool()) {
        return false;
    }

    const QByteArray &data = KWinUtils::instance()->readWindowProperty(client, m_atom_net_wm_window_type, XCB_ATOM_ATOM);

    if (data.isEmpty()) {
        return false;
    }

    QVector<xcb_atom_t> atom_list;
    const xcb_atom_t *atoms = reinterpret_cast<const xcb_atom_t*>(data.constData());

    for (int i = 0; i < data.size() / sizeof(xcb_atom_t) * sizeof(char); ++i) {
        atom_list.append(atoms[i]);
    }

    static xcb_atom_t _KDE_NET_WM_WINDOW_TYPE_OVERRIDE = KWinUtils::instance()->getXcbAtom("_KDE_NET_WM_WINDOW_TYPE_OVERRIDE", true);

    if (!enable) {
        // ??????override??????????????????????????????window
        if (atom_list.removeAll(_KDE_NET_WM_WINDOW_TYPE_OVERRIDE)) {
            const QByteArray data((const char*)atom_list.constData(), atom_list.size() * sizeof(xcb_atom_t));
            KWinUtils::instance()->setWindowProperty(client, m_atom_net_wm_window_type, XCB_ATOM_ATOM, 32, data);
            xcb_flush(QX11Info::connection());
            // ?????????????????????override?????????????????????????????????
            client->setProperty("__dde__override_type", true);

            return true;
        }
    } else if (!atom_list.contains(_KDE_NET_WM_WINDOW_TYPE_OVERRIDE)) {
        atom_list.append(_KDE_NET_WM_WINDOW_TYPE_OVERRIDE);
        const QByteArray data((const char*)atom_list.constData(), atom_list.size() * sizeof(xcb_atom_t));
        KWinUtils::instance()->setWindowProperty(client, m_atom_net_wm_window_type, XCB_ATOM_ATOM, 32, data);
        xcb_flush(QX11Info::connection());
        // ????????????override????????????
        client->setProperty("__dde__override_type", QVariant());

        return true;
    }
#endif
    return false;
}
