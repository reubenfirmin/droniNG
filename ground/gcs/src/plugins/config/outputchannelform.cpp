/**
 ******************************************************************************
 *
 * @file       outputchannelform.cpp
 * @author     The OpenPilot Team, http://www.openpilot.org Copyright (C) 2011.
 * @author     Tau Labs, http://taulabs.org, Copyright (C) 2014
 * @addtogroup GCSPlugins GCS Plugins
 * @{
 * @addtogroup ConfigPlugin Config Plugin
 * @{
 * @brief Servo output configuration form for the config output gadget
 *****************************************************************************/
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <http://www.gnu.org/licenses/>
 */

#include <cmath>
#include <limits>
#include <QSignalBlocker>
#include <QMap>
#include <QStyle>
#include <QTimer>

#include "outputchannelform.h"
#include "configoutputwidget.h"
#include "actuatorsettings.h"

OutputChannelForm::OutputChannelForm(const int index, QWidget *parent, const bool showLegend)
    : ConfigTaskWidget(parent)
    , ui()
    , m_index(index)
    , m_inChannelTest(false)
    , limitMin(1000)
    , limitMax(2000)
    , minMaxFixed(false)
    , inhibitRangeChanges(false)
{
    ui.setupUi(this);
    ui.actuatorNumber->setAlignment(Qt::AlignCenter);
    ui.actuatorNeutral->hide();
    ui.actuatorNeutralValue->setRange(ui.actuatorNeutral->minimum(), ui.actuatorNeutral->maximum());
    ui.actuatorNeutralValue->setValue(ui.actuatorNeutral->value());
    ui.actuatorNeutralValue->setToolTip(ui.actuatorNeutral->toolTip());
    if (showLegend) ui.legend2->setText(tr("Neutral"));
    connect(ui.actuatorNeutralValue, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int value) {
        ui.actuatorNeutral->setValue(value);
        notifyFormChanged();
    });
    connect(ui.actuatorNeutral, &QSlider::valueChanged, this, [this](int value) {
        const QSignalBlocker blocker(ui.actuatorNeutralValue);
        ui.actuatorNeutralValue->setValue(value);
    });
    connect(ui.actuatorNeutral, &QSlider::rangeChanged, this, [this](int minimum, int maximum) {
        const QSignalBlocker blocker(ui.actuatorNeutralValue);
        ui.actuatorNeutralValue->setRange(minimum, maximum);
    });

    ActuatorSettings *actuatorSettings = ActuatorSettings::GetInstance(getObjectManager());
    Q_ASSERT(actuatorSettings);

    if (!showLegend) {
        // Remove legend
        QGridLayout *grid_layout = dynamic_cast<QGridLayout *>(layout());
        Q_ASSERT(grid_layout);
        for (int col = 0; col < grid_layout->columnCount();
             col++) { // remove every item in first row
            QLayoutItem *item = grid_layout->itemAtPosition(0, col);
            if (!item)
                continue;
            // get widget from layout item
            QWidget *legend_widget = item->widget();
            if (!legend_widget)
                continue;
            // delete widget
            grid_layout->removeWidget(legend_widget);
            delete legend_widget;
        }
    }

    // The convention is Channel 1 to Channel 10.
    ui.actuatorNumber->setText(QString("%1:").arg(m_index + 1));

    // Register for ActuatorSettings changes:
    connect(ui.actuatorMin, &QAbstractSpinBox::editingFinished, this,
            &OutputChannelForm::setChannelRange);
    connect(ui.actuatorMax, &QAbstractSpinBox::editingFinished, this,
            &OutputChannelForm::setChannelRange);
    connect(ui.pb_reverseActuator, &QAbstractButton::clicked, this,
            &OutputChannelForm::reverseChannel);

    // Connect the channel out sliders to our signal in order to send updates in test mode
    connect(ui.actuatorNeutral, &QAbstractSlider::valueChanged, this,
            &OutputChannelForm::sendChannelTest);

    // Connect UI elements to dirty/clean (i.e. changed/unchanged) signal/slot
    connect(ui.actuatorMin, QOverload<int>::of(&QSpinBox::valueChanged), this,
            &OutputChannelForm::notifyFormChanged);
    connect(ui.actuatorMax, QOverload<int>::of(&QSpinBox::valueChanged), this,
            &OutputChannelForm::notifyFormChanged);
    connect(ui.actuatorNeutral, &QAbstractSlider::sliderReleased, this,
            &OutputChannelForm::notifyFormChanged);
    ui.actuatorLink->setChecked(false);
    connect(ui.actuatorLink, &QAbstractButton::toggled, this, &OutputChannelForm::linkToggled);

    disableMouseWheelEvents();
}

