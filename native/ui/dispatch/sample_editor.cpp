// The sample editor: the selection, the slice markers, the ops, SAVE and CHOP.

#include "ui/dispatch/dispatch_common.h"

#include "ui/std_filesystem.h"

#include <algorithm>
#include <string>
#include <vector>

namespace pt::ui {

namespace {

/** The waveform panel is 620px wide, so it asks the engine for 620 (min, max) pairs. */
constexpr int WAVEFORM_BINS = SampleEditorModule::WAVEFORM_W;

/** SOURCE mode → the channel the waveform is drawn from: 0 = left, 1 = right, 2 = averaged. */
int waveform_channel(int source_mode) {
    return (source_mode == 0) ? 0 : (source_mode == 1) ? 1 : 2;
}

}  // namespace

// ─── Opening and closing ─────────────────────────────────────────────────────────────────────────

void InputDispatcher::open_sample_editor() {
    const Instrument& ins = s_.project->instruments[static_cast<size_t>(s_.currentInstrument)];
    if (ins.instrumentType == songcore::InstrumentType::SOUNDFONT) return;

    s_.sampleEditor              = SampleEditorState{};   // a fresh session, every time
    s_.sampleEditor.sampleId     = s_.currentInstrument;
    s_.sampleEditor.instrumentId = s_.currentInstrument;
    s_.sampleEditor.cursorRow    = 1;
    s_.sampleEditor.cursorCol    = 0;

    s_.previousScreen = s_.currentScreen;
    s_.currentScreen  = ScreenType::SAMPLE_EDITOR;
    init_sample_editor_state();
}

void InputDispatcher::init_sample_editor_state() {
    SampleEditorState& se  = s_.sampleEditor;
    const Instrument&  ins = s_.project->instruments[static_cast<size_t>(se.instrumentId)];

    // The NAME is the FILE's, not the instrument's — you are editing a sample, and its file is what you
    // will save it back over.
    se.sampleFilePath = ins.sampleFilePath.value_or("");
    se.sampleName     = se.sampleFilePath.empty() ? "" : path_stem(se.sampleFilePath);

    se.totalFrames   = host_.sample_length(se.instrumentId);
    se.sampleRate    = host_.sample_rate_of(se.instrumentId);
    se.hasStereoData = host_.has_stereo_data(se.instrumentId);
    // BIT opens at the sample's own depth, which is also the highest it can offer.
    se.sourceBitDepth = host_.sample_bit_depth(se.instrumentId);
    se.bitDepth       = se.sourceBitDepth;

    // ⚠️ SOURCE OPENS ON STEREO FOR A STEREO SAMPLE, and this is a SAVE decision rather than a display
    // one. The mode is what `resolve_save_channels` reads: every value but STEREO writes a ONE-CHANNEL
    // file, so a session that never visited the row would silently throw away the right channel of a
    // stereo sample the user only meant to trim. The mode a save cannot lose is the one to open on.
    // (Mono has nothing to choose — the row draws "MONO" and is read-only there whatever this says.)
    se.sourceMode = se.hasStereoData ? 2 /*STEREO*/ : 0;

    se.waveformData  = host_.sample_waveform(se.instrumentId, WAVEFORM_BINS, 0, 0,
                                             waveform_channel(se.sourceMode));

    // The selection opens on the instrument's OWN sample window. The instrument stores it as 0..255 (it
    // is a playback parameter, not a frame count) and the editor works in frames, so it is scaled in
    // here and scaled back out on the way to a preview.
    if (se.totalFrames > 0) {
        se.selectionStart = (static_cast<int64_t>(ins.sampleStart) * se.totalFrames) / 255;
        se.selectionEnd   = (static_cast<int64_t>(ins.sampleEnd) * se.totalFrames) / 255;
        // ⚠️ START and END are two independent free 0-255 cells, so an INVERTED window is typeable
        // and arrives here as `start > end`. It runs to the END OF THE SAMPLE, which is not a repair
        // chosen here — it is what `derive_sample_window` does with the same pair, so the editor
        // shows the region the engine plays. Drawn as-is it would show no selection at all (the
        // waveform lights `>= start && < end`), which reads as "nothing selected".
        if (se.selectionStart >= se.selectionEnd) {
            // A START of 0xFF scales to the very last frame, so the tail it opens on has to be at
            // least one frame wide or the repair draws as "nothing selected" all over again.
            se.selectionStart = std::min<int64_t>(se.selectionStart, se.totalFrames - 1);
            se.selectionEnd   = se.totalFrames;
        }
    } else {
        se.selectionStart = 0;
        se.selectionEnd   = 0;
    }

    // The FILE's markers come from the PROJECT, which got them from the file's `cue ` chunk when the
    // sample was loaded (engine_setup.h / wav_writer.h). No file I/O here, and no race: the editor and
    // the loader are looking at the same list. (`sliceMarkers` is int64 — Kotlin's `List<Long>` — and a
    // frame index is an int everywhere else, so the narrowing is said out loud rather than left to the
    // compiler.)
    //
    // ⚠️ **SORTED, UNIQUE and strictly INSIDE the sample**, because a `cue ` chunk is written by whatever
    // made the file and nothing upstream promises any of the three. Every marker reader rests on all
    // three — `slice_bounds` reads marker `k − 1` and marker `k` as one slice's two edges, so an
    // out-of-order pair is a negative-length slice and a duplicate a zero-length one, which is what CHOP
    // would hand to the file writer. MANUAL seeds itself from this list and `place_slice_marker` then
    // maintains the invariant, so this is the only door it can come in through.
    se.fileMarkers.clear();
    se.fileMarkers.reserve(ins.sliceMarkers.size());
    for (const int64_t m : ins.sliceMarkers)
        if (m >= 1 && m < static_cast<int64_t>(se.totalFrames)) se.fileMarkers.push_back(static_cast<int>(m));
    std::sort(se.fileMarkers.begin(), se.fileMarkers.end());
    se.fileMarkers.erase(std::unique(se.fileMarkers.begin(), se.fileMarkers.end()), se.fileMarkers.end());

    // ⚠️ The DETECTOR's list opens EMPTY, and that is behaviour rather than tidiness: `sliceMethod`
    // survives a re-entry, so an editor re-opened while TRANSIENT is still selected must re-detect on
    // the new audio. Empty is the only thing the feed reads as "detect".
    se.transientMarkers.clear();
    // ⚠️ And so do the HAND-PLACED ones. The METHOD survives a re-entry; the markers cannot — they are
    // frame indices into audio that has just been replaced, and a position that meant a drum hit in one
    // sample means the middle of a chord in the next.
    se.manualMarkers.clear();
    se.manualKeyMethod = -1;
    se.manualKeyParam  = -1;
    se.sliceIndex = 0;
    // ⚠️ sliceMethod is deliberately NOT reset — it opens at OFF on a fresh session (the struct's
    // default) and SURVIVES a re-entry, so loading a second sample to compare does not silently drop you
    // back out of TRANSIENT mode.

    se.isModified       = false;
    se.showConfirmClose = false;
    se.playbackPosition = -1.0f;
}

void InputDispatcher::close_sample_editor() {
    // Anything the audition still owes the instrument, it pays now — leaving with a restore pending would
    // put the preview's sample window back onto a slot that is no longer on screen.
    run_due_sample_preview_restore(/*force=*/true);

    host_.restore_fx_preview_backup();          // drop an un-applied FX preview
    host_.free_sample_undo(s_.sampleEditor.instrumentId);   // unreachable once the editor is gone
    host_.clear_previews();                     // the 254/255 scratch slots

    s_.currentScreen = s_.previousScreen;
}

// ─── The selection: A+DPAD on rows 3..8 ──────────────────────────────────────────────────────────

void InputDispatcher::nudge_selection_edge(int64_t delta) {
    SampleEditorState& se = s_.sampleEditor;

    // ⚠️ **AN ANDROID CRASH, FOUND BY PORTING.** With NO sample loaded, `totalFrames` and `selectionEnd`
    // are both 0 — and Kotlin's arms are `coerceIn(0, selectionEnd - 1)` and `coerceIn(selectionStart + 1,
    // maxFrame)`, i.e. `coerceIn(0, -1)` and `coerceIn(1, 0)`. Both have min > max, and `coerceIn` REQUIRES
    // min <= max: it throws IllegalArgumentException, and the app dies. It is reachable in four presses —
    // EDIT on a fresh sampler slot, DOWN, DOWN, A+RIGHT — because nothing on the way in checks that the
    // slot has any audio in it. (In C++ it would be worse than a crash: `std::clamp` with lo > hi is UB.)
    //
    // A selection inside a sample with no frames is meaningless, so there is nothing to nudge. Zone B, so
    // it is fixed on Android too (AppInputDispatcher.nudgeSelectionEdge), per §4's rule.
    if (se.totalFrames <= 0) return;

    const int64_t maxFrame = se.totalFrames;
    const int     dir      = (delta >= 0) ? 1 : -1;

    // SNAP moves the edge on to the nearest zero crossing IN THE DIRECTION OF TRAVEL, which is what keeps
    // a trimmed sample from clicking at its own boundary. Searching in the direction of the nudge (rather
    // than the nearest in either) is what stops the edge sticking: a crossing you have just left is
    // always the nearest one.
    // ⚠️ THROUGH THE SOURCE MODE, so the crossing is looked for in the signal the SAVE will write. On a
    // stereo sample the left channel alone is one of two: an edge that sits exactly on a left crossing
    // leaves the right one wherever it happened to be, and the seam clicks in one ear.
    auto snap = [&](int64_t f) -> int64_t {
        if (!se.snapEnabled) return f;
        return host_.find_zero_crossing(se.instrumentId, static_cast<int>(f), dir, se.sourceMode);
    };

    if (se.cursorCol == 0) {
        // START can never reach END — an inverted selection is not a selection.
        const int64_t raw = std::clamp<int64_t>(se.selectionStart + delta, 0, se.selectionEnd - 1);
        se.selectionStart = std::min<int64_t>(snap(raw), se.selectionEnd - 1);
    } else {
        const int64_t raw = std::clamp<int64_t>(se.selectionEnd + delta, se.selectionStart + 1, maxFrame);
        se.selectionEnd   = std::clamp<int64_t>(snap(raw), se.selectionStart + 1, maxFrame);
    }
}

// ─── The slice markers: A+DPAD and A+B on row 11 ─────────────────────────────────────────────────

namespace {

/**
 * Put boundary `k` at `want` — or MAKE one there when `k` is not in the list — and answer with the
 * index it ended up at, which is not necessarily the one it started from. −1 when there is nowhere for
 * it to go.
 *
 * ⭐ **A boundary MAY be dragged past its neighbours; its NUMBER follows its POSITION.** The list stays
 * sorted because it is re-sorted here, in the one place a boundary can move, rather than because a
 * clamp forbids the crossing — and that is what lets a boundary made at the end be walked back to
 * anywhere in the sample instead of having to be deleted and remade.
 *
 * ⚠️ **Two boundaries may never share a frame.** `slice_bounds` reads marker `k − 1` as the left edge
 * of slice `k` and marker `k` as its right, so a duplicate is a zero-length slice — one CHOP would hand
 * to the file writer. A landing that is taken is stepped PAST in the direction of travel rather than
 * refused, so a drag through a crowd keeps moving.
 *
 * ⚠️ Frame 0 and the last frame are the sample's own bounds and not boundaries within it — the same
 * rule `compute_slice_cue_points` applies when it writes the `cue ` chunk.
 */
int place_slice_marker(std::vector<SliceMarker>& m, int k, int64_t want, int dir, int totalFrames) {
    const int64_t last = static_cast<int64_t>(totalFrames) - 1;
    if (last < 1) return -1;   // no room for an interior boundary at all

    // ⚠️ `i != k` and not a value compare: the boundary being dragged must not collide with itself.
    const auto taken = [&](int64_t f) {
        for (int i = 0; i < static_cast<int>(m.size()); ++i)
            if (i != k && static_cast<int64_t>(m[static_cast<size_t>(i)].frame) == f) return true;
        return false;
    };

    int64_t at = std::clamp<int64_t>(want, 1, last);
    while (at >= 1 && at <= last && taken(at)) at += dir;
    if (at < 1 || at > last) return -1;   // walked off the end through a wall of boundaries

    // ⚠️ MOVE the boundary, never rebuild it: `originFrame` is its identity and has to travel with it
    // through every crossing, or A+B loses the position it is meant to put the boundary back on. A
    // boundary that is MADE here rather than moved has no computed home, which is what −1 says.
    if (k >= 0 && k < static_cast<int>(m.size())) m[static_cast<size_t>(k)].frame = static_cast<int>(at);
    else                                          m.push_back(SliceMarker{static_cast<int>(at), -1});
    std::sort(m.begin(), m.end(),
              [](const SliceMarker& a, const SliceMarker& b) { return a.frame < b.frame; });

    for (int i = 0; i < static_cast<int>(m.size()); ++i)
        if (static_cast<int64_t>(m[static_cast<size_t>(i)].frame) == at) return i;
    return -1;   // unreachable: the frame was just written and the frames are unique
}

}  // namespace

/**
 * Take a copy of whatever the method currently answers with, so the user's nudges have something to
 * write into, and stamp it with the (method, parameter) it describes.
 *
 * Every method starts from the set it already shows, so a drag adjusts what is on screen rather than
 * throwing it away: the detected cuts under TRANSIENT, the arithmetic ones under DIVIDE, and under
 * MANUAL the boundaries the SAMPLE ITSELF came with — its `cue ` chunk, as the project loaded it. A
 * sample carrying no cue points starts MANUAL empty, and its first boundary is born on frame 0, which
 * is the sample's own start and not a boundary until something is dragged off it.
 */
void InputDispatcher::materialise_manual_markers() {
    SampleEditorState& se = s_.sampleEditor;
    if (se.manual_markers_live()) return;

    // ⚠️ Each copy remembers the frame it was made at, and that is the whole of "put slice 01 back where
    // DIVIDE had it" — the boundary can be dragged past its neighbours afterwards, so by the time A+B
    // arrives its index says nothing about which cut it is.
    se.manualMarkers.clear();
    if (se.sliceMethod == SampleEditorModule::SLICE_TRANSIENT) {
        for (const int f : se.transientMarkers) se.manualMarkers.push_back(SliceMarker{f, f});
    } else if (se.sliceMethod == SampleEditorModule::SLICE_DIVIDE) {
        const int div = std::max(se.sliceDivisions, 1);
        for (int i = 1; i < div; ++i) {
            const int f = static_cast<int>((static_cast<int64_t>(i) * se.totalFrames) / div);
            se.manualMarkers.push_back(SliceMarker{f, f});
        }
    } else if (se.sliceMethod == SampleEditorModule::SLICE_MANUAL) {
        // ⚠️ ORIGIN −1, unlike the two above: the file's cue points are not a position any method can
        // RECOMPUTE, so A+B on one REMOVES it. That is deliberate and it is the point of the seed — the
        // gesture that deletes a boundary the sample came with is the same A+B that deletes one placed by
        // hand, and there is no second meaning of A+B on this row to learn.
        for (const int f : se.fileMarkers) se.manualMarkers.push_back(SliceMarker{f, -1});
    }
    se.manualKeyMethod = se.sliceMethod;
    se.manualKeyParam  = se.manual_key_param();
}

/**
 * The selection follows the slice the row-11 cursor is on. ⚠️ Every gesture that moves a boundary or
 * changes which one is under the cursor ends here — a slice whose edge has just moved is one START must
 * play the new shape of, and the reset leaving the old shape behind is the defect this closes.
 */
void InputDispatcher::select_current_slice() {
    SampleEditorState& se = s_.sampleEditor;
    int64_t start = 0, end = 0;
    se.slice_bounds(se.sliceIndex, start, end);
    se.selectionStart = start;
    se.selectionEnd   = end;
}

void InputDispatcher::nudge_slice_marker(int64_t delta) {
    SampleEditorState& se = s_.sampleEditor;

    // The same guard `nudge_selection_edge` carries, for the same reason: this screen is reachable on an
    // empty slot in four presses, and `std::clamp` with lo > hi is UB rather than a wrong answer.
    if (se.totalFrames <= 0 || delta == 0) return;

    const bool manual = se.sliceMethod == SampleEditorModule::SLICE_MANUAL;
    const int  k      = se.slice_marker_index();

    // Slice 00's left edge is the sample's own start: under TRANSIENT and DIVIDE there is no boundary
    // there and nothing to drag. ⭐ Under MANUAL a rightward drag MAKES one instead of moving that edge
    // — the sample does not start later, it gains a cut — and the cursor follows the new boundary onto
    // whichever slice it opens. Leftward there is nowhere to go: frame 0 is the sample's own start.
    if (k < 0 && (!manual || delta < 0)) return;

    materialise_manual_markers();
    std::vector<SliceMarker>& m = se.manualMarkers;

    // A boundary past the end of the list is MANUAL's free slot — the next one, not yet made. It is
    // born on the boundary to its left, which is where the cell already reads, and this drag is what
    // carries it off there.
    if (k >= static_cast<int>(m.size()) && !manual) return;
    const int64_t from = (k >= 0 && k < static_cast<int>(m.size()))
                             ? static_cast<int64_t>(m[static_cast<size_t>(k)].frame)
                             : (k < 0 || m.empty() ? 0 : static_cast<int64_t>(m.back().frame));

    const int dir  = (delta > 0) ? 1 : -1;
    int64_t   want = from + delta;
    // SNAP is row 2's toggle and it already works on the selection edges; a boundary is the same kind of
    // cut through the same audio, so it reads the same switch and searches in the direction of travel.
    if (se.snapEnabled)
        want = host_.find_zero_crossing(
            se.instrumentId,
            static_cast<int>(std::clamp<int64_t>(want, 0, static_cast<int64_t>(se.totalFrames) - 1)),
            dir, se.sourceMode);

    const int landed = place_slice_marker(m, k, want, dir, se.totalFrames);
    if (landed < 0) return;

    se.sliceIndex = landed + 1;   // the boundary's own number — marker `j` is the left edge of slice j+1
    select_current_slice();
}

void InputDispatcher::reset_slice_marker() {
    SampleEditorState& se = s_.sampleEditor;

    const int k = se.slice_marker_index();
    if (k < 0) return;   // slice 00's left edge is the sample's own start: no boundary, nothing to undo

    // ⚠️ **MATERIALISE FIRST, like the drag and the tap do** — all three gestures on this row go through
    // the same door. The boundary under the cursor may be one the METHOD is still answering with and
    // nothing has copied yet, and under MANUAL that is a cue point the sample arrived with: A+B is how it
    // is deleted, so a "nothing has been placed yet" bail made the FIRST A+B of a session do nothing.
    materialise_manual_markers();
    std::vector<SliceMarker>& m = se.manualMarkers;
    if (k >= static_cast<int>(m.size())) return;      // MANUAL's free slot holds no boundary yet

    // ⭐ THE BOUNDARY ITSELF SAYS WHERE IT GOES, so there is no arm per method here and — more to the
    // point — nothing reads the boundary's INDEX to decide. The index is where it currently sits on
    // screen, which after a crossing is somebody else's cut.
    const int origin = m[static_cast<size_t>(k)].originFrame;

    if (origin < 0) {
        // Placed by hand, or seeded from the file's own cue points: there is no computed position to go
        // back to, so A+B REMOVES it. The boundaries after it renumber, and the cursor keeps its number
        // and therefore names the slice that has just grown into the gap.
        //
        // ⚠️ Emptying the list does NOT retire the stamp. An empty live list is "this sample has no
        // boundaries", and it is the answer every reader must get; drop the stamp and the read falls back
        // to `method_markers()`, which under MANUAL is the file's own cue points — every delete undone at
        // once by the one that finished the job.
        m.erase(m.begin() + k);
    } else {
        // ⚠️ Through `place_slice_marker` like every other move, because a NEIGHBOUR may have been
        // dragged across that position since — the boundary then takes the number its own place gives
        // it, rather than breaking the sort order.
        const int landed = place_slice_marker(m, k, origin, +1, se.totalFrames);
        if (landed < 0) return;
        se.sliceIndex = landed + 1;
    }

    select_current_slice();
}

/**
 * Cut a boundary at the playhead — the "slice it by ear" gesture. MANUAL only, and only while the
 * sample is actually sounding: with nothing playing there is no playhead to cut at.
 *
 * ⭐ **The frame comes from `playbackPosition`, the same field the waveform draws its playhead line
 * from, and that is the point rather than a shortcut.** A fresher number straight off the voice would
 * land the boundary somewhere the user never saw — they are tapping to a line on the screen and to
 * audio that left the device a buffer ago, so the line they were looking at IS the frame they meant.
 *
 * ⚠️ **And it is the line as it stood when A went DOWN** (`on_a_deferred`), not when it came up. The
 * press is the tap; the release is only where the mapper can tell a tap from an A+DPAD, and by then the
 * playhead has run on for as long as the user held the button.
 *
 * SNAP searches BACKWARD, unlike a drag's "in the direction of travel". A tap has no direction, and it
 * is always LATE — reaction time plus the audio buffer — so the zero crossing that matters is the one
 * before the hit rather than the one inside it.
 */
void InputDispatcher::tap_slice_marker() {
    SampleEditorState& se = s_.sampleEditor;

    // Consumed, not merely read: a press that ended in an A+DPAD leaves its snapshot behind, and the
    // next tap must not be able to cut at a playhead position from a gesture that was not a tap.
    const float at    = sliceTapPlayhead_;
    sliceTapPlayhead_ = -1.0f;

    if (se.sliceMethod != SampleEditorModule::SLICE_MANUAL) return;
    if (se.totalFrames <= 0 || at < 0.0f) return;

    const int64_t last = static_cast<int64_t>(se.totalFrames) - 1;
    int64_t       want = std::clamp<int64_t>(
        static_cast<int64_t>(at * static_cast<float>(se.totalFrames)), 0, last);
    if (se.snapEnabled)
        want = host_.find_zero_crossing(se.instrumentId, static_cast<int>(want), -1, se.sourceMode);

    materialise_manual_markers();
    std::vector<SliceMarker>& m = se.manualMarkers;

    // ⚠️ Past the end of the list on purpose: a tap always MAKES a boundary, wherever the cursor
    // happens to be sitting. `place_slice_marker` stamps a made one with origin −1 — "by hand, with
    // nowhere to go back to" — which is what makes A+B remove it rather than move it.
    const int landed = place_slice_marker(m, static_cast<int>(m.size()), want, +1, se.totalFrames);
    if (landed < 0) return;

    se.sliceIndex = landed + 1;   // the cursor follows the cut onto the slice it opens
    select_current_slice();
}

// ─── RATE and BIT: the two cells that rebuild the audio ──────────────────────────────────────────

void InputDispatcher::apply_sample_rate_and_bits() {
    SampleEditorState& se     = s_.sampleEditor;
    const int          factor = (se.rateMode == 1) ? 2 : (se.rateMode == 2) ? 4 : 1;
    const int          oldLen = se.totalFrames;

    host_.apply_rate_and_bits(se.instrumentId, factor, se.bitDepth);

    // ⚠️ The 2-phrase lookahead has ALREADY scheduled notes against the OLD base frequency — they would
    // play the re-decimated buffer at double or half pitch. Rolling the schedule back is what makes the
    // upcoming phrases re-derive it. (A no-op when stopped.)
    if (host_.is_playing()) host_.notify_data_changed();

    const int newLen = host_.sample_length(se.instrumentId);
    auto scale = [&](int64_t f) -> int64_t {
        if (oldLen <= 0) return 0;
        return std::clamp<int64_t>((f * newLen) / oldLen, 0, newLen);
    };
    se.selectionStart = scale(se.selectionStart);
    se.selectionEnd   = scale(se.selectionEnd);
    se.slicePosition  = scale(se.slicePosition);

    se.totalFrames = newLen;
    se.sampleRate  = host_.sample_rate_of(se.instrumentId);
    se.waveformData = host_.sample_waveform(se.instrumentId, WAVEFORM_BINS, 0, 0,
                                            waveform_channel(se.sourceMode));
    se.isModified = true;
}

// ─── The view, after an op ───────────────────────────────────────────────────────────────────────

void InputDispatcher::refresh_sample_view(bool reset_selection) {
    SampleEditorState& se     = s_.sampleEditor;
    const int          newLen = host_.sample_length(se.instrumentId);
    se.totalFrames = newLen;

    // ⚠️ RESET, not clamp. An op that SHORTENS the sample (crop, cut, a SYNC that compressed it) leaves a
    // selection that describes frames which no longer exist; clamping it would leave you with a partial
    // selection of the new audio that you never made. Selecting the whole result is the one answer that
    // is always true. Kotlin's `afterResize()` does the same.
    if (reset_selection) {
        se.selectionStart = 0;
        se.selectionEnd   = newLen;

        // ⚠️ AND SO DOES THE INSTRUMENT'S OWN WINDOW, for the same reason and a sharper one: those two
        // 0-255 cells are a FRACTION of the buffer, so a buffer that changed length silently re-aims
        // them at audio the user never pointed at. CROP to a loop with START/END at 40/C0 and the note
        // plays the middle 50 % of the crop — the file on disk holds the whole thing, so it sounds
        // wrong in the tracker and right everywhere else.
        //
        // `cropSample`, `deleteSampleRegion` and `pasteRegion` already reset the ENGINE's copy of the
        // pair; this is the PROJECT's, and without it the next ordinary push — the audition's own
        // restore, 100 ms later — puts the stale fraction straight back. The LOOP pair is the same two
        // cells one row down and gets the same treatment: after a resize it points into audio that is
        // not there any more, and a loop is the thing this editor is most used to cut.
        Instrument& ins = host_.edit_project().instruments[static_cast<size_t>(se.instrumentId)];
        if (ins.sampleStart != 0x00 || ins.sampleEnd != 0xFF ||
            ins.loopStart   != 0x00 || ins.loopEnd   != 0xFF) {
            ins.sampleStart = 0x00;
            ins.sampleEnd   = 0xFF;
            ins.loopStart   = 0x00;
            ins.loopEnd     = 0xFF;
            host_.push_instrument(se.instrumentId);
            mark_modified();
        }

        // ⚠️⚠️ **AND EVERY MARKER LIST, for the third time the same reason: a boundary is a FRAME INDEX,
        // and the frames have just been replaced.** A cut that meant a drum hit before a CROP means the
        // middle of the next hit after it, and nothing on screen says the number is stale — it is a line
        // over a waveform, and a wrong line looks exactly like a right one.
        //
        // ⭐ ALL THREE, not the file's alone. `manualMarkers` OVERRIDES the other two while its stamp is
        // live, so clearing only the source underneath a live override changes nothing the user can see;
        // and `transientMarkers` empty is what makes the feed re-detect, so the detector comes back with
        // the cuts in the NEW audio instead of holding the old ones for good.
        //
        // ⚠️ This is what makes a save after a resize honest. `compute_slice_cue_points` drops a marker
        // past the end, so a cropped sample used to write back the SURVIVING half of its old cue points —
        // fewer slices than before, in the wrong places, with no gesture that said so.
        se.fileMarkers.clear();
        se.transientMarkers.clear();
        se.manualMarkers.clear();
        se.manualKeyMethod = -1;
        se.manualKeyParam  = -1;
        se.sliceIndex      = 0;   // the ceiling has just collapsed to a single slice, the whole sample
    }

    se.waveformData = host_.sample_waveform(se.instrumentId, WAVEFORM_BINS,
                                            static_cast<int>(se.view_start()),
                                            static_cast<int>(se.view_end()),
                                            waveform_channel(se.sourceMode));
}

// ─── A on the op rows, the FX row, the name and the save buttons ─────────────────────────────────

void InputDispatcher::sample_editor_confirm() {
    SampleEditorState& se     = s_.sampleEditor;
    const int          instId = se.instrumentId;
    const int          startF = static_cast<int>(se.selectionStart);
    const int          endF   = static_cast<int>(se.selectionEnd);

    // Every destructive op opens the same way: drop any un-applied FX preview (so the op acts on the
    // CLEAN audio, not on the effect you were auditioning) and take an undo backup.
    auto begin_destructive = [&] {
        host_.restore_fx_preview_backup();
        host_.backup_sample(instId);
    };
    auto in_place = [&](auto&& op) {   // an op that does NOT change the length
        begin_destructive();
        op();
        refresh_sample_view(/*reset_selection=*/false);
        se.isModified = true;
    };
    auto resizing = [&](auto&& op) {   // an op that DOES
        begin_destructive();
        op();
        refresh_sample_view(/*reset_selection=*/true);
        se.isModified = true;
    };

    switch (se.cursorRow) {
        // ── Row 11: cut a boundary at the playhead, mid-audition ─────────────────────────────────
        //
        // The whole row, both columns, exactly as A+B on it is: which column the cursor sits in says
        // which NUMBER you are reading, and neither of them is what a tap is aimed at.
        //
        // ⚠️ It arrives on A's RELEASE, not its press (`defer_a_to_release`), and cuts at the playhead
        // as it stood on the PRESS (`on_a_deferred`). The row's other gestures all start with the same
        // A held down, so a tap that fired immediately would precede every one of them.
        //
        // ⚠️ This is the one A on this screen that does nothing destructive and takes no undo backup —
        // a boundary is editor state, not audio. It must sit ABOVE the `begin_destructive` rows for
        // that to stay obvious, not because the order matters to the switch.
        case 11:
            tap_slice_marker();
            break;

        // ── Row 13: CROP  COPY  CUT  DUPL  PASTE  DEL ────────────────────────────────────────────
        case 13:
            switch (se.cursorCol) {
                case 0:   // CROP — keep the selection, discard the rest
                    if (startF < endF) resizing([&] { host_.crop_sample(instId, startF, endF); });
                    break;
                case 1:   // COPY — the ONE op on this row that changes nothing, so it takes no backup
                    host_.copy_region(instId, startF, endF);
                    break;
                case 2:   // CUT = copy, then delete
                    if (startF < endF) resizing([&] {
                        host_.copy_region(instId, startF, endF);
                        host_.delete_sample_region(instId, startF, endF);
                    });
                    break;
                case 3:   // DUPL = copy the selection and paste it at the END
                    if (startF < endF) resizing([&] {
                        host_.copy_region(instId, startF, endF);
                        host_.paste_region(instId, se.totalFrames);
                    });
                    break;
                case 4:   // PASTE — inserts at the selection's START
                    if (host_.clipboard_length() > 0)
                        resizing([&] { host_.paste_region(instId, startF); });
                    break;
                case 5:   // DEL
                    if (startF < endF)
                        resizing([&] { host_.delete_sample_region(instId, startF, endF); });
                    break;
                default: break;
            }
            break;

        // ── Row 14: NORM  FADE+  FADE-  SLNC  REV  UNDO ──────────────────────────────────────────
        case 14:
            switch (se.cursorCol) {
                case 0: in_place([&] { host_.normalize_sample(instId, startF, endF); }); break;
                case 1: in_place([&] { host_.fade_in_sample(instId, startF, endF); }); break;
                case 2: in_place([&] { host_.fade_out_sample(instId, startF, endF); }); break;
                case 3: in_place([&] { host_.silence_region(instId, startF, endF); }); break;
                case 4: in_place([&] { host_.reverse_sample(instId, startF, endF); }); break;

                case 5: {   // UNDO — one level, and it may restore a DIFFERENT length
                    host_.restore_fx_preview_backup();
                    host_.undo_sample(instId);
                    refresh_sample_view(/*reset_selection=*/true);
                    // ⚠️ `isModified` is deliberately NOT cleared: one undo does not mean the sample is
                    // back to what the FILE holds — it means it is back one step. Kotlin leaves it too, and
                    // the flag's only job is to put the "ARE YOU SURE?" in front of an unsaved exit.
                    se.slicePosition = std::clamp<int64_t>(se.slicePosition, 0, se.totalFrames);
                    break;
                }
                default: break;
            }
            break;

        // ── Row 16: the FX row. Col 2 is APPLY; the other two are dialled with A+DPAD. ───────────
        case 16: {
            if (se.cursorCol != 2) break;

            if (se.fxType <= SampleEditorModule::FX_EQ) {
                // OTT / DUST / DRIVE need an AMOUNT to do anything; EQ has none (its value is a slot),
                // so it always applies.
                const bool worth_doing =
                    (se.fxValue > 0) || (se.fxType == SampleEditorModule::FX_EQ);
                if (!worth_doing) break;

                begin_destructive();
                host_.apply_sample_fx(instId, se.fxType, se.fxValue);
                refresh_sample_view(/*reset_selection=*/false);
                se.isModified = true;
                break;
            }

            // ── SYNC: fit the sample to the project's grid ───────────────────────────────────────
            //
            // Two ways to make a sample last a bar, and they are not the same tool. RPITCH RESAMPLES it —
            // faster is higher, which is what you want for a breakbeat. TSTRETCH holds the pitch and moves
            // the time (SOLA), which is what you want for anything with a tune in it.
            const int    bpm     = s_.project->tempo;
            const double rawSecs = (se.sampleRate > 0)
                                       ? static_cast<double>(se.totalFrames) / se.sampleRate
                                       : 0.0;
            if (rawSecs <= 0.0 || bpm <= 0) break;

            const double targetSecs = SampleEditorModule::duration_beats(se.durationIndex) * 60.0 / bpm;
            const int    oldLen     = se.totalFrames;

            auto rescale_after = [&](bool clear_pitch) {
                const int newLen = host_.sample_length(instId);
                auto scale = [&](int64_t f) -> int64_t {
                    if (oldLen <= 0) return 0;
                    return std::clamp<int64_t>((f * newLen) / oldLen, 0, newLen);
                };
                se.slicePosition = scale(se.slicePosition);
                if (clear_pitch) se.pitchSemitones = 0;
                // ⚠️ Both resamplers drop the engine's RATE/BIT original — the result IS the new
                // original. Left at NORM or LOFI, RATE would describe a cache that no longer exists,
                // and its next touch would decimate the audio a second time. BIT is KEPT: it is also
                // the depth SAVE writes, and rounding a second time to the same grid changes nothing.
                se.rateMode     = 0;
                refresh_sample_view(/*reset_selection=*/true);
                se.isModified = true;
            };

            // ⚠️ "Already on the grid" is a question about FRAMES, and both branches must ask it the
            // same way. A ratio window instead — a thousandth either side of 1.0 — is 8 ms on a 4-bar
            // loop at 120 BPM, which is most of a tick, so a loop inside the window was declared
            // finished while still audibly off the beat.
            const double  ratio      = targetSecs / rawSecs;
            const int64_t wantFrames = std::llround(static_cast<double>(se.totalFrames) * ratio);
            if (ratio <= 0.001 || wantFrames == se.totalFrames) break;

            if (se.syncType == 0) {   // RPITCH
                // ⚠️ FRACTIONAL, and that is the point. This is a fit-to-grid, not the musical
                // transpose row 2 dials in: one semitone is a 5.9 % step in length, so rounding to the
                // nearest one misses the target by up to half of that — 230 ms on a 4-bar loop, twenty
                // ticks. Nothing below here is integer: `pitch_shift_sample` takes a float and the
                // engine resamples by pow(2, semitones/12).
                const double exact     = 12.0 * std::log(rawSecs / targetSecs) / std::log(2.0);
                const float  semitones = static_cast<float>(std::clamp(exact, -24.0, 24.0));
                begin_destructive();
                host_.pitch_shift_sample(instId, semitones);
                // The shift is BAKED, so the pending one on row 2 is spent — leaving it would apply it
                // twice at the next save.
                rescale_after(/*clear_pitch=*/true);
            } else {                  // TSTRETCH
                begin_destructive();
                host_.time_stretch_sample(instId, static_cast<float>(ratio));
                rescale_after(/*clear_pitch=*/false);   // a stretch does not change the pitch
            }
            break;
        }

        // ── Row 18: NAME ────────────────────────────────────────────────────────────────────────
        case 18:
            open_qwerty(QwertyContext::SAMPLE_NAME, se.sampleName, "SAMPLE NAME:",
                        fs_.samples_directory());
            break;

        // ── Row 19: LOAD  SAVE  OVERWRITE  CHOP ─────────────────────────────────────────────────
        case 19:
            switch (se.cursorCol) {
                case 0: {   // LOAD — a different sample, into the slot the editor is already open on
                    // ⚠️ `previousScreen` is the EDITOR's return target (INSTRUMENT), and the browser must
                    // not take it: it would leave B on the editor going back to the browser. Kotlin never
                    // assigns it here for the same reason.
                    const ScreenType keep = s_.previousScreen;
                    open_file_browser(AppState::BrowserPurpose::LOAD_SAMPLE_EDITOR,
                                      browser_dir(BrowserDir::SAMPLES), {"wav"});
                    s_.previousScreen = keep;
                    break;
                }

                case 1: {   // SAVE — to <name>.wav, or ask for a name if that one is taken
                    const std::string base = se.sampleName.empty() ? "SAMPLE" : se.sampleName;
                    const std::string dir  = fs_.samples_directory();
                    bake_pending_pitch();

                    const std::string target = dir + "/" + base + ".wav";
                    if (!fs_.file_exists(target)) {
                        save_sample_to(target, /*adopt_name=*/true);
                        break;
                    }
                    // Taken. Suggest the next free `<base>_0001` and let the user confirm or change it —
                    // SAVE is not OVERWRITE, and the button next to it is.
                    std::string suggested = base;
                    for (int n = 1; fs_.file_exists(dir + "/" + suggested + ".wav"); ++n) {
                        char suffix[16];   // 16, not 8: "_%04d" of an unbounded int is up to 12 bytes, and gcc says so (-Wformat-truncation). The counter never gets near it; the buffer now cannot be the reason.
                        std::snprintf(suffix, sizeof(suffix), "_%04d", n);
                        suggested = base + suffix;
                    }
                    open_qwerty(QwertyContext::SAMPLE_SAVE, suggested, "SAVE AS:", dir,
                                /*max_length=*/24, /*clear_on_first_b=*/true);
                    break;
                }

                case 2:   // OVERWRITE — back over the file it came from. Nothing to do if it has none.
                    if (!se.sampleFilePath.empty()) {
                        bake_pending_pitch();
                        save_sample_to(se.sampleFilePath, /*adopt_name=*/false);
                    }
                    break;

                case 3:   // CHOP
                    sample_editor_chop();
                    break;

                default: break;
            }
            break;

        default:
            break;
    }
}

// ─── The pending pitch shift ─────────────────────────────────────────────────────────────────────

void InputDispatcher::bake_pending_pitch() {
    SampleEditorState& se = s_.sampleEditor;
    if (se.pitchSemitones == 0) return;

    const int oldLen = se.totalFrames;
    host_.pitch_shift_sample(se.instrumentId, static_cast<float>(se.pitchSemitones));
    const int newLen = host_.sample_length(se.instrumentId);

    auto scale = [&](int64_t f) -> int64_t {
        if (oldLen <= 0) return 0;
        return std::clamp<int64_t>((f * newLen) / oldLen, 0, newLen);
    };

    // Every frame-measured thing moves with the audio. The SELECTION is scaled rather than reset here
    // (unlike an op) because the user has not asked for anything to change — they asked to SAVE, and the
    // shift is a thing they dialled in earlier that is only now being made real.
    se.selectionStart = scale(se.selectionStart);
    se.selectionEnd   = scale(se.selectionEnd);
    se.slicePosition  = scale(se.slicePosition);

    se.totalFrames    = newLen;
    se.pitchSemitones = 0;   // spent
    se.rateMode       = 0;   // the shifted buffer IS the new original — see sample_edit.h
    // ⚠️ BIT is NOT reset. This runs on the way INTO a save, and BIT is the depth that save writes.
    se.waveformData   = host_.sample_waveform(se.instrumentId, WAVEFORM_BINS, 0, 0,
                                              waveform_channel(se.sourceMode));

    // The instrument's playback params were derived from the old buffer's length.
    host_.push_instrument(se.instrumentId);
    mark_modified();
}

// ─── The slices ──────────────────────────────────────────────────────────────────────────────────

std::vector<int> InputDispatcher::compute_slice_cue_points() const {
    const SampleEditorState& se = s_.sampleEditor;

    // DIVIDE only while it is still arithmetic — a boundary the user has dragged makes it a list like
    // any other, and the cue points must be the ones on the screen.
    if (se.sliceMethod == SampleEditorModule::SLICE_DIVIDE && se.marker_count() == 0) {
        const int div = std::max(se.sliceDivisions, 1);
        std::vector<int> cues;
        cues.reserve(static_cast<size_t>(std::max(div - 1, 0)));
        for (int i = 1; i < div; ++i)
            cues.push_back(static_cast<int>((static_cast<int64_t>(i) * se.totalFrames) / div));
        return cues;
    }

    // Whichever list the method is answering with — the detector's under TRANSIENT, the hand-placed one
    // under MANUAL (which starts as the file's, so ⭐ **a MANUAL save keeps the boundaries the sample
    // came with unless the user moved or deleted them**), and under OFF the file's own, so ⚠️ **a save
    // with slicing OFF can neither add a slice nor drop one.** A detour
    // through TRANSIENT leaves markers behind on purpose (they are what a return to it re-uses); writing
    // those into the `cue ` chunk would put slices into a file the user turned slicing off for, and
    // nothing on screen would say so.
    //
    // Frame 0 and the end frame are dropped: they are the sample's own bounds, not boundaries WITHIN
    // it, and a cue point at 0 gives every reader a zero-length first slice.
    std::vector<int> cues;
    for (int i = 0; i < se.marker_count(); ++i) {
        const int m = static_cast<int>(se.marker_position(i));
        if (m > 0 && m < se.totalFrames) cues.push_back(m);
    }
    return cues;
}

std::vector<std::pair<int64_t, int64_t>> InputDispatcher::current_slices() const {
    const SampleEditorState& se = s_.sampleEditor;

    // N markers → N+1 slices, whichever method the list came from — which is also what DIVIDE's div−1
    // computed cuts mean, so a dragged DIVIDE keeps its own count. OFF has nothing to chop; TRANSIENT
    // before the detector has run, and MANUAL with no boundaries at all, are one slice — the whole sample.
    const int markers = se.marker_count();
    const int count   = (se.sliceMethod == SampleEditorModule::SLICE_OFF)
                            ? 0
                            : (markers > 0)
                                  ? markers + 1
                                  : (se.sliceMethod == SampleEditorModule::SLICE_DIVIDE)
                                        ? std::max(se.sliceDivisions, 1)
                                        : 1;

    std::vector<std::pair<int64_t, int64_t>> out;
    out.reserve(static_cast<size_t>(std::max(count, 0)));
    for (int i = 0; i < count; ++i) {
        int64_t start = 0, end = 0;
        se.slice_bounds(i, start, end);
        out.emplace_back(start, end);
    }
    return out;
}

// ─── SAVE ────────────────────────────────────────────────────────────────────────────────────────

void InputDispatcher::save_sample_to(const std::string& path, bool adopt_name) {
    SampleEditorState& se   = s_.sampleEditor;
    const std::vector<int> cues = compute_slice_cue_points();

    if (!host_.save_sample_wav(se.instrumentId, path, cues, se.sourceMode, se.hasStereoData,
                               se.bitDepth)) {
        s_.statusMessage = "SAVE FAILED";
        s_.statusSuccess = false;
        return;
    }
    host_.adopt_saved_sample(se.instrumentId, se.bitDepth);

    // ⚠️ A MONO save is re-loaded from the file it just wrote, and that is not belt-and-braces. The
    // editor's buffer may still be STEREO (SOURCE=LEFT writes one channel of a two-channel sample), and
    // the slot would otherwise go on holding audio that no longer matches the file its instrument points
    // at. A true stereo save (SOURCE=STEREO) already matches, so it is left alone — re-decoding a
    // multi-megabyte file for nothing is exactly the cost the native load path exists to avoid.
    const bool wrote_mono = !(se.hasStereoData && se.sourceMode == 2);
    if (wrote_mono) host_.load_sample(se.instrumentId, path);

    Instrument& ins = host_.edit_project().instruments[static_cast<size_t>(se.instrumentId)];
    ins.sampleFilePath = path;
    // The markers go into the PROJECT as well as into the file — the .ptp is what a reload reads first,
    // and the two must agree.
    ins.sliceMarkers.clear();
    ins.sliceMarkers.reserve(cues.size());
    for (const int c : cues) ins.sliceMarkers.push_back(static_cast<int64_t>(c));

    se.sampleFilePath = path;
    if (adopt_name) se.sampleName = path_stem(path);
    se.isModified    = false;
    se.hasStereoData = host_.has_stereo_data(se.instrumentId);

    mark_modified();
    s_.currentScreen = s_.previousScreen;   // a save LEAVES the editor, as it does on Android
}

// ─── CHOP ────────────────────────────────────────────────────────────────────────────────────────

void InputDispatcher::sample_editor_chop() {
    SampleEditorState& se = s_.sampleEditor;
    if (se.sliceMethod == SampleEditorModule::SLICE_OFF) return;

    const std::vector<std::pair<int64_t, int64_t>> slices = current_slices();
    if (slices.empty()) return;

    // A slice becomes a FILE NAME, so anything a filesystem would choke on goes.
    std::string base = se.sampleName.empty() ? "SAMPLE" : se.sampleName;
    for (char& c : base) {
        const bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!safe) c = '_';
    }

    // Samples/Chops/<base>/ — its own folder, because a 32-slice break would otherwise bury the sample
    // directory it came from.
    const std::string samples = fs_.samples_directory();
    fs_.create_folder(samples, "Chops");                     // "" if it already exists — either is fine
    const std::string chops = samples + "/Chops";
    fs_.create_folder(chops, base);
    const std::string dir = chops + "/" + base;

    const int written = host_.chop_sample(se.instrumentId, dir, base, slices, se.bitDepth);
    s_.statusMessage   = written > 0 ? ("CHOPPED " + std::to_string(written)) : "CHOP FAILED";
    s_.statusSuccess   = written > 0;
}

// ─── The audition's deferred restore ─────────────────────────────────────────────────────────────

void InputDispatcher::run_due_sample_preview_restore(bool force) {
    if (!previewRestorePending_) return;
    if (!force && now_ms_ < previewRestoreAtMs_) return;

    previewRestorePending_ = false;
    host_.finish_sample_preview(previewRestoreInst_);
}

}  // namespace pt::ui
