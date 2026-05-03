// HTML form 反序列化辅助：空字符串字段 → Option::None。
//
// Why: axum::Form + serde 在 <input type="number"> 留空时收到 `field=`（empty string），
// 普通 `Option<i32>` 不会把 "" 解析成 None，而是尝试 i32::from_str("") 然后报
// "cannot parse integer from empty string"。这个 helper 把空串视作 None。
//
// How to apply:
//   #[derive(Deserialize)]
//   pub struct CreateForm {
//       #[serde(default, deserialize_with = "launcher_shared::formhelp::empty_str_as_none")]
//       pub days: Option<i64>,
//   }

use serde::{Deserialize, Deserializer};
use std::fmt::Display;
use std::str::FromStr;

pub fn empty_str_as_none<'de, D, T>(d: D) -> Result<Option<T>, D::Error>
where
    D: Deserializer<'de>,
    T: FromStr,
    T::Err: Display,
{
    let opt: Option<String> = Option::deserialize(d)?;
    match opt.as_deref() {
        None | Some("") => Ok(None),
        Some(s) => s.parse().map(Some).map_err(serde::de::Error::custom),
    }
}
