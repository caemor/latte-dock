/*
*  Copyright 2016  Smith AR <audoban@openmailbox.org>
*                  Michail Vourlakos <mvourlakos@gmail.com>
*
*  This file is part of Latte-Dock
*
*  Latte-Dock is free software; you can redistribute it and/or
*  modify it under the terms of the GNU General Public License as
*  published by the Free Software Foundation; either version 2 of
*  the License, or (at your option) any later version.
*
*  Latte-Dock is distributed in the hope that it will be useful,
*  but WITHOUT ANY WARRANTY; without even the implied warranty of
*  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*  GNU General Public License for more details.
*
*  You should have received a copy of the GNU General Public License
*  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "view.h"

// local
#include "contextmenu.h"
#include "effects.h"
#include "positioner.h"
#include "visibilitymanager.h"
#include "settings/primaryconfigview.h"
#include "../lattecorona.h"
#include "../layoutmanager.h"
#include "../layout/layout.h"
#include "../plasma/extended/theme.h"
#include "../screenpool.h"
#include "../settings/universalsettings.h"
#include "../shortcuts/globalshortcuts.h"
#include "../shortcuts/shortcutstracker.h"
#include "../../liblatte2/extras.h"

// Qt
#include <QAction>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlProperty>
#include <QQuickItem>
#include <QMenu>

// KDe
#include <KActionCollection>
#include <KActivities/Consumer>
#include <KWayland/Client/plasmashell.h>
#include <KWayland/Client/surface.h>
#include <KWindowSystem>

// Plasma
#include <Plasma/Containment>
#include <Plasma/ContainmentActions>
#include <PlasmaQuick/AppletQuickItem>

namespace Latte {

//! both alwaysVisible and byPassWM are passed through corona because
//! during the view window creation containment hasn't been set, but these variables
//! are needed in order for window flags to be set correctly
View::View(Plasma::Corona *corona, QScreen *targetScreen, bool byPassWM)
    : PlasmaQuick::ContainmentView(corona),
      m_contextMenu(new ViewPart::ContextMenu(this)),
      m_effects(new ViewPart::Effects(this)),
      m_positioner(new ViewPart::Positioner(this)) //needs to be created after Effects because it catches some of its signals
{
    setTitle(corona->kPackage().metadata().name());
    setIcon(qGuiApp->windowIcon());
    setResizeMode(QuickViewSharedEngine::SizeRootObjectToView);
    setColor(QColor(Qt::transparent));
    setClearBeforeRendering(true);

    const auto flags = Qt::FramelessWindowHint
                       | Qt::WindowStaysOnTopHint
                       | Qt::NoDropShadowWindowHint
                       | Qt::WindowDoesNotAcceptFocus;

    if (byPassWM) {
        setFlags(flags | Qt::BypassWindowManagerHint);
    } else {
        setFlags(flags);
    }

    KWindowSystem::setOnAllDesktops(winId(), true);

    if (targetScreen)
        m_positioner->setScreenToFollow(targetScreen);
    else
        m_positioner->setScreenToFollow(qGuiApp->primaryScreen());

    connect(this, &View::containmentChanged
    , this, [ &, byPassWM]() {
        qDebug() << "dock view c++ containment changed 1...";

        if (!this->containment())
            return;

        qDebug() << "dock view c++ containment changed 2...";

        //! First load default values from file
        restoreConfig();

        //! Afterwards override that values in case during creation something different is needed
        setByPassWM(byPassWM);

        //! Check the screen assigned to this dock
        reconsiderScreen();

        //! needs to be created before visibility creation because visibility uses it
        if (!m_windowsTracker) {
            m_windowsTracker = new ViewPart::WindowsTracker(this);
        }

        if (!m_visibility) {
            m_visibility = new ViewPart::VisibilityManager(this);

            connect(m_visibility, &ViewPart::VisibilityManager::isHiddenChanged, this, [&]() {
                if (m_visibility->isHidden()) {
                    deactivateApplets();
                }
            });
        }

        connect(this->containment(), SIGNAL(statusChanged(Plasma::Types::ItemStatus)), SLOT(statusChanged(Plasma::Types::ItemStatus)));
    }, Qt::DirectConnection);

    auto *latteCorona = qobject_cast<Latte::Corona *>(this->corona());

    if (latteCorona) {
        connect(latteCorona, &Latte::Corona::viewLocationChanged, this, &View::dockLocationChanged);
    }
}

View::~View()
{
    m_inDelete = true;

    disconnect(corona(), &Plasma::Corona::availableScreenRectChanged, this, &View::availableScreenRectChanged);
    disconnect(containment(), SIGNAL(statusChanged(Plasma::Types::ItemStatus)), this, SLOT(statusChanged(Plasma::Types::ItemStatus)));

    qDebug() << "dock view deleting...";
    rootContext()->setContextProperty(QStringLiteral("dock"), nullptr);
    rootContext()->setContextProperty(QStringLiteral("layoutManager"), nullptr);
    rootContext()->setContextProperty(QStringLiteral("shortcutsEngine"), nullptr);
    rootContext()->setContextProperty(QStringLiteral("themeExtended"), nullptr);
    rootContext()->setContextProperty(QStringLiteral("universalSettings"), nullptr);

    //! this disconnect does not free up connections correctly when
    //! latteView is deleted. A crash for this example is the following:
    //! switch to Alternative Session and disable compositing,
    //! the signal creating the crash was probably from deleted
    //! windows.
    //! this->disconnect();

    if (m_configView) {
        m_configView->setVisible(false);//hide();
    }

    if (m_contextMenu) {
        delete m_contextMenu;
    }

    //needs to be deleted before Effects because it catches some of its signals
    if (m_positioner) {
        delete m_positioner;
    }

    if (m_effects) {
        delete m_effects;
    }

    if (m_visibility) {
        delete m_visibility;
    }

    if (m_windowsTracker) {
        delete m_windowsTracker;
    }
}

void View::init()
{
    connect(this, &QQuickWindow::xChanged, this, &View::xChanged);
    connect(this, &QQuickWindow::xChanged, this, &View::updateAbsDockGeometry);
    connect(this, &QQuickWindow::yChanged, this, &View::yChanged);
    connect(this, &QQuickWindow::yChanged, this, &View::updateAbsDockGeometry);
    connect(this, &QQuickWindow::widthChanged, this, &View::widthChanged);
    connect(this, &QQuickWindow::widthChanged, this, &View::updateAbsDockGeometry);
    connect(this, &QQuickWindow::heightChanged, this, &View::heightChanged);
    connect(this, &QQuickWindow::heightChanged, this, &View::updateAbsDockGeometry);

    connect(corona(), &Plasma::Corona::availableScreenRectChanged, this, &View::availableScreenRectChanged);

    connect(this, &View::byPassWMChanged, this, &View::saveConfig);
    connect(this, &View::isPreferredForShortcutsChanged, this, &View::saveConfig);
    connect(this, &View::onPrimaryChanged, this, &View::saveConfig);
    connect(this, &View::typeChanged, this, &View::saveConfig);

    connect(this, SIGNAL(normalThicknessChanged()), corona(), SIGNAL(availableScreenRectChanged()));

    connect(m_positioner, &ViewPart::Positioner::onHideWindowsForSlidingOut, this, &View::hideWindowsForSlidingOut);
    connect(m_positioner, &ViewPart::Positioner::screenGeometryChanged, this, &View::screenGeometryChanged);
    connect(m_contextMenu, &ViewPart::ContextMenu::menuChanged, this, &View::contextMenuIsShownChanged);

    ///!!!!!
    rootContext()->setContextProperty(QStringLiteral("latteView"), this);

    auto *latteCorona = qobject_cast<Latte::Corona *>(this->corona());

    if (latteCorona) {
        rootContext()->setContextProperty(QStringLiteral("layoutManager"), latteCorona->layoutManager());
        rootContext()->setContextProperty(QStringLiteral("shortcutsEngine"), latteCorona->globalShortcuts()->shortcutsTracker());
        rootContext()->setContextProperty(QStringLiteral("themeExtended"), latteCorona->themeExtended());
        rootContext()->setContextProperty(QStringLiteral("universalSettings"), latteCorona->universalSettings());
    }

    setSource(corona()->kPackage().filePath("lattedockui"));
    // setVisible(true);
    m_positioner->syncGeometry();

    if (!KWindowSystem::isPlatformWayland()) {
        setVisible(true);
    }

    qDebug() << "SOURCE:" << source();
}

bool View::inDelete() const
{
    return m_inDelete;
}

void View::disconnectSensitiveSignals()
{
    disconnect(corona(), &Plasma::Corona::availableScreenRectChanged, this, &View::availableScreenRectChanged);
    setManagedLayout(nullptr);

    if (m_windowsTracker) {
        m_windowsTracker->setEnabled(false);
    }
}

void View::availableScreenRectChanged()
{
    if (m_inDelete)
        return;

    if (formFactor() == Plasma::Types::Vertical) {
        m_positioner->syncGeometry();
    }
}

void View::setupWaylandIntegration()
{
    if (m_shellSurface)
        return;

    if (Latte::Corona *c = qobject_cast<Latte::Corona *>(corona())) {
        using namespace KWayland::Client;
        PlasmaShell *interface {c->waylandCoronaInterface()};

        if (!interface)
            return;

        Surface *s{Surface::fromWindow(this)};

        if (!s)
            return;

        m_shellSurface = interface->createSurface(s, this);
        qDebug() << "WAYLAND dock window surface was created...";

        m_shellSurface->setSkipTaskbar(true);
        m_shellSurface->setRole(PlasmaShellSurface::Role::Panel);
        m_shellSurface->setPanelBehavior(PlasmaShellSurface::PanelBehavior::WindowsGoBelow);
    }
}

KWayland::Client::PlasmaShellSurface *View::surface()
{
    return m_shellSurface;
}

//! the main function which decides if this dock is at the
//! correct screen
void View::reconsiderScreen()
{
    m_positioner->reconsiderScreen();
}

void View::copyView()
{
    m_managedLayout->copyView(containment());
}

void View::removeView()
{
    if (m_managedLayout && m_managedLayout->viewsCount() > 1) {
        QAction *removeAct = this->containment()->actions()->action(QStringLiteral("remove"));

        if (removeAct) {
            removeAct->trigger();
        }
    }
}

bool View::settingsWindowIsShown()
{
    auto configView = qobject_cast<ViewPart::PrimaryConfigView *>(m_configView);

    return (configView != nullptr);
}

void View::showSettingsWindow()
{
    if (!settingsWindowIsShown()) {
        emit m_visibility->mustBeShown();
        showConfigurationInterface(containment());
        applyActivitiesToWindows();
    }
}

void View::showConfigurationInterface(Plasma::Applet *applet)
{
    if (!applet || !applet->containment())
        return;

    Plasma::Containment *c = qobject_cast<Plasma::Containment *>(applet);

    if (m_configView && c && c->isContainment() && c == this->containment()) {
        if (m_configView->isVisible()) {
            m_configView->setVisible(false);
            //m_configView->hide();
        } else {
            m_configView->setVisible(true);
            //m_configView->show();
        }

        return;
    } else if (m_configView) {
        if (m_configView->applet() == applet) {
            m_configView->setVisible(true);
            //m_configView->show();
            m_configView->requestActivate();
            return;
        } else {
            m_configView->setVisible(false);
            //m_configView->hide();
            m_configView->deleteLater();
        }
    }

    bool delayConfigView = false;

    if (c && containment() && c->isContainment() && c->id() == this->containment()->id()) {
        m_configView = new ViewPart::PrimaryConfigView(c, this);
        delayConfigView = true;
    } else {
        m_configView = new PlasmaQuick::ConfigView(applet);
    }

    m_configView.data()->init();

    if (!delayConfigView) {
        m_configView->setVisible(true);
        //m_configView.data()->show();
    } else {
        //add a timer for showing the configuration window the first time it is
        //created in order to give the containment's layouts the time to
        //calculate the window's height
        if (!KWindowSystem::isPlatformWayland()) {
            QTimer::singleShot(150, m_configView, SLOT(show()));
        } else {
            QTimer::singleShot(150, [this]() {
                m_configView->setVisible(true);
            });
        }
    }
}

QRect View::localGeometry() const
{
    return m_localGeometry;
}

void View::setLocalGeometry(const QRect &geometry)
{
    if (m_localGeometry == geometry) {
        return;
    }

    m_localGeometry = geometry;
    emit localGeometryChanged();
    updateAbsDockGeometry();
}

void View::updateAbsDockGeometry(bool bypassChecks)
{
    //! there was a -1 in height and width here. The reason of this
    //! if I remember correctly was related to multi-screen but I cant
    //! remember exactly the reason, something related to right edge in
    //! multi screen environment. BUT this was breaking the entire AlwaysVisible
    //! experience with struts. Removing them in order to restore correct
    //! behavior and keeping this comment in order to check for
    //! multi-screen breakage
    QRect absGeometry {x() + m_localGeometry.x(), y() + m_localGeometry.y()
                       , m_localGeometry.width(), m_localGeometry.height()};

    if (m_absGeometry == absGeometry && !bypassChecks)
        return;

    m_absGeometry = absGeometry;
    emit absGeometryChanged(m_absGeometry);

    //! this is needed in order to update correctly the screenGeometries
    if (visibility() && corona() && visibility()->mode() == Types::AlwaysVisible) {
        emit corona()->availableScreenRectChanged();
        emit corona()->availableScreenRegionChanged();
    }
}

void View::statusChanged(Plasma::Types::ItemStatus status)
{
    if (containment()) {
        if (containment()->status() >= Plasma::Types::NeedsAttentionStatus &&
            containment()->status() != Plasma::Types::HiddenStatus) {
            setBlockHiding(true);
        } else if (!containment()->isUserConfiguring()){
            setBlockHiding(false);
        }
    }
}

Types::ViewType View::type() const
{
    return m_type;
}

void View::setType(Types::ViewType type)
{
    if (m_type == type) {
        return;
    }

    m_type = type;
    emit typeChanged();
}

bool View::alternativesIsShown() const
{
    return m_alternativesIsShown;
}

void View::setAlternativesIsShown(bool show)
{
    if (m_alternativesIsShown == show) {
        return;
    }

    m_alternativesIsShown = show;

    setBlockHiding(show);
    emit alternativesIsShownChanged();
}

bool View::contextMenuIsShown() const
{
    if (!m_contextMenu) {
        return false;
    }

    return m_contextMenu->menu();
}

int View::currentThickness() const
{
    if (formFactor() == Plasma::Types::Vertical) {
        return m_effects->mask().isNull() ? width() : m_effects->mask().width() - m_effects->innerShadow();
    } else {
        return m_effects->mask().isNull() ? height() : m_effects->mask().height() - m_effects->innerShadow();
    }
}

int View::normalThickness() const
{
    return m_normalThickness;
}

void View::setNormalThickness(int thickness)
{
    if (m_normalThickness == thickness) {
        return;
    }

    m_normalThickness = thickness;
    emit normalThicknessChanged();
}

bool View::byPassWM() const
{
    return m_byPassWM;
}

void View::setByPassWM(bool bypass)
{
    if (m_byPassWM == bypass) {
        return;
    }

    m_byPassWM = bypass;
    emit byPassWMChanged();
}

bool View::behaveAsPlasmaPanel() const
{
    return m_behaveAsPlasmaPanel;
}

void View::setBehaveAsPlasmaPanel(bool behavior)
{
    if (m_behaveAsPlasmaPanel == behavior) {
        return;
    }

    m_behaveAsPlasmaPanel = behavior;

    emit behaveAsPlasmaPanelChanged();
}

bool View::inEditMode() const
{
    return m_inEditMode;
}

void View::setInEditMode(bool edit)
{
    if (m_inEditMode == edit) {
        return;
    }

    m_inEditMode = edit;

    emit inEditModeChanged();
}

bool View::isPreferredForShortcuts() const
{
    return m_isPreferredForShortcuts;
}

void View::setIsPreferredForShortcuts(bool preferred)
{
    if (m_isPreferredForShortcuts == preferred) {
        return;
    }

    m_isPreferredForShortcuts = preferred;

    emit isPreferredForShortcutsChanged();

    if (m_isPreferredForShortcuts && m_managedLayout) {
        emit m_managedLayout->preferredViewForShortcutsChanged(this);
    }
}

void View::preferredViewForShortcutsChangedSlot(Latte::View *view)
{
    if (view != this) {
        setIsPreferredForShortcuts(false);
    }
}

bool View::onPrimary() const
{
    return m_onPrimary;
}

void View::setOnPrimary(bool flag)
{
    if (m_onPrimary == flag) {
        return;
    }

    m_onPrimary = flag;
    emit onPrimaryChanged();
}

float View::maxLength() const
{
    return m_maxLength;
}

void View::setMaxLength(float length)
{
    if (m_maxLength == length) {
        return;
    }

    m_maxLength = length;
    emit maxLengthChanged();
}

int View::maxThickness() const
{
    return m_maxThickness;
}

void View::setMaxThickness(int thickness)
{
    if (m_maxThickness == thickness)
        return;

    m_maxThickness = thickness;
    emit maxThicknessChanged();
}

int View::alignment() const
{
    return m_alignment;
}

void View::setAlignment(int alignment)
{
    Types::Alignment align = static_cast<Types::Alignment>(alignment);

    if (m_alignment == alignment) {
        return;
    }

    m_alignment = align;
    emit alignmentChanged();
}

QRect View::absGeometry() const
{
    return m_absGeometry;
}

QRect View::screenGeometry() const
{
    if (this->screen()) {
        QRect geom = this->screen()->geometry();
        return geom;
    }

    return QRect();
}

int View::offset() const
{
    return m_offset;
}

void View::setOffset(int offset)
{
    if (m_offset == offset) {
        return;
    }

    m_offset = offset;
    emit offsetChanged();
}

int View::fontPixelSize() const
{
    return m_fontPixelSize;
}

void View::setFontPixelSize(int size)
{
    if (m_fontPixelSize == size) {
        return;
    }

    m_fontPixelSize = size;

    emit fontPixelSizeChanged();
}

void View::applyActivitiesToWindows()
{
    if (m_visibility) {
        QStringList activities = m_managedLayout->appliedActivities();
        m_windowsTracker->setWindowOnActivities(*this, activities);

        if (m_configView) {
            m_windowsTracker->setWindowOnActivities(*m_configView, activities);

            auto configView = qobject_cast<ViewPart::PrimaryConfigView *>(m_configView);

            if (configView && configView->secondaryWindow()) {
                m_windowsTracker->setWindowOnActivities(*configView->secondaryWindow(), activities);
            }
        }

        if (m_visibility->supportsKWinEdges()) {
            m_visibility->applyActivitiesToHiddenWindows(activities);
        }
    }
}

Layout *View::managedLayout() const
{
    return m_managedLayout;
}

void View::setManagedLayout(Layout *layout)
{
    if (m_managedLayout == layout) {
        return;
    }

    // clear mode
    for (auto &c : connectionsManagedLayout) {
        disconnect(c);
    }

    m_managedLayout = layout;

    if (m_managedLayout) {
        //! Sometimes the activity isnt completely ready, by adding a delay
        //! we try to catch up
        QTimer::singleShot(100, [this]() {
            if (m_managedLayout && m_visibility) {
                qDebug() << "DOCK VIEW FROM LAYOUT ::: " << m_managedLayout->name() << " - activities: " << m_managedLayout->appliedActivities();
                applyActivitiesToWindows();
                emit activitiesChanged();
            }
        });

        connectionsManagedLayout[0] = connect(m_managedLayout, &Layout::preferredViewForShortcutsChanged, this, &View::preferredViewForShortcutsChangedSlot);
    }

    Latte::Corona *latteCorona = qobject_cast<Latte::Corona *>(this->corona());

    if (latteCorona->layoutManager()->memoryUsage() == Types::MultipleLayouts) {
        connectionsManagedLayout[1] = connect(latteCorona->activitiesConsumer(), &KActivities::Consumer::runningActivitiesChanged, this, [&]() {
            if (m_managedLayout && m_visibility) {
                qDebug() << "DOCK VIEW FROM LAYOUT (runningActivitiesChanged) ::: " << m_managedLayout->name()
                         << " - activities: " << m_managedLayout->appliedActivities();
                applyActivitiesToWindows();
                emit activitiesChanged();
            }
        });

        connectionsManagedLayout[2] = connect(m_managedLayout, &Layout::activitiesChanged, this, [&]() {
            if (m_managedLayout) {
                applyActivitiesToWindows();
                emit activitiesChanged();
            }
        });

        connectionsManagedLayout[3] = connect(latteCorona->layoutManager(), &LayoutManager::layoutsChanged, this, [&]() {
            if (m_managedLayout) {
                applyActivitiesToWindows();
                emit activitiesChanged();
            }
        });

        //!IMPORTANT!!! ::: This fixes a bug when closing an Activity all docks from all Activities are
        //! disappearing! With this they reappear!!!
        connectionsManagedLayout[4] = connect(this, &QWindow::visibleChanged, this, [&]() {
            if (!isVisible() && m_managedLayout) {
                QTimer::singleShot(100, [this]() {
                    if (m_managedLayout && containment() && !containment()->destroyed()) {
                        setVisible(true);
                        applyActivitiesToWindows();
                        emit activitiesChanged();
                    }
                });

                QTimer::singleShot(1500, [this]() {
                    if (m_managedLayout && containment() && !containment()->destroyed()) {
                        setVisible(true);
                        applyActivitiesToWindows();
                        emit activitiesChanged();
                    }
                });
            }
        });
    }

    emit managedLayoutChanged();
}

void View::moveToLayout(QString layoutName)
{
    if (!m_managedLayout) {
        return;
    }

    QList<Plasma::Containment *> containments = m_managedLayout->unassignFromLayout(this);

    Latte::Corona *latteCorona = qobject_cast<Latte::Corona *>(this->corona());

    if (latteCorona && containments.size() > 0) {
        Layout *newLayout = latteCorona->layoutManager()->activeLayout(layoutName);

        if (newLayout) {
            newLayout->assignToLayout(this, containments);
        }
    }
}

void View::setBlockHiding(bool block)
{
    if (!block) {
        auto *configView = qobject_cast<ViewPart::PrimaryConfigView *>(m_configView);

        if (m_alternativesIsShown || (configView && configView->sticker() && configView->isVisible())) {
            return;
        }

        if (m_visibility) {
            m_visibility->setBlockHiding(false);
        }
    } else {
        if (m_visibility) {
            m_visibility->setBlockHiding(true);
        }
    }
}

void View::hideWindowsForSlidingOut()
{
    setBlockHiding(false);

    if (m_configView) {
        auto configDialog = qobject_cast<ViewPart::PrimaryConfigView *>(m_configView);

        if (configDialog) {
            configDialog->hideConfigWindow();
        }
    }
}

//! remove latte tasks plasmoid
void View::removeTasksPlasmoid()
{
    if (!tasksPresent() || !containment()) {
        return;
    }

    foreach (Plasma::Applet *applet, containment()->applets()) {
        KPluginMetaData meta = applet->kPackage().metadata();

        if (meta.pluginId() == "org.kde.latte.plasmoid") {
            QAction *closeApplet = applet->actions()->action(QStringLiteral("remove"));

            if (closeApplet) {
                closeApplet->trigger();
                //! remove only the first found
                return;
            }
        }
    }
}

//! check if the tasks plasmoid exist in the dock
bool View::tasksPresent()
{
    if (!this->containment()) {
        return false;
    }

    foreach (Plasma::Applet *applet, this->containment()->applets()) {
        const auto &provides = KPluginMetaData::readStringList(applet->pluginMetaData().rawData(), QStringLiteral("X-Plasma-Provides"));

        if (provides.contains(QLatin1String("org.kde.plasma.multitasking"))) {
            return true;
        }
    }

    return false;
}


//! check if the tasks plasmoid exist in the dock
bool View::latteTasksPresent()
{
    if (!this->containment()) {
        return false;
    }

    foreach (Plasma::Applet *applet, this->containment()->applets()) {
        KPluginMetaData metadata = applet->pluginMetaData();

        if (metadata.pluginId() == "org.kde.latte.plasmoid") {
            return true;
        }
    }

    return false;
}


//!check if the plasmoid with _name_ exists in the midedata
bool View::mimeContainsPlasmoid(QMimeData *mimeData, QString name)
{
    if (!mimeData) {
        return false;
    }

    if (mimeData->hasFormat(QStringLiteral("text/x-plasmoidservicename"))) {
        QString data = mimeData->data(QStringLiteral("text/x-plasmoidservicename"));
        const QStringList appletNames = data.split('\n', QString::SkipEmptyParts);

        foreach (const QString &appletName, appletNames) {
            if (appletName == name)
                return true;
        }
    }

    return false;
}

ViewPart::Effects *View::effects() const
{
    return m_effects;
}

ViewPart::Positioner *View::positioner() const
{
    return m_positioner;
}

ViewPart::VisibilityManager *View::visibility() const
{
    return m_visibility;
}

ViewPart::WindowsTracker *View::windowsTracker() const
{
    return m_windowsTracker;
}

bool View::event(QEvent *e)
{
    if (!m_inDelete) {
        emit eventTriggered(e);

        switch (e->type()) {
            case QEvent::Leave:
                engine()->trimComponentCache();
                break;

            case QEvent::PlatformSurface:
                if (auto pe = dynamic_cast<QPlatformSurfaceEvent *>(e)) {
                    switch (pe->surfaceEventType()) {
                        case QPlatformSurfaceEvent::SurfaceCreated:
                            setupWaylandIntegration();

                            if (m_shellSurface) {
                                m_positioner->syncGeometry();
                                m_effects->updateShadows();
                            }

                            break;

                        case QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed:
                            if (m_shellSurface) {
                                delete m_shellSurface;
                                m_shellSurface = nullptr;
                                qDebug() << "WAYLAND dock window surface was deleted...";
                                m_effects->clearShadows();
                            }

                            break;
                    }
                }

                break;

            default:
                break;
        }
    }

    return ContainmentView::event(e);;
}

void View::deactivateApplets()
{
    if (!containment()) {
        return;
    }

    foreach (auto applet, containment()->applets()) {
        PlasmaQuick::AppletQuickItem *ai = applet->property("_plasma_graphicObject").value<PlasmaQuick::AppletQuickItem *>();

        if (ai) {
            ai->setExpanded(false);
        }
    }
}

void View::toggleAppletExpanded(const int id)
{
    if (!containment()) {
        return;
    }

    foreach (auto applet, containment()->applets()) {
        if (applet->id() == id) {
            PlasmaQuick::AppletQuickItem *ai = applet->property("_plasma_graphicObject").value<PlasmaQuick::AppletQuickItem *>();

            if (ai) {
                if (!ai->isActivationTogglesExpanded()) {
                    ai->setActivationTogglesExpanded(true);
                }

                emit applet->activated();
            }
        }
    }
}

QVariantList View::containmentActions()
{
    QVariantList actions;
    /*if (containment()->corona()->immutability() != Plasma::Types::Mutable) {
        return actions;
    }*/
    //FIXME: the trigger string it should be better to be supported this way
    //const QString trigger = Plasma::ContainmentActions::eventToString(event);
    const QString trigger = "RightButton;NoModifier";
    Plasma::ContainmentActions *plugin = this->containment()->containmentActions().value(trigger);

    if (!plugin) {
        return actions;
    }

    if (plugin->containment() != this->containment()) {
        plugin->setContainment(this->containment());
        // now configure it
        KConfigGroup cfg(this->containment()->corona()->config(), "ActionPlugins");
        cfg = KConfigGroup(&cfg, QString::number(this->containment()->containmentType()));
        KConfigGroup pluginConfig = KConfigGroup(&cfg, trigger);
        plugin->restore(pluginConfig);
    }

    foreach (QAction *ac, plugin->contextualActions()) {
        actions << QVariant::fromValue<QAction *>(ac);
    }

    return actions;
}

