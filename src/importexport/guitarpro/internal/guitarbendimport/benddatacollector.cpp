/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-Studio-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2024 MuseScore Limited
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "benddatacollector.h"

#include <engraving/dom/chord.h>
#include <engraving/dom/note.h>
#include <engraving/dom/score.h>
#include <engraving/dom/tie.h>

using namespace mu::engraving;

namespace mu::iex::guitarpro {
constexpr int BEND_DIVISIONS = 60;

static void fillBendDataContextForNote(BendDataContext& bendDataCtx, const BendDataCollector::ImportedBendInfo& importedInfo);
static std::vector<BendDataCollector::BendSegment> bendSegmentsFromPitchValues(const PitchValues& pitchValues, bool noteTiedBack);
BendDataCollector::ImportedBendInfo fillBendInfo(const Note* note, const PitchValues& pitchValues);

void BendDataCollector::storeBendData(mu::engraving::Note* note, const mu::engraving::PitchValues& pitchValues)
{
    if (!pitchValues.empty()) {
        m_bendInfoForNote[note->track()][note->tick().ticks()] = fillBendInfo(note, pitchValues);
    }
}

BendDataContext BendDataCollector::collectBendDataContext()
{
    BendDataContext bendDataCtx;

    for (const auto& [track, trackInfo] : m_bendInfoForNote) {
        for (const auto& [tick, importedBendInfo] : trackInfo) {
            fillBendDataContextForNote(bendDataCtx, importedBendInfo);
        }
    }

    return bendDataCtx;
}

std::vector<BendDataCollector::BendSegment> bendSegmentsFromPitchValues(const PitchValues& pitchValues,
                                                                        bool noteTiedBack)
{
    enum PitchDiff {
        NONE,
        SAME,
        UP,
        DOWN
    };

    std::vector<BendDataCollector::BendSegment> bendSegments;
    PitchDiff currentPitchDiff;
    PitchDiff previousPitchDiff = PitchDiff::NONE;

    auto pitchDiff = [](int prevPitch, int currentPitch) {
        if (prevPitch == currentPitch) {
            return PitchDiff::SAME;
        }

        return (prevPitch < currentPitch) ? PitchDiff::UP : PitchDiff::DOWN;
    };

    if (pitchValues.front().pitch != 0 && !noteTiedBack) {
        BendDataCollector::BendSegment seg;
        seg.startTime = seg.endTime = 0;
        seg.pitchDiff = pitchValues.front().pitch;

        bendSegments.push_back(seg);
    }

    for (size_t i = 0; i < pitchValues.size() - 1; i++) {
        currentPitchDiff = pitchDiff(pitchValues[i].pitch, pitchValues[i + 1].pitch);
        if (currentPitchDiff == previousPitchDiff) {
            if (!bendSegments.empty()) {
                BendDataCollector::BendSegment& lastSeg = bendSegments.back();
                lastSeg.endTime = pitchValues[i + 1].time;
                lastSeg.pitchDiff = pitchValues[i + 1].pitch - pitchValues[i].pitch;
            }

            continue;
        }

        if (currentPitchDiff == PitchDiff::SAME) {
            BendDataCollector::BendSegment seg;
            seg.startTime = pitchValues[i].time;
            seg.endTime = pitchValues[i + 1].time;
            seg.pitchDiff = pitchValues[i + 1].pitch - pitchValues[i].pitch;
            bendSegments.push_back(seg);
        } else {
            if (previousPitchDiff != PitchDiff::SAME || bendSegments.empty()) {
                BendDataCollector::BendSegment seg;
                seg.startTime = pitchValues[i].time;
                seg.endTime = pitchValues[i + 1].time;
                bendSegments.push_back(seg);
            } else {
                BendDataCollector::BendSegment& lastSeg = bendSegments.back();
                lastSeg.middleTime = pitchValues[i].time;
                lastSeg.endTime = pitchValues[i + 1].time;
            }

            bendSegments.back().pitchDiff = pitchValues[i + 1].pitch - pitchValues[i].pitch;
        }

        previousPitchDiff = currentPitchDiff;
    }

    return bendSegments;
}

BendDataCollector::ImportedBendInfo fillBendInfo(const Note* note, const PitchValues& pitchValues)
{
    PitchValues adaptedPitchValues;
    adaptedPitchValues.reserve(pitchValues.size() + 2);

    if (pitchValues.front().time != 0) {
        PitchValue firstPv = pitchValues.front();
        firstPv.time = 0;
        adaptedPitchValues.push_back(firstPv);
    }

    for (const auto& pv : pitchValues) {
        adaptedPitchValues.push_back(pv);
    }

    if (pitchValues.back().time != BEND_DIVISIONS) {
        PitchValue lastPv = pitchValues.back();
        lastPv.time = BEND_DIVISIONS;
        adaptedPitchValues.push_back(lastPv);
    }

    BendDataCollector::ImportedBendInfo info;
    info.segments = bendSegmentsFromPitchValues(adaptedPitchValues, note->tieBack());
    info.note = note;

    for (const auto& bs : info.segments) {
        if (bs.pitchDiff != 0 && bs.startTime != bs.endTime) {
            info.pitchChangesAmount++;
        }
    }

    return info;
}

static bool isSlightBend(const BendDataCollector::ImportedBendInfo& importedInfo)
{
    if (importedInfo.pitchChangesAmount != 1 || importedInfo.note->tieFor() || importedInfo.segments.size() != 2) {
        return false;
    }

    for (const auto& seg : importedInfo.segments) {
        if (seg.pitchDiff == 25) {
            return true;
        }
    }

    return false;
}

void fillSlightBendDataContext(BendDataContext& bendDataCtx, const BendDataCollector::ImportedBendInfo& importedInfo)
{
    const Note* note = importedInfo.note;
    const Chord* chord = note->chord();
    Fraction tick = chord->tick();

    std::vector<Fraction> bendDurations;
    Fraction duration = chord->actualTicks();
    bendDurations.push_back(duration);

    bendDataCtx.bendChordDurations[note->track()][tick.ticks()] = std::move(bendDurations);

    BendDataContext::BendData slightBendData;
    slightBendData.quarterTones = 1;

    const auto& firstSeg = importedInfo.segments.front();
    if (firstSeg.middleTime != -1) {
        slightBendData.startFactor = (double)firstSeg.middleTime / BEND_DIVISIONS;
    }

    slightBendData.endFactor = (double)(firstSeg.endTime + 1) / BEND_DIVISIONS;
    slightBendData.type = GuitarBendType::SLIGHT_BEND;
    slightBendData.startTick = tick;
    bendDataCtx.bendDataByEndTick[note->track()][tick.ticks()] = std::move(slightBendData);
}

static bool isFirstPrebend(const BendDataCollector::ImportedBendInfo& importedInfo)
{
    const auto& firstSeg = importedInfo.segments.front();
    return firstSeg.startTime == firstSeg.endTime;
}

static void fillPrebendDataContext(BendDataContext& bendDataCtx, const BendDataCollector::ImportedBendInfo& importedInfo)
{
    const Note* note = importedInfo.note;
    BendDataContext::BendData prebendData;

    const auto& firstSeg = importedInfo.segments.front();
    prebendData.type = GuitarBendType::PRE_BEND;
    prebendData.startTick = note->tick();
    prebendData.quarterTones = firstSeg.pitchDiff / 25;

    bendDataCtx.bendDataByEndTick[note->track()][note->tick().ticks()] = std::move(prebendData);

    std::vector<Fraction> bendDurations;
    Fraction duration = note->chord()->actualTicks();
    bendDurations.push_back(duration);

    bendDataCtx.bendChordDurations[note->track()][note->tick().ticks()] = std::move(bendDurations);
}

static void fillNormalBendDataContext(BendDataContext& bendDataCtx, const BendDataCollector::ImportedBendInfo& importedInfo,
                                      size_t startIndex)
{
    // TODO: fill chords durations in proportion to bend diagram
    const Note* note = importedInfo.note;
    BendDataContext::BendData bendData;

    if (startIndex >= importedInfo.segments.size() - 1) {
        return;
    }

    const auto& firstSeg = importedInfo.segments.at(startIndex);
    bendData.type = GuitarBendType::BEND;
    bendData.startTick = note->tick();
    bendData.quarterTones = firstSeg.pitchDiff / 25;

    bendDataCtx.bendDataByEndTick[note->track()][note->chord()->actualTicks().ticks()] = std::move(bendData);

    std::vector<Fraction> bendDurations;
    Fraction duration = note->chord()->actualTicks();
    bendDurations.push_back(duration);

    // currently not adding new chords - so checking if END chord for new bend already exists
    // it's a test code
    {
        const Score* score = note->score();
        Fraction endChordTick = note->tick() + duration;
        const Measure* endChordMeasure = score->tick2measure(endChordTick);

        if (endChordMeasure) {
            const Chord* endChord = endChordMeasure->findChord(endChordTick, note->track());
            if (endChord) {
                bendDurations.push_back(endChord->actualTicks());
            }
        }
    }

    bendDataCtx.bendChordDurations[note->track()][note->tick().ticks()] = std::move(bendDurations);
}

static void fillBendDataContextForNote(BendDataContext& bendDataCtx, const BendDataCollector::ImportedBendInfo& importedInfo)
{
    const Note* note = importedInfo.note;
    IF_ASSERT_FAILED(note) {
        LOGE() << "couldn't fill bend data: note is NULL";
        return;
    }

    const auto& bendSegments = importedInfo.segments;
    IF_ASSERT_FAILED(!bendSegments.empty()) {
        LOGE() << "couldn't fill bend data: bend segments are empty";
        return;
    }

    std::vector<Fraction> bendDurations;
    if (isSlightBend(importedInfo)) {
        fillSlightBendDataContext(bendDataCtx, importedInfo);
        return;
    }

    size_t startIndex = 0;

    if (isFirstPrebend(importedInfo)) {
        fillPrebendDataContext(bendDataCtx, importedInfo);
        startIndex = 1;
    }

    fillNormalBendDataContext(bendDataCtx, importedInfo, startIndex);
}
} // namespace mu::iex::guitarpro
