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

#include "PhaseCalculator.h"
#include "PhaseCalculatorEditor.h"

const float PhaseCalculator::PASSBAND_EPS = 0.01F;

PhaseCalculator::PhaseCalculator()
    : GenericProcessor      ("Phase Calculator")
    , Thread                ("AR Modeler")
    , calcInterval          (50)
    , lowCut                (4.0)
    , highCut               (8.0)
    , htScaleFactor         (getScaleFactor(lowCut, highCut))
    , outputMode            (PH)
    , visEventChannel       (-1)
    , visContinuousChannel  (-1)
    , visHilbertBuffer      (VIS_HILBERT_LENGTH)
    , visForwardPlan        (VIS_HILBERT_LENGTH, &visHilbertBuffer, FFTW_MEASURE)
    , visBackwardPlan       (VIS_HILBERT_LENGTH, &visHilbertBuffer, FFTW_BACKWARD, FFTW_MEASURE)
{
    setProcessorType(PROCESSOR_TYPE_FILTER);
    setAROrder(20);
}

PhaseCalculator::~PhaseCalculator() {}

bool PhaseCalculator::hasEditor() const
{
    return true;
}


AudioProcessorEditor* PhaseCalculator::createEditor()
{
    editor = new PhaseCalculatorEditor(this);
    return editor;
}

void PhaseCalculator::createEventChannels()
{
    // add vis phase event channel
    const DataChannel* visChannel = getDataChannel(visContinuousChannel);
    float sampleRate = visChannel ? visChannel->getSampleRate() : CoreServices::getGlobalSampleRate();
    juce::uint16 subproc = visChannel ? visChannel->getSubProcessorIdx() : 0;

    EventChannel* chan = new EventChannel(EventChannel::DOUBLE_ARRAY, 1, 1, sampleRate, this, subproc);
    chan->setName(chan->getName() + ": PC visualized phase (deg.)");
    chan->setDescription("The accurate phase in degrees of each visualized event");
    chan->setIdentifier("phasecalc.visphase");

    // metadata storing source data channel
    if (visChannel)
    {
        MetaDataDescriptor sourceChanDesc(MetaDataDescriptor::UINT16, 3, "Source Channel",
            "Index at its source, Source processor ID and Sub Processor index of the channel that triggers this event",
            "source.channel.identifier.full");
        MetaDataValue sourceChanVal(sourceChanDesc);
        uint16 sourceInfo[3];
        sourceInfo[0] = visChannel->getSourceIndex();
        sourceInfo[1] = visChannel->getSourceNodeID();
        sourceInfo[2] = visChannel->getSubProcessorIdx();
        sourceChanVal.setValue(static_cast<const uint16*>(sourceInfo));
        chan->addMetaData(sourceChanDesc, sourceChanVal);
    }

    visPhaseChannel = eventChannelArray.add(chan);
}

void PhaseCalculator::setParameter(int parameterIndex, float newValue)
{
    int numInputs = getNumInputs();

    switch (parameterIndex) {
    case RECALC_INTERVAL:
        calcInterval = static_cast<int>(newValue);
        break;

    case AR_ORDER:
        setAROrder(static_cast<int>(newValue));
        break;

    case LOWCUT:
        setLowCut(newValue);
        break;

    case HIGHCUT:
        setHighCut(newValue);
        break;

    case OUTPUT_MODE:
    {
        OutputMode oldMode = outputMode;
        outputMode = static_cast<OutputMode>(static_cast<int>(newValue));
        if (oldMode == PH_AND_MAG || outputMode == PH_AND_MAG)
        {
            CoreServices::updateSignalChain(editor);  // add or remove channels if necessary
        }
        break;
    }

    case VIS_E_CHAN:
        jassert(newValue >= -1);
        visEventChannel = static_cast<int>(newValue);
        break;

    case VIS_C_CHAN:
        setVisContChan(static_cast<int>(newValue));
        break;
    }
}