void View::disableGrabItemBehavior()
{
    setMouseGrabEnabled(false);
}

void View::restoreGrabItemBehavior()
{
    if (mouseGrabberItem()) {
        mouseGrabberItem()->ungrabMouse();
    }
}

bool View::isHighestPriorityView() {
    if (m_managedLayout) {
        return this == m_managedLayout->highestPriorityView();
    }

    return false;
}

//!BEGIN overriding context menus behavior
void View::mousePressEvent(QMouseEvent *event)
{
    bool result = m_contextMenu->mousePressEvent(event);
    emit contextMenuIsShownChanged();

    if (result) {
        PlasmaQuick::ContainmentView::mousePressEvent(event);
    }
}
//!END overriding context menus behavior

//!BEGIN configuration functions
void View::saveConfig()
{
    if (!this->containment())
        return;

    auto config = this->containment()->config();
    config.writeEntry("onPrimary", onPrimary());
    config.writeEntry("byPassWM", byPassWM());
    config.writeEntry("isPreferredForShortcuts", isPreferredForShortcuts());
    config.writeEntry("viewType", (int)m_type);
    config.sync();
}

void View::restoreConfig()
{
    if (!this->containment())
        return;

    auto config = this->containment()->config();
    m_onPrimary = config.readEntry("onPrimary", true);
    m_byPassWM = config.readEntry("byPassWM", false);
    m_isPreferredForShortcuts = config.readEntry("isPreferredForShortcuts", false);

    //! Send changed signals at the end in order to be sure that saveConfig
    //! wont rewrite default/invalid values
    emit onPrimaryChanged();
    emit byPassWMChanged();
}
//!END configuration functions

}
//!END namespace
