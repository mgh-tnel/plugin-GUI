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

#include <cstring>       // memset (for Burg method)
#include <numeric>       // inner_product

#include "PhaseCalculator.h"
#include "PhaseCalculatorEditor.h"
#include "burg.h"        // Autoregressive modeling

PhaseCalculator::PhaseCalculator()
    : GenericProcessor      ("Phase Calculator")
    , Thread                ("AR Modeler")
    , calcInterval          (50)
    , arOrder               (20)
    , lowCut                (4.0)
    , highCut               (8.0)
    , haveSentWarning       (false)
    , outputMode            (PH)
    , stimEventChannel      (1)
    , stimContinuousChannel (0)
    , gtHilbertBuffer       (GT_HILBERT_LENGTH)
    , gtPlanForward         (GT_HILBERT_LENGTH, &gtHilbertBuffer, FFTW_MEASURE)
    , gtPlanBackward        (GT_HILBERT_LENGTH, &gtHilbertBuffer, FFTW_BACKWARD, FFTW_MEASURE)
{
    setProcessorType(PROCESSOR_TYPE_FILTER);
    setProcessLength(1 << 13, 1 << 12);
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

void PhaseCalculator::setParameter(int parameterIndex, float newValue)
{
    int numInputs = getNumInputs();

    switch (parameterIndex) {
    case NUM_FUTURE:
        setNumFuture(static_cast<int>(newValue));
        break;

    case RECALC_INTERVAL:
        calcInterval = static_cast<int>(newValue);
        break;

    case AR_ORDER:
        arOrder = static_cast<int>(newValue);
        h.resize(arOrder);
        g.resize(arOrder);
        for (int i = 0; i < getNumInputs(); i++)
            arParams[i]->resize(arOrder);
        break;

    case LOWCUT:
        lowCut = newValue;
        setFilterParameters();
        break;

    case HIGHCUT:
        highCut = newValue;
        setFilterParameters();
        break;

    case OUTPUT_MODE:
        outputMode = static_cast<OutputMode>(static_cast<int>(newValue));
        CoreServices::updateSignalChain(editor);  // add or remove channels if necessary
        break;

    case STIM_E_CHAN:
        jassert(newValue >= -1);
        stimEventChannel = static_cast<int>(newValue);
        break;

    case STIM_C_CHAN:
    {
        int newStimContChan = static_cast<int>(newValue);
        jassert(newStimContChan >= 0 && newStimContChan < filters.size());
        int tempStimEventChan = stimEventChannel;
        stimEventChannel = -1; // disable temporarily

        // clear timestamp queue
        while (!stimTsBuffer.empty())
            stimTsBuffer.pop();

        // update filter settings
        gtReverseFilter.copyParamsFrom(filters[newStimContChan]);

        stimContinuousChannel = newStimContChan;
        stimEventChannel = tempStimEventChan;
        break;
    }        
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

    // iterate over active input channels
    int nInputs = getNumInputs();
    Array<int> activeChannels = editor->getActiveChannels();
    int nActiveChannels = activeChannels.size();
    for (int activeChan = 0;
        activeChan < nActiveChannels && activeChannels[activeChan] < nInputs;
        ++activeChan)
    {
        int chan = activeChannels[activeChan];
        int nSamples = getNumSamples(chan);
        if (nSamples == 0)
            continue;

        // Filter the data.
        float* wpIn = buffer.getWritePointer(chan);
        filters[chan]->process(nSamples, &wpIn);

        // If there are more samples than we have room to process, process the most recent samples and output zero
        // for the rest (this is an error that should be noticed and fixed).
        int processPastLength = processLength - numFuture;
        int bufferStartIndex = jmax(nSamples - bufferLength, 0);
        int processStartIndex = jmax(nSamples - processPastLength, 0);
        
        jassert(processStartIndex >= bufferStartIndex); // since bufferLength >= processPastLength
        
        int nSamplesToEnqueue = nSamples - bufferStartIndex;
        int nSamplesToProcess = nSamples - processStartIndex;

        if (processStartIndex != 0)
        {
            // clear the extra samples and send a warning message
            buffer.clear(chan, 0, processStartIndex);
            if (!haveSentWarning)
            {
                CoreServices::sendStatusMessage("WARNING: Phase Calculator buffer is shorter than the sample buffer!");
                haveSentWarning = true;
            }
        }

        // shift old data and copy new data into sharedDataBuffer
        int nOldSamples = bufferLength - nSamplesToEnqueue;

        const double* rpBuffer = sharedDataBuffer.getReadPointer(chan, nSamplesToEnqueue);
        double* wpBuffer = sharedDataBuffer.getWritePointer(chan);

        // critical section for this channel's sharedDataBuffer
        // note that the floats are coerced to doubles here - this is important to avoid over/underflow when calculating the phase.
        {
            const ScopedLock myScopedLock(*sdbLock[chan]);

            // shift old data
            for (int i = 0; i < nOldSamples; i++)
                *wpBuffer++ = *rpBuffer++;

            // copy new data
            wpIn += bufferStartIndex;
            for (int i = 0; i < nSamplesToEnqueue; i++)
                *wpBuffer++ = *wpIn++;
        }

        if (chanState[chan] == NOT_FULL)
        {
            if (bufferFreeSpace[chan] <= nSamplesToEnqueue)
            {
                // now that dataToProcess for this channel is full,
                // let the thread start calculating the AR model.
                chanState.set(chan, FULL_NO_AR);
                bufferFreeSpace.set(chan, 0);
            }
            else
            {
                bufferFreeSpace.set(chan, bufferFreeSpace[chan] - nSamplesToEnqueue);
            }
        }

        // calc phase and write out (only if AR model has been calculated)
        if (chanState[chan] == FULL_AR) {

            // copy data to dataToProcess
            const double* rpSDB = sharedDataBuffer.getReadPointer(chan, bufferLength - processPastLength);
            hilbertBuffer[chan]->copyFrom(rpSDB, processPastLength);

            // use AR(20) model to predict upcoming data and append to dataToProcess
            double* wpProcess = hilbertBuffer[chan]->getRealPointer(processPastLength);

            // read current AR parameters once
            Array<double> currParams;
            for (int i = 0; i < arOrder; i++)
                currParams.set(i, (*arParams[chan])[i]);
            double* rpParam = currParams.getRawDataPointer();

            arPredict(wpProcess, numFuture, rpParam, arOrder);

            // Hilbert-transform dataToProcess
            pForward[chan]->execute();      // reads from dataToProcess, writes to fftData
            hilbertManip(hilbertBuffer[chan]);
            pBackward[chan]->execute();     // reads from fftData, writes to dataOut

            // calculate phase and write out to buffer
            const std::complex<double>* rpProcess =
                hilbertBuffer[chan]->getReadPointer(processPastLength - nSamplesToProcess);
            float* wpOut = buffer.getWritePointer(chan);
            float* wpOut2;
            if (outputMode == PH_AND_MAG)
                // second output channel
                wpOut2 = buffer.getWritePointer(nInputs + activeChan);

            for (int i = 0; i < nSamplesToProcess; i++)
            {
                switch (outputMode)
                {
                case MAG:
                    wpOut[i + processStartIndex] = static_cast<float>(std::abs(rpProcess[i]));
                    break;
                
                case PH_AND_MAG:
                    wpOut2[i + processStartIndex] = static_cast<float>(std::abs(rpProcess[i]));
                    // fall through
                case PH:
                    // output in degrees
                    wpOut[i + processStartIndex] = static_cast<float>(std::arg(rpProcess[i]) * (180.0 / Dsp::doublePi));
                    break;
                    
                case IM:
                    wpOut[i + processStartIndex] = static_cast<float>(std::imag(rpProcess[i]));
                    break;
                }
            }

            // unwrapping / smoothing
            if (outputMode == PH || outputMode == PH_AND_MAG)
            {
                unwrapBuffer(wpOut, nSamples, chan);
                smoothBuffer(wpOut, nSamples, chan);
            }
        }
        else // fifo not full / becoming full
        {
            // just output zeros
            buffer.clear(chan, processStartIndex, nSamplesToProcess);
        }

        // keep track of last sample
        lastSample.set(chan, buffer.getSample(chan, nSamples - 1));
    }

    if (stimEventChannel >= 0)
    {
        checkForEvents();
        calcStimPhases(getTimestamp(stimContinuousChannel) + getNumSamples(stimContinuousChannel) - 1);
    }
}

// starts thread when acquisition begins
bool PhaseCalculator::enable()
{
    if (!isEnabled)
        return false;

    startThread(AR_PRIORITY);

    // have to manually enable editor, I guess...
    PhaseCalculatorEditor* editor = static_cast<PhaseCalculatorEditor*>(getEditor());
    editor->enable();
    return true;
}

bool PhaseCalculator::disable()
{
    PhaseCalculatorEditor* editor = static_cast<PhaseCalculatorEditor*>(getEditor());
    editor->disable();

    signalThreadShouldExit();

    // reset channel states
    for (int i = 0; i < chanState.size(); i++)
        chanState.set(i, NOT_FULL);

    // reset bufferFreeSpace
    for (int i = 0; i < bufferFreeSpace.size(); i++)
        bufferFreeSpace.set(i, bufferLength);

    // reset last sample containers
    for (int i = 0; i < lastSample.size(); i++)
        lastSample.set(i, 0);

    // reset buffer overflow warning
    haveSentWarning = false;

    return true;
}


float PhaseCalculator::getRatioFuture()
{
    return static_cast<float>(numFuture) / processLength;
}

// thread routine
void PhaseCalculator::run()
{
    Array<double> data;
    data.resize(bufferLength);

    Array<double> paramsTemp;
    paramsTemp.resize(arOrder);

    ARTimer timer;
    int currInterval = calcInterval;
    timer.startTimer(currInterval);

    while (true)
    {
        if (threadShouldExit())
            return;

        for (int chan = 0; chan < chanState.size(); chan++)
        {
            if (chanState[chan] == NOT_FULL)
                continue;

            // critical section for sharedDataBuffer
            {
                const ScopedLock myScopedLock(*sdbLock[chan]);

                for (int i = 0; i < bufferLength; i++)
                    data.set(i, sharedDataBuffer.getSample(chan, i));
            }
            // end critical section

            double* inputseries = data.getRawDataPointer();
            double* paramsOut = paramsTemp.getRawDataPointer();
            double* perRaw = per.getRawDataPointer();
            double* pefRaw = pef.getRawDataPointer();
            double* hRaw = h.getRawDataPointer();
            double* gRaw = g.getRawDataPointer();

            // reset per and pef
            memset(perRaw, 0, bufferLength * sizeof(double));
            memset(pefRaw, 0, bufferLength * sizeof(double));

            // calculate parameters
            ARMaxEntropy(inputseries, bufferLength, arOrder, paramsOut, perRaw, pefRaw, hRaw, gRaw);

            // write params quasi-atomically
            juce::Array<double>* myParams = arParams[chan];
            for (int i = 0; i < arOrder; i++)
                myParams->set(i, paramsTemp[i]);

            chanState.set(chan, FULL_AR);
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
                return;
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
    // react to changed # of inputs
    int numInputs = getNumInputs();
    int prevNumInputs = sharedDataBuffer.getNumChannels();
    int numInputsChange = numInputs - prevNumInputs;

    sharedDataBuffer.setSize(numInputs, bufferLength);

    if (numInputsChange > 0)
    {
        // resize simple arrays
        bufferFreeSpace.insertMultiple(-1, bufferLength, numInputsChange);
        chanState.insertMultiple(-1, NOT_FULL, numInputsChange);
        lastSample.insertMultiple(-1, 0, numInputsChange);

        // add new objects at new indices
        for (int i = prevNumInputs; i < numInputs; i++)
        {
            // processing buffers
            hilbertBuffer.set(i, new FFTWArray(processLength));

            // FFT plans
            pForward.set(i, new FFTWPlan(processLength, hilbertBuffer[i], FFTW_MEASURE));
            pBackward.set(i, new FFTWPlan(processLength, hilbertBuffer[i], FFTW_BACKWARD, FFTW_MEASURE));

            // mutexes
            sdbLock.set(i, new CriticalSection());

            // AR parameters
            arParams.set(i, new juce::Array<double>());
            arParams[i]->resize(arOrder);

            // Bandpass filters
            filters.set(i, new BandpassFilter());
        }
    }
    else if (numInputsChange < 0)
    {
        // delete unneeded entries
        bufferFreeSpace.removeLast(-numInputsChange);
        hilbertBuffer.removeLast(-numInputsChange);
        pForward.removeLast(-numInputsChange);
        pBackward.removeLast(-numInputsChange);
        sdbLock.removeLast(-numInputsChange);
        arParams.removeLast(-numInputsChange);
        filters.removeLast(-numInputsChange);
    }
    // call this no matter what, since the sample rate may have changed.
    setFilterParameters();

    // create new data channels if necessary
    updateSubProcessorMap();
    updateExtraChannels();
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

std::queue<double>& PhaseCalculator::getStimPhaseBuffer(ScopedPointer<ScopedLock>& lock)
{
    lock = new ScopedLock(stimPhaseBufferLock);
    return stimPhaseBuffer;
}

// ------------ PRIVATE METHODS ---------------

void PhaseCalculator::handleEvent(const EventChannel* eventInfo,
    const MidiMessage& event, int samplePosition)
{
    if (stimEventChannel < 0)
        return;

    if (Event::getEventType(event) == EventChannel::TTL)
    {
        TTLEventPtr ttl = TTLEvent::deserializeFromMessage(event, eventInfo);
        if (ttl->getChannel() == stimEventChannel)
        {
            // add event to stimEventBuffer
            juce::int64 ts = ttl->getTimestamp();
            stimTsBuffer.push(ts);
        }
    }
}

void PhaseCalculator::addAngleToCanvas(double newAngle)
{
}

void PhaseCalculator::setProcessLength(int newProcessLength, int newNumFuture)
{
    jassert(newNumFuture <= newProcessLength - arOrder);

    processLength = newProcessLength;
    if (newNumFuture != numFuture)
        setNumFuture(newNumFuture);

    // update fields that depend on processLength
    int nInputs = getNumInputs();
    for (int i = 0; i < nInputs; i++)
    {
        // processing buffers
        hilbertBuffer[i]->resize(processLength);

        // FFT plans
        pForward.set(i, new FFTWPlan(processLength, hilbertBuffer[i], FFTW_MEASURE));
        pBackward.set(i, new FFTWPlan(processLength, hilbertBuffer[i], FFTW_BACKWARD, FFTW_MEASURE));
    }
}

void PhaseCalculator::setNumFuture(int newNumFuture)
{
    numFuture = newNumFuture;
    bufferLength = jmax(GT_HILBERT_LENGTH, processLength - newNumFuture);
    int nInputs = getNumInputs();
    sharedDataBuffer.setSize(nInputs, bufferLength);

    per.resize(bufferLength);
    pef.resize(bufferLength);

    for (int i = 0; i < nInputs; i++)
        bufferFreeSpace.set(i, bufferLength);
}

// from FilterNode code
void PhaseCalculator::setFilterParameters()
{
    int nChan = getNumInputs();
    for (int chan = 0; chan < nChan; chan++)
    {
        jassert(chan < filters.size());

        Dsp::Params params;
        params[0] = getDataChannel(chan)->getSampleRate();  // sample rate
        params[1] = 2;                                      // order
        params[2] = (highCut + lowCut) / 2;                 // center frequency
        params[3] = highCut - lowCut;                       // bandwidth

        filters[chan]->setParams(params);
    }

    // copy settings for corresponding channel to gtReverseFilter
    if (stimContinuousChannel >= 0 && stimContinuousChannel < nChan)
    {
        gtReverseFilter.copyParamsFrom(filters[stimContinuousChannel]);
    }
}

void PhaseCalculator::unwrapBuffer(float* wp, int nSamples, int chan)
{
    for (int startInd = 0; startInd < nSamples - 1; startInd++)
    {
        float diff = wp[startInd] - (startInd == 0 ? lastSample[chan] : wp[startInd - 1]);
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
                wp[i] -= 360 * (diff / abs(diff));

            if (endInd > -1)
                // skip to the end of this unwrapped section
                startInd = endInd;
        }
    }
}

void PhaseCalculator::smoothBuffer(float* wp, int nSamples, int chan)
{
    int actualMaxGL = jmin(GLITCH_LIMIT, nSamples - 1);
    float diff = wp[0] - lastSample[chan];
    if (diff < 0 && diff > -180)
    {
        // identify whether signal exceeds last sample of the previous buffer within glitchLimit samples.
        int endIndex = -1;
        for (int i = 1; i <= actualMaxGL; i++)
        {
            if (wp[i] > lastSample[chan])
            {
                endIndex = i;
                break;
            }
            // corner case where signal wraps before it exceeds lastSample
            else if (wp[i] - wp[i - 1] < -180 && (wp[i] + 360) > lastSample[chan])
            {
                wp[i] += 360;
                endIndex = i;
                break;
            }
        }

        if (endIndex != -1)
        {
            // interpolate points from buffer start to endIndex
            float slope = (wp[endIndex] - lastSample[chan]) / (endIndex + 1);
            for (int i = 0; i < endIndex; i++)
                wp[i] = lastSample[chan] + (i + 1) * slope;
        }
    }
}

void PhaseCalculator::updateSubProcessorMap()
{
    subProcessorMap.clear();
    if (outputMode == PH_AND_MAG)
    {
        uint16 maxUsedIdx = 0;
        Array<int> unmappedFullIds;

        // iterate over active input channels
        int numInputs = getNumInputs();
        Array<int> activeChans = editor->getActiveChannels();
        int numActiveChans = activeChans.size();
        for (int i = 0; i < numActiveChans && activeChans[i] < numInputs; ++i)
        {
            int c = activeChans[i];

            const DataChannel* chan = getDataChannel(c);
            uint16 sourceNodeId = chan->getSourceNodeID();
            uint16 subProcessorIdx = chan->getSubProcessorIdx();
            int procFullId = static_cast<int>(getProcessorFullId(sourceNodeId, subProcessorIdx));
            if (!subProcessorMap.contains(procFullId))
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
        int numUnmappedIds = unmappedFullIds.size();
        for (int i = 0; i < numUnmappedIds; ++i)
            subProcessorMap.set(unmappedFullIds[i], ++maxUsedIdx);
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
        // iterate over active input channels
        Array<int> activeChans = editor->getActiveChannels();
        int numActiveChans = activeChans.size();
        for (int i = 0; i < numActiveChans && activeChans[i] < numInputs; ++i)
        {
            int c = activeChans[i];

            // see GenericProcessor::createDataChannelsByType
            DataChannel* baseChan = dataChannelArray[c];
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

void PhaseCalculator::calcStimPhases(juce::int64 sdbEndTs)
{
    juce::int64 minTs = sdbEndTs - GT_TS_MAX_DELAY;
    juce::int64 maxTs = sdbEndTs - GT_TS_MIN_DELAY;

    // discard any timestamps less than minTs
    while (!stimTsBuffer.empty() && stimTsBuffer.front() < minTs)
        stimTsBuffer.pop();

    if (!stimTsBuffer.empty() && stimTsBuffer.front() <= maxTs)
    {
        // perform reverse filtering and Hilbert transform
        const double* rpBuffer = sharedDataBuffer.getReadPointer(stimContinuousChannel, bufferLength - 1);
        for (int i = 0; i < GT_HILBERT_LENGTH; ++i)
        {
            gtHilbertBuffer.set(i, rpBuffer[-i]);
        }
        gtPlanForward.execute();
        hilbertManip(&gtHilbertBuffer);
        gtPlanBackward.execute();

        juce::int64 ts;
        ScopedLock phaseBufferLock(stimPhaseBufferLock);
        while (!stimTsBuffer.empty() && (ts = stimTsBuffer.front()) <= maxTs)
        {
            stimTsBuffer.pop();
            juce::int64 delay = sdbEndTs - ts;
            std::complex<double> analyticPt = gtHilbertBuffer[delay];
            stimPhaseBuffer.push(std::arg(analyticPt));
        }
    }
}

void PhaseCalculator::arPredict(double* writeStart, int writeNum, const double* params, int order)
{
    std::reverse_iterator<double*> dataIter;
    int i;
    for (i = 0; i < writeNum; i++)
    {
        // the reverse iterator actually starts out pointing at element i-1
        dataIter = std::reverse_iterator<double*>(writeStart + i);
        writeStart[i] = -std::inner_product<const double*, std::reverse_iterator<const double*>, double>(
            params, params + order, dataIter, 0);
    }
}

void PhaseCalculator::hilbertManip(FFTWArray* fftData)
{
    int n = fftData->getLength();

    // Normalize DC and Nyquist, normalize and double prositive freqs, and set negative freqs to 0.
    int lastPosFreq = (n + 1) / 2 - 1;
    int firstNegFreq = n / 2 + 1;
    std::complex<double>* wp = fftData->getWritePointer();

    for (int i = 0; i < n; i++) {
        if (i > 0 && i <= lastPosFreq)
            // normalize and double
            wp[i] *= (2.0 / n);
        else if (i < firstNegFreq)
            // normalize but don't double
            wp[i] /= n;
        else
            // set to 0
            wp[i] = 0;
    }
}

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