void PhaseCalculator::process(AudioSampleBuffer& buffer)
{
    // handle subprocessors, if any
    HashMap<int, uint16>::Iterator it(subProcessorMap);
    while (it.next())
    {
        uint32 fullSourceID = static_cast<uint32>(it.getKey());
        int subProcessor = it.getValue();
        uint32 sourceTimestamp = getSourceTimestamp(fullSourceID);
        uint64 sourceSamples = getNumSourceSamples(fullSourceID);
        setTimestampAndSamples(sourceTimestamp, sourceSamples, subProcessor);
    }

    // check for events to visualize
    bool hasCanvas = static_cast<PhaseCalculatorEditor*>(getEditor())->canvas != nullptr;
    if (hasCanvas && visEventChannel > -1)
    {
        checkForEvents();
    }

    // iterate over active input channels
    Array<int> activeInputs = getActiveInputs();
    int nActiveInputs = activeInputs.size();
    for (int activeChan = 0; activeChan < nActiveInputs; ++activeChan)
    {
        int chan = activeInputs[activeChan];
        int nSamples = getNumSamples(chan);
        if (nSamples == 0) // nothing to do
        {
            continue;
        }

        // filter the data
        float* const wpIn = buffer.getWritePointer(chan);
        filters[chan]->process(nSamples, &wpIn);

		// shift old data and copy new data into historyBuffer (as much as can fit)
        int historyStartIndex = jmax(nSamples - historyLength, 0);
        int nSamplesToEnqueue = nSamples - historyStartIndex;
        int nOldSamples = historyLength - nSamplesToEnqueue;

        const double* rpBuffer = historyBuffer.getReadPointer(activeChan, nSamplesToEnqueue);
        double* wpBuffer = historyBuffer.getWritePointer(activeChan);

        // critical section for this channel's historyBuffer
        // note that the floats are coerced to doubles here - this is important to avoid over/underflow when calculating the phase.
        {
            const ScopedLock myHistoryLock(*historyLock[activeChan]);

            // shift old data
            for (int i = 0; i < nOldSamples; ++i)
            {
                *wpBuffer++ = *rpBuffer++;
            }

            // copy new data
            for (int i = 0; i < nSamplesToEnqueue; ++i)
            {
                *wpBuffer++ = wpIn[historyStartIndex + i];
            }
        }

        if (chanState[activeChan] == NOT_FULL)
        {
            int newBufferFreeSpace = jmax(bufferFreeSpace[activeChan] - nSamplesToEnqueue, 0);
            bufferFreeSpace.set(activeChan, newBufferFreeSpace);
            if (newBufferFreeSpace == 0)
            {
                // now that the historyBuffer for this channel is full,
                // let the thread start calculating the AR model.
                chanState.set(activeChan, FULL_NO_AR);
            }
        }

        // calc phase and write out (only if AR model has been calculated)
        if (chanState[activeChan] == FULL_AR) 
		{
			// TODO

            // read current AR parameters safely
            Array<double> localParams;
            localParams.resize(arOrder);
            double* pLocalParam = localParams.getRawDataPointer();
            const double* rpParam = arParams[activeChan]->getRawDataPointer();
            {
                const ScopedLock currParamLock(*arParamLock[activeChan]);

                for (int i = 0; i < arOrder; ++i)
                {
                    pLocalParam[i] = rpParam[i];
                }
            }

            // use AR(20) model to predict upcoming data and append to dataToProcess
            // get beyond end of history buffer indirectly to avoid juce assertion failure
            // rpBuffer = historyBuffer.getReadPointer(activeChan, historyLength - 1) + 1;
            // double* wpHilbert = hilbertBuffer[activeChan]->getRealPointer(hilbertPastLength);
            //
            // arPredict(rpBuffer, wpHilbert, predictionLength, pLocalParam, arOrder);

            // calculate phase and write out to buffer
            auto rpHilbert = hilbertBuffer[activeChan]->getComplexPointer(hilbertPastLength - nSamplesToProcess);
            float* wpOut = buffer.getWritePointer(chan);
            float* wpOut2;
            if (outputMode == PH_AND_MAG)
            {
                // second output channel
                int outChan2 = getNumInputs() + activeChan;
                jassert(outChan2 < buffer.getNumChannels());
                wpOut2 = buffer.getWritePointer(outChan2);
            }

            for (int i = 0; i < nSamplesToProcess; ++i)
            {
                switch (outputMode)
                {
                case MAG:
                    wpOut[i + outputStartIndex] = static_cast<float>(std::abs(rpHilbert[i]));
                    break;

                case PH_AND_MAG:
                    wpOut2[i + outputStartIndex] = static_cast<float>(std::abs(rpHilbert[i]));
                    // fall through
                case PH:
                    // output in degrees
                    wpOut[i + outputStartIndex] = static_cast<float>(std::arg(rpHilbert[i]) * (180.0 / Dsp::doublePi));
                    break;

                case IM:
                    wpOut[i + outputStartIndex] = static_cast<float>(std::imag(rpHilbert[i]));
                    break;
                }
            }

            // unwrapping / smoothing
            if (outputMode == PH || outputMode == PH_AND_MAG)
            {
                unwrapBuffer(wpOut, nSamples, activeChan);
                smoothBuffer(wpOut, nSamples, activeChan);
            }

			// update lastComputedSample and dsOffset
			// TODO lastComputedSample
			dsOffset.set(chan, (dsOffset[chan] + nSamples) % sampleRateMultiple[chan]);
        }
        else // fifo not full or AR model not ready
        {
            // just output zeros
            buffer.clear(chan, outputStartIndex, nSamplesToProcess);
        }

        // if this is the monitored channel for events, check whether we can add a new phase
        if (hasCanvas && chan == visContinuousChannel && chanState[activeChan] != NOT_FULL)
        {
            calcVisPhases(getTimestamp(chan) + getNumSamples(chan));
        }

        // keep track of last sample
        lastSample.set(activeChan, buffer.getSample(chan, nSamples - 1));
    }
}

