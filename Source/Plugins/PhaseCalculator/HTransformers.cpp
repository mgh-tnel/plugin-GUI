/*
------------------------------------------------------------------

This file is part of a plugin for the Open Ephys GUI
Copyright (C) 2018 Translational NeuroEngineering Lab

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

#include "HTransformers.h"
#include <algorithm> // std::max_element

namespace Hilbert
{
    const Array<double> HIGH_GAM_VALID({ 60, 200 });
    const Array<double> HIGH_GAM_DEFAULT({ 70, 150 });
    const Array<double> HIGH_GAM_EXTREMA({ 81.6443, 123.1104, 169.3574 });
    const int HIGH_GAM_DELAY = 3;
    // from Matlab: firls(6, [60 200]/250, [1 1], 'hilbert')
    const double HIGH_GAM_TRANSFORMER[HIGH_GAM_DELAY] = {
        -0.10383410506573287,
        0.0040553935691102303,
        -0.59258484603659545
    };

    const Array<double> MID_GAM_VALID({ 40, 90 });
    const Array<double> MID_GAM_DEFAULT({ 40, 90 });
    const Array<double> MID_GAM_EXTREMA({ 64.4559 });
    const int MID_GAM_DELAY = 2;
    // from Matlab: firls(4, [35 90]/250, [1 1], 'hilbert')
    const double MID_GAM_TRANSFORMER[MID_GAM_DELAY] = {
        -0.487176162115735,
        -0.069437334858668653
    };

    const Array<double> LOW_GAM_VALID({ 30, 55 });
    const Array<double> LOW_GAM_DEFAULT({ 30, 55 });
    const Array<double> LOW_GAM_EXTREMA({ 43.3609 });
    const int LOW_GAM_DELAY = 2;
    // from Matlab: firls(4, [30 55]/250, [1 1], 'hilbert')
    const double LOW_GAM_TRANSFORMER[LOW_GAM_DELAY] = {
        -1.5933788446351915,
        1.7241339075391682
    };

    const Array<double> BETA_VALID({ 10, 40 });
    const Array<double> BETA_DEFAULT({ 12, 30 });
    const Array<double> BETA_EXTREMA({ 21.5848 });
    const int BETA_DELAY = 9;
    // from Matlab: firpm(18, [12 30 40 240]/250, [1 1 0.7 0.7], 'hilbert')
    const double BETA_TRANSFORMER[BETA_DELAY] = {
        -0.099949575596234311,
        -0.020761484963254036,
        -0.080803573080958854,
        -0.027365064225587619,
        -0.11114477443975329,
        -0.025834076852645271,
        -0.16664116044989324,
        -0.015661948619847599,
        -0.45268524264113719
    };

    const Array<double> ALPHA_THETA_VALID({ 4, 18 });
    const Array<double> ALPHA_THETA_DEFAULT({ 4, 8 });
    const Array<double> ALPHA_THETA_EXTREMA({ /* none */ });
    const int ALPHA_THETA_DELAY = 9;
    // from Matlab: firpm(18, [4 246]/250, [1 1], 'hilbert')
    const double ALPHA_THETA_TRANSFORMER[ALPHA_THETA_DELAY] = {
        -0.28757250783614413,
        0.000027647225074994485,
        -0.094611325643268351,
        -0.00025887439499763831,
        -0.129436276914844,
        -0.0001608427426424053,
        -0.21315096860055227,
        -0.00055322197399797961,
        -0.63685698210351149
    };

    const String C_GAMMA{ L"\u03b3" };
    const String C_BETA{ L"\u03b2" };
    const String C_ALPHA_THETA{ L"\u03b1/\u03b8" };

    // exported constants

    const String BAND_NAME[NUM_BANDS] = {
        "High " + C_GAMMA       + " (60-200 Hz)",
        "Mid "  + C_GAMMA       + " (40-90 Hz)",
        "Low "  + C_GAMMA       + " (30-55 Hz)",
                  C_BETA        + " (10-40 Hz)",
                  C_ALPHA_THETA + " (4-18 Hz)"
    };

    const Array<double> VALID_BAND[NUM_BANDS] = {
        HIGH_GAM_VALID,
        MID_GAM_VALID,
        LOW_GAM_VALID,
        BETA_VALID,
        ALPHA_THETA_VALID
    };

    const Array<double> DEFAULT_BAND[NUM_BANDS] = {
        HIGH_GAM_DEFAULT,
        MID_GAM_DEFAULT,
        LOW_GAM_DEFAULT,
        BETA_DEFAULT,
        ALPHA_THETA_DEFAULT
    };

    const Array<double> EXTREMA[NUM_BANDS] = {
        HIGH_GAM_EXTREMA,
        MID_GAM_EXTREMA,
        LOW_GAM_EXTREMA,
        BETA_EXTREMA,
        ALPHA_THETA_EXTREMA
    };

    const int DELAY[NUM_BANDS] = {
        HIGH_GAM_DELAY,
        MID_GAM_DELAY,
        LOW_GAM_DELAY,
        BETA_DELAY,
        ALPHA_THETA_DELAY
    };

    const double* const TRANSFORMER[NUM_BANDS] = {
        HIGH_GAM_TRANSFORMER,
        MID_GAM_TRANSFORMER,
        LOW_GAM_TRANSFORMER,
        BETA_TRANSFORMER,
        ALPHA_THETA_TRANSFORMER
    };
}