OutputChannelForm::~OutputChannelForm()
{
    // Do nothing
}

/**
 * Restrict UI to protect users from accidental misuse.
 */
void OutputChannelForm::enableChannelTest(bool state)
{
    if (m_inChannelTest == state)
        return;
    m_inChannelTest = state;
    ui.actuatorNeutralValue->setVisible(!state);
    ui.actuatorNeutral->setVisible(state);

    if (m_inChannelTest) {
        // Prevent users from changing the minimum & maximum ranges while
        // moving the sliders. Thanks Ivan for the tip :)
        ui.actuatorMin->setEnabled(false);
        ui.actuatorMax->setEnabled(false);
        ui.pb_reverseActuator->setEnabled(false);
    } else {
        ui.actuatorMin->setEnabled(!minMaxFixed);
        ui.actuatorMax->setEnabled(!minMaxFixed);
        ui.pb_reverseActuator->setEnabled(!minMaxFixed);
    }
    alignFields();
}

/**
 * Toggles the channel linked state for use in testing mode
 */
void OutputChannelForm::linkToggled(bool state)
{
    Q_UNUSED(state)

    if (!m_inChannelTest)
        return; // we are not in Test Output mode

    // find the minimum slider value for the linked ones
    if (!parent())
        return;
    int min = 10000;
    int linked_count = 0;
    QList<OutputChannelForm *> outputChannelForms = parent()->findChildren<OutputChannelForm *>();
    // set the linked channels of the parent widget to the same value
    foreach (OutputChannelForm *outputChannelForm, outputChannelForms) {
        if (!outputChannelForm->ui.actuatorLink->checkState())
            continue;
        if (this == outputChannelForm)
            continue;
        int value = outputChannelForm->ui.actuatorNeutral->value();
        if (min > value)
            min = value;
        linked_count++;
    }

    if (linked_count <= 0)
        return; // no linked channels

    // set the linked channels to the same value
    foreach (OutputChannelForm *outputChannelForm, outputChannelForms) {
        if (!outputChannelForm->ui.actuatorLink->checkState())
            continue;
        outputChannelForm->ui.actuatorNeutral->setValue(min);
    }
}

/**
 * Set maximal channel value.
 */
void OutputChannelForm::setMax(int maximum)
{
    setMinmax(ui.actuatorMin->value(), maximum);
}

/**
 * Set minimal channel value.
 */
void OutputChannelForm::setMin(int minimum)
{
    setMinmax(minimum, ui.actuatorMax->value());
}

/**
 * Set minimal and maximal channel value.
 */
void OutputChannelForm::setMinmax(int minimum, int maximum)
{
    inhibitRangeChanges = true;

    if (minMaxFixed) {
        ui.actuatorMin->setValue(limitMin);
        ui.actuatorMax->setValue(limitMax);
    } else {
        ui.actuatorMin->setValue(minimum);
        ui.actuatorMax->setValue(maximum);
    }

    inhibitRangeChanges = false;

    setChannelRange();
}

/**
 * Set neutral of channel.
 */
void OutputChannelForm::setNeutral(int value)
{
    ui.actuatorNeutral->setValue(value);
}