// starts thread when acquisition begins
bool PhaseCalculator::enable()
{
    if (isEnabled)
    {
        startThread(AR_PRIORITY);

        // have to manually enable editor, I guess...
        PhaseCalculatorEditor* editor = static_cast<PhaseCalculatorEditor*>(getEditor());
        editor->enable();
    }

    return isEnabled;
}

bool PhaseCalculator::disable()
{
    PhaseCalculatorEditor* editor = static_cast<PhaseCalculatorEditor*>(getEditor());
    editor->disable();

    signalThreadShouldExit();

    // reset states of active inputs
    Array<int> activeInputs = getActiveInputs();
    int nActiveInputs = activeInputs.size();
    for (int activeChan = 0; activeChan < nActiveInputs; ++activeChan)
    {
        bufferFreeSpace.set(activeChan, historyLength);
        chanState.set(activeChan, NOT_FULL);
        lastSample.set(activeChan, 0);
		lastComputedSample.set(activeChan, 0);
        dsOffset.set(activeChan, sampleRateMultiple[activeChan] - 1);
        filters[activeInputs[activeChan]]->reset();
    }

    // clear timestamp and phase queues
    while (!visTsBuffer.empty())
    {
        visTsBuffer.pop();
    }

    ScopedLock phaseLock(visPhaseBufferLock);
    while (!visPhaseBuffer.empty())
    {
        visPhaseBuffer.pop();
    }

    return true;
}

// thread routine
void PhaseCalculator::run()
{
    Array<double> data;
    data.resize(historyLength);

    Array<double> paramsTemp;
    paramsTemp.resize(arOrder);

    ARTimer timer;
    int currInterval = calcInterval;
    timer.startTimer(currInterval);

    Array<int> activeInputs = getActiveInputs();
    int numActiveChans = activeInputs.size();

    while (true)
    {
        if (threadShouldExit())
        {
            return;
        }

        for (int activeChan = 0; activeChan < numActiveChans; ++activeChan)
        {
            if (chanState[activeChan] == NOT_FULL)
            {
                continue;
            }

            // critical section for historyBuffer
            {
                const ScopedLock myHistoryLock(*historyLock[activeChan]);

                for (int i = 0; i < historyLength; ++i)
                {
                    data.set(i, historyBuffer.getSample(activeChan, i));
                }
            }
            // end critical section

            // calculate parameters
            arModelers[activeInputs[activeChan]]->fitModel(data, paramsTemp);

            // write params safely
            {
                const ScopedLock myParamLock(*arParamLock[activeChan]);

                juce::Array<double>* myParams = arParams[activeChan];
                for (int i = 0; i < arOrder; ++i)
                {
                    myParams->set(i, paramsTemp[i]);
                }
            }

            chanState.set(activeChan, FULL_AR);
        }

        // update interval
        if (calcInterval != currInterval)
        {
            currInterval = calcInterval;
            timer.stopTimer();
            timer.startTimer(currInterval);
        }

        while (!timer.check())
        {
            if (threadShouldExit())
            {
                return;
            }

            if (calcInterval != currInterval)
            {
                currInterval = calcInterval;
                timer.stopTimer();
                timer.startTimer(currInterval);
            }
            sleep(10);
        }
    }
}

