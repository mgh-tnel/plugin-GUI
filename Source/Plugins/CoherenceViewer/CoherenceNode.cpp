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

#include "CoherenceNode.h"

/********** node ************/
CoherenceNode::CoherenceNode()
    : GenericProcessor  ("Coherence")
    , Thread            ("Coherence Calc")
    , dataWriter        (dataBuffer)
    , coherenceReader   (meanCoherence)
    , dataReader        (dataBuffer)
    , coherenceWriter   (meanCoherence)
    , segLen            (4)
    , nFreqs            (40/0.25)
    , freqStep          (0.25)
    , stepLen           (0.1)
    , winLen            (2)
    , interpRatio       (2)
    , nGroup1Chans      (0)
    , nGroup2Chans      (0)
    , Fs                (CoreServices::getGlobalSampleRate())
{
    setProcessorType(PROCESSOR_TYPE_SINK);
}

CoherenceNode::~CoherenceNode()
{}

void CoherenceNode::createEventChannels() 
{}

AudioProcessorEditor* CoherenceNode::createEditor()
{
    editor = new CoherenceEditor(this);
    return editor;
}


void CoherenceNode::process(AudioSampleBuffer& continuousBuffer)
{  
    //// Get current coherence vector ////
    if (meanCoherence.hasUpdate())
    {
        coherenceReader.pullUpdate();
        // Do something with coherence!
    }
   
    ///// Add incoming data to data buffer. Let thread get the ok to start at 8seconds ////
    // Check writer
    if (!dataWriter.isValid())
    {
        jassert("atomic sync data writer broken");
    }

    //for loop over active channels and update buffer with new data
    Array<int> activeInputs = getActiveInputs();
    int nActiveInputs = activeInputs.size();
    int nSamples = 0;
    for (int activeChan = 0; activeChan < nActiveInputs; ++activeChan)
    {
        int chan = activeInputs[activeChan];
        nSamples = getNumSamples(chan); // all channels the same?
        if (nSamples == 0)
        {
            continue;
        }

        // Get read pointer of incoming data to move to the stored data buffer
        const float* rpIn = continuousBuffer.getReadPointer(chan);

        // Handle overflow 
        if (nSamplesAdded + nSamples >= segLen * Fs)
        {
            nSamples = segLen * Fs - nSamplesAdded;
        }

        // Add to buffer the new samples.
        for (int n = 0; n < nSamples; n++)
        {
            dataWriter->getReference(activeChan).set(n, rpIn[n]);
        }  
    }

    nSamplesAdded += nSamples;
   // std::cout << nSamplesAdded << " of " << segLen*Fs << std::endl;
    // channel buf is full. Update buffer.
    if (nSamplesAdded >= segLen * Fs)
    {
        dataWriter.pushUpdate();
        // Reset samples added
        nSamplesAdded = 0;
        updateDataBufferSize();
    }
}

void CoherenceNode::run()
{  
    while (!threadShouldExit())
    {
        //// Check for new filled data buffer and run stats ////

        Array<int> activeInputs = getActiveInputs();
        int nActiveInputs = activeInputs.size();
        if (dataBuffer.hasUpdate())
        {
            std::cout << "starting thread" << std::endl;
            time_t my_time = time(NULL);
            std::cout << ctime(&my_time);
            dataReader.pullUpdate();

            for (int activeChan = 0; activeChan < nActiveInputs; ++activeChan)
            {
                int chan = activeInputs[activeChan];
                // get buffer and send it to TFR to fun real-timBe-coherence calcs
                int groupNum = getChanGroup(chan);
                if (groupNum != -1)
                {
                    int it = getGroupIt(groupNum, chan);
                    TFR->addTrial(dataReader->getReference(activeChan).getReadPointer(activeChan), it, groupNum);
                }
                else
                {
                    // channel isn't part of group 1 or 2
                    jassert("ungrouped channel");
                }
            }

            //// Get and send updated coherence  ////
            if (!coherenceWriter.isValid())
            {
                jassert("atomic sync coherence writer broken");
            }
            // Calc coherence
            std::vector<std::vector<double>> TFRMeanCoh = TFR->getCurrentMeanCoherence();
            // For loop over combinations
            for (int comb = 0; comb < nGroupCombs; ++comb)
            {
                for (int freq = 0; freq < nFreqs; freq++)
                {
                    // freq lookup list
                    coherenceWriter->at(comb).at(freq) = TFRMeanCoh[comb][freq];
                }

            }
            // Update coherence and reset data buffer
            coherenceWriter.pushUpdate();
            my_time = time(NULL);
            std::cout << ctime(&my_time) << "end thread\n\n";
            updateMeanCoherenceSize();
        }
    }
}

