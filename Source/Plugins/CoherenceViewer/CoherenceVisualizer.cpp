/*
------------------------------------------------------------------

This file is part of a plugin for the Open Ephys GUI
Copyright (C) 2019 Translational NeuroEngineering Laboratory

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

#include "CoherenceVisualizer.h"

CoherenceVisualizer::CoherenceVisualizer(CoherenceNode* n)
    : viewport  (new Viewport())
    , canvas    (new Component("canvas"))
    , processor (n)
{
    refreshRate = 2;
    juce::Rectangle<int> canvasBounds(0, 0, 1, 1);
    juce::Rectangle<int> bounds;

    curComb = 0;

    const int TEXT_HT = 18;

    // ------- Options Title ------- //
    int xPos = 5;
    optionsTitle = new Label("OptionsTitle", "Coherence Viewer Additional Settings");
    optionsTitle->setBounds(bounds = { xPos, 30, 400, 50 });
    optionsTitle->setFont(Font(20, Font::bold));
    canvas->addAndMakeVisible(optionsTitle);
    canvasBounds = canvasBounds.getUnion(bounds);

    // ------- OGrouping Titles ------- //
    group1Title = new Label("Group1Title", "G1 Chans");
    group1Title->setBounds(bounds = { xPos, 130, 50, 50 });
    group1Title->setFont(Font(20, Font::bold));
    canvas->addAndMakeVisible(group1Title);
    canvasBounds = canvasBounds.getUnion(bounds);

    group2Title = new Label("Group2Title", "G2 Chans");
    group2Title->setBounds(bounds = { xPos + 50, 130, 50, 50 });
    group2Title->setFont(Font(20, Font::bold));
    canvas->addAndMakeVisible(group2Title);
    canvasBounds = canvasBounds.getUnion(bounds);

    // ------- Group Boxes ------- //
	int numInputs = processor->getActiveInputs().size();
	for (int i = 0; i < numInputs; i+=1)
	{
		// Group 1 buttons
		ElectrodeButton* button = new ElectrodeButton(i + 1);
        button->setBounds(bounds = { xPos, 180 + i * 15, 20, 15 });
		button->setToggleState(false, dontSendNotification);
		button->setRadioGroupId(0);
		button->setButtonText(String(i + 1));
        button->addListener(this);
        group1Buttons.add(button);
        canvasBounds = canvasBounds.getUnion(bounds);
		
		canvas->addAndMakeVisible(button);
		
		// Group 2 buttons
		ElectrodeButton* button2 = new ElectrodeButton(i + 1);
        button2->setBounds(bounds = { xPos + 50, 180 + i * 15, 20, 15 });
		button2->setToggleState(false, dontSendNotification);
		button2->setRadioGroupId(0);
		button2->setButtonText(String(i + 1));
        button2->addListener(this);
        group2Buttons.add(button2);
        canvasBounds = canvasBounds.getUnion(bounds);

        canvas->addAndMakeVisible(button2);
        
	}

    int yPos = 90;

    // ------- Combination Choice ------- //
    combinationBox = new ComboBox("Combination Selection Box");
    combinationBox->setTooltip("Combination to graph");
    combinationBox->setBounds(bounds = { xPos, 90, 70, TEXT_HT });
    combinationBox->addListener(this);
    canvas->addAndMakeVisible(combinationBox);

    // ------- Exponential ------- //
    static const String linearTip = "Linear weighting of coherence.";
    static const String expTip = "Exponential weighting of coherence. Set alpha using -1/alpha weighting.";
    static const String resetTip = "Clears and resets the algorithm. Must be done after changes are made on this page!";

    xPos += 100;

    resetTFR = new TextButton("Reset Algorithm");
    resetTFR->setBounds(bounds = { xPos, yPos, 90, TEXT_HT });
    resetTFR->addListener(this);
    resetTFR->setTooltip(resetTip);
    canvas->addAndMakeVisible(resetTFR);
    canvasBounds = canvasBounds.getUnion(bounds);
    
    yPos += 40;
    linearButton = new ToggleButton("Linear");
    linearButton->setBounds(bounds = { xPos, yPos, 90, TEXT_HT });
    linearButton->setToggleState(true, dontSendNotification);
    linearButton->addListener(this);
    linearButton->setTooltip(linearTip);
    canvas->addAndMakeVisible(linearButton);
    canvasBounds = canvasBounds.getUnion(bounds);

    yPos += 20;
    expButton = new ToggleButton("Exponential");
    expButton->setBounds(bounds = { xPos, yPos, 90, TEXT_HT });
    expButton->setToggleState(false, dontSendNotification);
    expButton->addListener(this);
    expButton->setTooltip(expTip);
    canvas->addAndMakeVisible(expButton);
    canvasBounds = canvasBounds.getUnion(bounds);

    xPos += 15;
    yPos += 20;
    alpha = new Label("alpha", "Alpha: ");
    alpha->setBounds(bounds = { xPos, yPos, 45, TEXT_HT });
    alpha->setColour(Label::backgroundColourId, Colours::grey);
    canvas->addAndMakeVisible(alpha);

    xPos += 50;
    alphaE = new Label("alphaE", "0.3");
    alphaE->setEditable(true);
    alphaE->addListener(this);
    alphaE->setBounds(bounds = { xPos, yPos, 30, TEXT_HT });
    alphaE->setColour(Label::backgroundColourId, Colours::grey);
    alphaE->setColour(Label::textColourId, Colours::white);
    canvas->addAndMakeVisible(alphaE);


    // ------- Plot ------- //
    cohPlot = new MatlabLikePlot();
    cohPlot->setBounds(bounds = { 230, 90, 600, 500 });
    cohPlot->setRange(0, 40, 0, 1, true);
    cohPlot->setControlButtonsVisibile(false);

    canvas->addAndMakeVisible(cohPlot);
    canvasBounds = canvasBounds.getUnion(bounds);
    
    // some extra padding
    canvasBounds.setBottom(canvasBounds.getBottom() + 10);
    canvasBounds.setRight(canvasBounds.getRight() + 10);

    canvas->setBounds(canvasBounds);
    viewport->setViewedComponent(canvas, false);
    viewport->setScrollBarsShown(true, true);
    addAndMakeVisible(viewport);

    startCallbacks();
}


CoherenceVisualizer::~CoherenceVisualizer()
{
    stopCallbacks();
}

void CoherenceVisualizer::resized()
{
    viewport->setSize(getWidth(), getHeight());
}

void CoherenceVisualizer::refreshState() {}
void CoherenceVisualizer::update() 
{
    freqStep = processor->freqStep;
}

void CoherenceVisualizer::updateCombList()
{
    combinationBox->clear(dontSendNotification);
    for (int i = 0, comb = 1; i < group1Channels.size(); i++)
    {
        for (int j = 0; j < group2Channels.size(); j++, ++comb)
        {
            // using 1-based comb ids since 0 is reserved for "nothing selected"
            combinationBox->addItem(String(group1Channels[i] + 1) + " x " + String(group2Channels[j] + 1), comb);
        }
    }
}


void CoherenceVisualizer::refresh() 
{
    if (processor->meanCoherence.hasUpdate())
    {
        AtomicScopedReadPtr<std::vector<std::vector<double>>> coherenceReader(processor->meanCoherence);
        coherenceReader.pullUpdate();

        coh.resize(coherenceReader->size());
        
        for (int comb = 0; comb < processor->nGroup1Chans * processor->nGroup2Chans; comb++)
        {
            int vecSize = coherenceReader->at(comb).size();
            coh[comb].resize(vecSize);
            for (int i = 0; i < vecSize; i++)
            {
                coh[comb][i] = coherenceReader->at(comb)[i];
            }
        }
    }

    if (coh.size() > 0)
    {
        XYline cohLine(1, freqStep, coh[curComb], 1, Colours::yellow);

        cohPlot->clearplot();
        cohPlot->plotxy(cohLine);
        cohPlot->repaint();
    }
}

void CoherenceVisualizer::labelTextChanged(Label* labelThatHasChanged)
{
    if (labelThatHasChanged == alphaE)
    {
        float newVal;
        if (updateFloatLabel(labelThatHasChanged, 0, INT_MAX, 8, &newVal))
        {
            if (expButton->getState())
            {
                processor->updateAlpha(newVal);
            }
        }
    }
}

void CoherenceVisualizer::comboBoxChanged(ComboBox* comboBoxThatHasChanged)
{
    if (comboBoxThatHasChanged == combinationBox)
    {
        curComb = static_cast<int>(combinationBox->getSelectedId() - 1);
    }
}

void CoherenceVisualizer::buttonClicked(Button* buttonClicked)
{
    if (buttonClicked == resetTFR)
    {
        processor->resetTFR();
        std::cout << "Done with TFR reset" << std::endl;;
    }

    if (buttonClicked == linearButton)
    {
        expButton->setToggleState(false, dontSendNotification);

        processor->updateAlpha(0);
    }
    
    if (buttonClicked == expButton)
    {
        linearButton->setToggleState(false, dontSendNotification);
        processor->updateAlpha(alphaE->getText().getFloatValue());
    }

    if (group1Buttons.contains((ElectrodeButton*)buttonClicked))
    {
        ElectrodeButton* eButton = static_cast<ElectrodeButton*>(buttonClicked);
        int buttonChan = eButton->getChannelNum() - 1;
        // Add to group 1 channels
        // Make sure to check that not in group2Buttons..
        if (group1Channels.contains(buttonChan))
        {
            int it = group1Channels.indexOf(buttonChan);
            group1Channels.remove(it);
        }

        else
        {
            if (group2Channels.contains(buttonChan))
            {
                group2Buttons[buttonChan]->setToggleState(false, dontSendNotification);
                int it = group2Channels.indexOf(buttonChan);
                group2Channels.remove(it);
            }

            group1Channels.addUsingDefaultSort(buttonChan);
        }
        processor->updateGroup(group1Channels, group2Channels);
        updateCombList();
    }
    if (group2Buttons.contains((ElectrodeButton*)buttonClicked))
    {
        ElectrodeButton* eButton = static_cast<ElectrodeButton*>(buttonClicked);
        int buttonChan = eButton->getChannelNum() - 1;
        if (group2Channels.contains(buttonChan))
        {
            int it = group2Channels.indexOf(buttonChan);
            group2Channels.remove(it);
        }
        
        else
        {
            if (group1Channels.contains(buttonChan))
            {
                group1Buttons[buttonChan]->setToggleState(false, dontSendNotification);
                int it = group1Channels.indexOf(buttonChan);
                group1Channels.remove(it);
            }

            group2Channels.addUsingDefaultSort(buttonChan);
        }      
        processor->updateGroup(group1Channels, group2Channels);
        updateCombList();
    }

    

}

void CoherenceVisualizer::beginAnimation() 
{
    for (int i = 0; i < group1Buttons.size(); i++)
    {
        group1Buttons[i]->setEnabled(false);
    }
    for (int i = 0; i < group2Buttons.size(); i++)
    {
        group2Buttons[i]->setEnabled(false);
    }
}
void CoherenceVisualizer::endAnimation() 
{
    for (int i = 0; i < group1Buttons.size(); i++)
    {
        group1Buttons[i]->setEnabled(true);
    }
    for (int i = 0; i < group2Buttons.size(); i++)
    {
        group2Buttons[i]->setEnabled(true);
    }
}

bool CoherenceVisualizer::updateFloatLabel(Label* label, float min, float max,
    float defaultValue, float* out)
{
    const String& in = label->getText();
    float parsedFloat;
    try
    {
        parsedFloat = std::stof(in.toRawUTF8());
    }
    catch (const std::logic_error&)
    {
        label->setText(String(defaultValue), dontSendNotification);
        return false;
    }

    *out = jmax(min, jmin(max, parsedFloat));

    label->setText(String(*out), dontSendNotification);
    return true;
}
void CoherenceVisualizer::setParameter(int, float) {}
void CoherenceVisualizer::setParameter(int, int, int, float) {}