void PhaseCalculator::updateSettings()
{
    // update arrays that store one entry per input
    int numInputs = getNumInputs();
    int prevNumInputs = filters.size();
    int numInputsChange = numInputs - prevNumInputs;

    if (numInputsChange > 0)
    {
        // (temporary, until validateSampleRate call):
        sampleRateMultiple.insertMultiple(-1, 1, numInputsChange);
        dsOffset.insertMultiple(-1, 0, numInputsChange);

        // add new objects at new indices
        for (int i = prevNumInputs; i < numInputs; i++)
        {
            filters.add(new BandpassFilter());
            // (temporary, until validateSampleRate call)
            arModelers.add(new ARModeler());
        }
    }
    else if (numInputsChange < 0)
    {
        // delete unneeded entries
        sampleRateMultiple.removeLast(-numInputsChange);
        dsOffset.removeLast(-numInputsChange);
        filters.removeLast(-numInputsChange);
        arModelers.removeLast(-numInputsChange);
    }

    // set filter parameters (sample rates may have changed)
    setFilterParameters();

    // check whether active channels can be processed
    Array<int> activeInputs = getActiveInputs();
    for (int chan : activeInputs)
    {
        validateSampleRate(chan);
    }

    // create new data channels if necessary
    updateSubProcessorMap();
    updateExtraChannels();

    if (outputMode == PH_AND_MAG)
    {
        // keep previously selected input channels from becoming selected extra channels
        deselectAllExtraChannels();
    }
}


Array<int> PhaseCalculator::getActiveInputs()
{
    int numInputs = getNumInputs();
    auto ed = static_cast<PhaseCalculatorEditor*>(getEditor());
    Array<int> activeChannels = ed->getActiveChannels();
    int numToRemove = 0;
    for (int i = activeChannels.size() - 1;
        i >= 0 && activeChannels[i] >= numInputs;
        --i, ++numToRemove);
    activeChannels.removeLast(numToRemove);
    return activeChannels;
}


bool PhaseCalculator::isGeneratesTimestamps() const
{
    return true;
}

int PhaseCalculator::getNumSubProcessors() const
{
    return subProcessorMap.size();
}

float PhaseCalculator::getSampleRate(int subProcessorIdx) const
{
    jassert(subProcessorIdx < getNumSubProcessors());
    int chan = getDataChannelIndex(0, getNodeId(), subProcessorIdx);
    return getDataChannel(chan)->getSampleRate();
}

float PhaseCalculator::getBitVolts(int subProcessorIdx) const
{
    jassert(subProcessorIdx < getNumSubProcessors());
    int chan = getDataChannelIndex(0, getNodeId(), subProcessorIdx);
    return getDataChannel(chan)->getBitVolts();
}

std::queue<double>& PhaseCalculator::getVisPhaseBuffer(ScopedPointer<ScopedLock>& lock)
{
    lock = new ScopedLock(visPhaseBufferLock);
    return visPhaseBuffer;
}

void PhaseCalculator::saveCustomChannelParametersToXml(XmlElement* channelElement,
    int channelNumber, InfoObjectCommon::InfoObjectType channelType)
{
    if (channelType == InfoObjectCommon::DATA_CHANNEL && channelNumber == visContinuousChannel)
    {
        channelElement->setAttribute("visualize", 1);
    }
}

void PhaseCalculator::loadCustomChannelParametersFromXml(XmlElement* channelElement,
    InfoObjectCommon::InfoObjectType channelType)
{
    if (channelElement->hasAttribute("visualize"))
    {
        // The saved channel should be added to the dropdown at this point.
        setVisContChan(channelElement->getIntAttribute("number"));
        static_cast<PhaseCalculatorEditor*>(getEditor())->refreshVisContinuousChan();
    }
}

// ------------ PRIVATE METHODS ---------------

void PhaseCalculator::handleEvent(const EventChannel* eventInfo,
    const MidiMessage& event, int samplePosition)
{
    if (visEventChannel < 0)
    {
        return;
    }

    if (Event::getEventType(event) == EventChannel::TTL)
    {
        TTLEventPtr ttl = TTLEvent::deserializeFromMessage(event, eventInfo);
        if (ttl->getChannel() == visEventChannel && ttl->getState())
        {
            // add timestamp to the queue for visualization
            juce::int64 ts = ttl->getTimestamp();
            jassert(visTsBuffer.empty() || visTsBuffer.back() <= ts);
            visTsBuffer.push(ts);
        }
    }
}

