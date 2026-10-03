use crate::i18n::{Language, tr};
use anyhow::{Context, Result, bail};
use directories::ProjectDirs;
use serde::{Deserialize, Serialize};
use std::{
    fs,
    io::Write,
    path::{Path, PathBuf},
};

#[derive(Clone, Serialize, Deserialize)]
pub struct Console {
    pub id: u64,
    pub name: String,
    pub host: String,
    pub port: u16,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub token_env: Option<String>,
    /// Credentials entered in the UI never go to disk.
    #[serde(skip)]
    pub token: Option<String>,
}

impl Console {
    pub fn validate(&self) -> Result<()> {
        if self.name.trim().is_empty()
            || self.name.chars().count() > 48
            || self.name.chars().any(char::is_control)
        {
            bail!(tr("error.name"));
        }
        if self.host.is_empty()
            || self.host.len() > 253
            || self
                .host
                .chars()
                .any(|c| c.is_whitespace() || c.is_control())
            || self.host.contains('/')
        {
            bail!(tr("error.host"));
        }
        if self.port == 0 {
            bail!(tr("error.port"));
        }
        if self.token.as_ref().is_some_and(|t| t.len() > 255) {
            bail!(tr("error.token"));
        }
        Ok(())
    }

    pub fn credential(&self) -> Result<String> {
        let token = match &self.token {
            Some(t) => t.clone(),
            None => std::env::var(self.token_env.as_deref().unwrap_or("PSXTERM_TOKEN"))
                .unwrap_or_default(),
        };
        if token.len() > 255 {
            bail!(tr("error.token"));
        }
        Ok(token)
    }

    pub fn address(&self) -> String {
        if self.host.contains(':') {
            format!("[{}]:{}", self.host, self.port)
        } else {
            format!("{}:{}", self.host, self.port)
        }
    }
}

#[derive(Serialize, Deserialize)]
pub struct Config {
    #[serde(default = "version")]
    pub version: u32,
    pub consoles: Vec<Console>,
    #[serde(default)]
    pub language: Language,
}
fn version() -> u32 {
    1
}

impl Config {
    pub fn load(path: &Path) -> Result<Self> {
        let config = match fs::read(path) {
            Ok(bytes) => serde_json::from_slice::<Self>(&bytes).context(tr("error.config"))?,
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => Self {
                version: 1,
                consoles: vec![],
                language: Language::En,
            },
            Err(e) => return Err(e).context(tr("error.config_read")),
        };
        if config.version != 1 || config.consoles.len() > 32 {
            bail!(tr("error.config_version"));
        }
        let mut ids = std::collections::HashSet::new();
        for console in &config.consoles {
            console.validate()?;
            if !ids.insert(console.id) {
                bail!(tr("error.config_ids"));
            }
        }
        Ok(config)
    }

    pub fn save(&self, path: &Path) -> Result<()> {
        let parent = path
            .parent()
            .filter(|p| !p.as_os_str().is_empty())
            .unwrap_or(Path::new("."));
        fs::create_dir_all(parent)?;
        let mut temp = tempfile::NamedTempFile::new_in(parent)?;
        serde_json::to_writer_pretty(&mut temp, self)?;
        temp.write_all(b"\n")?;
        temp.as_file().sync_all()?;
        temp.persist(path)
            .map_err(|e| e.error)
            .context(tr("error.config_save"))?;
        Ok(())
    }

    pub fn next_id(&self) -> u64 {
        self.consoles
            .iter()
            .map(|c| c.id)
            .max()
            .unwrap_or(0)
            .saturating_add(1)
    }
}

pub fn default_path() -> Result<PathBuf> {
    Ok(ProjectDirs::from("dev", "seregonwar", "PSXTerm")
        .context(tr("error.config_dir"))?
        .config_dir()
        .join("consoles.json"))
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn secrets_are_not_saved_and_updates_replace_atomically() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("profiles.json");
        let mut c = Config {
            version: 1,
            language: Language::En,
            consoles: vec![Console {
                id: 1,
                name: "PS5".into(),
                host: "::1".into(),
                port: 2323,
                token_env: Some("MY_TOKEN".into()),
                token: Some("secret-value".into()),
            }],
        };
        c.save(&path).unwrap();
        assert!(!fs::read_to_string(&path).unwrap().contains("secret-value"));
        assert_eq!(
            Config::load(&path).unwrap().consoles[0].address(),
            "[::1]:2323"
        );
        c.consoles[0].name = "Studio".into();
        c.save(&path).unwrap();
        assert_eq!(Config::load(&path).unwrap().consoles[0].name, "Studio");
    }
    #[test]
    fn bad_config_is_reported_not_overwritten() {
        let d = tempfile::tempdir().unwrap();
        let p = d.path().join("c.json");
        fs::write(&p, "broken").unwrap();
        assert!(Config::load(&p).is_err());
        assert_eq!(fs::read_to_string(&p).unwrap(), "broken");
    }
}
