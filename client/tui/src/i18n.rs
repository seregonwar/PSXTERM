//! UI strings live in locales/*.json; protocol commands and GPL text are unchanged.
use serde::{Deserialize, Serialize};
use std::{
    collections::HashMap,
    sync::{
        OnceLock,
        atomic::{AtomicU8, Ordering},
    },
};
#[derive(Clone, Copy, Default, Debug, PartialEq, Eq, Serialize, Deserialize, clap::ValueEnum)]
#[serde(rename_all = "lowercase")]
pub enum Language {
    #[default]
    En,
    It,
}
static LANGUAGE: AtomicU8 = AtomicU8::new(0);
static EN: OnceLock<HashMap<String, String>> = OnceLock::new();
static IT: OnceLock<HashMap<String, String>> = OnceLock::new();
pub fn set(language: Language) {
    LANGUAGE.store(u8::from(language == Language::It), Ordering::Relaxed);
}
pub fn language() -> Language {
    if LANGUAGE.load(Ordering::Relaxed) == 1 {
        Language::It
    } else {
        Language::En
    }
}
pub fn tr(key: &str) -> String {
    translate(language(), key)
}
pub fn translate(language: Language, key: &str) -> String {
    let english = EN.get_or_init(|| {
        serde_json::from_str(include_str!("../locales/en.json")).expect("valid English resource")
    });
    let map = match language {
        Language::En => english,
        Language::It => IT.get_or_init(|| {
            serde_json::from_str(include_str!("../locales/it.json"))
                .expect("valid Italian resource")
        }),
    };
    map.get(key)
        .or_else(|| english.get(key))
        .cloned()
        .unwrap_or_else(|| format!("[{key}]"))
}
pub fn trf(key: &str, args: &[String]) -> String {
    let mut text = tr(key);
    for (i, arg) in args.iter().enumerate() {
        text = text.replace(&format!("{{{i}}}"), arg);
    }
    text
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn translations_have_matching_keys_and_placeholders() {
        let en: HashMap<String, String> =
            serde_json::from_str(include_str!("../locales/en.json")).unwrap();
        let it: HashMap<String, String> =
            serde_json::from_str(include_str!("../locales/it.json")).unwrap();
        assert_eq!(en.len(), it.len());
        for (key, value) in &en {
            let other = it.get(key).unwrap_or_else(|| panic!("missing key {key}"));
            for n in 0..8 {
                assert_eq!(
                    value.contains(&format!("{{{n}}}")),
                    other.contains(&format!("{{{n}}}")),
                    "placeholder {key}"
                );
            }
        }
        assert_eq!(translate(Language::En, "action.new"), "New terminal");
        assert_eq!(translate(Language::It, "action.new"), "Nuovo terminale");
    }
}
