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
#include "CoherenceNodeEditor.h"
/********** node ************/
CoherenceNode::CoherenceNode()
    : GenericProcessor  ("Coherence")
    , Thread            ("Coherence Calc")
    , segLen            (8)
    , freqStep          (1)
    , freqStart         (1)
    , freqEnd           (40)
    , stepLen           (0.1)
    , winLen            (2)
    , interpRatio       (2)
    , nGroup1Chans      (0)
    , nGroup2Chans      (0)
    , Fs                (0)
    , alpha             (0)
    , ready             (false)
    , group1Channels    ({})
    , group2Channels    ({})
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
    AtomicScopedWritePtr<Array<FFTWArrayType>> dataWriter(dataBuffer);
    //AtomicScopedReadPtr<std::vector<std::vector<double>>> coherenceReader(meanCoherence);
    //// Get current coherence vector ////
    //if (meanCoherence.hasUpdate())
    //{
        //coherenceReader.pullUpdate();
        // Do something with coherence!
    //}
   
    ///// Add incoming data to data buffer. Let thread get the ok to start at 8seconds ////
    // Check writer
    if (!dataWriter.isValid())
    {
        jassertfalse; // atomic sync data writer broken
    }

    //for loop over active channels and update buffer with new data
    Array<int> activeInputs = getActiveInputs();
    int nActiveInputs = activeInputs.size();
    int nSamples = 0;
    for (int activeChan = 0; activeChan < nActiveInputs; ++activeChan)
    {
        int chan = activeInputs[activeChan];
        int groupNum = getChanGroup(chan);
        if (groupNum != -1)
        {
            int groupIt = (groupNum == 1 ? getGroupIt(groupNum, chan) : getGroupIt(groupNum, chan) + nGroup1Chans);
            
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
                dataWriter->getReference(groupIt).set(nSamplesAdded + n, rpIn[n]);
            }
        }       
    }

    nSamplesAdded += nSamples;

    // channel buf is full. Update buffer.
    if (nSamplesAdded >= segLen * Fs)
    {
        dataWriter.pushUpdate();
        // Reset samples added
        nSamplesAdded = 0;
        //updateDataBufferSize();
    }
}

void CoherenceNode::run()
{  
    AtomicScopedReadPtr<Array<FFTWArrayType>> dataReader(dataBuffer);
    AtomicScopedWritePtr<std::vector<std::vector<double>>> coherenceWriter(meanCoherence);
    
    while (!threadShouldExit())
    {
        //// Check for new filled data buffer and run stats ////        
        if (dataBuffer.hasUpdate())
        {
            dataReader.pullUpdate();
            Array<int> activeInputs = getActiveInputs();
            int nActiveInputs = activeInputs.size();
            for (int activeChan = 0; activeChan < nActiveInputs; ++activeChan)
            {
                int chan = activeInputs[activeChan];
                // get buffer and send it to TFR
                // Check to make sure channel is in one of our groups
                int groupNum = getChanGroup(chan);
                if (groupNum != -1)
                {
                    int groupIt = (groupNum == 1 ? getGroupIt(groupNum, chan) : getGroupIt(groupNum, chan) + nGroup1Chans);
                    TFR->addTrial(dataReader->getReference(groupIt), groupIt);
                }
                else
                {
                    // channel isn't part of group 1 or 2
                    jassertfalse; // ungrouped channel
                }
            }

            //// Get and send updated coherence  ////
            if (!coherenceWriter.isValid())
            {
                jassertfalse; // atomic sync coherence writer broken
            }

            // Calc coherence at each combination of interest
            for (int itX = 0, comb = 0; itX < nGroup1Chans; itX++)
            {
                for (int itY = 0; itY < nGroup2Chans; itY++, comb++)
                {
                    TFR->getMeanCoherence(itX, itY + nGroup1Chans, coherenceWriter->at(comb).data(), comb);
                }
            }

            // Update coherence and reset data buffer
      
            coherenceWriter.pushUpdate();
        }
    }
}

void CoherenceNode::updateDataBufferSize(int newSize)
{
    int totalChans = nGroup1Chans + nGroup2Chans;

    // no writers or readers can exist here
    // so this can't be called during acquisition
    dataBuffer.map([=](Array<FFTWArrayType>& arr)
    {
        arr.resize(totalChans);

        for (int i = 0; i < totalChans; i++)
        {
            arr.getReference(i).resize(newSize);
        }
    });
}

void CoherenceNode::updateMeanCoherenceSize()
{
    meanCoherence.map([=](std::vector<std::vector<double>>& vec)
    {
        // Update meanCoherence size to new num combinations
        vec.resize(nGroupCombs);

        // Update meanCoherence to new num freq at each existing combination
        for (int comb = 0; comb < nGroupCombs; comb++)
        {
            vec[comb].resize(nFreqs);
        }
    });
}

