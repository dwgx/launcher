use chrono::{Duration, Utc, DateTime};
use serde::{Serialize, Deserialize};

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Tier {
    #[serde(rename = "1day")]   Day1,
    #[serde(rename = "3day")]   Day3,
    #[serde(rename = "1week")]  Week1,
    #[serde(rename = "2week")]  Week2,
    #[serde(rename = "1month")] Month1,
}

impl Tier {
    pub fn duration(self) -> Duration {
        match self {
            Tier::Day1   => Duration::days(1),
            Tier::Day3   => Duration::days(3),
            Tier::Week1  => Duration::weeks(1),
            Tier::Week2  => Duration::weeks(2),
            Tier::Month1 => Duration::days(30),
        }
    }

    pub fn expires_from(self, base: DateTime<Utc>) -> DateTime<Utc> {
        base + self.duration()
    }

    pub fn parse(s: &str) -> Option<Tier> {
        Some(match s {
            "1day"   => Tier::Day1,
            "3day"   => Tier::Day3,
            "1week"  => Tier::Week1,
            "2week"  => Tier::Week2,
            "1month" => Tier::Month1,
            _ => return None,
        })
    }
}

pub fn now() -> DateTime<Utc> { Utc::now() }