void PhaseCalculator::setAROrder(int newOrder)
{
    if (newOrder == arOrder) { return; }

    arOrder = newOrder;
    updateHistoryLength();

    // update dependent per-channel objects
    int numInputs = getNumInputs();
    for (int chan = 0; chan < numInputs; ++chan)
    {
        bool s = arModelers[chan]->setParams(arOrder, historyLength, sampleRateMultiple[chan]);
        jassert(s);
    }

    for (int i = 0; i < numActiveChansAllocated; i++)
    {
        arParams[i]->resize(arOrder);
    }
}

void PhaseCalculator::setLowCut(float newLowCut)
{
    if (newLowCut == lowCut) { return; }

    lowCut = newLowCut;
    if (lowCut >= highCut)
    {
        // push highCut up
        highCut = lowCut + PASSBAND_EPS;
        static_cast<PhaseCalculatorEditor*>(getEditor())->refreshHighCut();
    }
	updateScaleFactor();
	setFilterParameters();
}

void PhaseCalculator::setHighCut(float newHighCut)
{
    if (newHighCut == highCut) { return; }

    highCut = newHighCut;
    if (highCut <= lowCut)
    {
        // push lowCut down
        lowCut = highCut - PASSBAND_EPS;
        static_cast<PhaseCalculatorEditor*>(getEditor())->refreshLowCut();
    }
	updateScaleFactor();
    setFilterParameters();
}

void PhaseCalculator::setVisContChan(int newChan)
{
    if (newChan >= 0)
    {
        jassert(newChan < filters.size());
        jassert(getActiveInputs().indexOf(newChan) != -1);

        // disable event receival temporarily so we can flush the buffer
        int tempVisEventChan = visEventChannel;
        visEventChannel = -1;

        // clear timestamp queue
        while (!visTsBuffer.empty())
        {
            visTsBuffer.pop();
        }

        // update filter settings
        visReverseFilter.setParams(filters[newChan]->getParams());
        visEventChannel = tempVisEventChan;
    }
    visContinuousChannel = newChan;
}

void PhaseCalculator::updateHistoryLength()
{
    Array<int> activeInputs = getActiveInputs();

    // minimum - must have enough samples to do a Hilbert transform on past values for visualization
    int newHistoryLength = VIS_HILBERT_LENGTH;
    for (int chan : activeInputs)
    {
        newHistoryLength = jmax(newHistoryLength,
            arOrder * sampleRateMultiple[chan] + 1, // minimum to train AR model
            HT_FS * sampleRateMultiple[chan]);      // use @ least 1 second to train model
    }

    if (newHistoryLength == historyLength) { return; }

    historyLength = newHistoryLength;

    // update things that depend on historyLength
    historyBuffer.setSize(numActiveChansAllocated, historyLength);

    for (int i = 0; i < numActiveChansAllocated; ++i)
    {
        bufferFreeSpace.set(i, historyLength);
    }

    for (int chan : activeInputs)
    {
        bool success = arModelers[chan]->setParams(arOrder, historyLength, sampleRateMultiple[chan]);
        jassert(success);
    }
}

void PhaseCalculator::updateScaleFactor()
{
	htScaleFactor = getScaleFactor(lowCut, highCut);
}

void PhaseCalculator::setFilterParameters()
{
    int numInputs = getNumInputs();
    jassert(filters.size() == numInputs);
    double currLowCut = lowCut, currHighCut = highCut;
    jassert(currLowCut >= 0 && currLowCut < currHighCut);

    for (int chan = 0; chan < numInputs; ++chan)
    {
        Dsp::Params params;
        params[0] = getDataChannel(chan)->getSampleRate();  // sample rate
        params[1] = 2;                                      // order
        params[2] = (currHighCut + currLowCut) / 2;         // center frequency
        params[3] = currHighCut - currLowCut;               // bandwidth

        filters[chan]->setParams(params);
    }
}

void PhaseCalculator::addActiveChannel()
{
    numActiveChansAllocated++;

    historyBuffer.setSize(numActiveChansAllocated, historyLength);

    // simple arrays
    bufferFreeSpace.add(historyLength);
    chanState.add(NOT_FULL);
    lastSample.add(0);
	lastComputedSample.add(0);

    // owned arrays
    historyLock.add(new CriticalSection());
    arParamLock.add(new CriticalSection());
    arParams.add(new Array<double>());
    arParams.getLast()->resize(arOrder);
}