void CoherenceNode::updateDataBufferSize()
{
    dataWriter->clear();
    for (int i = 0; i < nGroup1Chans + nGroup2Chans; i++)
    {
        dataWriter->add(std::move(FFTWArray(segLen * Fs)));
    }
}

void CoherenceNode::updateMeanCoherenceSize()
{
    coherenceWriter->clear();
    coherenceWriter->resize(nGroupCombs);
    for (int i = 0; i < nGroupCombs; i++)
    {
        coherenceWriter->at(i).resize(segLen * Fs);
    }
}

void CoherenceNode::updateSettings()
{
    // Array of samples per channel and if ready to go
    nSamplesAdded = 0;
    
    // (Start - end freq) / stepsize
    nFreqs = int(40 / freqStep);

    // Set channels in group (need to update from editor)
    group1Channels.clear();
    group1Channels.addArray({ 1, 2, 3, 4, 5, 6, 7, 8 });
    group2Channels.clear();
    group2Channels.addArray({ 9, 10, 11, 12, 13, 14, 15, 16 });

    // Set number of channels in each group
    nGroup1Chans = group1Channels.size();
    nGroup2Chans = group2Channels.size();
    nGroupCombs = nGroup1Chans * nGroup2Chans;

    // Seg/win/step/interp - move to params eventually
    winLen = 2;
    stepLen = 0.1;
    interpRatio = 2;

    // Trim time close to edge
    int nSamplesWin = winLen * Fs;
    int nTimes = ((segLen * Fs) - (nSamplesWin)) / Fs * (1/stepLen) + 1; // Trim half of window on both sides, so 1 window length is trimmed total

    updateDataBufferSize();
    updateMeanCoherenceSize();

    // Overwrite TFR 
	TFR = new CumulativeTFR(nGroup1Chans, nGroup2Chans, nFreqs, nTimes, Fs, winLen, stepLen, freqStep, interpRatio, segLen);
}

void CoherenceNode::setParameter(int parameterIndex, float newValue)
{
    // Set new region channels and such in here?
    switch (parameterIndex)
    {
    case SEGMENT_LENGTH:
        segLen = static_cast<int>(newValue);
        break;

    case WINDOW_LENGTH:
        winLen = static_cast<int>(newValue);
        break;
    }
    updateSettings();
}

int CoherenceNode::getChanGroup(int chan)
{
    if (group1Channels.contains(chan))
    {
        return 1;
    }
    else if (group2Channels.contains(chan))
    {
        return 2;
    }
    else
    {
        return -1; // Channel isn't in group 1 or 2. Error!
    }
}

int CoherenceNode::getGroupIt(int group, int chan)
{
    int groupIt;
    if (group == 1)
    {
        int * it = std::find(group1Channels.begin(), group1Channels.end(), chan);
        groupIt = it - group1Channels.begin();
    }
    else
    {
        int * it = std::find(group2Channels.begin(), group2Channels.end(), chan);
        groupIt = it - group2Channels.begin();
    }
    return groupIt;
}

bool CoherenceNode::enable()
{
    if (isEnabled)
    {
        // Start coherence calculation thread
        startThread(COH_PRIORITY);
    }
    return isEnabled;
}

bool CoherenceNode::disable()
{
    CoherenceEditor* editor = static_cast<CoherenceEditor*>(getEditor());
    editor->disable();

    signalThreadShouldExit();

    return true;
}


Array<int> CoherenceNode::getActiveInputs()
{
    int numInputs = getNumInputs();
    auto ed = static_cast<CoherenceEditor*>(getEditor());
    if (numInputs == 0 || !ed)
    {
        return Array<int>();
    }

    Array<int> activeChannels = ed->getActiveChannels();
    return activeChannels;
}


