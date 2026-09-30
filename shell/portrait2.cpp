#include "portrait2.h"

#include "button_glyphs.h"
#include "font.h"
#include "sdl-input.h"
#include "skin.h"

#include "ui/canvas.h"  // DESIGN_W / DESIGN_H — the 640×480 the FIT frame is fitted from

#include <algorithm>
#include <cmath>

namespace ptshell {

namespace tl = pt::ui::touch_layout;
using pt::ui::Button;

namespace {

SDL_Rect to_sdl(const tl::LayoutRect& lr) { return SDL_Rect{lr.x, lr.y, lr.w, lr.h}; }

// `VirtualBtnThemed`'s image selection, ported: the shifts are WIDE, A/B use the DARK square variant —
// falling back to the plain square when a theme ships no dark PNG, exactly as Kotlin's
// `buttonSquarePressedDark ?: buttonSquarePressed` does — and everything else is the plain square, each
// in its pressed or normal state. The fallback is resolved HERE (against what actually loaded) rather
// than in `Skin::draw`, which only knows how to no-op a missing piece, not which piece to try instead.
SkinPiece piece_for(const Skin& skin, Button b, bool pressed) {
    switch (b) {
        case Button::L_SHIFT:
        case Button::R_SHIFT:
            return pressed ? SkinPiece::BtnWidePressed : SkinPiece::BtnWideNormal;
        case Button::A:
        case Button::B: {
            const SkinPiece dark =
                pressed ? SkinPiece::BtnSquarePressedDark : SkinPiece::BtnSquareNormalDark;
            if (skin.piece(dark)) return dark;
            return pressed ? SkinPiece::BtnSquarePressed : SkinPiece::BtnSquareNormal;
        }
        default:
            return pressed ? SkinPiece::BtnSquarePressed : SkinPiece::BtnSquareNormal;
    }
}

// A PORTRAIT2 button's label, ported one-for-one from `VirtualControlsPortrait2`'s per-button call: the
// text (or, for the D-pad, an `Arrow` the shell draws itself — Helvetica has no arrow glyphs), which
// SIZE class it uses (large = A/B and the arrows; small = Sel/Start and the L/R shift), and which X
// OFFSET (wide = the two shift buttons; square for the rest). Y offset and the pressed shift are the
// same for all, so they are read from the metrics at the call site rather than repeated here.
struct Portrait2Label {
    const char* text;   // the letters; "" when arrow
    bool        arrow;
    Arrow       dir;    // meaningful only when arrow
    bool        large;  // large font (A/B/arrows) vs small (Sel/Start/L/R shift)
    bool        wide;   // wide X offset (L/R shift) vs the square offset
};

Portrait2Label label_for(Button b) {
    switch (b) {
        case Button::L_SHIFT:    return {"L Shift", false, Arrow::Up,    false, true};
        case Button::R_SHIFT:    return {"R Shift", false, Arrow::Up,    false, true};
        case Button::A:          return {"A",       false, Arrow::Up,    true,  false};
        case Button::B:          return {"B",       false, Arrow::Up,    true,  false};
        case Button::SELECT:     return {"Sel",     false, Arrow::Up,    false, false};
        case Button::START:      return {"Start",   false, Arrow::Up,    false, false};
        case Button::DPAD_UP:    return {"",        true,  Arrow::Up,    true,  false};
        case Button::DPAD_DOWN:  return {"",        true,  Arrow::Down,  true,  false};
        case Button::DPAD_LEFT:  return {"",        true,  Arrow::Left,  true,  false};
        case Button::DPAD_RIGHT: return {"",        true,  Arrow::Right, true,  false};
        default:                 return {"?",       false, Arrow::Up,    true,  false};
    }
}

// The D-pad arrow codepoints (↑↓←→, U+2190–2193) as UTF-8, for the arrow FONT path — Kotlin drew these
// exact characters as Text through the system fallback; the shell blits the real glyph from the bundled
// Linux Biolinum arrow font, same as a letter. Only reached when the arrow font loaded.
const char* arrow_utf8(Arrow d) {
    switch (d) {
        case Arrow::Up:    return "\xE2\x86\x91";  // ↑ U+2191
        case Arrow::Down:  return "\xE2\x86\x93";  // ↓ U+2193
        case Arrow::Left:  return "\xE2\x86\x90";  // ← U+2190
        case Arrow::Right: return "\xE2\x86\x92";  // → U+2192
    }
    return "";
}

// Material Design arrows and keyboard-shift icon in Hurmit Nerd Font.
// Keep TRNS labels and control symbols in one typeface and icon family.
const char* hermit_label(Button b) {
    switch (b) {
        case Button::L_SHIFT:    return u8"L \U000F0636";
        case Button::R_SHIFT:    return u8"R \U000F0636";
        case Button::A:          return "A";
        case Button::B:          return "B";
        case Button::SELECT:     return "sel.";
        case Button::START:      return "start.";
        case Button::DPAD_UP:    return u8"\U000F005D";
        case Button::DPAD_DOWN:  return u8"\U000F0045";
        case Button::DPAD_LEFT:  return u8"\U000F004D";
        case Button::DPAD_RIGHT: return u8"\U000F0054";
        default:                 return "?";
    }
}

// The arrow GLYPH is rendered at a FRACTION of the letter px. Measured against the Kotlin app's arrows
// (before/after device shots): Biolinum's arrow glyph fills more of the em than Android's system-fallback
// arrow did, so at the letter size it came out ~1.7x too tall. 0.6 matches the "before" size (ink height
// ratio 27/46 ≈ 0.59), and it is BASELINE-anchored (see draw_buttons) so shrinking keeps the arrow's
// bottom where a full glyph's baseline is — the before/after shots share that bottom edge exactly.
constexpr float ARROW_PX_FRAC = 0.6f;

// Sizing the FALLBACK line-arrow (used only when the arrow font is missing) so it reads at the same
// scale and height as the A/B letters beside it: the visible arrow is about a capital's height, and its
// box is centred on the letters' vertical mid-band.
constexpr float CAP_HEIGHT_FRAC = 0.72f;  // Helvetica cap height / em
constexpr float ARROW_BOX_FRAC  = 1.00f;  // arrow box / large-font px (the sprite carries its own margin)

}  // namespace

void PortraitSkin::layout(int outW, int outH, bool enabled, bool fit) {
    outW_ = outW;
    outH_ = outH;

    // PORTRAIT = the output is taller than it is wide. On a phone that is the physical orientation; on a
    // resizable desktop window it is dragging the window tall. A landscape or square output stays on the
    // plain centred present path (active_ == false) — every handheld, and the desktop's default shape.
    active_ = enabled && outW > 0 && outH > 0 && outH > outW;
    if (!active_) {
        buttons_.count = 0;
        frame_         = SDL_Rect{0, 0, 0, 0};
        return;
    }

    // The bands + the frame-in-bezel, host-checked by `pttouch --positions`. density=1 and the dp
    // fallback are inert for amiga-2 (its bezelThicknessX > 0), so the only inputs that decide the
    // geometry are the output size and the skin unit X the function derives from it.
    //
    // A CHROMELESS skin takes the BARE layout instead: no bands to place, so the screen area is the
    // whole device width and the cluster hangs off the bottom. Same struct, same cluster arithmetic
    // below — only where the two rects land differs, which is why this is one call site and not two
    // renderers.
    geom_ = chromeless() ? tl::portrait2_skin_bare(outW, outH)
                         : tl::portrait2_skin(outW, outH, /*density=*/1.0f, /*bezelThicknessDp=*/9.0f,
                                              /*bezelThicknessX=*/bezelX_);

    // SETTINGS > SCALING decides where the 640×480 frame lands inside the bezel. INTEGER uses the
    // pre-computed integer-scaled, centred `geom_.frame` (the tracker's own black canvas hides the gap).
    // FIT fills the bezel's inner area with the largest 4:3 fit — the SAME fractional scale Kotlin's
    // PortraitLayout2 BILINEAR arm uses (`min(innerW/640, innerH/480)`), centred — so the picture grows
    // to nearly fill the bezel instead of snapping to a whole multiple. The texture filtering that makes
    // a fractional scale smooth rather than uneven follows `SdlVideo::set_scaling`, which runs on the
    // same `scalingBilinear` value later in the frame; this only chooses the destination rect.
    if (fit && !geom_.innerBezel.empty()) {
        const tl::LayoutRect ib = geom_.innerBezel;
        const float s = std::min(static_cast<float>(ib.w) / pt::ui::DESIGN_W,
                                 static_cast<float>(ib.h) / pt::ui::DESIGN_H);
        const int   w = static_cast<int>(pt::ui::DESIGN_W * s);
        const int   h = static_cast<int>(pt::ui::DESIGN_H * s);
        frame_ = SDL_Rect{ib.x + (ib.w - w) / 2, ib.y + (ib.h - h) / 2, w, h};
    } else {
        frame_ = to_sdl(geom_.frame);
    }

    // The ten buttons inside the cluster band. `portrait2_rects` re-derives X from the band it is given,
    // and Kotlin hands it `buttonAreaH.coerceAtLeast(100)` — so that floor is restated here, not assumed.
    buttons_ = tl::portrait2_rects(geom_.buttons.w, std::max(geom_.buttons.h, 100));
}

void PortraitSkin::draw_chrome(SDL_Renderer* r, const Skin& skin, uint32_t innerBezelArgb) const {
    if (!active_) return;

    // The bare skin has no panels or bezel; its header is drawn after the frame in draw_buttons.
    if (chromeless()) return;

    // Band 1 — the vent panel (absent in case C, so guard on empty()).
    if (!geom_.topPanel.empty()) skin.draw(r, SkinPiece::TopPanel, to_sdl(geom_.topPanel));

    // Band 2 — the bezel, then its padded inner area painted the TRACKER's own background colour. Kotlin
    // (ScreenLayouts.kt:481) painted this black; the shell fills it with the live pt-ui theme background
    // instead, so the letterbox gap around the frame matches the tracker's own module fill and the whole
    // bezel reads as one surface — the same colour and the same reasoning as the landscape letterbox
    // (SdlVideo::present). It must go down BEFORE the frame, which present_skinned draws after this
    // underlay. The colour arrives as an argument (not stored) so the theme editor tracks live.
    skin.draw(r, SkinPiece::ScreenBezel, to_sdl(geom_.bezel));
    if (!geom_.innerBezel.empty()) {
        const SDL_Rect ib = to_sdl(geom_.innerBezel);
        SDL_SetRenderDrawColor(r, static_cast<Uint8>((innerBezelArgb >> 16) & 0xFF),
                               static_cast<Uint8>((innerBezelArgb >> 8) & 0xFF),
                               static_cast<Uint8>(innerBezelArgb & 0xFF), 255);
        SDL_RenderFillRect(r, &ib);
    }

    // Band 3 — the branding strip (full device width). Band 4 — the button backing; the buttons land on
    // top of it in draw_buttons, after the frame.
    skin.draw(r, SkinPiece::BrandingPanel, to_sdl(geom_.branding));
    skin.draw(r, SkinPiece::ButtonBacking, to_sdl(geom_.buttons));
}

void PortraitSkin::draw_buttons(SDL_Renderer* r, const Skin& skin, Font& font, Font& arrowFont,
                                Font& hermitFont, const SdlInput& input, bool playing, bool dirty,
                                const float peaks[2]) const {
    if (!active_) return;

    if (chromeless()) {
        // Overlay the unused top margin of the tracker frame, as in the reference screenshot.
        const int headerH = outW_ * 22 / 100;
        const int pad = std::max(4, headerH / 9);
        const int logoH = headerH * 31 / 100;
        const SkinTexture& logo = skin.piece(SkinPiece::Logo);
        if (logo && logo.height > 0) {
            const int logoW = logoH * logo.width / logo.height;
            skin.draw_tinted(r, SkinPiece::Logo,
                             SDL_Rect{outW_ - pad - logoW, headerH * 52 / 100,
                                      logoW, logoH}, ink_rgb());
        }
        if (hermitFont.loaded())
            hermitFont.draw_text("florktracker.", pad, headerH * 55 / 100,
                                 headerH * 0.26f, ink_rgb());

        // A quiet separator uses the same two live theme colours as the header and keys.
        const uint32_t bg = themeBg_, ink = ink_rgb();
        const auto faded = [bg, ink](int shift) {
            return static_cast<Uint8>((7 * ((bg >> shift) & 0xFF) + ((ink >> shift) & 0xFF)) / 8);
        };
        SDL_SetRenderDrawColor(r, faded(16), faded(8), faded(0), 255);
        SDL_RenderDrawLine(r, pad / 2, headerH - 1, outW_ - pad / 2, headerH - 1);

        // Keep the status below the tracker image, in the gap above the keys.
        const int gap = geom_.buttons.y - (frame_.y + frame_.h);
        if (hermitFont.loaded() && gap >= 34) {
            const int y = frame_.y + frame_.h + gap / 2 - 12;
            const float px = std::max(13.0f, outW_ * 0.027f);
            const float iconPx = px * 1.2f;
            const char* transportIcon = playing ? u8"\U000F040A" : u8"\U000F04DB";
            hermitFont.draw_text(transportIcon, pad, y - 5, iconPx, ink);
            hermitFont.draw_text(playing ? "play." : "stop.",
                                 pad + hermitFont.text_width(transportIcon, iconPx) + 5, y, px, ink);
            if (dirty) {
                const char* saveIcon = u8"\U000F0193";
                const int width = hermitFont.text_width(saveIcon, iconPx) + 5 +
                                  hermitFont.text_width("unsaved.", px);
                const int x = (outW_ - width) / 2;
                hermitFont.draw_text(saveIcon, x, y - 5, iconPx, ink);
                hermitFont.draw_text("unsaved.", x + hermitFont.text_width(saveIcon, iconPx) + 5,
                                     y, px, ink);
            }

            const int meterW = std::max(44, outW_ / 8);
            const int meterX = outW_ - pad - meterW;
            const int meterY = y + 3;
            const char* waveIcon = u8"\U000F147D";
            hermitFont.draw_text(waveIcon, meterX - hermitFont.text_width(waveIcon, iconPx) - 6,
                                 y - 10, iconPx, ink);
            SDL_SetRenderDrawColor(r, faded(16), faded(8), faded(0), 255);
            for (int channel = 0; channel < 2; ++channel) {
                const SDL_Rect track{meterX, meterY + channel * 9, meterW, 4};
                SDL_RenderFillRect(r, &track);
                const int level = playing ? static_cast<int>(std::clamp(peaks[channel], 0.0f, 1.0f) * 7) : 0;
                const SDL_Rect fill{meterX, track.y, meterW * level / 7, track.h};
                SDL_SetRenderDrawColor(r, static_cast<Uint8>((ink >> 16) & 0xFF),
                                       static_cast<Uint8>((ink >> 8) & 0xFF),
                                       static_cast<Uint8>(ink & 0xFF), 255);
                SDL_RenderFillRect(r, &fill);
                SDL_SetRenderDrawColor(r, faded(16), faded(8), faded(0), 255);
            }
        }
    }

    const int ox = geom_.buttons.x;
    const int oy = geom_.buttons.y;

    // The Helvetica label metrics, IN PIXELS. Density cancels for on-screen size exactly as it does for
    // positions (touch_layout.h): Kotlin draws `largeSp.sp` at `largeSp * density` px, and
    // largeSp = x*11/density, so the pixel size is x*11 — which is what `portrait2(..., density=1)`
    // returns as `large_sp`. So the SAME golden-checked Portrait2 fields give the on-device px with no
    // density threaded through the shell. Same coerced button-cluster box `portrait2_rects` used.
    const bool          useHelv = font.loaded();
    const tl::Portrait2 fm = tl::portrait2(geom_.buttons.w, std::max(geom_.buttons.h, 100), 1.0f);
    const auto          R = [](float v) { return static_cast<int>(std::lround(v)); };

    // ONE colour for the shape and the character on it. The TRANSPARENT skin's art is a bare outline
    // authored in white, so it takes its colour from the same tint the label does; the chrome skins'
    // art is finished casing art and must not be multiplied by anything, which is the whole of the
    // difference below.
    const uint32_t ink   = ink_rgb();
    const bool     tinted = chromeless();

    for (int i = 0; i < buttons_.count; ++i) {
        const tl::ButtonRect& br = buttons_.r[i];
        const SDL_Rect        dst{br.x + ox, br.y + oy, br.w, br.h};
        const bool            pressed = input.is_held(br.button);
        // FillBounds: RenderCopy stretches the button PNG to the cell — Compose's ContentScale.FillBounds.
        // A missing piece is a Skin::draw no-op, so an incomplete theme shows the backing through.
        const SkinPiece piece = piece_for(skin, br.button, pressed);
        if (tinted) skin.draw_tinted(r, piece, dst, ink);
        else        skin.draw(r, piece, dst);

        if (tinted && hermitFont.loaded()) {
            const Portrait2Label lab = label_for(br.button);
            const bool smallCaption = br.button == Button::SELECT || br.button == Button::START;
            const float px = smallCaption ? fm.small_sp * 0.82f
                                          : (lab.large ? fm.large_sp : fm.small_sp);
            const std::string text = hermit_label(br.button);
            const int x = lab.wide ? dst.x + dst.w / 4
                                   : dst.x + (dst.w - hermitFont.text_width(text, px)) / 2;
            const int y = dst.y + R(fm.off_y_dp) + (pressed ? R(fm.pressed_dp) : 0);
            hermitFont.draw_text(text, x, y, px, ink);
            continue;
        }

        // No Helvetica (asset missing / unparseable) → the shared 5×5 label font, as before it existed.
        if (!useHelv) {
            draw_label(r, br.button, dst, ink);
            continue;
        }

        const Portrait2Label lab  = label_for(br.button);
        const float          px   = lab.large ? fm.large_sp : fm.small_sp;
        const int            offX = R(lab.wide ? fm.wide_off_x_dp : fm.sq_off_x_dp);
        const int            offY = R(fm.off_y_dp) + (pressed ? R(fm.pressed_dp) : 0);

        if (lab.arrow) {
            // The D-pad. Preferred: the REAL arrow glyph from the bundled arrow font (Linux Biolinum),
            // blitted as text at the SAME left offset and top the letters use — so ↑↓←→ read like the A/B
            // letters beside them, exactly as Kotlin drew them (Text through the system fallback). If the
            // arrow font did not load, fall back to the shell-drawn smooth line arrow (a box a capital's
            // height on the letters' baseline), so a missing font degrades to lines rather than nothing.
            if (arrowFont.loaded()) {
                // Smaller than a letter (ARROW_PX_FRAC), BASELINE-anchored: place the shrunk glyph so its
                // baseline is exactly where a full-size glyph's would be (dst.y+offY+ascent(px)), which
                // keeps the arrow's BOTTOM fixed as it shrinks — matching the Kotlin "before" arrows,
                // which share that bottom edge with the (larger) full-size render.
                const float apx  = px * ARROW_PX_FRAC;
                const int   yTop = dst.y + offY + arrowFont.ascent_px(px) - arrowFont.ascent_px(apx);
                arrowFont.draw_text(arrow_utf8(lab.dir), dst.x + offX, yTop, apx, ink);
            } else {
                const int      baseline = dst.y + offY + font.ascent_px(px);
                const int      capH     = R(px * CAP_HEIGHT_FRAC);
                const int      side     = R(px * ARROW_BOX_FRAC);
                const SDL_Rect abox{dst.x + offX, baseline - capH / 2 - side / 2, side, side};
                font.draw_arrow(lab.dir, abox, ink);
            }
        } else {
            // Top-start + offset, like Kotlin's `Text` with `Alignment.TopStart` and a start/top padding.
            font.draw_text(lab.text, dst.x + offX, dst.y + offY, px, ink);
        }
    }
}

uint64_t PortraitSkin::signature(const SdlInput& input) const {
    if (!active_) return 0;

    uint64_t bits = 0;
    for (int i = 0; i < buttons_.count; ++i)
        if (input.is_held(buttons_.r[i].button))
            bits |= (1ull << static_cast<int>(buttons_.r[i].button));

    // ⚠️ A CHROMELESS skin's INK colour belongs in here. It is the live theme's TXT VALUE, applied at
    // blit time, so editing that one colour restyles the whole cluster without moving a canvas pixel or
    // changing the casing clear — the two things the C7 gate otherwise compares. Left out, the buttons
    // would keep the old colour until something else happened to force a frame: the gate's
    // blind-channel shape exactly, the same one the button HIGHLIGHT hits one panel over.
    //
    // Bits 10-15, which nothing else uses: the ten button flags end at bit 9, geometry starts at 16.
    // Six bits means this is a FOLD, not the colour — two inks in 64 can share a code, so it is the net
    // under the canvas compare rather than a channel of its own. Exactness would cost a documented
    // field; catching the case where the canvas genuinely did not change does not need it.
    if (chromeless()) {
        const uint32_t ink = ink_rgb();
        const uint32_t f   = (ink ^ (ink >> 12)) ^ ((ink ^ (ink >> 12)) >> 6);
        bits ^= static_cast<uint64_t>(f & 0x3Fu) << 10;
    }

    // Geometry too, so a rotate/resize that moves the skin forces a repaint even with the same buttons
    // held. Bit 62 marks "portrait active" — distinct from SdlTouch's bit 63 — so a landscape↔portrait
    // switch always changes the value the C7 gate compares, and the value is never 0 while active.
    return bits | (static_cast<uint64_t>(outW_ & 0xFFFF) << 16) |
           (static_cast<uint64_t>(outH_ & 0xFFFF) << 32) | (1ull << 62);
}

}  // namespace ptshell