bool PhaseCalculator::validateSampleRate(int chan)
{
    auto e = getEditor();
    bool p, r, a;
    e->getChannelSelectionState(chan, &p, &r, &a);
    if (!p) { return false; }

    // test whether sample rate is a multiple of HT_FS
    float fsMult = getDataChannel(chan)->getSampleRate() / HT_FS;
    float fsMultRound = std::round(fsMult);
    if (std::abs(fsMult - fsMultRound) < FLT_EPSILON)
    {
        // leave selected
        int fsMultInt = static_cast<int>(fsMultRound);
        sampleRateMultiple.set(chan, fsMultInt);
        dsOffset.set(chan, fsMultInt - 1);
        bool s = arModelers[chan]->setParams(arOrder, historyLength, fsMultInt);
        jassert(s);
        return true;
    }

    // deselect and send warning
    deselectChannel(chan);
    CoreServices::sendStatusMessage("Channel " + String(chan + 1) + " was deselected because " +
        " its sample rate is not a multiple of " + String(HT_FS));
    return false;
}

void PhaseCalculator::unwrapBuffer(float* wp, int nSamples, int activeChan)
{
    for (int startInd = 0; startInd < nSamples - 1; startInd++)
    {
        float diff = wp[startInd] - (startInd == 0 ? lastSample[activeChan] : wp[startInd - 1]);
        if (abs(diff) > 180)
        {
            // search forward for a jump in the opposite direction
            int endInd;
            int maxInd;
            if (diff < 0)
            // for downward jumps, unwrap if there's a jump back up within GLITCH_LIMIT samples
            {
                endInd = -1;
                maxInd = jmin(startInd + GLITCH_LIMIT, nSamples - 1);
            }
            else
            // for upward jumps, default to unwrapping until the end of the buffer, but stop if there's a jump back down sooner.
            {
                endInd = nSamples;
                maxInd = nSamples - 1;
            }
            for (int currInd = startInd + 1; currInd <= maxInd; currInd++)
            {
                float diff2 = wp[currInd] - wp[currInd - 1];
                if (abs(diff2) > 180 && ((diff > 0) != (diff2 > 0)))
                {
                    endInd = currInd;
                    break;
                }
            }

            // unwrap [startInd, endInd)
            for (int i = startInd; i < endInd; i++)
            {
                wp[i] -= 360 * (diff / abs(diff));
            }

            if (endInd > -1)
            {
                // skip to the end of this unwrapped section
                startInd = endInd;
            }
        }
    }
}

void PhaseCalculator::smoothBuffer(float* wp, int nSamples, int activeChan)
{
    int actualGL = jmin(GLITCH_LIMIT, nSamples - 1);
    float diff = wp[0] - lastSample[activeChan];
    if (diff < 0 && diff > -180)
    {
        // identify whether signal exceeds last sample of the previous buffer within glitchLimit samples.
        int endIndex = -1;
        for (int i = 1; i <= actualGL; i++)
        {
            if (wp[i] > lastSample[activeChan])
            {
                endIndex = i;
                break;
            }
            // corner case where signal wraps before it exceeds lastSample
            else if (wp[i] - wp[i - 1] < -180 && (wp[i] + 360) > lastSample[activeChan])
            {
                wp[i] += 360;
                endIndex = i;
                break;
            }
        }

        if (endIndex != -1)
        {
            // interpolate points from buffer start to endIndex
            float slope = (wp[endIndex] - lastSample[activeChan]) / (endIndex + 1);
            for (int i = 0; i < endIndex; i++)
            {
                wp[i] = lastSample[activeChan] + (i + 1) * slope;
            }
        }
    }
}

void PhaseCalculator::updateSubProcessorMap()
{
    if (outputMode != PH_AND_MAG)
    {
        subProcessorMap.clear();
        return;
    }

    // fill map according to selected channels, and remove outdated entries.
    uint16 maxUsedIdx = 0;
    SortedSet<int> foundFullIds;
    Array<int> unmappedFullIds;

    Array<int> activeInputs = getActiveInputs();
    for (int chan : activeInputs)
    {
        const DataChannel* chanInfo = getDataChannel(chan);
        uint16 sourceNodeId = chanInfo->getSourceNodeID();
        uint16 subProcessorIdx = chanInfo->getSubProcessorIdx();
        int procFullId = static_cast<int>(getProcessorFullId(sourceNodeId, subProcessorIdx));
        foundFullIds.add(procFullId);

        if (subProcessorMap.contains(procFullId))
        {
            maxUsedIdx = jmax(maxUsedIdx, subProcessorMap[subProcessorIdx]);
        }
        else // add new entry for this source subprocessor
        {
            // try to match index if possible
            if (!subProcessorMap.containsValue(subProcessorIdx))
            {
                subProcessorMap.set(procFullId, subProcessorIdx);
                maxUsedIdx = jmax(maxUsedIdx, subProcessorIdx);
            }
            else
            {
                unmappedFullIds.add(procFullId);
            }
        }
    }
    // assign remaining unmapped ids
    for (int id : unmappedFullIds)
    {
        subProcessorMap.set(id, ++maxUsedIdx);
    }

    // remove outdated entries
    Array<int> outdatedFullIds;
    HashMap<int, juce::uint16>::Iterator it(subProcessorMap);
    while (it.next())
    {
        int key = it.getKey();
        if (!foundFullIds.contains(key))
        {
            outdatedFullIds.add(key);
        }
    }
    for (int id : outdatedFullIds)
    {
        subProcessorMap.remove(id);
    }
}