void CoherenceNode::updateSettings()
{
    // Array of samples per channel and if ready to go
    nSamplesAdded = 0;
    
    // (Start - end freq) / stepsize
    freqStep = 1.0/float(winLen*interpRatio);
    //freqStep = 1; // for debugging
    nFreqs = int((freqEnd - freqStart) / freqStep);
    //foi = 0.5:1 / (win_len*interp_ratio) : 30

    // Default to this. Probably will move to canvas tab.
    int numInputs = getNumInputs();
    // Default selected groups
    if (numInputs > 0)
    {
        if (group1Channels.size() == 0) // if groups are empty currently
        {
            for (int i = 0; i < numInputs; i++)
            {
                if (i < numInputs / 2)
                {
                    group1Channels.add(i);
                }
                else
                {
                    group2Channels.add(i);
                }
            }
        }
        // Set number of channels in each group
        nGroup1Chans = group1Channels.size();
        nGroup2Chans = group2Channels.size();
        nGroupCombs = nGroup1Chans * nGroup2Chans;

        // Seg/win/step/interp - move to params eventually
        interpRatio = 2;

        if (nGroup1Chans > 0)
        {
            float newFs = getDataChannel(group1Channels[0])->getSampleRate();
            if (newFs != Fs)
            {
                Fs = newFs;
                updateDataBufferSize(segLen * Fs);
            }
        }

        
        updateMeanCoherenceSize();
    }
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
    case START_FREQ:
        freqStart = static_cast<int>(newValue);
        break;
    case END_FREQ:
        freqEnd = static_cast<int>(newValue);
        break;
    case STEP_LENGTH:
        stepLen = static_cast<float>(newValue);
        break;
    }
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
    if (group == 1)
    {
        int * it = std::find(group1Channels.begin(), group1Channels.end(), chan);
        return it - group1Channels.begin();
    }
    else if (group == 2)
    {
        int * it = std::find(group2Channels.begin(), group2Channels.end(), chan);
        return it - group2Channels.begin();
    }
    else
    {
        return -1;
    }
}


void CoherenceNode::updateGroup(Array<int> group1Chans, Array<int> group2Chans)
{
    group1Channels = group1Chans;
    group2Channels = group2Chans;

    nGroup1Chans = group1Channels.size();
    nGroup2Chans = group2Channels.size();

    nGroupCombs = nGroup1Chans * nGroup2Chans;
}

void CoherenceNode::updateAlpha(float a)
{
    alpha = a;
}

void CoherenceNode::updateReady(bool isReady)
{
    ready = isReady;
}

void CoherenceNode::resetTFR()
{
    if ((group1Channels.size() > 0) && (group2Channels.size() > 0))
    {
        ready = true;

        nSamplesAdded = 0;
        updateDataBufferSize(segLen*Fs);
        updateMeanCoherenceSize();

        // Trim time close to edge
        int nSamplesWin = winLen * Fs;
        nTimes = ((segLen * Fs) - (nSamplesWin)) / Fs * (1 / stepLen) + 1; // Trim half of window on both sides, so 1 window length is trimmed total

        if (nGroup1Chans > 0)
        {
            float newFs = getDataChannel(group1Channels[0])->getSampleRate();
            if (newFs != Fs)
            {
                Fs = newFs;
                updateDataBufferSize(segLen * Fs);
            }
        }

        TFR = new CumulativeTFR(nGroup1Chans, nGroup2Chans, nFreqs, nTimes, Fs, winLen, stepLen,
            freqStep, freqStart, segLen, alpha);
    }
    else
    {
        ready = false;
    }

}


bool CoherenceNode::isReady()
{
    if (!ready)
    {
        resetTFR();
    }
    return ready && (getNumInputs() > 0);
}

bool CoherenceNode::enable()
{
    if (isEnabled)
    {
        // Start coherence calculation thread
        startThread(COH_PRIORITY);
        //editor->enable();
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


bool CoherenceNode::hasEditor() const
{
    return true;
}

void CoherenceNode::saveCustomParametersToXml(XmlElement* parentElement)
{
    XmlElement* mainNode = parentElement->createNewChildElement("COHERENCENODE");
    
    // ------ Save Groups ------ //
    XmlElement* group1Node = mainNode->createNewChildElement("Group1");
    XmlElement* group2Node = mainNode->createNewChildElement("Group2");
    
    for (int i = 0; i < group1Channels.size(); i++)
    {
        group1Node->setAttribute("Chan" + String(i), group1Channels[i]);
    }
    for (int i = 0; i < group2Channels.size(); i++)
    {
        group2Node->setAttribute("Chan" + String(i), group2Channels[i]);
    }

    // ------ Save Other Params ------ //
    mainNode->setAttribute("alpha", alpha);
}

void CoherenceNode::loadCustomParametersFromXml()
{
    int numActiveInputs = getActiveInputs().size();
    if (parametersAsXml)
    {
        forEachXmlChildElementWithTagName(*parametersAsXml, mainNode, "COHERENCENODE")
        {
            // Load group 1 channels
            forEachXmlChildElementWithTagName(*mainNode, node, "Group1")
            {
                group1Channels.clear();
                for (int i = 0; i < numActiveInputs; i++)
                {
                    int channel = node->getIntAttribute("Chan" + String(i), -1);
                    if (channel != -1)
                    {
                        group1Channels.add(channel);
                    }
                    else
                    {
                        break;
                    }
                }
            }
            // Load group 2 channels
            forEachXmlChildElementWithTagName(*mainNode, node, "Group2")
            {
                group2Channels.clear();
                for (int i = 0; i < numActiveInputs; i++)
                {
                    int channel = node->getIntAttribute("Chan" + String(i), -1);
                    if (channel != -1)
                    {
                        group2Channels.add(channel);
                    }
                    else
                    {
                        break;
                    }
                }
            }
            // Load other params
            alpha = mainNode->getDoubleAttribute("alpha");
        }
        
        //Start TFR
        if (group1Channels.size() > 0 && group2Channels.size() > 0)
        {
            resetTFR();
        }
    }
}