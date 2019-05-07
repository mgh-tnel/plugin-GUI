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

#ifndef CUMULATIVE_TFR_H_INCLUDED
#define CUMULATIVE_TFR_H_INCLUDED

#include <FFTWWrapper.h>

#include "CircularArray.h"

#include <vector>
#include <complex>

class CumulativeTFR
{
    // shorten some things
    template<typename T>
    using vector = std::vector<T>;

    using RealAccum = StatisticsAccumulator<double>;
   // using ComplexAccum = StatisticsAccumulator<std::complex<double>>;

    struct ComplexAccum
    {
        ComplexAccum() 
            : count (0)
            , sum   (0,0)
        {}

        std::complex<double> getAverage()
        {
            return count > 0 ? sum / (double)count : std::complex<double>();
        }

        void addValue(std::complex<double> x)
        {
            sum += x;
            ++count;
        }

    private:
        std::complex<double> sum;
        size_t count;
    };

    struct ComplexWeightedAccum
    {
        ComplexWeightedAccum(double alpha)
            : count (0)
            , sum   (0, 0)
            , alpha (alpha)
        {}

        std::complex<double> getAverage()
        {
            return count > 0 ? sum / (double)count : std::complex<double>();
        }

        void addValue(std::complex<double> x)
        {
            sum = x + (1 - alpha) * sum;
            count = 1 + (1 - alpha) * count;
        }

    private:
        std::complex<double> sum;
        size_t count;
        const double alpha;

    };


    struct RealWeightedAccum
    {
        RealWeightedAccum(double alpha)
            : count (0)
            , sum   (0)
            , alpha (alpha)
        {}

        double getAverage()
        {
            return count > 0 ? sum / (double)count : double();
        }

        void addValue(double x)
        {
            sum = x + (1 - alpha) * sum;
            count = 1 + (1 - alpha) * count;
        }

    private:
        double sum;
        size_t count;

        const double alpha;

    };

public:
    CumulativeTFR(int ng1, int ng2, int nf, int nt, int Fs,
        int winLen = 2, float stepLen = 0.1, float freqStep = 0.25,
        int freqStart = 1, float interpRatio = 2, double fftSec = 10.0, double alpha = 0);

    // Handle a new buffer of data. Preform FFT and create pxxs, pyys.
    void addTrial(const double* fftIn, int chan);

    // Function to get coherence between two channels
    void getMeanCoherence(int chanX, int chanY, double* meanDest, int comb);
    
    vector<vector<double>> getCurrentStdCoherence();

private:
	// Generate wavelet call in update settings if segLen changes
    void CumulativeTFR::generateWavelet();
    // calc pxys
	void CumulativeTFR::calcCrssspctrm();

    int nGroup1Chans;
    int nGroup2Chans;
    const int nFreqs;
    const int Fs;
    const int nTimes;
    int segmentLen;
    int windowLen;
    float stepLen;
    int interpRatio;

    float freqStep;
    int freqStart;
    int freqEnd;

    int trimTime;

    // # channels x # frequencies x # times
	vector<vector<vector<const std::complex<double>>>> spectrumBuffer;
    vector<vector<std::complex<double>>> waveletArray;

    float hannNorm;

    const int nfft;

    FFTWArray convInput;
    FFTWArray freqData;
    FFTWArray region2Data;
    FFTWArray convOutput;

    FFTWPlan fftPlan;
    FFTWPlan ifftPlan;

	//Array<AudioBuffer<double>> spectrumBuffer;

    // I'm feeling like it might make more sense to have just one of these for power of
    // all the channels of interest. Also, maybe allow selecting arbitrary pairs of channels
    // to calculate coherence b/w rather than dividing selected channels into group 1 and
    // group 2. Thoughts?
    double alpha;
    // # group 1 channels x # frequencies x # times
   // vector<vector<vector<RealWeightedAccum>>> pxxs;
    //vector<vector<vector<RealWeightedAccum>>> pyys;

    // # channel combinations x # frequencies x # times
    vector<vector<vector<ComplexWeightedAccum>>> pxys;


    // # channels x # frequencies x # times
    vector<vector<vector<RealWeightedAccum>>> powBuffer;

    // # frequencies x # times
    vector<vector<vector<std::complex<double>>>> pxy;
    vector<vector<vector<std::complex<double>>>> pxySum;
    vector<vector<vector<int>>> pxyCount;


    // statistics for all trials, combined over trial times
    // # channel combinations x # frequencies
    vector<vector<double>> meanCoherence;
    vector<vector<double>> stdCoherence;

    // update meanCoherence and stdCoherence from pxxs, pyys, and pxys
    void updateCoherenceStats();

    // calculate a single magnitude-squared coherence from cross spectrum and auto-power values
    static double singleCoherence(double pxx, double pyy, std::complex<double> pxy);

    int getChanGroupIndex(int chan);

    Array<std::pair<int, int>> combPairs;
    Array<int> combPairHandled;
    
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CumulativeTFR);
};

#endif // CUMULATIVE_TFR_H_INCLUDED