void PhaseCalculator::updateExtraChannels()
{
    // reset dataChannelArray to # of inputs
    int numInputs = getNumInputs();
    int numChannels = dataChannelArray.size();
    jassert(numChannels >= numInputs);
    dataChannelArray.removeLast(numChannels - numInputs);

    if (outputMode == PH_AND_MAG)
    {
        Array<int> activeInputs = getActiveInputs();
        for (int chan : activeInputs)
        {
            // see GenericProcessor::createDataChannelsByType
            DataChannel* baseChan = dataChannelArray[chan];
            uint16 sourceNodeId = baseChan->getSourceNodeID();
            uint16 subProcessorIdx = baseChan->getSubProcessorIdx();
            uint32 baseFullId = getProcessorFullId(sourceNodeId, subProcessorIdx);

            DataChannel* newChan = new DataChannel(
                baseChan->getChannelType(),
                baseChan->getSampleRate(),
                this,
                subProcessorMap[static_cast<int>(baseFullId)]);
            newChan->setBitVolts(baseChan->getBitVolts());
            newChan->addToHistoricString(getName());
            dataChannelArray.add(newChan);
        }
    }
    settings.numOutputs = dataChannelArray.size();
}

void PhaseCalculator::deselectChannel(int chan)
{
    jassert(chan >= 0 && chan < getTotalDataChannels());

    auto ed = getEditor();
    bool p, r, a;
    ed->getChannelSelectionState(chan, &p, &r, &a);
    ed->setChannelSelectionState(chan - 1, false, r, a);
}

void PhaseCalculator::deselectAllExtraChannels()
{
    jassert(outputMode == PH_AND_MAG);
    Array<int> activeChans = getEditor()->getActiveChannels();
    int nInputs = getNumInputs();
    int nExtraChans = 0;
    for (int chan : activeChans)
    {
        if (chan < nInputs)
        {
            nExtraChans++;
        }
        else if (chan < nInputs + nExtraChans)
        {
            deselectChannel(chan);
        }
    }
}

void PhaseCalculator::calcVisPhases(juce::int64 sdbEndTs)
{
    juce::int64 minTs = sdbEndTs - VIS_TS_MAX_DELAY;
    juce::int64 maxTs = sdbEndTs - VIS_TS_MIN_DELAY;

    // discard any timestamps less than minTs
    while (!visTsBuffer.empty() && visTsBuffer.front() < minTs)
    {
        visTsBuffer.pop();
    }

    if (!visTsBuffer.empty() && visTsBuffer.front() <= maxTs)
    {
        // perform reverse filtering and Hilbert transform
        Array<int> activeInputs = getActiveInputs();
        int visActiveChan = activeInputs.indexOf(visContinuousChannel);
        jassert(visActiveChan != -1);
        const double* rpBuffer = historyBuffer.getReadPointer(visActiveChan, historyLength - 1);
        for (int i = 0; i < VIS_HILBERT_LENGTH; ++i)
        {
            visHilbertBuffer.set(i, rpBuffer[-i]);
        }

        double* realPtr = visHilbertBuffer.getRealPointer();
        visReverseFilter.reset();
        visReverseFilter.process(VIS_HILBERT_LENGTH, &realPtr);

        // un-reverse values
        visHilbertBuffer.reverseReal(VIS_HILBERT_LENGTH);

        visForwardPlan.execute();
        hilbertManip(&visHilbertBuffer);
        visBackwardPlan.execute();

        juce::int64 ts;
        ScopedLock phaseBufferLock(visPhaseBufferLock);
        while (!visTsBuffer.empty() && (ts = visTsBuffer.front()) <= maxTs)
        {
            visTsBuffer.pop();
            juce::int64 delay = sdbEndTs - ts;
            std::complex<double> analyticPt = visHilbertBuffer.getAsComplex(VIS_HILBERT_LENGTH - delay);
            double phaseRad = std::arg(analyticPt);
            visPhaseBuffer.push(phaseRad);

            // add to event channel
            double eventData = phaseRad * 180.0 / Dsp::doublePi;
            juce::int64 eventTs = sdbEndTs - getNumSamples(visContinuousChannel);
            BinaryEventPtr event = BinaryEvent::createBinaryEvent(visPhaseChannel, eventTs, &eventData, sizeof(double));
            addEvent(visPhaseChannel, event, 0);
        }
    }
}

