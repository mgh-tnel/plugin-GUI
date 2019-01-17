/*
------------------------------------------------------------------

This file is part of a plugin for the Open Ephys GUI
Copyright (C) 2017 Translational NeuroEngineering Laboratory, MGH

------------------------------------------------------------------

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "PhaseCalculatorEditor.h"
#include "PhaseCalculatorCanvas.h"
#include <climits> // INT_MAX
#include <string>  // stoi, stof, stod

PhaseCalculatorEditor::PhaseCalculatorEditor(PhaseCalculator* parentNode, bool useDefaultParameterEditors)
    : VisualizerEditor  (parentNode, 325, useDefaultParameterEditors)
    , extraChanManager  (parentNode)
    , prevExtraChans    (0)
{
    tabText = "Event Phase Plot";
    int filterWidth = 80;

    // make the canvas now, so that restoring its parameters always works.
    canvas = new PhaseCalculatorCanvas(parentNode);

    lowCutLabel = new Label("lowCutL", "Low cut");
    lowCutLabel->setBounds(10, 30, 80, 20);
    lowCutLabel->setFont(Font("Small Text", 12, Font::plain));
    lowCutLabel->setColour(Label::textColourId, Colours::darkgrey);
    addAndMakeVisible(lowCutLabel);

    lowCutEditable = new Label("lowCutE");
    lowCutEditable->setEditable(true);
    lowCutEditable->addListener(this);
    lowCutEditable->setBounds(15, 47, 60, 18);
    lowCutEditable->setText(String(parentNode->lowCut), dontSendNotification);
    lowCutEditable->setColour(Label::backgroundColourId, Colours::grey);
    lowCutEditable->setColour(Label::textColourId, Colours::white);
    addAndMakeVisible(lowCutEditable);

    highCutLabel = new Label("highCutL", "High cut");
    highCutLabel->setBounds(10, 70, 80, 20);
    highCutLabel->setFont(Font("Small Text", 12, Font::plain));
    highCutLabel->setColour(Label::textColourId, Colours::darkgrey);
    addAndMakeVisible(highCutLabel);

    highCutEditable = new Label("highCutE");
    highCutEditable->setEditable(true);
    highCutEditable->addListener(this);
    highCutEditable->setBounds(15, 87, 60, 18);
    highCutEditable->setText(String(parentNode->highCut), dontSendNotification);
    highCutEditable->setColour(Label::backgroundColourId, Colours::grey);
    highCutEditable->setColour(Label::textColourId, Colours::white);
    addAndMakeVisible(highCutEditable);

    hilbertLengthLabel = new Label("hilbertLength", "Buffer length:");
    hilbertLengthLabel->setBounds(filterWidth + 8, 25, 180, 20);
    hilbertLengthLabel->setFont(Font("Small Text", 12, Font::plain));
    hilbertLengthLabel->setColour(Label::textColourId, Colours::darkgrey);
    addAndMakeVisible(hilbertLengthLabel);

    hilbertLengthBox = new ComboBox("Buffer size");
    hilbertLengthBox->setEditableText(true);
    for (int pow = PhaseCalculator::MIN_HILB_LEN_POW; pow <= PhaseCalculator::MAX_HILB_LEN_POW; ++pow)
    {
        hilbertLengthBox->addItem(String(1 << pow), pow);
    }
    hilbertLengthBox->setText(String(parentNode->hilbertLength), dontSendNotification);
    hilbertLengthBox->setTooltip(HILB_LENGTH_TOOLTIP);
    hilbertLengthBox->setBounds(filterWidth + 10, 45, 80, 20);
    hilbertLengthBox->addListener(this);
    addAndMakeVisible(hilbertLengthBox);

    hilbertLengthUnitLabel = new Label("hilbertLengthUnit", "Samp.");
    hilbertLengthUnitLabel->setBounds(filterWidth + 90, 45, 40, 20);
    hilbertLengthUnitLabel->setFont(Font("Small Text", 12, Font::plain));
    hilbertLengthUnitLabel->setColour(Label::textColourId, Colours::darkgrey);
    addAndMakeVisible(hilbertLengthUnitLabel);

    pastLengthLabel = new Label("pastLengthL", "Past:");
    pastLengthLabel->setBounds(filterWidth + 8, 85, 60, 15);
    pastLengthLabel->setFont(Font("Small Text", 12, Font::plain));
    pastLengthLabel->setColour(Label::backgroundColourId, Colour(230, 168, 0));
    pastLengthLabel->setColour(Label::textColourId, Colours::darkgrey);
    addAndMakeVisible(pastLengthLabel);

    predLengthLabel = new Label("predLengthL", "Future:");
    predLengthLabel->setBounds(filterWidth + 70, 85, 60, 15);
    predLengthLabel->setFont(Font("Small Text", 12, Font::plain));
    predLengthLabel->setColour(Label::backgroundColourId, Colour(102, 140, 255));
    predLengthLabel->setColour(Label::textColourId, Colours::darkgrey);
    addAndMakeVisible(predLengthLabel);

    pastLengthEditable = new Label("pastLengthE");
    pastLengthEditable->setEditable(true);
    pastLengthEditable->addListener(this);
    pastLengthEditable->setText(String(parentNode->hilbertLength - parentNode->predictionLength), dontSendNotification);
    pastLengthEditable->setBounds(filterWidth + 8, 102, 60, 18);
    pastLengthEditable->setColour(Label::backgroundColourId, Colours::grey);
    pastLengthEditable->setColour(Label::textColourId, Colours::white);
    addAndMakeVisible(pastLengthEditable);

    predLengthEditable = new Label("predLengthE");
    predLengthEditable->setEditable(true);
    predLengthEditable->addListener(this);
    predLengthEditable->setText(String(parentNode->predictionLength), dontSendNotification);
    predLengthEditable->setBounds(filterWidth + 70, 102, 60, 18);
    predLengthEditable->setColour(Label::backgroundColourId, Colours::grey);
    predLengthEditable->setColour(Label::textColourId, Colours::white);
    addAndMakeVisible(predLengthEditable);

    predLengthSlider = new Slider("predLength");
    predLengthSlider->setLookAndFeel(&v3LookAndFeel);
    predLengthSlider->setSliderStyle(Slider::LinearBar);
    predLengthSlider->setTextBoxStyle(Slider::NoTextBox, false, 40, 20);
    predLengthSlider->setScrollWheelEnabled(false);
    predLengthSlider->setBounds(filterWidth + 8, 70, 122, 10);
    predLengthSlider->setColour(Slider::thumbColourId, Colour(255, 187, 0));
    predLengthSlider->setColour(Slider::backgroundColourId, Colour(51, 102, 255));
    predLengthSlider->setTooltip(PRED_LENGTH_TOOLTIP);
    predLengthSlider->addListener(this);
    predLengthSlider->setRange(0, parentNode->hilbertLength, 1);
    predLengthSlider->setValue(parentNode->hilbertLength - parentNode->predictionLength, dontSendNotification);
    addAndMakeVisible(predLengthSlider);

    recalcIntervalLabel = new Label("recalcL", "AR Refresh:");
    recalcIntervalLabel->setBounds(filterWidth + 140, 25, 100, 20);
    recalcIntervalLabel->setFont(Font("Small Text", 12, Font::plain));
    recalcIntervalLabel->setColour(Label::textColourId, Colours::darkgrey);
    addAndMakeVisible(recalcIntervalLabel);

    recalcIntervalEditable = new Label("recalcE");
    recalcIntervalEditable->setEditable(true);
    recalcIntervalEditable->addListener(this);
    recalcIntervalEditable->setBounds(filterWidth + 145, 44, 55, 18);
    recalcIntervalEditable->setColour(Label::backgroundColourId, Colours::grey);
    recalcIntervalEditable->setColour(Label::textColourId, Colours::white);
    recalcIntervalEditable->setText(String(parentNode->calcInterval), dontSendNotification);
    recalcIntervalEditable->setTooltip(RECALC_INTERVAL_TOOLTIP);
    addAndMakeVisible(recalcIntervalEditable);

    recalcIntervalUnit = new Label("recalcU", "ms");
    recalcIntervalUnit->setBounds(filterWidth + 200, 47, 25, 15);
    recalcIntervalUnit->setFont(Font("Small Text", 12, Font::plain));
    recalcIntervalUnit->setColour(Label::textColourId, Colours::darkgrey);
    addAndMakeVisible(recalcIntervalUnit);

    arOrderLabel = new Label("arOrderL", "Order:");
    arOrderLabel->setBounds(filterWidth + 140, 65, 60, 20);
    arOrderLabel->setFont(Font("Small Text", 12, Font::plain));
    arOrderLabel->setColour(Label::textColourId, Colours::darkgrey);
    addAndMakeVisible(arOrderLabel);

    arOrderEditable = new Label("arOrderE");
    arOrderEditable->setEditable(true);
    arOrderEditable->addListener(this);
    arOrderEditable->setBounds(filterWidth + 195, 66, 25, 18);
    arOrderEditable->setColour(Label::backgroundColourId, Colours::grey);
    arOrderEditable->setColour(Label::textColourId, Colours::white);
    arOrderEditable->setText(String(parentNode->arOrder), sendNotificationAsync);
    arOrderEditable->setTooltip(AR_ORDER_TOOLTIP);
    addAndMakeVisible(arOrderEditable);

    outputModeLabel = new Label("outputModeL", "Output:");
    outputModeLabel->setBounds(filterWidth + 140, 87, 70, 20);
    outputModeLabel->setFont(Font("Small Text", 12, Font::plain));
    outputModeLabel->setColour(Label::textColourId, Colours::darkgrey);
    addAndMakeVisible(outputModeLabel);

    outputModeBox = new ComboBox("outputModeB");
    outputModeBox->addItem("PHASE", PH);
    outputModeBox->addItem("MAG", MAG);
    outputModeBox->addItem("PH+MAG", PH_AND_MAG);
    outputModeBox->addItem("IMAG", IM);
    outputModeBox->setSelectedId(parentNode->outputMode);
    outputModeBox->setTooltip(OUTPUT_MODE_TOOLTIP);
    outputModeBox->setBounds(filterWidth + 145, 105, 76, 19);
    outputModeBox->addListener(this);
    addAndMakeVisible(outputModeBox);

    // new channels should be disabled by default
    channelSelector->paramButtonsToggledByDefault(false);
}

PhaseCalculatorEditor::~PhaseCalculatorEditor() {}

void PhaseCalculatorEditor::comboBoxChanged(ComboBox* comboBoxThatHasChanged)
{
    PhaseCalculator* processor = static_cast<PhaseCalculator*>(getProcessor());

    if (comboBoxThatHasChanged == hilbertLengthBox)
    {
        int newId = hilbertLengthBox->getSelectedId();
        int newHilbertLength;
        if (newId) // one of the items in the list is selected
        {            
            newHilbertLength = (1 << newId);
        }
        else if (!updateControl(comboBoxThatHasChanged, 1 << PhaseCalculator::MIN_HILB_LEN_POW,
            1 << PhaseCalculator::MAX_HILB_LEN_POW, processor->hilbertLength, &newHilbertLength))
        {
            return;
        }

        processor->setParameter(HILBERT_LENGTH, static_cast<float>(newHilbertLength));
    }
    else if (comboBoxThatHasChanged == outputModeBox)
    {
        processor->setParameter(OUTPUT_MODE, static_cast<float>(outputModeBox->getSelectedId()));
    }
}

void PhaseCalculatorEditor::labelTextChanged(Label* labelThatHasChanged)
{
    PhaseCalculator* processor = static_cast<PhaseCalculator*>(getProcessor());

    int sliderMax = static_cast<int>(predLengthSlider->getMaximum());

    if (labelThatHasChanged == pastLengthEditable)
    {
        int intInput;
        bool valid = updateControl(labelThatHasChanged, 0, processor->hilbertLength,
            processor->hilbertLength - processor->predictionLength, &intInput);

        if (valid)
        {
            processor->setParameter(PAST_LENGTH, static_cast<float>(intInput));
        }        
    }
    else if (labelThatHasChanged == predLengthEditable)
    {
        int intInput;
        bool valid = updateControl(labelThatHasChanged, 0, processor->hilbertLength,
            processor->predictionLength, &intInput);
        
        if (valid)
        {
            processor->setParameter(PRED_LENGTH, static_cast<float>(intInput));
        }
    }
    else if (labelThatHasChanged == recalcIntervalEditable)
    {
        int intInput;
        bool valid = updateControl(labelThatHasChanged, 0, INT_MAX, processor->calcInterval, &intInput);

        if (valid)
        {
            processor->setParameter(RECALC_INTERVAL, static_cast<float>(intInput));
        }
    }
    else if (labelThatHasChanged == arOrderEditable)
    {
        int intInput;
        bool valid = updateControl(labelThatHasChanged, 1, INT_MAX, processor->arOrder, &intInput);

        if (valid)
        {
            processor->setParameter(AR_ORDER, static_cast<float>(intInput));
        }
    }
    else if (labelThatHasChanged == lowCutEditable)
    {
        float floatInput;
        bool valid = updateControl(labelThatHasChanged, PhaseCalculator::PASSBAND_EPS,
            processor->minNyquist - PhaseCalculator::PASSBAND_EPS, processor->lowCut, &floatInput);

        if (valid)
        {
            processor->setParameter(LOWCUT, floatInput);
        }
    }
    else if (labelThatHasChanged == highCutEditable)
    {
        float floatInput;
        bool valid = updateControl(labelThatHasChanged, 2 * PhaseCalculator::PASSBAND_EPS,
            processor->minNyquist, processor->highCut, &floatInput);

        if (valid) 
        {
            processor->setParameter(HIGHCUT, floatInput);
        }
    }
}

void PhaseCalculatorEditor::sliderEvent(Slider* slider)
{
    if (slider == predLengthSlider)
    {
        int newVal = static_cast<int>(slider->getValue());
        int maxVal = static_cast<int>(slider->getMaximum());
        getProcessor()->setParameter(PRED_LENGTH, static_cast<float>(maxVal - newVal));
    }
}

void PhaseCalculatorEditor::channelChanged(int chan, bool newState)
{
    auto pc = static_cast<PhaseCalculator*>(getProcessor());    
    if (chan < pc->getNumInputs())
    {
        Array<int> activeInputs = pc->getActiveInputs();
        if (newState && activeInputs.size() > pc->numActiveChansAllocated)
        {            
            pc->addActiveChannel();
        }

        if (pc->outputMode == PH_AND_MAG)
        {
            if (newState)
            {
                extraChanManager.addExtraChan(chan, activeInputs);
            }
            else
            {
                extraChanManager.removeExtraChan(chan, activeInputs);
            }

            // Update signal chain to add/remove output channels if necessary
            CoreServices::updateSignalChain(this);
        }
        else
        {
            // Can just do a partial update
            pc->updateMinNyquist();     // minNyquist may have changed depending on active chans
            pc->setFilterParameters();  // need to update in case the passband has changed
            updateVisualizer();         // update the available continuous channels for visualizer
        }
    }
}

void PhaseCalculatorEditor::startAcquisition()
{
    GenericEditor::startAcquisition();
    hilbertLengthBox->setEnabled(false);
    predLengthSlider->setEnabled(false);
    pastLengthEditable->setEnabled(false);
    predLengthEditable->setEnabled(false);
    lowCutEditable->setEnabled(false);
    highCutEditable->setEnabled(false);
    arOrderEditable->setEnabled(false);
    outputModeBox->setEnabled(false);
    channelSelector->inactivateButtons();
}

void PhaseCalculatorEditor::stopAcquisition()
{
    GenericEditor::stopAcquisition();
    hilbertLengthBox->setEnabled(true);
    predLengthSlider->setEnabled(true);
    pastLengthEditable->setEnabled(true);
    predLengthEditable->setEnabled(true);
    lowCutEditable->setEnabled(true);
    highCutEditable->setEnabled(true);
    arOrderEditable->setEnabled(true);
    outputModeBox->setEnabled(true);
    channelSelector->activateButtons();
}

Visualizer* PhaseCalculatorEditor::createNewCanvas()
{
    return canvas;
}

void PhaseCalculatorEditor::updateSettings()
{
    auto pc = static_cast<PhaseCalculator*>(getProcessor());

    // only care about any of this stuff if we have extra channels
    // (and preserve when deselecting/reselecting PH_AND_MAG)
    if (pc->outputMode != PH_AND_MAG || channelSelector == nullptr) { return; }
    
    int numChans = pc->getNumOutputs();
    int numInputs = pc->getNumInputs();
    int extraChans = numChans - numInputs;

    int prevNumChans = channelSelector->getNumChannels();
    int prevNumInputs = prevNumChans - prevExtraChans;
    prevExtraChans = extraChans; // update for next time

    extraChanManager.resize(extraChans);
    channelSelector->setNumChannels(numChans);

    // super hacky, access record buttons to add or remove listeners
    Component* rbmComponent = channelSelector->getChildComponent(9);
    auto recordButtonManager = dynamic_cast<ButtonGroupManager*>(rbmComponent);
    if (recordButtonManager == nullptr)
    {
        jassertfalse;
        return;
    }

    // remove listeners on channels that are no longer "extra channels"
    // and set their record status to false since they're actually new channels
    for (int chan = prevNumInputs; chan < jmin(prevNumChans, numInputs); ++chan)
    {
        juce::Button* recordButton = recordButtonManager->getButtonAt(chan);
        recordButton->removeListener(&extraChanManager);
        // make sure listener really gets called
        recordButton->setToggleState(true, dontSendNotification);
        channelSelector->setRecordStatus(chan, false);
    }

    // add listeners for current "extra channels" and restore record statuses
    // (it's OK if addListener gets called more than once for a button)
    for (int eChan = 0; eChan < extraChans; ++eChan)
    {
        int chan = numInputs + eChan;
        juce::Button* recordButton = recordButtonManager->getButtonAt(chan);
        recordButton->removeListener(&extraChanManager);
        // make sure listener really gets called
        bool recordStatus = extraChanManager.getRecordStatus(eChan);
        recordButton->setToggleState(!recordStatus, dontSendNotification);
        channelSelector->setRecordStatus(chan, recordStatus);
        recordButton->addListener(&extraChanManager);
    }
}

void PhaseCalculatorEditor::saveCustomParameters(XmlElement* xml)
{
    VisualizerEditor::saveCustomParameters(xml);

    xml->setAttribute("Type", "PhaseCalculatorEditor");
    PhaseCalculator* processor = (PhaseCalculator*)(getProcessor());
    
    XmlElement* paramValues = xml->createNewChildElement("VALUES");
    paramValues->setAttribute("hilbertLength", processor->hilbertLength);
    paramValues->setAttribute("predLength", processor->predictionLength);
    paramValues->setAttribute("calcInterval", processor->calcInterval);
    paramValues->setAttribute("arOrder", processor->arOrder);
    paramValues->setAttribute("lowCut", processor->lowCut);
    paramValues->setAttribute("highCut", processor->highCut);
    paramValues->setAttribute("outputMode", processor->outputMode);
}

void PhaseCalculatorEditor::loadCustomParameters(XmlElement* xml)
{
    VisualizerEditor::loadCustomParameters(xml);

    forEachXmlChildElementWithTagName(*xml, xmlNode, "VALUES")
    {
        // some parameters have two fallbacks for backwards compatability
        hilbertLengthBox->setText(xmlNode->getStringAttribute("hilbertLength", 
            xmlNode->getStringAttribute("processLength", hilbertLengthBox->getText())), sendNotificationSync);
        predLengthEditable->setText(xmlNode->getStringAttribute("predLength",
            xmlNode->getStringAttribute("numFuture", predLengthEditable->getText())), sendNotificationSync);
        recalcIntervalEditable->setText(xmlNode->getStringAttribute("calcInterval", recalcIntervalEditable->getText()), sendNotificationSync);
        arOrderEditable->setText(xmlNode->getStringAttribute("arOrder", arOrderEditable->getText()), sendNotificationSync);
        lowCutEditable->setText(xmlNode->getStringAttribute("lowCut", lowCutEditable->getText()), sendNotificationSync);
        highCutEditable->setText(xmlNode->getStringAttribute("highCut", highCutEditable->getText()), sendNotificationSync);
        outputModeBox->setSelectedId(xmlNode->getIntAttribute("outputMode", outputModeBox->getSelectedId()), sendNotificationSync);
    }
}

void PhaseCalculatorEditor::refreshLowCut()
{
    auto p = static_cast<PhaseCalculator*>(getProcessor());
    lowCutEditable->setText(String(p->lowCut), dontSendNotification);
}

void PhaseCalculatorEditor::refreshHighCut()
{
    auto p = static_cast<PhaseCalculator*>(getProcessor());
    highCutEditable->setText(String(p->highCut), dontSendNotification);
}

void PhaseCalculatorEditor::refreshPredLength()
{
    auto p = static_cast<PhaseCalculator*>(getProcessor());
    int newPredLength = p->predictionLength;

    jassert(predLengthSlider->getMinimum() == 0);
    int maximum = static_cast<int>(predLengthSlider->getMaximum());
    jassert(newPredLength >= 0 && newPredLength <= maximum);

    predLengthSlider->setValue(maximum - newPredLength, dontSendNotification);
    pastLengthEditable->setText(String(maximum - newPredLength), dontSendNotification);
    predLengthEditable->setText(String(newPredLength), dontSendNotification);
}

void PhaseCalculatorEditor::refreshHilbertLength()
{
    auto p = static_cast<PhaseCalculator*>(getProcessor());
    int newHilbertLength = p->hilbertLength;

    hilbertLengthBox->setText(String(newHilbertLength), dontSendNotification);
    predLengthSlider->setRange(0, newHilbertLength, 1);

    // if possible, maintain prediction length while making past + pred = hilbertLength
    int sliderVal = static_cast<int>(predLengthSlider->getValue());
    pastLengthEditable->setText(String(sliderVal), dontSendNotification);
    predLengthEditable->setText(String(newHilbertLength - sliderVal), dontSendNotification);
}

void PhaseCalculatorEditor::refreshVisContinuousChan()
{
    auto p = static_cast<PhaseCalculator*>(getProcessor());
    if (canvas != nullptr)
    {
        auto c = static_cast<PhaseCalculatorCanvas*>(canvas.get());
        c->displayContinuousChan(p->visContinuousChannel);
    }
}

// static utilities

template<>
int PhaseCalculatorEditor::fromString<int>(const char* in)
{
    return std::stoi(in);
}

template<>
float PhaseCalculatorEditor::fromString<float>(const char* in)
{
    return std::stof(in);
}

template<>
double PhaseCalculatorEditor::fromString<double>(const char* in)
{
    return std::stod(in);
}


// -------- ExtraChanManager ---------

PhaseCalculatorEditor::ExtraChanManager::ExtraChanManager(const PhaseCalculator* processor)
    : p(processor)
{}

void PhaseCalculatorEditor::ExtraChanManager::buttonClicked(Button* button)
{
    int numInputs = p->getNumInputs();
    int chanInd = button->getParentComponent()->getIndexOfChildComponent(button);
    int extraChanInd = chanInd - numInputs;
    if (extraChanInd < 0 || extraChanInd >= recordStatus.size())
    {
        jassertfalse;
        return;
    }
    recordStatus.set(extraChanInd, button->getToggleState());
}

void PhaseCalculatorEditor::ExtraChanManager::addExtraChan(int inputChan, const Array<int>& activeInputs)
{
    int newInputIndex = activeInputs.indexOf(inputChan);
    jassert(newInputIndex <= recordStatus.size());
    recordStatus.insert(newInputIndex, false);
}

void PhaseCalculatorEditor::ExtraChanManager::removeExtraChan(int inputChan, const Array<int>& activeInputs)
{
    // find # of lower-index active inputs
    int i = 0;
    int numActiveInputs = activeInputs.size();
    for (; i < numActiveInputs && activeInputs[i] < inputChan; ++i);
    jassert(i < recordStatus.size());
    recordStatus.remove(i);
}

void PhaseCalculatorEditor::ExtraChanManager::resize(int numExtraChans)
{
    recordStatus.resize(numExtraChans);
}

bool PhaseCalculatorEditor::ExtraChanManager::getRecordStatus(int extraChan) const
{
    return recordStatus[extraChan];
}