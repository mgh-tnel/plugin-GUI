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

#include "CumulativeTFR.h"
#include <cmath>

CumulativeTFR::CumulativeTFR(int ng1, int ng2, int nf, int nt, int Fs, double fftSec)
    : nGroup1Chans  (ng1)
    , nGroup2Chans  (ng2)
    , nFreqs        (nf)
    , nTimes        (nt)
    , nfft          (int(fftSec * Fs))
    , convInput     (nfft)
    , freqData      (nfft)
    , convOutput    (nfft)
    , fftPlan       (nfft, &convInput, &freqData, FFTW_FORWARD, FFTW_MEASURE)
    , ifftPlan      (nfft, &freqData, &convOutput, FFTW_BACKWARD, FFTW_MEASURE)
    , pxxs          (ng1,
                    vector<vector<RealAccum>>(nf,
                    vector<RealAccum>(nt)))
    , pyys          (ng2,
                    vector<vector<RealAccum>>(nf,
                    vector<RealAccum>(nt)))
    , pxys          (ng1 * ng2,
                    vector<vector<ComplexAccum>>(nf,
                    vector<ComplexAccum>(nt)))
{}


void CumulativeTFR::addTrial(AudioBuffer<float> dataBuffer, int chan)
{
    const float* rpChan = dataBuffer.getReadPointer(chan);
	
    int segmentLen = 8;
	int windowLen = 2;
	float stepLen = 0.1;
	int interpRatio = 2;

	float winsPerSegment = (segmentLen - windowLen) / stepLen;

	//// Update convInput ////

	// copy dataBuffer to fft input
	int dataSize = dataBuffer.getNumSamples();
	convInput.copyFrom(dataBuffer.getReadPointer(0), dataSize, 0); // Double to float...?
    
	//// Execute fft ////
	fftPlan.execute();

    FFTWArray tempBuf = freqData;

	//// Use freqData to find pow/crss ////
	int channel = chan; 

    
    // fourier transform of https://dsp.stackexchange.com/questions/736/how-do-i-implement-cross-correlation-to-prove-two-audio-files-are-similar
    // fft over wavelet?
    
    // Multiply the two fft datas
    // Use this to find power and cross spectrum
    // Update the vectors

	//// Ready to update coherence ////
	updateCoherenceStats();
}


void CumulativeTFR::updateCoherenceStats()
{
    for (int c1 = 0, comb = 0; c1 < nGroup1Chans; ++c1)
    {
        for (int c2 = 0; c2 < nGroup2Chans; ++c2, ++comb)
        {
            double* meanDest = meanCoherence[comb].data();
            double* stdDest = stdCoherence[comb].data();

            for (int f = 0; f < nFreqs; ++f)
            {
                // compute coherence at each time
                RealAccum coh;

                for (int t = 0; t < nTimes; ++t)
                {
                    coh.addValue(singleCoherence(
                        pxxs[c1][f][t].getAverage(),
                        pyys[c2][f][t].getAverage(),
                        pxys[comb][f][t].getAverage()));
                }

                meanDest[f] = coh.getAverage();
                if (nTimes < 2)
                {
                    stdDest[f] = 0;
                }
                else
                {
                    stdDest[f] = std::sqrt(coh.getVariance() * nTimes / (nTimes - 1));
                }
            }
        }
    }
}


double CumulativeTFR::singleCoherence(double pxx, double pyy, std::complex<double> pxy)
{
    return std::norm(pxy) / (pxx * pyy);
}


double CumulativeTFR::calcCrssspctrm(int combination, FFTWArray freqData)
{
	// Get region 1 and region 2 data
	std::complex<double> complexData = freqData.getAsComplex(combination); // Something like this but for both channels
}