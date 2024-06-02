/*  This file is part of the KDE project
    SPDX-FileCopyrightText: 2024 Xaver Hugl <xaver.hugl@gmail.com>

    SPDX-License-Identifier: LGPL-2.0-or-later
*/
#include "kwinbrightness.h"

#include <KScreen/ConfigMonitor>
#include <KScreen/GetConfigOperation>

KWinDisplayDetector::KWinDisplayDetector(QObject *parent)
    : DisplayBrightnessDetector(parent)
{
    m_setConfigTimer.setInterval(0);
    m_setConfigTimer.setSingleShot(true);
    connect(&m_setConfigTimer, &QTimer::timeout, this, &KWinDisplayDetector::setConfig);
}

KWinDisplayDetector::~KWinDisplayDetector()
{
}

void KWinDisplayDetector::detect()
{
    const auto op = new KScreen::GetConfigOperation(KScreen::GetConfigOperation::Option::NoEDID, this);
    connect(op, &KScreen::GetConfigOperation::finished, this, [this](KScreen::ConfigOperation *configOp) {
        if (configOp->hasError()) {
            Q_EMIT detectionFinished(false);
            return;
        }
        m_config = static_cast<KScreen::GetConfigOperation *>(configOp)->config();

        KScreen::ConfigMonitor::instance()->addConfig(m_config);
        connect(KScreen::ConfigMonitor::instance(), &KScreen::ConfigMonitor::configurationChanged, this, &KWinDisplayDetector::checkOutputs);
        checkOutputs();
        Q_EMIT detectionFinished(true);
    });
}

QList<DisplayBrightness *> KWinDisplayDetector::displays() const
{
    return m_displayList;
}

void KWinDisplayDetector::checkOutputs()
{
    const KScreen::OutputList outputs = m_config->outputs();
    bool changed = false;
    // remove all actually removed outputs
    changed |= std::erase_if(m_displays, [&outputs](const auto &pair) {
        return std::ranges::none_of(outputs, [output = pair.first](const auto &other) {
            return output == other.get();
        });
    });
    // remove all that aren't HDR (anymore)
    changed |= std::erase_if(m_displays, [](const auto &pair) {
        return !pair.first->isHdrEnabled();
    });
    for (const auto &output : outputs) {
        if (!output->isHdrEnabled()) {
            continue;
        }
        auto &brightness = m_displays[output.get()];
        if (!brightness) {
            brightness = std::make_unique<KWinDisplayBrightness>(output, this);
            changed = true;
        }
    }
    if (changed) {
        QList<DisplayBrightness *> newList;
        newList.reserve(m_displays.size());
        for (const auto &[output, brightness] : m_displays) {
            newList.push_back(brightness.get());
        }
        m_displayList = newList;
        Q_EMIT displaysChanged();
    }
}

void KWinDisplayDetector::scheduleSetConfig()
{
    if (m_setConfigOp) {
        m_setConfigOutOfDate = true;
    }
    m_setConfigTimer.start();
}

void KWinDisplayDetector::setConfig()
{
    if (m_setConfigOp) {
        return;
    }
    m_setConfigOutOfDate = false;
    for (const auto &[output, display] : m_displays) {
        display->applyPendingBrightness();
    }
    m_setConfigOp = new KScreen::SetConfigOperation(m_config);
    connect(m_setConfigOp, &KScreen::SetConfigOperation::finished, this, &KWinDisplayDetector::setConfigDone);
}

void KWinDisplayDetector::setConfigDone()
{
    m_setConfigOp = nullptr;
    if (m_setConfigOutOfDate) {
        setConfig();
    } else {
        for (const auto &[output, display] : m_displays) {
            display->setConfigOperationDone();
        }
    }
}

KWinDisplayBrightness::KWinDisplayBrightness(const KScreen::OutputPtr &output, KWinDisplayDetector *detector)
    : m_output(output)
    , m_detector(detector)
    , m_desiredBrightness(m_output->brightness())
{
    connect(m_output.get(), &KScreen::Output::brightnessChanged, this, &KWinDisplayBrightness::handleBrightnessChanged);
}

int KWinDisplayBrightness::knownSafeMinBrightness() const
{
    return 0;
}

int KWinDisplayBrightness::maxBrightness() const
{
    return 10'000;
}

int KWinDisplayBrightness::brightness() const
{
    return std::round(m_output->brightness() * 10'000);
}

void KWinDisplayBrightness::setBrightness(int brightness)
{
    m_desiredBrightness = brightness / 10'000.0;
    m_detector->scheduleSetConfig();
}

void KWinDisplayBrightness::handleBrightnessChanged()
{
    if (m_inhibitChangeSignal) {
        return;
    }
    Q_EMIT brightnessChanged(std::round(m_output->brightness() * 10'000), 10'000);
}

void KWinDisplayBrightness::applyPendingBrightness()
{
    // this will trigger handleBrightnessChanged
    m_output->setBrightness(m_desiredBrightness);
    m_inhibitChangeSignal = true;
}

void KWinDisplayBrightness::setConfigOperationDone()
{
    m_inhibitChangeSignal = false;
    if (m_desiredBrightness != std::round(m_output->brightness() * 10'000)) {
        handleBrightnessChanged();
    }
}