void OutputChannelForm::alignFields()
{
    if (!parent()) return;
    const auto forms = parent()->findChildren<OutputChannelForm *>();
    QMap<int, int> widths;
    int numericWidth = 124;
    for (auto *form : forms) {
        for (auto *spin : {form->ui.actuatorMin, form->ui.actuatorNeutralValue, form->ui.actuatorMax})
            numericWidth = qMax(numericWidth, spin->sizeHint().width() + 16);
        for (int index = 0; index < form->ui.gridLayout->count(); ++index) {
            auto *item = form->ui.gridLayout->itemAt(index);
            auto *widget = item->widget();
            if (!widget || widget->isHidden()) continue;
            int row, column, rows, columns;
            form->ui.gridLayout->getItemPosition(index, &row, &column, &rows, &columns);
            const auto *label = qobject_cast<QLabel *>(widget);
            const int width = label ? label->fontMetrics().horizontalAdvance(label->text()) + 8
                : widget == form->ui.actuatorNeutral ? 200 : widget->sizeHint().width();
            widths[column] = qMax(widths.value(column), width);
        }
    }
    for (int column : {2, 4, 6}) widths[column] = qMax(widths.value(column), numericWidth);
    for (auto *form : forms) {
        auto *grid = form->ui.gridLayout;
        for (int index = 0; index < grid->count(); ++index) {
            auto *widget = grid->itemAt(index)->widget();
            if (!widget) continue;
            int row, column, rows, columns;
            grid->getItemPosition(index, &row, &column, &rows, &columns);
            const int width = widget == form->ui.actuatorLink
                ? widget->style()->pixelMetric(QStyle::PM_IndicatorWidth, nullptr, widget)
                : widths.value(column);
            if (widget->minimumWidth() != width || widget->maximumWidth() != width)
                widget->setFixedWidth(width);
            if (grid->itemAt(index)->alignment() != Qt::AlignCenter)
                grid->setAlignment(widget, Qt::AlignCenter);
        }
        for (auto it = widths.cbegin(); it != widths.cend(); ++it)
            if (grid->columnMinimumWidth(it.key()) != it.value())
                grid->setColumnMinimumWidth(it.key(), it.value());
    }
}

bool OutputChannelForm::event(QEvent *event)
{
    const bool handled = ConfigTaskWidget::event(event);
    // Each channel owns its controls, but all rows share the same column sizes.
    // Recalculate after styling, assignments, protocol changes and test-mode toggles.
    if (event->type() == QEvent::LayoutRequest && !alignmentPending) {
        alignmentPending = true;
        QTimer::singleShot(0, this, [this] {
            alignFields();
            alignmentPending = false;
        });
    }
    return handled;
}

/**
 * Set the channel assignment label.
 */
void OutputChannelForm::setAssignment(const QString &assignment)
{
    ui.actuatorName->setText(assignment);
    alignFields();
}

/**
 * Sets the minimum/maximum value of the channel output sliders.
 * Have to do it here because setMinimum is not a slot.
 *
 * One added trick: if the slider is at its min when the value
 * is changed, then keep it on the min.
 */
void OutputChannelForm::setChannelRange()
{
    if (inhibitRangeChanges) {
        return;
    }

    if (minMaxFixed) { // special case Dshot
        ui.actuatorNeutral->setRange(limitMin, limitMax);
        ui.actuatorNeutral->setEnabled(true);
    } else if (ui.actuatorMin->value() < ui.actuatorMax->value()) {
        ui.actuatorNeutral->setRange(ui.actuatorMin->value(), ui.actuatorMax->value());
        ui.actuatorNeutral->setEnabled(true);
    } else if (ui.actuatorMin->value() > ui.actuatorMax->value()) {
        ui.actuatorNeutral->setRange(ui.actuatorMax->value(), ui.actuatorMin->value());
        ui.actuatorNeutral->setEnabled(true);
    } else {
        // when the min and max is equal, disable this slider to prevent crash
        // from Qt bug: https://bugreports.qt.io/browse/QTBUG-43398
        ui.actuatorNeutral->setRange(ui.actuatorMin->value() - 1, ui.actuatorMin->value() + 1);
        ui.actuatorNeutral->setEnabled(false);
        setNeutral(ui.actuatorMin->value());
    }

    // Force a full slider update
    updateSlider();
}

/**
 * Reverses the channel when the checkbox is clicked
 */