void PhaseCalculator::arPredict(const double* readEnd, double* writeStart, int writeNum, const double* params, int order)
{
    for (int s = 0; s < writeNum; ++s)
    {
        // s = index to write output
        writeStart[s] = 0;
        for (int ind = s - 1; ind > s - 1 - order; --ind)
        {
            // ind = index of previous output to read
            writeStart[s] -= params[s - 1 - ind] * (ind < 0 ? readEnd[ind] : writeStart[ind]);
        }
    }
}

void PhaseCalculator::hilbertManip(FFTWArray* fftData)
{
    int n = fftData->getLength();

    // Normalize DC and Nyquist, normalize and double prositive freqs, and set negative freqs to 0.
    int lastPosFreq = (n + 1) / 2 - 1;
    int firstNegFreq = n / 2 + 1;
    int numPosNegFreqDoubles = lastPosFreq * 2; // sizeof(complex<double>) = 2 * sizeof(double)
    bool hasNyquist = (n % 2 == 0);

    std::complex<double>* wp = fftData->getComplexPointer();

    // normalize but don't double DC value
    wp[0] /= n;

    // normalize and double positive frequencies
    FloatVectorOperations::multiply(reinterpret_cast<double*>(wp + 1), 2.0 / n, numPosNegFreqDoubles);

    if (hasNyquist)
    {
        // normalize but don't double Nyquist frequency
        wp[lastPosFreq + 1] /= n;
    }

    // set negative frequencies to 0
    FloatVectorOperations::clear(reinterpret_cast<double*>(wp + firstNegFreq), numPosNegFreqDoubles);
}

double PhaseCalculator::getScaleFactor(double lowCut, double highCut)
{
	jassert(HT_SCALE_FACTOR_QUERY_FREQS >= 2);
	int numFreqs = HT_SCALE_FACTOR_QUERY_FREQS;

	double meanAbsResponse = 0;

	// at each frequency, calculate the filter response
	for (int kFreq = 0; kFreq < numFreqs; ++kFreq)
	{
		double freq = lowCut + kFreq * (highCut - lowCut) / (numFreqs - 1);
		double normFreq = freq / (HT_FS / 2);
		std::complex<double> response = 0;

		for (int kCoef = 0; kCoef <= HT_ORDER; ++kCoef)
		{
			response += std::polar(HT_COEF[kCoef], -kCoef * normFreq * Dsp::doublePi);
		}

		meanAbsResponse += std::abs(response) / numFreqs;
	}

	return 1 / meanAbsResponse;
}

// Hilbert transformer coefficients (FIR filter)
// Obtained by matlab call "firpm(HT_ORDER, [4 HT_FS/2-4]/(HT_FS/2), [1 1], 'hilbert')"
// Should be modified if HT_ORDER or HT_FS are changed, or if targeting frequencies lower than 4 Hz.
const double PhaseCalculator::HT_COEF[HT_ORDER + 1] = {
    -0.287572507836144,
    2.76472250749945e-05,
    -0.0946113256432684,
    -0.000258874394997638,
    -0.129436276914844,
    -0.000160842742642405,
    -0.213150968600552,
    -0.00055322197399798,
    -0.636856982103511,
    0,
    0.636856982103511,
    0.00055322197399798,
    0.213150968600552,
    0.000160842742642405,
    0.129436276914844,
    0.000258874394997638,
    0.0946113256432684,
    -2.76472250749945e-05,
    0.287572507836144
};

// ----------- ARTimer ---------------

ARTimer::ARTimer() : Timer()
{
    hasRung = false;
}

ARTimer::~ARTimer() {}

void ARTimer::timerCallback()
{
    hasRung = true;
}

bool ARTimer::check()
{
    bool temp = hasRung;
    hasRung = false;
    return temp;
}
