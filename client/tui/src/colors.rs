//! Predictable readable basic foreground colors, independent of the host palette.
use ratatui::style::Color;
use serde::{Deserialize, Serialize};

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Serialize, Deserialize, clap::ValueEnum)]
#[serde(rename_all = "lowercase")]
pub enum OutputColors {
    Original,
    #[default]
    Readable,
}

const READABLE: [(u8, u8, u8); 16] = [
    (150, 157, 171),
    (233, 105, 114),
    (146, 216, 137),
    (238, 207, 118),
    (128, 170, 255),
    (205, 155, 244),
    (101, 213, 221),
    (220, 222, 226),
    (164, 170, 181),
    (255, 145, 155),
    (176, 233, 166),
    (250, 222, 139),
    (169, 193, 255),
    (229, 186, 255),
    (157, 233, 240),
    (245, 247, 250),
];

impl OutputColors {
    pub fn foreground(self, index: u8) -> Color {
        if self == Self::Readable
            && let Some(&(r, g, b)) = READABLE.get(usize::from(index))
        {
            Color::Rgb(r, g, b)
        } else {
            Color::Indexed(index)
        }
    }
    pub fn toggle(self) -> Self {
        match self {
            Self::Original => Self::Readable,
            Self::Readable => Self::Original,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn luminance((r, g, b): (u8, u8, u8)) -> f64 {
        let linear = |c| {
            let c = f64::from(c) / 255.0;
            if c <= 0.04045 {
                c / 12.92
            } else {
                ((c + 0.055) / 1.055).powf(2.4)
            }
        };
        0.2126 * linear(r) + 0.7152 * linear(g) + 0.0722 * linear(b)
    }
    #[test]
    fn basic_colors_have_text_contrast_and_extended_colors_stay_original() {
        let background = luminance((24, 27, 31));
        for (index, rgb) in READABLE.into_iter().enumerate() {
            let contrast = (luminance(rgb) + 0.05) / (background + 0.05);
            assert!(contrast >= 4.5, "color {index}: {contrast}");
            assert_eq!(
                OutputColors::Original.foreground(index as u8),
                Color::Indexed(index as u8)
            );
        }
        for index in 16..=255 {
            assert_eq!(
                OutputColors::Readable.foreground(index),
                Color::Indexed(index)
            );
        }
    }
}