void OutputChannelForm::reverseChannel()
{
    // can't reverse Dshot at this time
    if (!ui.actuatorMin->isEnabled() || !ui.actuatorMax->isEnabled())
        return;

    // Swap the min & max values
    setMinmax(ui.actuatorMax->value(), ui.actuatorMin->value());

    setChannelRange();
}

/**
 * Inverts the slider when the output channel is reversed
 */
/**
 * @brief OutputChannelForm::updateSlider
 */
void OutputChannelForm::updateSlider()
{
    ui.actuatorNeutralValue->setEnabled(ui.actuatorNeutral->isEnabledTo(this));
    // Invert the slider
    if (ui.actuatorMin->value() > ui.actuatorMax->value()) {
        ui.actuatorNeutral->setInvertedAppearance(true);

        // Set the QSlider groove colors so that the fill is on the side of the minimum value
        ui.actuatorNeutral->setProperty("state", "inverted");
    } else {
        ui.actuatorNeutral->setInvertedAppearance(false);

        // Set the QSlider groove colors so that the fill is on the side of the minimum value
        ui.actuatorNeutral->setProperty("state", "normal");
    }

    // Force refresh of style sheet.
    ui.actuatorNeutral->setStyle(QApplication::style());
}

/**
 * Emits the channel value which will be sent to the UAV to move the servo.
 * Returns immediately if we are not in testing mode.
 */
void OutputChannelForm::sendChannelTest(int value)
{
    int in_value = value;

    QSlider *ob = dynamic_cast<QSlider *>(QObject::sender());
    if (!ob)
        return;

    if (ui.actuatorLink->checkState() && parent()) { // the channel is linked to other channels
        QList<OutputChannelForm *> outputChannelForms =
            parent()->findChildren<OutputChannelForm *>();
        // set the linked channels of the parent widget to the same value
        foreach (OutputChannelForm *outputChannelForm, outputChannelForms) {
            if (this == outputChannelForm)
                continue;
            if (!outputChannelForm->ui.actuatorLink->checkState())
                continue;

            int val = in_value;
            if (val < outputChannelForm->ui.actuatorNeutral->minimum())
                val = outputChannelForm->ui.actuatorNeutral->minimum();
            if (val > outputChannelForm->ui.actuatorNeutral->maximum())
                val = outputChannelForm->ui.actuatorNeutral->maximum();

            if (outputChannelForm->ui.actuatorNeutral->value() == val)
                continue;

            outputChannelForm->ui.actuatorNeutral->setValue(val);
        }
    }

    if (!m_inChannelTest)
        return; // we are not in Test Output mode

    emit channelChanged(index(), value);
}

/**
 * @brief OutputChannelForm::setUpdated Slot that receives signals indicating the UI is updated
 */
void OutputChannelForm::notifyFormChanged()
{
    // If we are not in Test Output mode, set form as dirty
    if (!m_inChannelTest) {
        emit formChanged();
    }

    setChannelRange();
}

void OutputChannelForm::updateChannelLimits(int minPulse, int maxPulse, bool digitalProtocol)
{
    limitMin = minPulse;
    limitMax = maxPulse;
    minMaxFixed = digitalProtocol;
    const QString suffix = digitalProtocol ? QString() : tr(" us");
    ui.actuatorNeutralValue->setSuffix(suffix);
    ui.actuatorMin->setSuffix(suffix);
    ui.actuatorMax->setSuffix(suffix);

    if (digitalProtocol) {
        // digital protocol like Dshot, fix min to 0, max to maxPulse, and neutral range goes
        // between minPulse and maxPulse
        ui.actuatorMin->setRange(0, 0);
        ui.actuatorMax->setRange(limitMax, limitMax);
    } else {
        ui.actuatorMin->setRange(limitMin, limitMax);
        ui.actuatorMax->setRange(limitMin, limitMax);
    }
    // these are fixed with digital protocols like Dshot
    ui.actuatorMin->setEnabled(!minMaxFixed && !m_inChannelTest);
    ui.actuatorMax->setEnabled(!minMaxFixed && !m_inChannelTest);
    ui.pb_reverseActuator->setEnabled(!minMaxFixed && !m_inChannelTest);

    setChannelRange();
    alignFields();
}
