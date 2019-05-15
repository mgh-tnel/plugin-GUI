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

#ifndef COHERENCE_VIS_H_INCLUDED
#define COHERENCE_VIS_H_INCLUDED

#include "AtomicSynchronizer.h"
#include "CoherenceNode.h"
#include <VisualizerWindowHeaders.h>
#include "../../Processors/Visualization/MatlabLikePlot.h"

class CoherenceVisualizer : public Visualizer
    , public ComboBox::Listener
{
public:
    CoherenceVisualizer(CoherenceNode* n);
    ~CoherenceVisualizer();

    void resized() override;

    void refreshState() override;
    void update() override;
    void refresh() override;
    void beginAnimation() override;
    void endAnimation() override;
    void setParameter(int, float) override;
    void setParameter(int, int, int, float) override;
    void comboBoxChanged(ComboBox* comboBoxThatHasChanged) override;

private:
    CoherenceNode* processor;

    ScopedPointer<Viewport>  viewport;
    ScopedPointer<Component> canvas;

    ScopedPointer<MatlabLikePlot> referencePlot;
    ScopedPointer<MatlabLikePlot> currentPlot;

    ScopedPointer<Label> optionsTitle;
    ScopedPointer<ComboBox> combinationBox;

    float freqStep;
    int nCombs;
    int curComb;

    MatlabLikePlot* cohPlot;
    std::vector<double> coherence;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CoherenceVisualizer);
};

#endif // COHERENCE_VIS_H_INCLUDED