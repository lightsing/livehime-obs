#pragma once

#include <QGuiApplication>
#include <QPalette>
#include <array>
#include <cmath>

namespace livehime {
// OBS applies its active theme to the application palette before loading its QSS.
// Keep semantic colors out of stored chat and ranking rows so existing rows recolor too.
struct PanelPalette {
    QColor surface, card, text, muted, accent, hover, divider;
    QColor giftAction, giftName, entryStart, entryEnd, connected, retrying;

    static QColor mix(QColor from, QColor to, qreal amount) {
        return QColor::fromRgb(qRound(from.red() + (to.red() - from.red()) * amount),
                               qRound(from.green() + (to.green() - from.green()) * amount),
                               qRound(from.blue() + (to.blue() - from.blue()) * amount));
    }

    static qreal luminance(QColor color) {
        static const auto linear = [] {
            std::array<qreal, 256> table{};
            for (int i = 0; i < 256; ++i) {
                const qreal c = i / 255.;
                table[i] = c <= .04045 ? c / 12.92 : std::pow((c + .055) / 1.055, 2.4);
            }
            return table;
        }();
        return .2126 * linear[color.red()] + .7152 * linear[color.green()] + .0722 * linear[color.blue()];
    }
    static qreal contrast(QColor a, QColor b) {
        const auto x = luminance(a), y = luminance(b);
        return (qMax(x, y) + .05) / (qMin(x, y) + .05);
    }
    static QColor readable(QColor ink, QColor background, QColor preferred, qreal minimum = 4.5) {
        if (contrast(ink, background) >= minimum)
            return ink;
        if (contrast(preferred, background) < minimum)
            preferred = contrast(Qt::white, background) > contrast(Qt::black, background) ? Qt::white : Qt::black;
        for (int step = 1; step < 16; ++step) {
            const auto color = mix(ink, preferred, step / 16.);
            if (contrast(color, background) >= minimum)
                return color;
        }
        return preferred;
    }
    explicit PanelPalette(const QPalette &p) {
        surface = p.color(QPalette::Active, QPalette::Window);
        text = readable(p.color(QPalette::Active, QPalette::WindowText), surface, p.color(QPalette::Active, QPalette::Text));
        card = p.color(QPalette::Active, QPalette::Base);
        if (contrast(surface, card) < 1.12)
            card = mix(surface, text, .08);
        // Custom themes can use opposite polarities for inputs and windows.
        if (contrast(text, card) < 4.5)
            card = mix(surface, text, .08);
        const QColor primary = p.color(QPalette::Active, QPalette::Highlight);
        muted = readable(mix(text, surface, .30), card, text);
        accent = readable(readable(primary, surface, text), card, text);
        hover = mix(surface, accent, .12);
        divider = readable(mix(surface, accent, .52), surface, text, 2.0);
        giftAction = readable(mix(muted, accent, .35), card, text);
        // Muted amber keeps gifts recognizable even in a monochrome OBS theme.
        giftName = readable(mix(QColor("#eabc48"), text, .18), card, text);
        entryStart = accent;
        entryEnd = readable(mix(accent, text, .48), card, text);
        connected = mix(surface, QColor("#39a775"), .27);
        retrying = mix(surface, QColor("#d55763"), .38);
    }
    QColor badge(QColor source) const { return mix(card, source.isValid() ? source : accent, .31); }
    QColor badgeText(QColor fill, QColor source = {}) const {
        return readable(source.isValid() ? source : text, fill, text);
    }
    static const PanelPalette &current() {
        const auto palette = QGuiApplication::palette();
        static auto key = palette.cacheKey();
        static PanelPalette colors(palette);
        if (key != palette.cacheKey()) {
            colors = PanelPalette(palette);
            key = palette.cacheKey();
        }
        return colors;
    }
};
} // namespace livehime
