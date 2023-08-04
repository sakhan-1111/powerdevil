/*
 *  SPDX-FileCopyrightText: 2008-2010 Dario Freddi <drf@kde.org>
 *  SPDX-FileCopyrightText: 2023 Jakob Petsovits <jpetso@petsovits.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ExternalServiceSettings.h"

// KDE
#include <KAuth/Action>
#include <KAuth/ExecuteJob>
#include <KLocalizedString>
#include <Solid/Battery>
#include <Solid/Device>

// Qt
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusServiceWatcher>
#include <QWindow>

namespace
{
constexpr int ChargeThresholdUnsupported = -1;
}

namespace PowerDevil
{

ExternalServiceSettings::ExternalServiceSettings(QObject *parent)
    : QObject(parent)
    , m_chargeStartThreshold(ChargeThresholdUnsupported)
    , m_chargeStopThreshold(ChargeThresholdUnsupported)
    , m_backendChargeStartThreshold(ChargeThresholdUnsupported)
    , m_backendChargeStopThreshold(ChargeThresholdUnsupported)
    , m_chargeStopThresholdMightNeedReconnect(false)
{
}

void ExternalServiceSettings::load(QWindow *parentWindowForKAuth)
{
    KAuth::Action action(QStringLiteral("org.kde.powerdevil.chargethresholdhelper.getthreshold"));
    action.setHelperId(QStringLiteral("org.kde.powerdevil.chargethresholdhelper"));
    action.setParentWindow(parentWindowForKAuth);
    KAuth::ExecuteJob *job = action.execute();
    connect(job, &KAuth::ExecuteJob::result, this, [this, job]() {
        if (!job->error()) {
            const auto data = job->data();
            setBackendChargeStartThreshold(data.value(QStringLiteral("chargeStartThreshold")).toInt());
            setBackendChargeStopThreshold(data.value(QStringLiteral("chargeStopThreshold")).toInt());
            setChargeStopThreshold(m_backendChargeStopThreshold);
            setChargeStartThreshold(m_backendChargeStartThreshold);
        } else {
            qWarning() << "org.kde.powerdevil.chargethresholdhelper.getthreshold failed:" << job->errorText();
            setBackendChargeStartThreshold(ChargeThresholdUnsupported);
            setBackendChargeStopThreshold(ChargeThresholdUnsupported);
        }
    });
    job->start();
}

void ExternalServiceSettings::save(QWindow *parentWindowForKAuth)
{
    if ((isChargeStartThresholdSupported() && m_chargeStartThreshold != m_backendChargeStartThreshold)
        || (isChargeStopThresholdSupported() && m_chargeStopThreshold != m_backendChargeStopThreshold)) {
        int newChargeStartThreshold = isChargeStartThresholdSupported() ? m_chargeStartThreshold : ChargeThresholdUnsupported;
        int newChargeStopThreshold = isChargeStopThresholdSupported() ? m_chargeStopThreshold : ChargeThresholdUnsupported;

        KAuth::Action action(QStringLiteral("org.kde.powerdevil.chargethresholdhelper.setthreshold"));
        action.setHelperId(QStringLiteral("org.kde.powerdevil.chargethresholdhelper"));
        action.setArguments({
            {QStringLiteral("chargeStartThreshold"), newChargeStartThreshold},
            {QStringLiteral("chargeStopThreshold"), newChargeStopThreshold},
        });
        action.setParentWindow(parentWindowForKAuth);
        KAuth::ExecuteJob *job = action.execute();

        connect(job, &KAuth::ExecuteJob::result, this, [this, job, newChargeStartThreshold, newChargeStopThreshold]() {
            if (!job->error()) {
                setBackendChargeStartThreshold(newChargeStartThreshold);
                setBackendChargeStopThreshold(newChargeStopThreshold);
            } else {
                qWarning() << "org.kde.powerdevil.chargethresholdhelper.setthreshold failed:" << job->errorText();
            }
            setChargeStopThreshold(m_backendChargeStopThreshold);
            setChargeStartThreshold(m_backendChargeStartThreshold);
        });
        job->start();
    }
}

bool ExternalServiceSettings::isSaveNeeded() const
{
    return (isChargeStartThresholdSupported() && m_chargeStartThreshold != m_backendChargeStartThreshold)
        || (isChargeStopThresholdSupported() && m_chargeStopThreshold != m_backendChargeStopThreshold);
}

bool ExternalServiceSettings::isChargeStartThresholdSupported() const
{
    return m_backendChargeStartThreshold != ChargeThresholdUnsupported;
}

bool ExternalServiceSettings::isChargeStopThresholdSupported() const
{
    return m_backendChargeStopThreshold != ChargeThresholdUnsupported;
}

void ExternalServiceSettings::setBackendChargeStartThreshold(int threshold)
{
    bool wasChargeStartThresholdSupported = isChargeStartThresholdSupported();
    m_backendChargeStartThreshold = threshold;
    if (wasChargeStartThresholdSupported != isChargeStartThresholdSupported()) {
        Q_EMIT isChargeStartThresholdSupportedChanged();
    }
}

void ExternalServiceSettings::setBackendChargeStopThreshold(int threshold)
{
    bool wasChargeStopThresholdSupported = isChargeStopThresholdSupported();
    m_backendChargeStopThreshold = threshold;
    if (wasChargeStopThresholdSupported != isChargeStopThresholdSupported()) {
        Q_EMIT isChargeStopThresholdSupportedChanged();
    }
}

int ExternalServiceSettings::chargeStartThreshold() const
{
    return m_chargeStartThreshold;
}

int ExternalServiceSettings::chargeStopThreshold() const
{
    return m_chargeStopThreshold;
}

void ExternalServiceSettings::setChargeStartThreshold(int threshold)
{
    if (threshold == m_chargeStartThreshold) {
        return;
    }
    m_chargeStartThreshold = threshold;
    Q_EMIT chargeStartThresholdChanged();
    Q_EMIT settingsChanged();
}

void ExternalServiceSettings::setChargeStopThreshold(int threshold)
{
    if (threshold == m_chargeStopThreshold) {
        return;
    }
    m_chargeStopThreshold = threshold;
    Q_EMIT chargeStopThresholdChanged();

    if (m_chargeStopThreshold > m_backendChargeStopThreshold) {
        // Only show message if there is actually a charging or fully charged battery
        const auto devices = Solid::Device::listFromType(Solid::DeviceInterface::Battery, QString());
        for (const Solid::Device &device : devices) {
            const Solid::Battery *b = qobject_cast<const Solid::Battery *>(device.asDeviceInterface(Solid::DeviceInterface::Battery));
            if (b->chargeState() == Solid::Battery::Charging || b->chargeState() == Solid::Battery::FullyCharged) {
                setChargeStopThresholdMightNeedReconnect(true);
                break;
            }
        }
    } else {
        setChargeStopThresholdMightNeedReconnect(false);
    }

    Q_EMIT settingsChanged();
}

bool ExternalServiceSettings::chargeStopThresholdMightNeedReconnect() const
{
    return m_chargeStopThresholdMightNeedReconnect;
}

void ExternalServiceSettings::setChargeStopThresholdMightNeedReconnect(bool mightNeedReconnect)
{
    if (mightNeedReconnect == m_chargeStopThresholdMightNeedReconnect) {
        return;
    }
    m_chargeStopThresholdMightNeedReconnect = mightNeedReconnect;
    Q_EMIT chargeStopThresholdMightNeedReconnectChanged();
}

} // namespace PowerDevil

#include "moc_ExternalServiceSettings.cpp"