/************** editor *************/
CoherenceEditor::CoherenceEditor(CoherenceNode* p)
    : VisualizerEditor(p, 300, true)
{
    tabText = "Coherence";

    // Segment length
    int x = 0, y = 0, h = 0, w = 0;
    segLabel = createLabel("segLabel", "Segment Length:", { x + 5, y + 25, w + 60, h + 27 });
    addAndMakeVisible(segLabel);

    segEditable = createEditable("segEditable", "8", "Input length of segment", { x + 70, y + 25, w + 35, h + 27 });
    addAndMakeVisible(segEditable);

    // Window Length
    y += 35;
    winLabel = createLabel("winLabel", "Window Length:", { x + 5, y + 25, w + 60, h + 27 });
    addAndMakeVisible(winLabel);

    winEditable = createEditable("winEditable", "2", "Input length of window", { x + 70, y + 25, w + 35, h + 27 });
    addAndMakeVisible(winEditable);
    
    // Step Length
    y += 35;
    stepLabel = createLabel("stepLabel", "Step Length:", { x + 5, y + 25, w + 60, h + 27 });
    addAndMakeVisible(stepLabel);

    stepEditable = createEditable("stepEditable", "0.25", "Input step size between windows; higher number = less resource intensive", 
        { x + 70, y + 25, w + 35, h + 27 });
    addAndMakeVisible(stepEditable);

    // Frequencies of interest
    y = 0;
    x += 105;
    foiLabel = createLabel("foiLabel", "Frequencies of Interest", { x + 15, y + 25, w + 80, h + 27 });
    addAndMakeVisible(foiLabel);

    // Start freq
    y += 35;
    fstartLabel = createLabel("fstartLabel", "Freq Start:", { x + 5, y + 25, w + 60, h + 27 });
    addAndMakeVisible(fstartLabel);

    fstartEditable = createEditable("fstartEditable", "1", "Start of range of frequencies", { x + 70, y + 25, w + 35, h + 27 });
    addAndMakeVisible(fstartEditable);

    // End Freq
    y += 35;
    fendLabel = createLabel("fendLabel", "Freq End:", { x + 5, y + 25, w + 60, h + 27 });
    addAndMakeVisible(fendLabel);

    fendEditable = createEditable("fendEditable", "40", "End of range of frequencies", { x + 70, y + 25, w + 35, h + 27 });
    addAndMakeVisible(fendEditable);

    setEnabledState(false);
}

CoherenceEditor::~CoherenceEditor() {}

Label* CoherenceEditor::createEditable(const String& name, const String& initialValue,
    const String& tooltip, juce::Rectangle<int> bounds)
{
    Label* editable = new Label(name, initialValue);
    editable->setEditable(true);
    editable->addListener(this);
    editable->setBounds(bounds);
    editable->setColour(Label::backgroundColourId, Colours::grey);
    editable->setColour(Label::textColourId, Colours::white);
    if (tooltip.length() > 0)
    {
        editable->setTooltip(tooltip);
    }
    return editable;
}

Label* CoherenceEditor::createLabel(const String& name, const String& text,
    juce::Rectangle<int> bounds)
{
    Label* label = new Label(name, text);
    label->setBounds(bounds);
    label->setFont(Font("Small Text", 12, Font::plain));
    label->setColour(Label::textColourId, Colours::darkgrey);
    return label;
}

void CoherenceEditor::comboBoxChanged(ComboBox* comboBoxThatHasChanged)
{

}

void CoherenceEditor::labelTextChanged(Label* labelThatHasChanged)
{
    auto processor = static_cast<CoherenceNode*>(getProcessor());
    if (labelThatHasChanged == segEditable)
    {
        int newVal;
        if (updateIntLabel(labelThatHasChanged, 0, INT_MAX, 8, &newVal))
        {
            processor->setParameter(CoherenceNode::SEGMENT_LENGTH, static_cast<int>(newVal));
        }
    }
    if (labelThatHasChanged == winEditable)
    {
        int newVal;
        if (updateIntLabel(labelThatHasChanged, 0, INT_MAX, 8, &newVal))
        {
            processor->setParameter(CoherenceNode::WINDOW_LENGTH, static_cast<int>(newVal));
        }
    }
}

bool CoherenceEditor::updateIntLabel(Label* label, int min, int max, int defaultValue, int* out)
{
    const String& in = label->getText();
    int parsedInt;
    try
    {
        parsedInt = std::stoi(in.toRawUTF8());
    }
    catch (const std::logic_error&)
    {
        label->setText(String(defaultValue), dontSendNotification);
        return false;
    }

    *out = jmax(min, jmin(max, parsedInt));

    label->setText(String(*out), dontSendNotification);
    return true;
}


Visualizer* CoherenceEditor::createNewCanvas()
{
    canvas = new CoherenceVisualizer();
    return canvas;
}

bool CoherenceNode::hasEditor() const
{
    return true;
}

void CoherenceNode::saveCustomChannelParametersToXml(XmlElement* channelElement, int channelNumber, InfoObjectCommon::InfoObjectType channelType)
{

}

void CoherenceNode::loadCustomChannelParametersFromXml(XmlElement* channelElement, InfoObjectCommon::InfoObjectType channelType)
{

}